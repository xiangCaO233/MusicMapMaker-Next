/// @file ImageViewer.cpp
/// @brief 查看本地图片与未导出的长谱面图。
/// @details
/// 查看器用原图像素作为惟一导航坐标，源图、纹理和窗口可用空间分别取值。
/// 文件源在打开时解码；长截图源保留导出批次，可直接重放局部原图。
/// UI 状态、后台采样和图形上传是不同阶段，任何阶段都不借用编辑会话。
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#    define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "ui/image/ImageViewer.h"

#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "graphic/imguivk/VKTexture.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "ui/UIManager.h"
#include "ui/export/BeatmapLongScreenshot.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/workspace/IWorkspacePresentation.h"

#include <lunasvg.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace MMM::UI
{
namespace
{
/// @brief 导出模态有自己的资源视图，打开时不会换掉文件查看页。
constexpr char EXPORT_POPUP_ID[] = "ExportImageViewer";
/// @brief 图像拖动接管滚轮，窗体本身没有可滚动正文。
constexpr ImGuiWindowFlags IMAGE_WINDOW_FLAGS =
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
/// @brief 适应窗口时上传一次的整图缩略边长。
constexpr int OVERVIEW_EDGE = 1536;
/// @brief 局部纹理受限，避免分配超出移动 GPU 的整图。
constexpr int DETAIL_EDGE = 2048;
/// @brief SVG 使用有界栅格底图，局部取样不必重复解析 XML。
/// @note 大于概览的上限，使常见小图放大后仍能看清矢量轮廓。
constexpr int SVG_EDGE = 3072;
/// @brief 本地输入文件上限。
constexpr std::uint64_t MAX_FILE_BYTES = 128ULL * 1024 * 1024;
/// @brief 允许重新打开已导出的长 PNG。
constexpr std::int64_t MAX_FILE_PIXELS = 160000000;

/// @brief 将后缀和白名单逐字符比较，避免每个文件行分配小写字符串。
/// @param extension 文件快照保留的扩展名，包含前导点。
/// @param lower 内置 ASCII 小写扩展名；Unicode 文件名本身仍按原始路径打开。
/// @return 两者长度及各对应字母一致时返回 true。
/// @warning 文件列表热路径，比较次数受支持格式的固定数量约束。
bool matchesExtension(std::string_view extension, std::string_view lower)
{
    return std::ranges::equal(extension, lower, [](char value, char expected) {
        return (value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value) ==
               expected;
    });
}

/// @brief 经过尺寸校验的文件图像；共享所有权只用于跨线程固定源像素。
/// @details 文件关闭后撤销在途更新。正在采样的任务可短期保有最后一个所有权，
/// 使界面无需阻塞回收大图，也不会将查看结果交给已切换的文件。
/// @details 位图可来自 stb 或 SVG；预览只借用统一的 RGBA 数据。
struct FileImage {
    /// @brief 原点为零的整图区域。
    ImageRegion m_size;
    /// @brief 与解码器匹配的释放器。
    std::shared_ptr<const unsigned char> m_data;
    /// @brief SVG 行跨度可以大于实际像素宽度。
    std::size_t m_stride{};
};

/// @brief 先检查单帧像素预算，再把尺寸传给解码器或复制层。
/// @param width 在文件头中读到的宽度。
/// @param height 在文件头中读到的高度。
/// @return 两边为正且乘积在预算内时返回 true。
bool validFileDimensions(std::int64_t width, std::int64_t height)
{
    return width > 0 && height > 0 && width <= MAX_FILE_PIXELS / height;
}

/// @brief 在背景任务中打开一张本地图，路径保留平台原生编码。
/// @param path 用户在项目文件树里激活的原生绝对路径。
/// @param stop 关窗或换图时收到的撤销请求。
/// @return 完整、可按区域采样的位图；损坏和超出预算均返回空。
/// @warning 后台文件读取与解码入口，UI 线程不调用。
/// @details GIF 使用第一帧，SVG 按比例栅格化。
/// 位图保留源像素，用于拖动后重取可见范围，GPU 无需支持整图尺寸。
std::optional<FileImage> readFileImage(const std::filesystem::path& path,
                                       std::stop_token              stop)
{
    std::error_code error;
    // 普通文件检查避免把目录、设备或管道阻塞到单一解码任务中。
    if ( !std::filesystem::is_regular_file(path, error) || error ) return {};
    const auto bytes = std::filesystem::file_size(path, error);
    if ( error || bytes == 0 || bytes > MAX_FILE_BYTES ||
         stop.stop_requested() )
        return {};
    const auto extension = Config::pathToUtf8(path.extension());

    std::ifstream file(path, std::ios::binary);
    if ( !file ) return {};
    // ifstream 接受原生路径，避免窄字符串 fopen 在 Windows 丢失非 ASCII
    // 文件名。
    std::vector<unsigned char> input(static_cast<std::size_t>(bytes));
    // 提前确定输入预算，再按实际文件大小分配；文件缩短时本次打开失败。
    if ( !file.read(reinterpret_cast<char*>(input.data()),
                    static_cast<std::streamsize>(input.size())) ||
         stop.stop_requested() )
        return {};

    if ( matchesExtension(extension, ".svg") ) {
        // 解析与渲染都在工作线程，保留透明背景给查看画布合成。
        auto doc = lunasvg::Document::loadFromData(
            reinterpret_cast<const char*>(input.data()), input.size());
        if ( !doc || !std::isfinite(doc->width()) ||
             !std::isfinite(doc->height()) || doc->width() <= 0 ||
             doc->height() <= 0 )
            return {};
        // SVG 默认长度很小也保留细节；限制最长边并避免极端长宽比变成零像素。
        const double scale = static_cast<double>(SVG_EDGE) /
                             std::max(doc->width(), doc->height());
        const int    w =
            std::max(1, static_cast<int>(std::lround(doc->width() * scale)));
        const int h =
            std::max(1, static_cast<int>(std::lround(doc->height() * scale)));
        auto bitmap =
            std::make_shared<lunasvg::Bitmap>(doc->renderToBitmap(w, h));
        if ( !bitmap->data() ) return {};
        bitmap->convertToRGBA();
        // 平台位图可能包含行填充，按 stride 取样，不能按纹理列数猜测。
        return FileImage{ { 0, 0, bitmap->width(), bitmap->height() },
                          { bitmap, bitmap->data() },
                          static_cast<std::size_t>(bitmap->stride()) };
    }

    int       width{}, height{}, channels{};
    const int inputBytes = static_cast<int>(input.size());
    if ( !stbi_info_from_memory(
             input.data(), inputBytes, &width, &height, &channels) ||
         !validFileDimensions(width, height) )
        return {};
    // 解码前校验尺寸，在后台直接接管 stb 分配，避免复制大型长图的完整像素。
    std::shared_ptr<const unsigned char> data(
        stbi_load_from_memory(input.data(),
                              inputBytes,
                              &width,
                              &height,
                              &channels,
                              STBI_rgb_alpha),
        [](const unsigned char* pixels) {
            stbi_image_free(const_cast<unsigned char*>(pixels));
        });
    if ( !data || stop.stop_requested() ) return {};
    return FileImage{ { 0, 0, width, height },
                      std::move(data),
                      std::size_t{ 4 } * width };
}

/// @brief 将本地图片区域缩到指定预算，和谱面使用相同的原图像素中心采样。
/// @param image 背景解码后获得的只读原图，已校验尺寸和行跨度。
/// @param region 完全位于原图内的半开矩形；局部保持原图坐标原点。
/// @param edge 纹理最大边长，传入概览或局部的固定预算。
/// @param stop 从查看器传入的停止状态，逐行检查。
/// @return 可上传的连续像素，取消时不返回部分扫描行。
ImagePixels sampleFileImage(const FileImage& image, ImageRegion region,
                            int edge, std::stop_token stop)
{
    const double ratio = std::min(
        1.0,
        static_cast<double>(edge) / std::max(region.m_width, region.m_height));
    ImagePixels output{
        // 每边至少采样一个像素，适配单轨长图等狭窄的比例。
        std::max(1, static_cast<int>(std::lround(region.m_width * ratio))),
        std::max(1, static_cast<int>(std::lround(region.m_height * ratio))),
        {}
    };
    output.m_pixels.resize(std::size_t{ 4 } * output.m_width * output.m_height);
    for ( int y = 0; y < output.m_height; ++y ) {
        if ( stop.stop_requested() ) return {};
        const auto  sy  = region.m_y + (2LL * y + 1) * region.m_height /
                                           (2LL * output.m_height);
        const auto* row = image.m_data.get() + sy * image.m_stride;
        auto*       target =
            output.m_pixels.data() + std::size_t{ 4 } * y * output.m_width;
        // 逐行保留原始 alpha；不透明底色由查看器绘制。
        // 横纵抽样独立，比例缩小时也必须尊重源文件的对齐和像素跨度。
        if ( output.m_width == region.m_width ) {
            // 一比一的可见区直接复制连续字节，避免高倍率时重复作横向插值。
            std::memcpy(target, row + 4ULL * region.m_x, 4ULL * output.m_width);
        } else {
            for ( int x = 0; x < output.m_width; ++x ) {
                const auto sx = region.m_x + (2LL * x + 1) * region.m_width /
                                                 (2LL * output.m_width);
                std::memcpy(target + 4ULL * x, row + 4ULL * sx, 4);
            }
        }
    }
    return output;
}

/// @brief 检查上次生成的高清区域是否覆盖当前必需的可见区域。
/// @param outer 已请求的包含余量的区域。
/// @param inner 当前必须清楚显示的图内范围。
/// @return 各边均在原图的同一坐标系内比较，相接的边可以等于。
bool contains(ImageRegion outer, ImageRegion inner)
{
    return outer.m_x <= inner.m_x && outer.m_y <= inner.m_y &&
           outer.m_x + outer.m_width >= inner.m_x + inner.m_width &&
           outer.m_y + outer.m_height >= inner.m_y + inner.m_height;
}

/// @brief 记录图片来源，决定弹窗应在哪个父级提交。
/// @details 文件从 UIManager 的普通视图序列绘制标签页，导出由参数窗绘制子模态。
/// 两个宿主共用同一套交互和纹理装载实现，各持有自己的状态。
enum class Source { None, File, Screenshot };

/// @brief 后台任务结果，打开本地文件时附带供后续取样的源像素。
struct RenderedImage {
    /// @brief 局部刷新不替换来源。
    std::optional<FileImage> m_file;
    /// @brief 成品坐标，与上传后的纹理尺寸分开。
    ImageRegion m_region;
    /// @brief 可能是整图缩略或局部高清。
    ImagePixels m_pixels;
};

/// @brief 资源阶段上传的 GPU 图层及其原图区域。
struct ImageLayer {
    /// @brief 仅由纹理阶段替换。
    std::unique_ptr<Graphic::VKTexture> m_texture;
    /// @brief 提前解析的 ImGui 描述符。
    ImTextureID m_id{};
    /// @brief 用于平移和缩放时精确对齐。
    ImageRegion m_region;
};

/// @brief 查看区域在 ImGui 屏幕坐标中，裁剪与缩放同时采用此范围。
struct ImageViewport {
    /// @brief 左上边界包含在交互区，右下边界不包含。
    ImVec2 m_min, m_max;

    /// @brief 屏幕点是否在图像交互区域内，不含工具栏。
    /// @param point 鼠标或触点换算后的屏幕逻辑像素。
    /// @return 落在半开裁剪范围内时返回 true，用于首次认领手势。
    bool hits(ImVec2 point) const
    {
        return point.x >= m_min.x && point.x < m_max.x && point.y >= m_min.y &&
               point.y < m_max.y;
    }
};
}  // namespace

/// @brief 项目树中的图片后缀路由，解码仍以文件头和内容为准。
/// @param extension 文件快照传入的扩展名，项目根与名称均无需重新查询。
/// @warning 可在文件树热路径调用；不会规范路径、分配字符串或访问文件。
bool isImageViewerExtension(std::string_view extension)
{
    constexpr std::array<std::string_view, 8> extensions{
        ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".svg", ".tga", ".psd"
    };
    return std::ranges::any_of(extensions, [extension](auto supported) {
        return matchesExtension(extension, supported);
    });
}

/// @brief 一个窗口及其后台任务。查看器一次只持有一个来源。
/// @warning
/// 每帧只根据窗口状态绘制；取消、切换及发起取样都发生在明确的事件边沿。
/// @details 预览保留两张小纹理：适应窗口的整图和放大后视野附近的局部图。
/// 平移只改变坐标；旧局部图仍对齐于原图，新的像素随后覆盖可见范围。
/// 当前和在途的局部图层不需要占用完整图像大小的显存。
/// 工作结果只被纹理载入阶段消费，因此 ImGui 绘制引用的描述符不会在同一帧销毁。
/// 比例和偏移只由 UI 输入求得；工作线程只收到固定矩形，不会追读手势状态。
/// 下载、资源更新或项目状态与此查看器无关，文件预览不进入谱面媒体控制器。
struct ImageViewer::Impl {
    /// @brief 当前图像来源，未打开时不会提交窗口。
    Source m_source{ Source::None };
    /// @brief 当前窗口的关闭指针；文件页和导出模态各自消费关闭边沿。
    bool m_show{};
    /// @brief 文件标签聚焦或导出子模态的打开请求，只使用一次。
    bool m_openRequested{};
    /// @brief 本地文件需要解码，或已完成解码且失败。
    bool m_fileRequested{}, m_failed{};
    /// @brief 显式关闭后请求资源阶段释放 GPU 图像。
    bool m_clearTextures{};
    /// @brief 最新明确打开的本地文件。
    std::filesystem::path m_path;
    /// @brief 标签显示文件名，稳定后缀在换文件时保持原来停靠身份。
    std::string m_fileTitle;
    /// @brief 保留打开的位图，跨局部任务共享像素。
    std::optional<FileImage> m_file;
    /// @brief 成品快照。
    std::shared_ptr<const BeatmapScreenshotImage> m_screenshot;
    /// @brief 原图像素范围，原点为零。
    ImageRegion m_bounds;
    /// @brief 整图缩略和可选的可见区。
    /// 局部层保留原图矩形，即使等待新的像素，原有部分也能持续对齐。
    ImageLayer m_overview, m_detail;
    /// @brief 导出父窗持有，闭窗前稳定。
    /// 不通过本视图送入 Vulkan 销毁队列，关闭时只清除借用标识。
    ImTextureID m_borrowedThumbnail{};
    /// @brief 导出缩略图的实际像素数。
    ImVec2 m_thumbnailSize{};
    /// @brief 图片中心相对视口中心的屏幕偏移。
    ImVec2 m_pan{};
    /// @brief 每原图像素占用的屏幕单位，零表示适应窗口。
    /// 在适应模式下随视口变化重新求解；手动缩放后采用已提交的绝对倍率。
    float m_scale{};
    /// @brief 已排队区域，用于合并连续平移。
    /// 以含余量的上次请求做覆盖判断，单帧滚动不会反复处理相同纹理。
    ImageRegion m_requestedRegion;
    /// @brief 最后请求时的倍率，避免略微滚动重复生成。
    float m_requestedScale{};
    /// @brief 手势认领从触点起始代际决定。
    std::uint64_t m_touchEpoch{};
    /// @brief 起点在图像区，后续事件不得转移给工具栏。
    /// 用于区分普通鼠标和原生多指输入，移动端视图外的原始触点不生效。
    bool m_touchOwned{};
    /// @brief 切换指头数时重建基线。
    unsigned m_touchCount{};
    /// @brief 上一帧的触点或双指中心。
    ImVec2 m_lastTouch{};
    /// @brief 上一帧双指距离。
    float m_lastSpan{};
    /// @brief 工作线程写，完成之后 UI 独占移动。
    /// 取消路径保持缓冲有效，直到读到工作线程发布的完成标志。
    std::optional<RenderedImage> m_result;
    /// @brief 只由 UI 在两个阶段之间移动。
    std::optional<RenderedImage> m_pending;
    /// @warning 有任务时每帧 acquire 查询，唯一写入者是任务末尾的 release。
    std::atomic<bool> m_done{};
    /// @warning 析构先回收后台，再销毁任务可能发布到的结果缓冲。
    std::jthread m_worker;

    /// @brief 断开 UI 所有权并撤销在途工作，关闭时不等待文件解码。
    /// @details 关闭后未上传的像素立刻失效；已提交的 GPU 资源延迟到安全阶段。
    /// 开下一张图片时旧任务可能还在解码，复用线程槽位前先等待完成标志。
    void close()
    {
        m_worker.request_stop();
        // 不从这里读 m_result；任务发布前可能正在改动它。
        // 未上传结果由 UI 独占，可立即丢弃。已被上一帧使用的纹理交给资源阶段。
        m_pending.reset();
        m_clearTextures = true;
        m_source        = Source::None;
        // 事件处理结束后本帧不再绘制该来源；活动的别处视图可以打开新图。
        m_show = m_openRequested = m_fileRequested = m_failed = false;
        m_path.clear();
        m_fileTitle.clear();
        // 丢弃两类来源的界面引用，同时让已开始的任务安全结束。
        m_screenshot.reset();
        m_file.reset();
        m_borrowedThumbnail = {};
        m_thumbnailSize     = {};
        m_bounds            = {};
        // 新一轮不能继承视野覆盖，否则会误以为已经请求过另一张图的同一区域。
        m_requestedRegion = {};
        m_requestedScale  = 0;
        m_scale           = 0;
        m_pan             = {};
        m_touchCount      = 0;
        m_touchOwned      = false;
        m_touchEpoch      = Graphic::mobileTouchSnapshot().m_epoch;
        // 从当前触摸代际开始，开窗按钮的抬手不应变成图片拖动。
    }

    /// @brief 在用户打开文件后执行一次后台解码与概览采样。
    /// @details 正常打开既返回整图缩略又固定供后续局部请求使用的源像素。
    /// 文件内容变化时仍显示本次打开的读入结果，关闭重开才更换版本。
    /// @warning 常规帧只检查是否有打开请求和线程占用，不读取文件。
    void startFileLoad()
    {
        if ( !m_fileRequested || m_worker.joinable() ) return;
        m_fileRequested = false;
        // 发起后消费一次性标记，失败时显示状态，由用户重新打开触发新解码。
        // 第一张纹理要求整图，局部的区域采样要等图的实际尺寸已确认。
        // 新线程只能在旧任务已经移交结果之后启动，发布标志此时无需与旧任务同步。
        m_done.store(false, std::memory_order_relaxed);
        m_worker = std::jthread([this, path = m_path](std::stop_token stop) {
            if ( auto file = readFileImage(path, stop) ) {
                const auto region = file->m_size;
                auto       pixels =
                    sampleFileImage(*file, region, OVERVIEW_EDGE, stop);
                if ( !pixels.m_pixels.empty() )
                    m_result = RenderedImage{ std::move(file),
                                              region,
                                              std::move(pixels) };
            }
            m_done.store(true, std::memory_order_release);
        });
    }

    /// @brief 只在可见区需要新细节时提交后台采样，不阻塞图像交互。
    /// @param region 原图坐标的目标区域，包含视口周边小段余量。
    /// @param scale 请求时的缩放，用于合并后续接近请求。
    /// @details 位图采样与谱面重新栅格化使用同一张局部纹理的空间定位。
    /// @warning 低频拥有一次来源引用，确保关窗后后台不访问已释放的源数据。
    void requestDetail(ImageRegion region, float scale)
    {
        if ( m_worker.joinable() || m_pending || region.m_width <= 0 ||
             region.m_height <= 0 )
            return;
        m_requestedRegion = region;
        // 记录的是提交目标，而非已经上传的区域，避免同一帧重复排队。
        m_requestedScale = scale;
        m_done.store(false, std::memory_order_relaxed);
        m_worker =
            std::jthread([this, file = m_file, chart = m_screenshot, region](
                             std::stop_token stop) mutable {
                // 文件保留已解码源，谱面按原始绘制命令重放，两者都限制纹理边长。
                auto pixels =
                    file ? sampleFileImage(*file, region, DETAIL_EDGE, stop)
                         : previewBeatmapScreenshotRegion(
                               *chart, region, DETAIL_EDGE, stop)
                               .value_or(ImagePixels{});
                if ( !pixels.m_pixels.empty() )
                    m_result = RenderedImage{ {}, region, std::move(pixels) };
                // 关闭时后台可能持有最后一个源图引用，在发布之前回收大缓冲。
                file.reset();
                chart.reset();
                m_done.store(true, std::memory_order_release);
            });
    }

    /// @brief 一次消费工作结果。旧来源收到的最后一帧不会进入上传队列。
    /// @details 关闭和完成可能同帧发生，停止状态优先于完成结果。
    /// 打开文件失败和局部取样失败区分处理，后者仍有整图概览可浏览。
    /// @warning 每帧只看发布标志；标志之后没有剩余解码或文件 I/O。
    void collect()
    {
        if ( !m_worker.joinable() || !m_done.load(std::memory_order_acquire) )
            return;
        // 已发布之后只回收线程句柄，不在此等待解码进度。
        const bool cancelled = m_worker.get_stop_token().stop_requested();
        m_worker.join();
        if ( !cancelled && m_source != Source::None ) {
            if ( m_result )
                m_pending = std::move(m_result);
            else if ( m_source == Source::File && !m_file )
                m_failed = true;
        }
        m_result.reset();
    }

    /// @brief 相对图像视口中的一个锚点缩放，无需等局部纹理更新。
    /// @param oldScale 放大前用于屏幕投影的绝对倍率。
    /// @param target 本次手势或工具栏请求的目标倍率。
    /// @param fit 当前可用区域的下限，避免整图缩比小于适应窗口的尺度。
    /// @param anchor 相对于视口中心的屏幕偏移，有鼠标时固定鼠标下的原图位置。
    /// @warning 交互热路径只计算变换标量。
    void zoom(float oldScale, float target, float fit, ImVec2 anchor)
    {
        // 屏幕比例下限为整图可见，放大上限仍由原始像素提供实际细节。
        const float next = std::clamp(target, fit, 16.0f);
        // 平移也以锚点为基准，滚轮下的物件在放大前后保持原位置。
        m_pan   = anchor + (m_pan - anchor) * (next / oldScale);
        m_scale = next;
    }

    /// @brief 单指拖动、双指中心平移与缩放，从固定的触点起点认领。
    /// @param viewport 图像视口的屏幕边界，菜单和文件名不属于交互区。
    /// @param fit 本帧图像缩放下限。
    /// @return 当前触点被查看器认领时为 true，用于排除平台合成的鼠标增量。
    /// @warning 每帧只读取最多两点，鼠标路径不会同时消费触摸增量。
    bool applyTouch(const ImageViewport& viewport, float fit)
    {
        const auto& raw = Graphic::mobileTouchSnapshot();
        if ( raw.m_epoch != m_touchEpoch ) {
            // 只有触点从查看区域内部开始才认领，工具栏拖进画面仍是外部手势。
            m_touchEpoch      = raw.m_epoch;
            const auto screen = ImGui::GetIO().DisplaySize;
            // 同时检查窗口的 ImGui 命中，避免上层模态遮挡时抢走触点。
            m_touchOwned = ImGui::IsItemHovered() &&
                           viewport.hits({ raw.m_originX * screen.x,
                                           raw.m_originY * screen.y });
            m_touchCount = 0;
        }
        if ( !m_touchOwned || raw.m_cancelled || !raw.m_count ) return false;
        // 移动工作区坐标以主屏逻辑尺寸为单位，不使用帧缓冲物理尺寸。
        const auto screen = ImGui::GetIO().DisplaySize;
        const auto point  = [screen](const Graphic::TouchContact& c) {
            // 两轴转换为屏幕单位后再计算手指距离，避免长宽比扭曲缩放。
            return ImVec2{ c.m_x * screen.x, c.m_y * screen.y };
        };
        const auto     a      = point(raw.m_contacts[0]);
        auto           center = a;
        float          span   = 0;
        const unsigned count =
            static_cast<unsigned>(std::min<std::size_t>(2, raw.m_count));
        if ( count == 2 ) {
            const auto b = point(raw.m_contacts[1]);
            center       = (a + b) * 0.5f;
            span         = std::hypot(a.x - b.x, a.y - b.y);
        }
        // 单指与双指互切时只记录新基线，防止整张图片跳到另一根手指下。
        if ( count == m_touchCount ) {
            if ( count == 2 && span > 2 && m_lastSpan > 2 ) {
                const float current = m_scale > 0 ? m_scale : fit;
                zoom(current,
                     current * span / m_lastSpan,
                     fit,
                     m_lastTouch - (viewport.m_min + viewport.m_max) * 0.5f);
            }
            m_pan += center - m_lastTouch;
        }
        m_lastTouch  = center;
        m_lastSpan   = span;
        m_touchCount = count;
        return true;
    }

    /// @brief 把屏幕视野反投影到成品矩形，扩出边缘以覆盖少量后续拖动。
    /// @param viewport 实际图像视口，屏幕逻辑像素坐标。
    /// @param origin 图片当前左上角的屏幕坐标，拖动时可位于视口外。
    /// @param scale 成品像素到屏幕单位的倍率，必须大于零。
    /// @param extra 各边额外覆盖的屏幕距离，零表示严格可见范围。
    /// @return 边界向外取整并钳制到原图的请求区域。
    /// @warning 只算整数边界；本地平移和滚轮增量不会等待后台。
    ImageRegion visibleRegion(ImageViewport viewport, ImVec2 origin,
                              float scale, float extra) const
    {
        // 可见区是在缩放与平移之后求交，余量也从屏幕像素反算到原图。
        const auto begin =
            (viewport.m_min - origin - ImVec2{ extra, extra }) / scale;
        const auto end =
            (viewport.m_max - origin + ImVec2{ extra, extra }) / scale;
        const int x = static_cast<int>(
            std::floor(std::clamp(begin.x, 0.0f, float(m_bounds.m_width))));
        const int y = static_cast<int>(
            std::floor(std::clamp(begin.y, 0.0f, float(m_bounds.m_height))));
        return { x,
                 y,
                 static_cast<int>(std::ceil(
                     std::clamp(end.x, 0.0f, float(m_bounds.m_width)))) -
                     x,
                 static_cast<int>(std::ceil(
                     std::clamp(end.y, 0.0f, float(m_bounds.m_height)))) -
                     y };
    }

    /// @brief 在画布内处理浏览及缩放，按成品坐标套合概览和局部层。
    /// @param dpiScale 状态行和触摸按钮使用的界面内容倍率。
    /// @pre 当前来源具有有效尺寸与已注册的概览纹理。
    /// @details 工具栏先处理坐标倍率，随后裁剪实际画面，避免随鼠标拖出窗口。
    /// @warning 每帧只提交有限绘制命令；离开拖动时才请求需要的高清范围。
    void drawImage(float dpiScale)
    {
        const auto available = ImGui::GetContentRegionAvail();
        if ( available.x < 2 || available.y < 2 ) return;
        const ImVec2 size{ float(m_bounds.m_width), float(m_bounds.m_height) };
        // 给控件消息和缩放比例留一行，移动端按钮仍可用普通点击操作。
        const float buttonHeight =
            workspaceAllowsHoverFeedback() ? 0.0f : 40.0f * dpiScale;
        const float toolbarHeight =
            std::max(ImGui::GetFrameHeight(), buttonHeight) +
            ImGui::GetStyle().ItemSpacing.y +
            ImGui::GetTextLineHeightWithSpacing();
        const ImVec2 area{ available.x,
                           std::max(1.0f, available.y - toolbarHeight) };
        const float  fit = std::min({ 1.0f, area.x / size.x, area.y / size.y });
        float        current = std::max(fit, m_scale);
        if ( FeedbackButton("−##ImageZoomOut",
                            { 40 * dpiScale, buttonHeight }) )
            zoom(current, current / 1.25f, fit, {});
        ImGui::SameLine();
        if ( FeedbackButton("+##ImageZoomIn", { 40 * dpiScale, buttonHeight }) )
            zoom(current, current * 1.25f, fit, {});
        ImGui::SameLine();
        if ( FeedbackButton("1:1##ImageActualSize", { 0, buttonHeight }) ) {
            // 原始大小从图心开始显示；此后用户可平移至任意图边。
            m_scale = 1;
            m_pan   = {};
        }
        ImGui::SameLine();
        if ( FeedbackButton(TR("ui.image_viewer.fit").data(),
                            { 0, buttonHeight }) ) {
            m_scale = 0;
            m_pan   = {};
        }
        current = std::max(fit, m_scale);
        ImGui::Text("%d × %d   %.1f%%",
                    m_bounds.m_width,
                    m_bounds.m_height,
                    current * 100.0f);

        const auto room = ImGui::GetContentRegionAvail();
        if ( room.x < 1 || room.y < 1 ) return;
        const auto          min = ImGui::GetCursorScreenPos();
        const ImageViewport viewport{ min, min + room };
        const ImVec2        middle = min + room * 0.5f;
        // 仅分配命中区域，图像由裁剪后的画笔绘制，鼠标松开不会触发额外按钮动作。
        ImGui::InvisibleButton(
            "##ImagePanSurface", room, ImGuiButtonFlags_MouseButtonLeft);
        const bool touch = applyTouch(viewport, fit);
        // 滚轮只在画布内生效，工具栏按钮的输入不会同时移动图像。
        const auto& io = ImGui::GetIO();
        if ( !touch && ImGui::IsItemHovered() && io.MouseWheel != 0 )
            zoom(current,
                 current * std::pow(1.25f, io.MouseWheel),
                 fit,
                 io.MousePos - middle);
        // Android 的原始触摸由上面的导航消费，不能再重复使用合成鼠标位移。
        const bool dragging = ImGui::IsItemActive() &&
                              Graphic::mobileTouchSnapshot().m_count == 0;
        if ( !touch && dragging ) m_pan += io.MouseDelta;
        current = std::max(fit, m_scale);

        // 小于视口的轴始终居中；放大的轴可拖到首尾有效像素。
        const ImVec2 extent = size * current;
        const ImVec2 limits{ std::max(0.0f, (extent.x - room.x) * 0.5f),
                             std::max(0.0f, (extent.y - room.y) * 0.5f) };
        m_pan.x           = std::clamp(m_pan.x, -limits.x, limits.x);
        m_pan.y           = std::clamp(m_pan.y, -limits.y, limits.y);
        const auto origin = middle + m_pan - extent * 0.5f;
        auto*      draw   = ImGui::GetWindowDrawList();
        // 两层都以同一不透明画布为底，透明 PNG 或 SVG 的 alpha 不会叠加两次。
        const auto background =
            ImGui::GetColorU32(ImGuiCol_WindowBg) | IM_COL32_A_MASK;
        const auto overviewSize =
            m_borrowedThumbnail
                ? m_thumbnailSize
                : ImVec2{ float(m_overview.m_texture->width()),
                          float(m_overview.m_texture->height()) };
        // 极细图像的窄边可能无法再缩小，宽边或高边仍可能需要原图细节。
        // 长条两边的整纹理四舍五入不同，按较低采样密度判断。
        const float overviewScale =
            std::min(overviewSize.x / size.x, overviewSize.y / size.y);
        const bool wantsDetail =
            (overviewSize.x < size.x || overviewSize.y < size.y) &&
            current > overviewScale * 1.1f;
        draw->PushClipRect(viewport.m_min, viewport.m_max, true);
        draw->AddRectFilled(viewport.m_min, viewport.m_max, background);
        const auto overview =
            m_borrowedThumbnail ? m_borrowedThumbnail : m_overview.m_id;
        draw->AddImage(overview, origin, origin + extent);
        // 概览先提供完整覆盖，局部更新未完成时平移也不会露出空白绘图区。
        if ( wantsDetail && m_detail.m_id ) {
            // 旧局部图按它自己的成品坐标放置，只在对应位置与概览叠合。
            const auto& region = m_detail.m_region;
            const auto  topLeft =
                origin +
                ImVec2{ float(region.m_x), float(region.m_y) } * current;
            const auto bottomRight = topLeft + ImVec2{
                float(region.m_width), float(region.m_height)
            } * current;
            // 有透明像素的局部图层先清掉概览对应区域，避免叠加两次透明度。
            draw->AddRectFilled(topLeft, bottomRight, background);
            draw->AddImage(m_detail.m_id, topLeft, bottomRight);
        }
        draw->PopClipRect();

        // 手势进行中用已存在的图层保持响应，结束后只取最终视野的像素。
        if ( touch || dragging || !wantsDetail ) return;
        // 仅判定严格可见区是否被覆盖，下一片则在视口外保留预取余量。
        const auto visible = visibleRegion(viewport, origin, current, 0);
        // 隔一段边距再刷新，避免微量平移不断重建纹理；倍率明显提高则可提升采样。
        if ( !contains(m_requestedRegion, visible) ||
             // 超出旧区域或明显放大后才生成新纹理，避免微动时重排任务。
             current > m_requestedScale * 1.4f )
            requestDetail(
                visibleRegion(viewport, origin, current, 100 * dpiScale),
                current);
    }

    /// @brief 两个宿主共用工具栏、加载提示和图像区域。
    /// @warning 每帧只读已发布的结果，图像还没就绪时不计算缩放。
    void drawContent(float dpiScale)
    {
        // 图像尺寸来自源像素；等待初次解码时不绘制可导致除零的图像控件。
        if ( (m_borrowedThumbnail || m_overview.m_id) && m_bounds.m_width > 0 &&
             m_bounds.m_height > 0 )
            drawImage(dpiScale);
        else
            ImGui::TextWrapped("%s",
                               TR(m_failed ? "ui.image_viewer.unavailable"
                                           : "ui.image_viewer.loading")
                                   .data());
    }

    /// @brief 从导出参数窗口显示原图，可以返回之前的导出设置。
    /// @param dpiScale 当前内容倍率，最终模态尺寸由平台安全区约束。
    /// @note 只由导出参数窗口调用，确保 ImGui 识别正确的父级。
    /// 首次出现时居中，之后按原生窗口尺寸计算适应比例和实际可见范围。
    /// @warning 热路径只绘制窗口和已发布状态，解码不在此等待。
    void drawPopup(float dpiScale)
    {
        if ( !m_openRequested && !m_show ) return;
        // 译名可热切换，固定后缀保留弹窗身份与子窗口的层次关系。
        const auto title =
            TR("ui.image_viewer.title").toString() + "###" + EXPORT_POPUP_ID;
        if ( std::exchange(m_openRequested, false) ) {
            FeedbackOpenPopup(title.c_str());
            // 打开请求仅在相同 ImGui 父级消费一次，后续帧只绘制现有模态。
            m_show = true;
        }
        Utils::CenteredModalPopupScope popup(dpiScale);
        if ( !popup.begin(title.c_str(),
                          &m_show,
                          ImGuiWindowFlags_NoDocking | IMAGE_WINDOW_FLAGS,
                          { 1100 * dpiScale, 800 * dpiScale },
                          false,
                          ImVec2{ 320 * dpiScale, 240 * dpiScale }) ) {
            // 宿主的标题栏关闭、Escape 和父级折叠都可能使模态消失。
            if ( !ImGui::IsPopupOpen(title.c_str()) ) close();
            return;
        }
        drawContent(dpiScale);
        ImGui::EndPopup();
    }
};

/// @brief 初始化查看器资源能力和独立窗口状态，首次打开才加载图像。
/// @note 视图标识不随语言、文件名和导出标题变化，避免重复注册。
ImageViewer::ImageViewer(const char* viewId)
    : IUIView(viewId), ITextureLoader(viewId), m_impl(std::make_unique<Impl>())
{
}

/// @brief 随 UIManager 释放；其销毁协议先完成正在使用描述符的 Vulkan 命令。
/// @warning 退出阶段的回收可能等待尚未结束的文件解码，普通关窗只发出停止请求。
ImageViewer::~ImageViewer() = default;

/// @brief 从当前 UI 管理器选取对应宿主的图片资源视图。
/// @param manager 拥有纹理上传和视图析构顺序的 UIManager。
/// @param viewId 对应中央文件标签或导出模态的固定注册名。
/// @return 由管理器持有的有效实例，调用方只借用。
/// @note 延迟注册允许文件树活动期间打开查看器，无需重建侧栏上下文。
ImageViewer& ImageViewer::get(UIManager& manager, const char* viewId)
{
    if ( auto* existing = manager.getView<ImageViewer>(viewId) )
        return *existing;
    auto  viewer = std::make_unique<ImageViewer>(viewId);
    auto& result = *viewer;
    manager.registerView(viewId, std::move(viewer));
    // 注册表在窗口隐藏后仍保留实例，重新打开只覆盖源及导航状态。
    return result;
}

/// @brief 文件树复用一个可关闭的中央标签页。
ImageViewer& ImageViewer::fileTab(UIManager& manager)
{
    return get(manager, FILE_IMAGE_TAB_ID);
}

/// @brief 导出参数窗使用自己的子模态，独立持有后台任务与纹理。
ImageViewer& ImageViewer::exportPopup(UIManager& manager)
{
    return get(manager, EXPORT_POPUP_ID);
}

/// @brief 从文件树激活静态图片，由中央查看页处理解码与焦点。
/// @param path 文件系统原生路径，按值接管以跨过文件列表缓存刷新。
/// @details 目标不存在或读取失败时仍打开窗口，并显示统一的失败提示。
/// 路径仅用于背景解码，不改变当前项目、播放状态或系统文件关联。
void ImageViewer::openFile(std::filesystem::path path)
{
    if ( m_impl->m_source == Source::File && m_impl->m_path == path &&
         !m_impl->m_failed ) {
        // 已打开的图片只切回标签，保留缩放与拖动位置。
        // 解码在途时同样复用，快速连点不会取消尚未发布的同一文件。
        m_impl->m_openRequested = true;
        return;
    }
    m_impl->close();
    m_impl->m_source = Source::File;
    m_impl->m_fileTitle =
        Config::pathToUtf8(path.filename()) + "###" + FILE_IMAGE_TAB_ID;
    m_impl->m_path = std::move(path);
    // 项目文件树只传路径，读取和 PNG 解码都保留在后台任务。
    m_impl->m_show = m_impl->m_openRequested = m_impl->m_fileRequested = true;
}

/// @brief 打开导出预览的子窗口，直到关闭为止都沿用可见缩略图所属的快照。
/// @param image 父窗准备好的只读正式绘制批次，供后台生成高清区域。
/// @param imageSize 导出成品原始区域，后续坐标不从缩略图反推。
/// @param thumbnail 父级预览的 ImGui 纹理标识，只借用显存资源。
/// @param thumbnailSize 父级缩略图实际大小，用于判定何时需要高清取样。
/// @note 用户关闭子窗口时，父级缩略图和配置继续可用。
void ImageViewer::openScreenshot(
    std::shared_ptr<const BeatmapScreenshotImage> image, ImageRegion imageSize,
    ImTextureID thumbnail, ImVec2 thumbnailSize)
{
    m_impl->close();
    m_impl->m_source            = Source::Screenshot;
    m_impl->m_screenshot        = std::move(image);
    m_impl->m_bounds            = imageSize;
    m_impl->m_borrowedThumbnail = thumbnail;
    // 已在导出窗口显示的缩图可立即借用，按需细节仍从当前导出参数采样。
    m_impl->m_thumbnailSize = thumbnailSize;
    m_impl->m_openRequested = true;
}

/// @brief 保持预览模态的 ImGui ID 层级，由原参数窗口提交同一查看器。
/// @param dpiScale 参数窗口本帧使用的内容倍率。
/// @warning UI 热路径只推进对应来源；文件查看器由自身 update 绘制。
void ImageViewer::drawScreenshotPopup(float dpiScale)
{
    if ( m_impl->m_source == Source::Screenshot ) m_impl->drawPopup(dpiScale);
}

/// @brief 释放子窗口借用的父级纹理身份，撤销后台绘制但保留导出参数。
/// @note 独立文件打开流程不应被参数窗口的清理请求改动。
void ImageViewer::closeScreenshot()
{
    if ( m_impl->m_source == Source::Screenshot ) m_impl->close();
}

/// @brief 推进工程文件的中央标签页和一次性解码请求。
/// @warning 每帧入口不执行磁盘操作；新文件的解码由独占后台线程处理。
void ImageViewer::update(UIManager*)
{
    if ( m_impl->m_source != Source::File ) return;
    m_impl->startFileLoad();
    if ( m_impl->m_openRequested ) {
        // 宿主在项目切换时可能还在构建；等中央节点可用才提交窗口。
        const auto dockId = MainDockSpaceUI::getCenterDockId();
        if ( !dockId ) return;
        ImGui::SetNextWindowDockID(dockId, ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
        // 后续帧由用户决定选哪个标签，后台像素就绪也不会再抢焦点。
        m_impl->m_openRequested = false;
    }
    // 移动外壳可接管默认停靠、输入屏蔽与标签长按关闭。
    // 图片页由资源管理器显式打开，工作区还原不需要持久化临时查看窗口。
    const bool wasOpen = m_impl->m_show;
    const bool visible = beginWorkspaceWindow(
        m_name,
        m_impl->m_fileTitle,
        &m_impl->m_show,
        IMAGE_WINDOW_FLAGS | ImGuiWindowFlags_NoSavedSettings);
    FeedbackCurrentWindowCloseButton(wasOpen, &m_impl->m_show);
    if ( visible && m_impl->m_show )
        m_impl->drawContent(
            Config::AppConfig::instance().getWindowContentScale());
    ImGui::End();
    // 点击关闭后取消解码，下一资源阶段回收已上传的纹理。
    if ( !m_impl->m_show ) m_impl->close();
}

/// @brief 资源阶段检查是否需要上传局部图或释放已有纹理。
/// @return 像素上传和关闭回收两种工作都需要调用 reloadTextures。
/// @warning 没有任务完成时只读取状态，不通过锁等待后台或调用 GPU。
bool ImageViewer::needReload()
{
    m_impl->collect();
    // 发布结果可能会开启一次上传；没有工作时不访问 Vulkan 描述符。
    return m_impl->m_clearTextures || m_impl->m_pending.has_value();
}

/// @brief 在已有渲染资源阶段创建一张有限大小的图片纹理。
/// @param physical 查找支持的内存类型和图像格式。
/// @param device 创建 VkImage、采样器和描述符的设备。
/// @param pool 上传命令所用的队列对应命令池。
/// @param queue 普通 UI 图层使用的图形队列，回收前应完成在途命令。
/// @warning 低频资源路径：静态上传和释放发生时可等待图形队列。
/// @details 旧细节可以保留至新区域生成，但不能跨打开的图片叠加旧来源。
void ImageViewer::reloadTextures(vk::PhysicalDevice& physical,
                                 vk::Device& device, vk::CommandPool& pool,
                                 vk::Queue& queue)
{
    if ( std::exchange(m_impl->m_clearTextures, false) ) {
        // 关闭后的旧描述符仍可能被上一帧引用，仅在资源阶段同步后销毁。
        if ( m_impl->m_overview.m_texture || m_impl->m_detail.m_texture )
            (void)queue.waitIdle();
        m_impl->m_overview = {};
        m_impl->m_detail   = {};
    }
    if ( !m_impl->m_pending ) return;
    // 清理单独发生时就到此结束，不分配 staging 或解析图像。
    auto rendered = std::move(*m_impl->m_pending);
    // 任务取消时清除 pending；这里接管的像素因此只属于当前显示来源。
    m_impl->m_pending.reset();
    auto texture =
        std::make_unique<Graphic::VKTexture>(rendered.m_pixels.m_pixels.data(),
                                             rendered.m_pixels.m_width,
                                             rendered.m_pixels.m_height,
                                             physical,
                                             device,
                                             pool,
                                             queue);
    // 描述符准备失败时保留当前概览，不发布不可绘制的图层。
    const auto id =
        texture->isValid() ? texture->getImTextureID() : ImTextureID{};
    if ( !id ) {
        // 整图上传失败没有可交互的范围；局部失败则允许继续用已有概览。
        if ( rendered.m_file ) m_impl->m_failed = true;
        return;
    }
    // VKTexture 完成同队列的静态上传后再替换图层，旧描述符已无在途引用。
    ImageLayer layer{ std::move(texture), id, rendered.m_region };
    if ( rendered.m_file ) {
        // 整图准备完整后同时发布原始尺寸与文件数据，供后续局部请求借用。
        m_impl->m_bounds   = rendered.m_file->m_size;
        m_impl->m_file     = std::move(rendered.m_file);
        m_impl->m_overview = std::move(layer);
    } else {
        m_impl->m_detail = std::move(layer);
    }
}
}  // namespace MMM::UI
