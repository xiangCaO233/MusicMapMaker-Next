#include "ui/imgui/markdown/MarkdownImageCache.h"

#include "common/VideoFrameDecoder.h"
#include "config/AppPaths.h"
#include "graphic/imguivk/VKTexture.h"
#include "log/colorful-log.h"
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
/// @brief Markdown 远程图片与内置教程 GIF 的后台解码和 GPU 图集缓存。
/// @details UI 线程只扫描文档、排队键和查询已上传纹理；网络、图片解码及
/// GIF 临时文件操作全部在线程池任务中完成，GPU 上传只在纹理准备阶段执行。
///
/// 资源限制：
/// - 单次下载最多 32 MiB；
/// - 远程 GIF 源帧内存预算 128 MiB，可信打包教程原图最多 512 MiB；
/// - 单张输入宽高最多 4096 像素；
/// - GIF 时长最多 120 秒；
/// - GIF 采样最多 96 帧且目标约 15 FPS；
/// - 更新日志缩略动画单帧最长边 320 像素，放大和教程最多 1280 像素；
/// - 单个高清图集最多使用 128 MiB，采样帧数按尺寸自适应；
/// - 静态图最长边缩小到 1280 像素；
/// - 图集单行宽度不超过 4096 像素；
/// - 正文与教程已上传 RGBA 数据预算为 192 MiB，更新放大预览独占 128 MiB；
/// - 文档缓存最多接纳 32 个不同目标；
/// - 同时只运行一个下载与解码任务。
/// - 失败目标保留条目状态，避免可见帧反复发起相同请求。
/// 正文与放大预览使用同一 URL、不同缓存键：正文保持小帧，放大时才申请
/// 高清帧。预览尚未就绪或失败时继续显示正文帧，避免弹窗出现长时间空白。
/// 高清图集只保留最近打开的一张；切换时先等待已提交的 GPU 命令结束，
/// 再销毁旧页和扣除预算。纹理页按完整帧行切分，帧与描述符一一对应。
/// 正文可能包含多段动画，不能挤占用户双击打开的高清预览预算。
/// 两类计数独立，预览关闭或切换时归还自己的容量，总驻留上限为 320 MiB。
/// 图集帧数随分辨率缩减，但持续时间不变，因此较大原图可能降低采样帧率。
/// 教程 GIF 的来源仅限同步到配置根的打包资源，Markdown 的远程地址
/// 解析仍只允许 HTTP(S)；两条来源复用输入字节与图集大小上限。
/// 原始教程动画按可信资源的 512 MiB 源帧上限用 stb 解码，避免依赖
/// 没有 GIF 解码器的 FFmpeg 预编译包；仅保留当前段落的高清 GPU 图集。

namespace MMM::UI
{
namespace
{
constexpr std::size_t MAX_DOWNLOAD_BYTES =
    32U * 1024U * 1024U;  ///< 单个下载上限。
constexpr std::size_t MAX_CACHE_BYTES =
    192U * 1024U * 1024U;  ///< GPU 图集总预算。
constexpr std::size_t MAX_WALKTHROUGH_SOURCE_BYTES =
    512U * 1024U * 1024U;  ///< 可信教程原始 GIF 的源帧预算。
constexpr std::size_t MAX_ATLAS_BYTES =
    128U * 1024U * 1024U;                     ///< 单张动画图集的 RGBA 上限。
constexpr unsigned MAX_TEXTURE_EDGE = 4096U;  ///< 单页 Vulkan 图集边长。
constexpr unsigned PREVIEW_GIF_EDGE = 1280U;  ///< 放大预览目标最长边。
/// @brief 与网络 URL 命名空间隔离的内置资源键前缀。
constexpr std::string_view WALKTHROUGH_GIF_PREFIX = "walkthrough-gif:";
/// @brief 高清预览的内部缓存键，不接受外部 Markdown 直接构造。
constexpr std::string_view PREVIEW_IMAGE_PREFIX = "update-preview:";

/// @brief 只接受单个 ASCII 文件名，阻止教程图片键逃出资源目录。
/// @param destination 由教学步骤提供的完整媒体键。
/// @return 只有安全的单文件 GIF 名称才允许进入本地资源加载分支。
/// @note 拒绝路径分隔符、连续句点和其它 scheme，不对任意磁盘路径开放。
bool validWalkthroughGif(std::string_view destination)
{
    if ( !destination.starts_with(WALKTHROUGH_GIF_PREFIX) ) return false;
    const auto file = destination.substr(WALKTHROUGH_GIF_PREFIX.size());
    if ( file.size() < 5U || !file.ends_with(".gif") ) return false;
    return std::all_of(file.begin(),
                       file.end(),
                       [](char character) {
                           return (character >= 'a' && character <= 'z') ||
                                  (character >= '0' && character <= '9') ||
                                  character == '-' || character == '.';
                       }) &&
           file.find("..") == std::string_view::npos;
}

/// @brief 后台读取已打包的单张教程动画，沿用相同的图片解码限制。
/// @param destination 已通过单文件名语法验证的媒体键。
/// @return 文件丢失、超限或解码失败时返回空图集。
/// @warning 后台任务可访问磁盘；UI 的每帧查询绝不能调用此函数。
MarkdownImagePixels loadWalkthroughGif(const std::string& destination)
{
    if ( !validWalkthroughGif(destination) ) return {};
    // 校验后的文件名不含路径分隔符；配置目录由资源同步流程维护。
    const auto      path = Config::AppPaths::assetsRootPath() / "walkthroughs" /
                           "canonrock" / "gifs" /
                           destination.substr(WALKTHROUGH_GIF_PREFIX.size());
    std::error_code error;
    const auto      size = std::filesystem::file_size(path, error);
    // 先查元数据再分配，防止资源被替换为任意大文件时扩张工作线程内存。
    if ( error || size == 0U || size > MAX_DOWNLOAD_BYTES ) return {};
    std::ifstream file(path, std::ios::binary);
    if ( !file ) return {};
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    // 文件在读取期间被替换或截断时不把残缺字节交给 GIF 解码器。
    if ( !file || file.gcount() != static_cast<std::streamsize>(bytes.size()) )
        return {};
    // 可信的打包原图可以在后台展开较大的源帧；GPU 图集仍受单图上限约束。
    // 每次切步会回收上一张教程图集，不把十四段高清动画同时留在显存。
    return decodeUpdateImage(bytes, 1280U, MAX_WALKTHROUGH_SOURCE_BYTES);
}

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
/// @param sourceFrameBudget 本次来源允许展开的源帧字节数。
/// @return 可安全整图解码时返回帧数，否则返回零并交给逐帧解码路径。
/// @pre width 和 height 已由 stbi_info_from_memory 验证为正且不超过 4096。
/// @details 逐块解析 GIF 容器，只读取长度和标记，不解压 LZW 数据。
/// 每个图像块最多产生一张完整逻辑画布，故帧数乘画布字节数是源帧容量上界。
std::size_t boundedGifFrameCount(std::span<const unsigned char> bytes,
                                 unsigned width, unsigned height,
                                 std::size_t sourceFrameBudget)
{
    // GIF 每帧由完整逻辑画布构成；先限制单帧，再逐个图像块累计数量。
    const std::size_t frameBytes =
        static_cast<std::size_t>(width) * height * 4U;
    if ( bytes.size() < 13U || frameBytes > sourceFrameBudget ) return 0U;
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
        if ( ++frames > sourceFrameBudget / frameBytes ) return 0U;
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
/// @param animatedEdge 动画帧最长边，静态图保留既有独立上限。
/// @return 已分配 RGBA 缓冲和完整帧布局。
MarkdownImagePixels makeAtlas(unsigned width, unsigned height, unsigned frames,
                              double duration, unsigned animatedEdge)
{
    MarkdownImagePixels out;
    // 按时长区分动画与静态图，单采样帧的短 GIF 也要遵守动画尺寸预算。
    const double scale =
        std::min(1.0,
                 (duration > 0.0 ? static_cast<double>(animatedEdge) : 1280.0) /
                     std::max(width, height));
    out.frameWidth  = std::max(1U, static_cast<unsigned>(width * scale));
    out.frameHeight = std::max(1U, static_cast<unsigned>(height * scale));
    // 列数同时受帧数和单页 Vulkan 纹理宽度限制。
    out.columns = std::min(frames, MAX_TEXTURE_EDGE / out.frameWidth);
    out.frames  = frames;
    out.width   = out.columns * out.frameWidth;
    // 向上取整行数，最后一行允许未填满。
    out.height = ((frames + out.columns - 1U) / out.columns) * out.frameHeight;
    out.duration = duration;
    // 一次分配完整图集，后续帧直接写入对应区域。
    out.pixels.resize(static_cast<std::size_t>(out.width) * out.height * 4U);
    return out;
}

/// @brief 按 RGBA 图集预算收紧采样帧数，保留尽可能高的预览分辨率。
/// @param width GIF 逻辑画布宽度。
/// @param height GIF 逻辑画布高度。
/// @param requested 按时长计算的目标采样帧数。
/// @param animatedEdge 本次缓存允许的单帧最长边。
/// @return 带有最后一行空槽开销时仍能落在单图预算内的帧数。
/// @details 帧宽先按最终缩放倍率计算，再用与 makeAtlas 相同的列数和
/// 行数公式估算实际分配容量；只减少采样数量，不把高清图再次缩成缩略图。
/// 采样仍覆盖完整 GIF 时长，播放索引无需知道源帧被压缩多少。
/// 保留至少一帧，防止极端输入导致零尺寸图集。
/// 图集预算仅计算 CPU RGBA 与对应 GPU 像素，网络源字节另受下载上限约束。
unsigned boundedAtlasFrames(unsigned width, unsigned height, unsigned requested,
                            unsigned animatedEdge)
{
    const double scale = std::min(
        1.0, static_cast<double>(animatedEdge) / std::max(width, height));
    const unsigned frameWidth =
        std::max(1U, static_cast<unsigned>(width * scale));
    const unsigned frameHeight =
        std::max(1U, static_cast<unsigned>(height * scale));
    // 按 makeAtlas 的列布局计入最后一行空槽，防止仅计算有效帧后超预算。
    for ( unsigned frames = requested; frames > 1U; --frames ) {
        const auto columns = std::min(frames, MAX_TEXTURE_EDGE / frameWidth);
        const auto rows    = (frames + columns - 1U) / columns;
        const auto bytes   = static_cast<std::size_t>(columns) * frameWidth *
                             rows * frameHeight * 4U;
        if ( bytes <= MAX_ATLAS_BYTES ) return frames;
    }
    return 1U;
}

/// @brief 使用 stb_image 在内存中解码有界 GIF，并按播放时间采样为图集。
/// @param bytes GIF 文件字节。
/// @param expectedWidth 已验证的逻辑画布宽度。
/// @param expectedHeight 已验证的逻辑画布高度。
/// @param maximumFrames 预扫描确认的源帧数量。
/// @return 解码成功时返回动画图集，否则返回空布局。
/// @param animatedEdge 内置教程与远程图片各自的帧尺寸上限。
/// @details 仅对源帧容量经过预扫描的 GIF 使用此路径，避免依赖可选的
/// FFmpeg GIF 解复用器；输出仍按约 15 FPS 和 96 帧上限采样。
/// @warning 后台资源路径：完整 GIF 解码会分配源帧缓冲，禁止在 UI 热路径调用。
MarkdownImagePixels decodeBoundedGif(std::span<const unsigned char> bytes,
                                     unsigned    expectedWidth,
                                     unsigned    expectedHeight,
                                     std::size_t maximumFrames,
                                     unsigned    animatedEdge)
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
    const unsigned requested =
        std::clamp(static_cast<unsigned>(std::ceil(duration * 15.0)), 1U, 96U);
    const unsigned frames = boundedAtlasFrames(
        expectedWidth, expectedHeight, requested, animatedEdge);
    auto out = makeAtlas(
        expectedWidth, expectedHeight, frames, duration, animatedEdge);
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
MarkdownImagePixels loadUpdateImage(const std::string& url,
                                    unsigned           animatedEdge)
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
    return decodeUpdateImage(bytes, animatedEdge);
}

/// @brief 将下载字节解码为静态或动画 RGBA 图集。
/// @param bytes 受下载上限约束的图片文件内容。
/// @param animatedEdge 动画帧的最长边，放大预览可高于缩略图上限。
/// @param sourceFrameBudget 仅可信教程资源可使用较大的源帧展开预算。
/// @return 解码和尺寸校验成功时返回图集，否则返回空。
/// @warning 后台低频路径：GIF 会创建任务专属临时目录并逐帧调用解码器。
MarkdownImagePixels decodeUpdateImage(std::span<const unsigned char> bytes,
                                      unsigned    animatedEdge,
                                      std::size_t sourceFrameBudget)
{
    // 直接调用场景也必须执行与下载回调相同的大小边界。
    if ( bytes.empty() || bytes.size() > MAX_DOWNLOAD_BYTES ||
         animatedEdge == 0U || animatedEdge > PREVIEW_GIF_EDGE ||
         sourceFrameBudget == 0U ||
         sourceFrameBudget > MAX_WALKTHROUGH_SOURCE_BYTES )
        return {};
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
        auto out = makeAtlas(width, height, 1U, 0.0, animatedEdge);
        copyFrame(out, 0U, pixels.get(), width, height);
        return out;
    }
    // 常见小型 GIF 直接从内存读取，避免依赖各平台预编译 FFmpeg 的 GIF 能力。
    // 大型 GIF 保留原有逐帧解码路径，以免一次展开全部源帧。
    // 打包教程有固定且受版本管理的输入，可使用更高的源帧预算。
    // 下载图片始终沿用较低的默认预算，不能由远程内容自行提高上限。
    const auto boundedFrames =
        boundedGifFrameCount(bytes, width, height, sourceFrameBudget);
    if ( boundedFrames > 0U ) {
        auto decoded =
            decodeBoundedGif(bytes, width, height, boundedFrames, animatedEdge);
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
    const unsigned requested =
        std::clamp(static_cast<unsigned>(std::ceil(duration * 15.0)), 1U, 96U);
    const unsigned frames =
        boundedAtlasFrames(width, height, requested, animatedEdge);
    auto out = makeAtlas(width, height, frames, duration, animatedEdge);
    for ( unsigned index = 0; index < frames; ++index ) {
        // 在整个动画时长内均匀取样，不保存解码器原始全部帧。
        const auto* frame = decoder.decodeFrameAt(duration * index / frames);
        if ( !frame ) return {};
        copyFrame(out, index, frame->rgba.data(), frame->width, frame->height);
    }
    return out;
}

/// @brief 缓存仅在 UI/资源准备线程访问，工作任务只返回独占 CPU 数据。
/// @details m_entries 保存正文和高清键；任务通过 m_active 关联唯一结果。
/// 工作线程只捕获键并返回像素，不读取或修改本结构，UI 查询无需锁。
/// 已上传字节数只在完整纹理页发布或销毁时改变，失败页不进入预算。
struct MarkdownImageCache::Impl {
    /// @brief 单个 Markdown 目标的 GPU 纹理、动画布局和失败状态。
    struct Entry {
        std::vector<std::unique_ptr<Graphic::VKTexture>>
                                 m_textures;     ///< 各图集页的 GPU 所有权。
        std::vector<ImTextureID> m_ids;          ///< 与页索引对应的描述符。
        MarkdownImagePixels      m_layout;       ///< 不保留像素的帧布局。
        double                   m_startTime{};  ///< 动画起播时钟。
        bool                     m_failed{};     ///< 失败后不逐帧重复请求。
    };
    std::map<std::string, Entry, std::less<>>
                                     m_entries;  ///< 按 Markdown 目标去重。
    std::vector<std::string>         m_pending;  ///< 有界、尚未开始下载的键。
    std::string                      m_active;   ///< 当前任务键。
    std::future<MarkdownImagePixels> m_future;   ///< 单任务非阻塞交接。
    std::size_t                      m_bytes{};  ///< 正文与教程的已上传容量。
    std::size_t m_previewBytes{};     ///< 独立高清预览容量，不受正文动画挤占。
    std::size_t m_documentEntries{};  ///< 远程文档图片条目数量。
    std::string m_previewKey;         ///< 当前放大预览唯一保留的高清键。
    bool        m_previewReleasePending{};  ///< 关闭或切换预览后等待安全回收。
    std::string m_walkthroughKey;           ///< 当前教学段落唯一保留的动画键。
    bool        m_walkthroughReleasePending{};  ///< 切步或退出后等待安全回收。
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
            if ( span.kind == MarkdownInlineKind::Image )
                prepareImage(span.destination);
        });
    });
}

/// @brief 为步骤主动预热一张图片，避免将 GIF 嵌入文字文档。
/// @param destination 原始图片键；本地资源必须使用内置教程 scheme。
/// @warning UI 低频路径：只登记键和排队，真正读盘由线程池完成。
void MarkdownImageCache::prepareImage(std::string_view destination)
{
    const bool walkthrough = validWalkthroughGif(destination);
    if ( walkthrough && m_impl->m_walkthroughKey != destination ) {
        // 教程只保留当前段落：切步先登记新身份，再异步回收旧纹理。
        // 身份更新也使旧后台任务的结果在上传阶段自动失效。
        m_impl->m_walkthroughKey.assign(destination);
        m_impl->m_walkthroughReleasePending = true;
    }
    if ( m_impl->m_entries.contains(destination) ||
         (!walkthrough && m_impl->m_documentEntries >= 32U) )
        return;
    auto  key   = std::string(destination);
    auto& entry = m_impl->m_entries[key];
    // 本地路径只有受限教程 scheme 可用，远程 Markdown 仍按旧策略解析。
    entry.m_failed = !walkthrough && resolveUpdateImageUrl(key).empty();
    // 教程图片由独立身份管理，不占正文最多 32 张的缓存配额。
    // 否则长路线反复切步会耗尽配额，后续段落无法再次排队。
    if ( !walkthrough ) ++m_impl->m_documentEntries;
    if ( !entry.m_failed ) m_impl->m_pending.push_back(std::move(key));
}

/// @brief 将放大预览登记为独立高清键，保留正文缩略图的显存成本。
/// @param destination 更新日志中的原始 Markdown 图片目标。
/// @details 只有 URL 解析通过的目标可进入此分支；内部前缀不传给网络。
/// 已登记键复用纹理与播放相位，新目标只更新当前预览身份并排队。
/// @warning 低频 UI 路径：只修改队列，不同步访问网络或 GPU。
void MarkdownImageCache::preparePreviewImage(std::string_view destination)
{
    if ( destination.empty() || resolveUpdateImageUrl(destination).empty() )
        return;
    const auto original = m_impl->m_entries.find(destination);
    // 静态图片已按 1280 像素上限上传；只有动画需要独立高清图集。
    // 以时长区分来源，短 GIF 即使只采到一帧仍需按动画处理。
    if ( original != m_impl->m_entries.end() &&
         !original->second.m_textures.empty() &&
         original->second.m_layout.duration <= 0.0 )
        return;
    auto key = std::string(PREVIEW_IMAGE_PREFIX) + std::string(destination);
    // 若此前保留的是另一张图，资源准备阶段先归还旧图集预算。
    if ( m_impl->m_previewKey != key ) m_impl->m_previewReleasePending = true;
    m_impl->m_previewKey = key;
    if ( m_impl->m_entries.contains(key) ) return;
    m_impl->m_entries.emplace(key, Impl::Entry{});
    // 用户明确打开的高清图优先于尚未开始的正文图片，避免长时间显示缩略帧。
    // 在途任务仍由原有单任务交接完成，不并行展开多张动画或阻塞 UI。
    m_impl->m_pending.insert(m_impl->m_pending.begin(), std::move(key));
}

/// @brief 关闭放大窗口时撤销当前预览请求并安排 GPU 安全回收。
/// @details 在途后台解码不能中断，但返回后会因键已失效而直接丢弃。
/// @warning 低频 UI 操作：这里只标记状态，不等待设备或销毁纹理。
void MarkdownImageCache::releasePreviewImage()
{
    m_impl->m_previewKey.clear();
    m_impl->m_previewReleasePending = true;
}

/// @brief 结束教学时使当前教程图集失效，留待资源准备阶段安全销毁。
/// @warning 低频 UI 操作：这里只更新身份，不等待设备或访问文件。
void MarkdownImageCache::releaseWalkthroughImage()
{
    m_impl->m_walkthroughKey.clear();
    m_impl->m_walkthroughReleasePending = true;
}

/// @brief 重新进入一个教学阶段时让现有动画从首帧播放。
/// @warning UI 低频路径：只更新时间标量，未上传的图集在上传时自行起播。
void MarkdownImageCache::restartImage(std::string_view destination)
{
    const auto it = m_impl->m_entries.find(destination);
    if ( it != m_impl->m_entries.end() && !it->second.m_textures.empty() )
        it->second.m_startTime = ImGui::GetTime();
}
/// @brief 启动下一后台图片任务并非阻塞轮询当前任务完成状态。
/// @return 当前 future 已就绪、需要进入纹理准备阶段时返回 true。
/// @details 未启动的旧预览和教程任务可直接丢弃；已开始的任务不可取消，
/// 其结果由 reloadTextures 检查当前身份后决定是否保留。
/// 普通正文键保持原顺序，不因用户快速切换预览被错误移除。
/// @warning UI 热路径：不等待任务；至多在队列切换时提交一个线程池工作。
bool MarkdownImageCache::needReload()
{
    // 连续切换时跳过尚未开始的旧任务，不浪费网络与本地解码预算。
    // 普通文档图片保持原顺序；只有失效的预览或教程键会被移除。
    while ( !m_impl->m_pending.empty() &&
            ((m_impl->m_pending.front().starts_with(PREVIEW_IMAGE_PREFIX) &&
              m_impl->m_pending.front() != m_impl->m_previewKey) ||
             (m_impl->m_pending.front().starts_with(WALKTHROUGH_GIF_PREFIX) &&
              m_impl->m_pending.front() != m_impl->m_walkthroughKey)) ) {
        m_impl->m_entries.erase(m_impl->m_pending.front());
        m_impl->m_pending.erase(m_impl->m_pending.begin());
    }
    if ( !m_impl->m_future.valid() && !m_impl->m_pending.empty() ) {
        // 单任务模型防止多个大图同时占用下载和解码内存。
        auto* pool = Runtime::AppThreadPool::instance().get();
        if ( !pool ) return false;
        // 取出队首作为唯一活动键，future 结果与该键一一对应。
        m_impl->m_active = std::move(m_impl->m_pending.front());
        m_impl->m_pending.erase(m_impl->m_pending.begin());
        // 复制短键交给任务，不捕获缓存对象；视图关闭也不会悬挂 this。
        // 本地与远程均在工作线程执行，needReload 继续保持零等待轮询。
        const auto key   = m_impl->m_active;
        m_impl->m_future = pool->enqueue([key]() {
            if ( validWalkthroughGif(key) ) return loadWalkthroughGif(key);
            // 高清键只由预览入口生成；先剥离内部前缀再解析真实 URL。
            if ( key.starts_with(PREVIEW_IMAGE_PREFIX) ) {
                return loadUpdateImage(
                    resolveUpdateImageUrl(std::string_view(key).substr(
                        PREVIEW_IMAGE_PREFIX.size())),
                    PREVIEW_GIF_EDGE);
            }
            return loadUpdateImage(resolveUpdateImageUrl(key));
        });
    }
    // 零秒 wait_for 只查询状态，不阻塞 UI 线程。
    return m_impl->m_previewReleasePending ||
           m_impl->m_walkthroughReleasePending ||
           (m_impl->m_future.valid() &&
            m_impl->m_future.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready);
}
/// @brief 消费已完成 CPU 图集并创建对应 Vulkan 纹理。
/// @param physical Vulkan 物理设备。
/// @param device Vulkan 逻辑设备。
/// @param pool 上传命令池。
/// @param queue 执行上传的 Vulkan 队列。
/// @details 高清图集可能高于单张 Vulkan 纹理的最低尺寸保证，
/// 因此沿完整帧行切成多个 4096 像素以内的页，再批量发布描述符。
/// 每页使用连续的源 RGBA 行，上传完成后立即释放整份 CPU 图集。
/// 当前帧查询按原图集行号计算页号和页内 UV，不改变动画起播时间。
/// 旧高清页销毁前必须等待设备空闲，否则在途 ImGui 命令可能仍引用它。
/// 普通正文图不主动回收，受 192 MiB 预算约束；预览独享 128 MiB。
/// @warning 低频资源准备路径：仅 future 就绪时分配和上传 GPU 资源；
/// 用户切换放大图片或教学段落时可能触发一次 device.waitIdle，不得逐帧调用。
void MarkdownImageCache::reloadTextures(vk::PhysicalDevice& physical,
                                        vk::Device&         device,
                                        vk::CommandPool& pool, vk::Queue& queue)
{
    if ( m_impl->m_previewReleasePending ||
         m_impl->m_walkthroughReleasePending ) {
        // ImGui 可能仍在消费上一帧描述符，只对真正持有纹理的旧页等待。
        // 两种缓存同时失效时共用一次等待，避免重复阻塞渲染线程。
        const auto stale = [this](const auto& item) {
            return (m_impl->m_previewReleasePending &&
                    item.first.starts_with(PREVIEW_IMAGE_PREFIX) &&
                    item.first != m_impl->m_previewKey) ||
                   (m_impl->m_walkthroughReleasePending &&
                    item.first.starts_with(WALKTHROUGH_GIF_PREFIX) &&
                    item.first != m_impl->m_walkthroughKey);
        };
        const bool hasOldTexture = std::any_of(
            m_impl->m_entries.begin(),
            m_impl->m_entries.end(),
            [&stale](const auto& item) {
                return stale(item) && !item.second.m_textures.empty();
            });
        if ( hasOldTexture ) (void)device.waitIdle();
        for ( auto old = m_impl->m_entries.begin();
              old != m_impl->m_entries.end(); ) {
            if ( stale(*old) ) {
                const auto& layout = old->second.m_layout;
                // 预览与正文分开记账，关闭高清页不能扣减正文已占用的容量。
                // 未上传的空条目尺寸为零，不会向任一计数器归还额外空间。
                auto& used = old->first.starts_with(PREVIEW_IMAGE_PREFIX)
                                 ? m_impl->m_previewBytes
                                 : m_impl->m_bytes;
                used -=
                    static_cast<std::size_t>(layout.width) * layout.height * 4U;
                old = m_impl->m_entries.erase(old);
            } else {
                ++old;
            }
        }
        m_impl->m_previewReleasePending     = false;
        m_impl->m_walkthroughReleasePending = false;
    }
    // 防御渲染器提前调用，未完成任务绝不在此等待。
    if ( !m_impl->m_future.valid() ||
         m_impl->m_future.wait_for(std::chrono::seconds(0)) !=
             std::future_status::ready )
        return;
    // get 同时使 future 失效，下一次 needReload 可以启动后续排队任务。
    auto       data = m_impl->m_future.get();
    const auto it   = m_impl->m_entries.find(m_impl->m_active);
    if ( it == m_impl->m_entries.end() ) return;
    auto&      entry   = it->second;
    const bool preview = m_impl->m_active.starts_with(PREVIEW_IMAGE_PREFIX);
    const bool walkthrough =
        m_impl->m_active.starts_with(WALKTHROUGH_GIF_PREFIX);
    // 已切换到另一张预览时丢弃迟到的解码结果，不能覆盖最新选择。
    if ( preview && m_impl->m_active != m_impl->m_previewKey ) {
        m_impl->m_entries.erase(it);
        return;
    }
    // 教学段落已结束或切换时，同样不能上传迟到的旧动画。
    if ( walkthrough && m_impl->m_active != m_impl->m_walkthroughKey ) {
        m_impl->m_entries.erase(it);
        return;
    }
    if ( data.pixels.empty() ) {
        // 解码失败独立于显存预算；不能把两种原因都当作原图画质不足。
        // 错误只在任务结果交接时记录，后续每帧查询不重复刷日志。
        XERROR("更新图片解码失败: {}", m_impl->m_active);
        entry.m_failed = true;
        return;
    }
    // 放大图集有独立保留空间，正文已经加载很多 GIF 时也应上传高清帧。
    // 教程仍沿用正文池；预览池始终只保留当前一张，不能无界增长。
    auto&      used   = preview ? m_impl->m_previewBytes : m_impl->m_bytes;
    const auto budget = preview ? MAX_ATLAS_BYTES : MAX_CACHE_BYTES;
    // used 只在完整上传和安全回收处改变，始终不超过其对应预算。
    // 两条资源生命周期分支必须使用同一分类，避免减错池造成无符号下溢。
    if ( data.pixels.size() > budget - used ) {
        // 预算拒绝只发生一次；保留具体容量，便于区分低清回退和解码失败。
        XERROR("更新图片图集预算不足: {}，需要 {} 字节，已用 {}，上限 {}",
               m_impl->m_active,
               data.pixels.size(),
               used,
               budget);
        entry.m_failed = true;
        return;
    }
    // 每页仅包含完整的帧行，避免大图集超过 Vulkan 保证的 4096 像素边长。
    // 不在同一帧的上下半段分割，当前帧的 UV 始终落在单个纹理中。
    const unsigned rowsPerPage = MAX_TEXTURE_EDGE / data.frameHeight;
    const unsigned pageRows    = rowsPerPage * data.frameHeight;
    std::vector<std::unique_ptr<Graphic::VKTexture>> textures;
    std::vector<ImTextureID>                         ids;
    for ( unsigned top = 0U; top < data.height; top += pageRows ) {
        const unsigned pageHeight = std::min(pageRows, data.height - top);
        auto           texture    = std::make_unique<Graphic::VKTexture>(
            data.pixels.data() +
                static_cast<std::size_t>(top) * data.width * 4U,
            data.width,
            pageHeight,
            physical,
            device,
            pool,
            queue);
        if ( !texture->isValid() ) {
            // 已创建的页尚未暴露给绘制，失败时由局部容器自动回收。
            entry.m_failed = true;
            return;
        }
        ids.push_back(texture->getImTextureID());
        textures.push_back(std::move(texture));
    }
    // 全部页成功后才发布描述符；半张动画不可进入 UI 绘制。
    // 局部容器先接管每一页，任意上传失败可整体回滚 GPU 所有权。
    entry.m_ids      = std::move(ids);
    entry.m_textures = std::move(textures);
    used += data.pixels.size();
    // 高清发布后记录实际帧尺寸，避免仅凭弹窗大小判断是否真的加载了原图。
    if ( preview )
        XINFO("更新高清预览已上传: {}，帧 {}x{}，{} 帧，{} 字节",
              m_impl->m_active,
              data.frameWidth,
              data.frameHeight,
              data.frames,
              data.pixels.size());
    // 上传完成后主动释放大像素容量，只保留轻量帧布局。
    std::vector<unsigned char>().swap(data.pixels);
    entry.m_layout = std::move(data);
    // 每张动画从纹理就绪时开始独立循环计时。
    entry.m_startTime = ImGui::GetTime();
    if ( preview ) {
        const auto original = std::string_view(m_impl->m_active)
                                  .substr(PREVIEW_IMAGE_PREFIX.size());
        const auto thumb    = m_impl->m_entries.find(original);
        // 放大切换时与正文缩略动画使用同一相位，避免突然跳回首帧。
        if ( thumb != m_impl->m_entries.end() &&
             !thumb->second.m_textures.empty() )
            entry.m_startTime = thumb->second.m_startTime;
    }
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
    if ( entry.m_textures.empty() ) return { .failed = entry.m_failed };
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
    // 行号再映射到页号；页内高度可能比完整页短，UV 必须用实际高度。
    const float x =
        static_cast<float>((frame % layout.columns) * layout.frameWidth);
    const unsigned row         = frame / layout.columns;
    const unsigned rowsPerPage = MAX_TEXTURE_EDGE / layout.frameHeight;
    const unsigned page        = row / rowsPerPage;
    const unsigned pageTop     = page * rowsPerPage * layout.frameHeight;
    const unsigned pageHeight =
        std::min(rowsPerPage * layout.frameHeight, layout.height - pageTop);
    const float y =
        static_cast<float>((row % rowsPerPage) * layout.frameHeight);
    // UV 内缩半个像素，减少相邻动画帧双线性采样串色。
    return { entry.m_ids[page],
             { static_cast<float>(layout.frameWidth),
               static_cast<float>(layout.frameHeight) },
             { (x + 0.5F) / layout.width, (y + 0.5F) / pageHeight },
             { (x + layout.frameWidth - 0.5F) / layout.width,
               (y + layout.frameHeight - 0.5F) / pageHeight },
             false };
}

/// @brief 放大预览在高清解码和上传完成前沿用已加载的正文帧。
/// @param destination 当前弹窗对应的原始 Markdown 目标。
/// @return 高清就绪时返回当前页，否则返回正文缩略帧。
/// @details 只比较已记录的预览键，避免每帧拼接字符串或创建临时条目。
/// @warning UI 每帧调用，只进行有序表查询和当前帧 UV 计算。
MarkdownImage MarkdownImageCache::findPreviewImage(
    std::string_view destination) const
{
    const auto key = std::string_view(m_impl->m_previewKey);
    if ( key.starts_with(PREVIEW_IMAGE_PREFIX) &&
         key.substr(PREVIEW_IMAGE_PREFIX.size()) == destination ) {
        const auto it = m_impl->m_entries.find(key);
        if ( it != m_impl->m_entries.end() && !it->second.m_textures.empty() )
            return findImage(key);
    }
    return findImage(destination);
}
}  // namespace MMM::UI
