#include "ui/imgui/markdown/MarkdownImageCache.h"

#include "common/VideoFrameDecoder.h"
#include "graphic/imguivk/VKTexture.h"
#include "runtime/AppThreadPool.h"
#include "ui/imgui/markdown/MarkdownParser.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <ice/thread/ThreadPool.hpp>
#include <map>
#include <memory>
#include <span>
#include <stb_image.h>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

/// @file MarkdownImageCache.cpp
/// @brief Markdown 远程图片的安全 URL 解析、后台下载解码、图集与 GPU 缓存实现。
/// @details UI 线程只扫描文档、排队键和查询已上传纹理；网络、图片解码及
/// GIF 临时文件操作全部在线程池任务中完成，GPU 上传只在纹理准备阶段执行。
///
/// 资源限制：
/// - 单次下载最多 32 MiB；
/// - 内存 GIF 源帧数据最多 128 MiB，超出时交给逐帧视频解码器；
/// - 单张输入宽高最多 4096 像素；
/// - GIF 时长最多 120 秒；
/// - GIF 采样最多 96 帧且目标约 15 FPS；
/// - 动画单帧最长边缩小到 320 像素；
/// - 静态图最长边缩小到 1280 像素；
/// - 图集单行宽度不超过 4096 像素；
/// - 全缓存已上传 RGBA 数据预算为 192 MiB；
/// - 文档缓存最多接纳 32 个不同目标；
/// - 同时只运行一个下载与解码任务。
/// - 失败目标保留条目状态，避免可见帧反复发起相同请求。

namespace MMM::UI
{
namespace
{
constexpr std::size_t MAX_DOWNLOAD_BYTES =
    32U * 1024U * 1024U;  ///< 单个下载上限。
constexpr std::size_t MAX_CACHE_BYTES =
    192U * 1024U * 1024U;  ///< GPU 图集总预算。
constexpr std::size_t MAX_MEMORY_GIF_BYTES =
    128U * 1024U * 1024U;  ///< 内存 GIF 源帧总预算。

/// @brief 跳过 GIF 扩展或图像数据的连续子块。
/// @param bytes 完整 GIF 文件字节。
/// @param offset 当前读取位置，成功后指向下一个块标记。
/// @return 子块由零长度块正确终止时返回 true。
/// @details GIF 扩展和图像压缩数据都使用长度前缀子块；不能按字节值搜索
/// 终止符，否则压缩内容中的零值会被误认成块边界。
bool skipGifSubBlocks(std::span<const unsigned char> bytes, std::size_t& offset)
{
    while ( offset < bytes.size() ) {
        // 先消费长度字节，零长度表示该组子块结束。
        const auto length = bytes[offset++];
        if ( length == 0U ) return true;
        // 不完整的块不能进入 stb 解码，以免预扫描低估源帧内存。
        if ( length > bytes.size() - offset ) return false;
        offset += length;
    }
    return false;
}

/// @brief 在调用整图 GIF 解码器前统计帧数并限制解压后的源帧容量。
/// @param bytes 已通过签名和尺寸校验的 GIF 文件。
/// @param width 逻辑画布宽度。
/// @param height 逻辑画布高度。
/// @return 可安全整图解码时返回帧数，否则返回零并交给逐帧解码路径。
/// @pre width 和 height 已由 stbi_info_from_memory 验证为正且不超过 4096。
/// @details 逐块解析 GIF 容器，只读取长度和标记，不解压 LZW 数据。
/// 每个图像块最多产生一张完整逻辑画布，故帧数乘画布字节数是源帧容量上界。
std::size_t boundedGifFrameCount(std::span<const unsigned char> bytes,
                                 unsigned width, unsigned height)
{
    // GIF 每帧由完整逻辑画布构成；先限制单帧，再逐个图像块累计数量。
    const std::size_t frameBytes =
        static_cast<std::size_t>(width) * height * 4U;
    if ( bytes.size() < 13U || frameBytes > MAX_MEMORY_GIF_BYTES ) return 0U;
    std::size_t offset = 13U;
    // 逻辑屏幕描述符后可能紧跟全局调色板，位数由 packed 低三位决定。
    if ( (bytes[10] & 0x80U) != 0U ) {
        const auto paletteBytes = 3U * (1U << ((bytes[10] & 0x07U) + 1U));
        if ( paletteBytes > bytes.size() - offset ) return 0U;
        offset += paletteBytes;
    }

    std::size_t frames = 0U;
    while ( offset < bytes.size() ) {
        // 只有标准 trailer 才完成预扫描；截断数据不进入整图解码。
        const auto marker = bytes[offset++];
        if ( marker == 0x3BU ) return frames;
        if ( marker == 0x21U ) {
            // 扩展块包含一个标签，后续内容同样按子块长度跳过。
            // 图形控制扩展不算作图像帧。
            if ( offset == bytes.size() ) return 0U;
            ++offset;
            if ( !skipGifSubBlocks(bytes, offset) ) return 0U;
            continue;
        }
        // 图像描述符固定九字节，packed 位还可声明独立的局部调色板。
        if ( marker != 0x2CU || bytes.size() - offset < 9U ) return 0U;
        const auto packed = bytes[offset + 8U];
        offset += 9U;
        if ( (packed & 0x80U) != 0U ) {
            const auto paletteBytes = 3U * (1U << ((packed & 0x07U) + 1U));
            if ( paletteBytes > bytes.size() - offset ) return 0U;
            offset += paletteBytes;
        }
        // LZW 最小码长占一字节，之后才是以零长度块结尾的图像数据。
        if ( offset == bytes.size() ) return 0U;
        ++offset;
        if ( !skipGifSubBlocks(bytes, offset) ) return 0U;
        // 在解码前累计帧数，防止高度压缩的长动画放大内存。
        if ( ++frames > MAX_MEMORY_GIF_BYTES / frameBytes ) return 0U;
    }
    return 0U;
}

/// @brief 限制下载大小，拒绝无界响应。
/// @param data libcurl 本次提供的响应数据。
/// @param size 单个数据单元字节数。
/// @param count 数据单元数量。
/// @param context 指向目标字节容器的上下文。
/// @return 成功追加的字节数；超过上限时返回零中止传输。
std::size_t appendDownload(char* data, std::size_t size, std::size_t count,
                           void* context)
{
    // curl 可能传入 size*count，先用除法形式检查乘法和总容量溢出。
    auto& bytes = *static_cast<std::vector<unsigned char>*>(context);
    if ( size && count > (MAX_DOWNLOAD_BYTES - bytes.size()) / size ) return 0;
    // 通过上限检查后乘法和 vector 扩展都在允许范围内。
    const auto length = size * count;
    bytes.insert(bytes.end(), data, data + length);
    return length;
}
/// @brief 将一帧缩小后写入图集，避免存储原尺寸动画的全部帧。
/// @param out 已分配目标图集及帧布局。
/// @param index 目标帧索引。
/// @param source 源 RGBA 像素首地址。
/// @param width 源帧宽度。
/// @param height 源帧高度。
/// @note 使用最近邻采样，避免后台缩略图生成引入额外滤镜缓冲。
void copyFrame(MarkdownImagePixels& out, unsigned index,
               const unsigned char* source, unsigned width, unsigned height)
{
    // 帧索引按固定列数映射到图集左上角。
    const unsigned left = (index % out.columns) * out.frameWidth;
    const unsigned top  = (index / out.columns) * out.frameHeight;
    for ( unsigned y = 0; y < out.frameHeight; ++y ) {
        for ( unsigned x = 0; x < out.frameWidth; ++x ) {
            // 目标像素按整数比例映射回源像素，始终落在源边界内。
            const auto sourceOffset =
                (static_cast<std::size_t>(y) * height / out.frameHeight *
                     width +
                 static_cast<std::size_t>(x) * width / out.frameWidth) *
                4U;
            // 图集以紧密 RGBA 行主序存储。
            const auto targetOffset =
                (static_cast<std::size_t>(top + y) * out.width + left + x) * 4U;
            std::memcpy(
                out.pixels.data() + targetOffset, source + sourceOffset, 4U);
        }
    }
}
/// @brief 创建有界帧网格，保持宽高比。
/// @param width 源帧宽度。
/// @param height 源帧高度。
/// @param frames 待存储帧数。
/// @param duration 动画循环时长；静态图为零。
/// @return 已分配 RGBA 缓冲和完整帧布局。
MarkdownImagePixels makeAtlas(unsigned width, unsigned height, unsigned frames,
                              double duration)
{
    MarkdownImagePixels out;
    // 动画采用较小上限控制多帧内存，静态图保留更高阅读分辨率。
    const double scale =
        std::min(1.0, (frames > 1U ? 320.0 : 1280.0) / std::max(width, height));
    out.frameWidth  = std::max(1U, static_cast<unsigned>(width * scale));
    out.frameHeight = std::max(1U, static_cast<unsigned>(height * scale));
    // 列数同时受帧数和 4096 像素图集宽度限制。
    out.columns = std::min(frames, 4096U / out.frameWidth);
    out.frames  = frames;
    out.width   = out.columns * out.frameWidth;
    // 向上取整行数，最后一行允许未填满。
    out.height = ((frames + out.columns - 1U) / out.columns) * out.frameHeight;
    out.duration = duration;
    // 一次分配完整图集，后续帧直接写入对应区域。
    out.pixels.resize(static_cast<std::size_t>(out.width) * out.height * 4U);
    return out;
}

/// @brief 使用 stb_image 在内存中解码有界 GIF，并按播放时间采样为图集。
/// @param bytes GIF 文件字节。
/// @param expectedWidth 已验证的逻辑画布宽度。
/// @param expectedHeight 已验证的逻辑画布高度。
/// @param maximumFrames 预扫描确认的源帧数量。
/// @return 解码成功时返回动画图集，否则返回空布局。
/// @details 仅对源帧容量经过预扫描的 GIF 使用此路径，避免依赖可选的
/// FFmpeg GIF 解复用器；输出仍按约 15 FPS 和 96 帧上限采样。
/// @warning 后台资源路径：完整 GIF 解码会分配源帧缓冲，禁止在 UI 热路径调用。
MarkdownImagePixels decodeBoundedGif(std::span<const unsigned char> bytes,
                                     unsigned    expectedWidth,
                                     unsigned    expectedHeight,
                                     std::size_t maximumFrames)
{
    int* rawDelays = nullptr;
    int  width = 0, height = 0, sourceFrames = 0, channels = 0;
    // stb 分别分配像素和毫秒延时数组，两个所有权都立即交给 RAII。
    auto pixels = std::unique_ptr<unsigned char, decltype(&stbi_image_free)>(
        stbi_load_gif_from_memory(bytes.data(),
                                  static_cast<int>(bytes.size()),
                                  &rawDelays,
                                  &width,
                                  &height,
                                  &sourceFrames,
                                  &channels,
                                  4),
        stbi_image_free);
    auto delays = std::unique_ptr<int, decltype(&stbi_image_free)>(
        rawDelays, stbi_image_free);
    // 解码结果必须与预扫描画布及帧数一致，才可按连续 RGBA 帧寻址。
    if ( !pixels || !delays || width != static_cast<int>(expectedWidth) ||
         height != static_cast<int>(expectedHeight) || sourceFrames <= 0 ||
         static_cast<std::size_t>(sourceFrames) > maximumFrames )
        return {};

    // GIF 的零延时帧按 10 ms 展示；累计时长沿用既有 120 秒上限。
    // 延时由解码器按帧提供，不能用均匀帧率代替原动画节奏。
    double durationMs = 0.0;
    for ( int index = 0; index < sourceFrames; ++index ) {
        durationMs += std::max(delays.get()[index], 10);
        if ( durationMs > 120000.0 ) return {};
    }
    const double duration = durationMs / 1000.0;
    // 图集帧数只依赖总时长，不随源文件的帧数线性增长。
    const unsigned frames =
        std::clamp(static_cast<unsigned>(std::ceil(duration * 15.0)), 1U, 96U);
    auto       out = makeAtlas(expectedWidth, expectedHeight, frames, duration);
    const auto frameBytes =
        static_cast<std::size_t>(expectedWidth) * expectedHeight * 4U;
    int    sourceIndex = 0;
    double sourceEndMs = std::max(delays.get()[0], 10);
    for ( unsigned index = 0; index < frames; ++index ) {
        // 目标采样时间递增，源索引也只前进；无需为每个图集帧重新查找。
        const double targetMs = durationMs * index / frames;
        while ( sourceIndex + 1 < sourceFrames && targetMs >= sourceEndMs ) {
            ++sourceIndex;
            sourceEndMs += std::max(delays.get()[sourceIndex], 10);
        }
        // 源帧按整张逻辑画布顺序排列，拷贝时才缩放到最终图集尺寸。
        copyFrame(out,
                  index,
                  pixels.get() + sourceIndex * frameBytes,
                  expectedWidth,
                  expectedHeight);
    }
    return out;
}
/// @brief 只清理本次成功创建的独立临时目录，不触及用户资源。
struct TemporaryImage {
    std::filesystem::path path;  ///< 精确的任务目录。
    /// @brief 解码器先关闭文件后，回收任务临时文件。
    ~TemporaryImage()
    {
        if ( !path.empty() ) {
            // 只对成功创建并记录的唯一任务目录执行递归清理。
            std::error_code ec;
            // 清理失败不覆盖主要解码结果，也不通过异常退出后台任务。
            std::filesystem::remove_all(path, ec);
        }
    }
};
}  // namespace

/// @brief 将 Markdown 图片目标解析为更新站点允许的 HTTP(S) URL。
/// @param destination Markdown 圆括号内的原始目标。
/// @return 合法绝对 URL；控制字符、本地路径或未知 scheme 返回空。
/// @details 绝对 HTTP(S) 原样保留，协议相对地址补 https，根相对地址绑定
/// 站点根，普通相对地址绑定更新检查目录；任何其他冒号形式均被拒绝。
std::string resolveUpdateImageUrl(std::string_view destination)
{
    // 换行、制表符和反斜杠可能造成请求拆分或本地路径歧义，统一拒绝。
    if ( destination.empty() ||
         destination.find_first_of("\r\n\t\\") != std::string_view::npos )
        return {};
    if ( destination.starts_with("https://") ||
         destination.starts_with("http://") )
        // 网络层后续仍限制实际与重定向协议为 HTTP(S)。
        return std::string(destination);
    if ( destination.starts_with("//") )
        // 协议相对目标固定升级为 https。
        return "https:" + std::string(destination);
    if ( destination.starts_with('/') )
        // 根相对资源绑定官方站点域名。
        return "https://mmm.xiang233.top" + std::string(destination);
    // 其余包含冒号的目标可能是 file、data 或自定义 scheme，禁止请求。
    if ( destination.find(':') != std::string_view::npos ) return {};
    // 普通文件名或相对路径按更新检查页面目录解析。
    return "https://mmm.xiang233.top/download/check/" +
           std::string(destination);
}

/// @brief 下载并解码一个已验证的更新图片 URL。
/// @param url 由 resolveUpdateImageUrl 生成的 HTTP(S) URL。
/// @return 成功时返回有界 RGBA 图集，失败时返回空布局。
/// @warning 后台低频路径：执行网络请求，禁止从 UI 或渲染热路径直接调用。
MarkdownImagePixels loadUpdateImage(const std::string& url)
{
    // 下载字节容器由 curl 回调按 32 MiB 上限增长。
    std::vector<unsigned char> bytes;
    // unique_ptr 保证所有提前返回路径都执行 curl_easy_cleanup。
    auto curl = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>(
        curl_easy_init(), curl_easy_cleanup);
    if ( !curl ) return {};
    // 初始和重定向协议都限制为 HTTP(S)，不允许 file 等本地 scheme。
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    // 允许有限重定向以兼容 CDN，但最多三跳。
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 3L);
    // 连接和总任务都有明确超时，避免后台 future 长期占用。
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L);
    // 后台线程禁用信号式超时，并把 HTTP 错误转为失败返回。
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, appendDownload);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &bytes);
    // 空响应或任意传输错误都不进入图片解码器。
    if ( curl_easy_perform(curl.get()) != CURLE_OK || bytes.empty() ) return {};
    return decodeUpdateImage(bytes);
}

/// @brief 将下载字节解码为静态或动画 RGBA 图集。
/// @param bytes 受下载上限约束的图片文件内容。
/// @return 解码和尺寸校验成功时返回图集，否则返回空。
/// @warning 后台低频路径：GIF 会创建任务专属临时目录并逐帧调用解码器。
MarkdownImagePixels decodeUpdateImage(std::span<const unsigned char> bytes)
{
    // 直接调用场景也必须执行与下载回调相同的大小边界。
    if ( bytes.empty() || bytes.size() > MAX_DOWNLOAD_BYTES ) return {};
    // 先读取头部尺寸，拒绝无效或超过 GPU 支持边界的图像。
    int width = 0, height = 0, channels = 0;
    if ( !stbi_info_from_memory(bytes.data(),
                                static_cast<int>(bytes.size()),
                                &width,
                                &height,
                                &channels) ||
         width <= 0 || height <= 0 || width > 4096 || height > 4096 )
        return {};
    // 只把标准 GIF87a/GIF89a 签名交给多帧视频解码路径。
    const bool gif =
        bytes.size() >= 6U && (std::memcmp(bytes.data(), "GIF89a", 6U) == 0 ||
                               std::memcmp(bytes.data(), "GIF87a", 6U) == 0);
    if ( !gif ) {
        // 静态格式由 stb_image 直接从内存解码为强制四通道 RGBA。
        auto pixels =
            std::unique_ptr<unsigned char, decltype(&stbi_image_free)>(
                stbi_load_from_memory(bytes.data(),
                                      static_cast<int>(bytes.size()),
                                      &width,
                                      &height,
                                      &channels,
                                      4),
                stbi_image_free);
        if ( !pixels ) return {};
        // 静态图也通过单帧图集路径缩放并统一布局结构。
        auto out = makeAtlas(width, height, 1U, 0.0);
        copyFrame(out, 0U, pixels.get(), width, height);
        return out;
    }
    // 常见小型 GIF 直接从内存读取，避免依赖各平台预编译 FFmpeg 的 GIF 能力。
    // 大型 GIF 保留原有逐帧解码路径，以免一次展开全部源帧。
    const auto boundedFrames = boundedGifFrameCount(bytes, width, height);
    if ( boundedFrames > 0U ) {
        auto decoded = decodeBoundedGif(bytes, width, height, boundedFrames);
        if ( !decoded.pixels.empty() ) return decoded;
    }
    // 预扫描超出源帧预算时仍尝试逐帧解码，以保留大型动画的现有支持。
    // 该路径不持有整段动画的完整 RGBA 源帧。
    // GIF 解码器需要文件路径，先取得系统临时目录。
    std::error_code ec;
    const auto      root = std::filesystem::temp_directory_path(ec);
    if ( ec ) return {};
    // RAII 对象只清理本任务成功创建的唯一目录。
    TemporaryImage temporary;
    const auto     stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for ( unsigned attempt = 0; attempt < 16U; ++attempt ) {
        // 单调时间戳加尝试序号降低并发任务目录碰撞概率。
        auto path = root / ("mmm-update-image-" + std::to_string(stamp) + "-" +
                            std::to_string(attempt));
        if ( std::filesystem::create_directory(path, ec) ) {
            // 只有 create_directory 明确成功才记录为可清理目录。
            temporary.path = std::move(path);
            break;
        }
    }
    if ( temporary.path.empty() ) return {};
    // 固定文件名只存在于任务私有目录中，不与用户资源冲突。
    const auto path = temporary.path / "image.gif";
    {
        std::ofstream file(path, std::ios::binary);
        // 原始字节完整写入，交由统一 VideoFrameDecoder 读取各帧。
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        file.close();
        if ( !file ) return {};
    }
    // 解码器在 TemporaryImage 析构前关闭，避免 Windows 文件占用阻止清理。
    Utils::VideoFrameDecoder decoder;
    if ( !decoder.open(path) ) return {};
    // 非有限、零长度或超长动画会放大采样成本，统一拒绝。
    const double duration = decoder.info().duration;
    if ( !std::isfinite(duration) || duration <= 0.0 || duration > 120.0 )
        return {};
    // 约 15 FPS 采样并限制为 1–96 帧，长动画会降低实际采样率。
    const unsigned frames =
        std::clamp(static_cast<unsigned>(std::ceil(duration * 15.0)), 1U, 96U);
    auto out = makeAtlas(width, height, frames, duration);
    for ( unsigned index = 0; index < frames; ++index ) {
        // 在整个动画时长内均匀取样，不保存解码器原始全部帧。
        const auto* frame = decoder.decodeFrameAt(duration * index / frames);
        if ( !frame ) return {};
        copyFrame(out, index, frame->rgba.data(), frame->width, frame->height);
    }
    return out;
}

/// @brief 缓存仅在 UI/资源准备线程访问，工作任务只返回独占 CPU 数据。
struct MarkdownImageCache::Impl {
    /// @brief 单个 Markdown 目标的 GPU 纹理、动画布局和失败状态。
    struct Entry {
        std::unique_ptr<Graphic::VKTexture> m_texture;  ///< 图集 GPU 所有权。
        MarkdownImagePixels                 m_layout;  ///< 不保留像素的帧布局。
        ImTextureID m_id{};         ///< 资源准备阶段注册的描述符。
        double      m_startTime{};  ///< 动画起播时钟。
        bool        m_failed{};     ///< 失败后不逐帧重复请求。
    };
    std::map<std::string, Entry, std::less<>>
                                     m_entries;  ///< 按 Markdown 目标去重。
    std::vector<std::string>         m_pending;  ///< 有界、尚未开始下载的键。
    std::string                      m_active;   ///< 当前任务键。
    std::future<MarkdownImagePixels> m_future;   ///< 单任务非阻塞交接。
    std::size_t                      m_bytes{};  ///< 已上传图集总大小。
};
/// @brief 创建空图片缓存，不启动网络请求或 GPU 上传。
MarkdownImageCache::MarkdownImageCache()
    : IUIView("UpdateMarkdownImages")
    , ITextureLoader("UpdateMarkdownImages")
    , m_impl(std::make_unique<Impl>())
{
}
/// @brief 释放缓存实现及其独占纹理和任务 future。
/// @warning UIManager 保证析构前后台任务完成且 GPU 不再引用纹理。
MarkdownImageCache::~MarkdownImageCache() = default;
/// @brief 扫描 Markdown 文档中的图片目标并加入有界去重队列。
/// @param markdown 待显示的完整文档文本。
/// @warning UI 低频路径：只解析内存文本，不执行网络或文件操作。
void MarkdownImageCache::prepareDocument(std::string_view markdown)
{
    // 围栏代码内容不应被解释为真实图片请求。
    visitMarkdownBlocks(markdown, [&](const MarkdownBlock& block) {
        if ( block.kind == MarkdownBlockKind::Code ) return;
        visitMarkdownInline(block.text, [&](const MarkdownInlineSpan& span) {
            // 非图片、已登记目标和超过条目上限的目标都直接忽略。
            if ( span.kind != MarkdownInlineKind::Image ||
                 m_impl->m_entries.contains(span.destination) ||
                 m_impl->m_entries.size() >= 32U )
                return;
            // map 以原始 Markdown 目标为键，渲染查询无需再次规范化。
            auto  key   = std::string(span.destination);
            auto& entry = m_impl->m_entries[key];
            // URL 不合法时永久标记失败，避免每帧重复排队。
            entry.m_failed = resolveUpdateImageUrl(key).empty();
            if ( !entry.m_failed ) m_impl->m_pending.push_back(std::move(key));
        });
    });
}
/// @brief 启动下一后台图片任务并非阻塞轮询当前任务完成状态。
/// @return 当前 future 已就绪、需要进入纹理准备阶段时返回 true。
/// @warning UI 热路径：不等待任务；至多在队列切换时提交一个线程池工作。
bool MarkdownImageCache::needReload()
{
    if ( !m_impl->m_future.valid() && !m_impl->m_pending.empty() ) {
        // 单任务模型防止多个大图同时占用下载和解码内存。
        auto* pool = Runtime::AppThreadPool::instance().get();
        if ( !pool ) return false;
        // 取出队首作为唯一活动键，future 结果与该键一一对应。
        m_impl->m_active = std::move(m_impl->m_pending.front());
        m_impl->m_pending.erase(m_impl->m_pending.begin());
        // prepareDocument 已验证过 URL，此处重新解析获得任务捕获值。
        const auto url = resolveUpdateImageUrl(m_impl->m_active);
        m_impl->m_future =
            pool->enqueue([url]() { return loadUpdateImage(url); });
    }
    // 零秒 wait_for 只查询状态，不阻塞 UI 线程。
    return m_impl->m_future.valid() &&
           m_impl->m_future.wait_for(std::chrono::seconds(0)) ==
               std::future_status::ready;
}
/// @brief 消费已完成 CPU 图集并创建对应 Vulkan 纹理。
/// @param physical Vulkan 物理设备。
/// @param device Vulkan 逻辑设备。
/// @param pool 上传命令池。
/// @param queue 执行上传的 Vulkan 队列。
/// @warning 低频资源准备路径：仅 future 就绪时分配和上传 GPU 资源。
void MarkdownImageCache::reloadTextures(vk::PhysicalDevice& physical,
                                        vk::Device&         device,
                                        vk::CommandPool& pool, vk::Queue& queue)
{
    // 防御渲染器提前调用，未完成任务绝不在此等待。
    if ( !m_impl->m_future.valid() ||
         m_impl->m_future.wait_for(std::chrono::seconds(0)) !=
             std::future_status::ready )
        return;
    // get 同时使 future 失效，下一次 needReload 可以启动后续排队任务。
    auto       data = m_impl->m_future.get();
    const auto it   = m_impl->m_entries.find(m_impl->m_active);
    if ( it == m_impl->m_entries.end() ) return;
    auto& entry = it->second;
    if ( data.pixels.empty() ||
         data.pixels.size() > MAX_CACHE_BYTES - m_impl->m_bytes ) {
        // 解码失败或缓存预算不足时永久标记该目标失败。
        entry.m_failed = true;
        return;
    }
    // CPU RGBA 图集作为单张纹理上传，动画帧选择通过 UV 完成。
    auto texture = std::make_unique<Graphic::VKTexture>(data.pixels.data(),
                                                        data.width,
                                                        data.height,
                                                        physical,
                                                        device,
                                                        pool,
                                                        queue);
    if ( !texture->isValid() ) {
        // 无效 GPU 资源不计入预算，也不暴露描述符给渲染器。
        entry.m_failed = true;
        return;
    }
    // 先保存描述符与所有权，再累计实际上传字节预算。
    entry.m_id      = texture->getImTextureID();
    entry.m_texture = std::move(texture);
    m_impl->m_bytes += data.pixels.size();
    // 上传完成后主动释放大像素容量，只保留轻量帧布局。
    std::vector<unsigned char>().swap(data.pixels);
    entry.m_layout = std::move(data);
    // 每张动画从纹理就绪时开始独立循环计时。
    entry.m_startTime = ImGui::GetTime();
}
/// @brief 查询目标图片当前应显示的纹理、尺寸和 UV。
/// @param destination 原始 Markdown 图片目标。
/// @return 未登记或失败、仍加载中、或已就绪图片的轻量快照。
/// @warning UI 热路径：只查 map 和计算当前帧，不复制纹理所有权或访问文件。
MarkdownImage MarkdownImageCache::findImage(std::string_view destination) const
{
    // 未登记目标视为失败，渲染器可直接显示替代文本。
    const auto it = m_impl->m_entries.find(destination);
    if ( it == m_impl->m_entries.end() ) return { .failed = true };
    const auto& entry = it->second;
    // 无纹理且 failed=false 表示任务仍在排队或处理中。
    if ( !entry.m_texture ) return { .failed = entry.m_failed };
    const auto& layout = entry.m_layout;
    // 动画按 ImGui 单调时间循环，静态图固定使用第零帧。
    const unsigned frame =
        layout.duration > 0.0
            ? std::min(
                  layout.frames - 1U,
                  static_cast<unsigned>(
                      std::fmod(
                          std::max(0.0, ImGui::GetTime() - entry.m_startTime),
                          layout.duration) /
                      layout.duration * layout.frames))
            : 0U;
    // 帧索引按图集列数转换为左上角像素位置。
    const float x =
        static_cast<float>((frame % layout.columns) * layout.frameWidth);
    const float y =
        static_cast<float>((frame / layout.columns) * layout.frameHeight);
    // UV 内缩半个像素，减少相邻动画帧双线性采样串色。
    return { entry.m_id,
             { static_cast<float>(layout.frameWidth),
               static_cast<float>(layout.frameHeight) },
             { (x + 0.5F) / layout.width, (y + 0.5F) / layout.height },
             { (x + layout.frameWidth - 0.5F) / layout.width,
               (y + layout.frameHeight - 0.5F) / layout.height },
             false };
}
}  // namespace MMM::UI
