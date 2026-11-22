#pragma once

#include "common/render/RenderSnapshot.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "ui/image/ImageRegion.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace MMM::Config
{
struct EditorConfig;
}

namespace MMM::UI
{
/// @brief 界面与导出快照共用的默认单列时长，单位秒。
inline constexpr int DEFAULT_SCREENSHOT_COLUMN_SECONDS = 15;

/// @brief 皮肤采样使用的图集，在同一次参数调整、预览及导出中共享。
/// @note 载入后发布为只读数据；修改参数只重建几何，无需重新读图。
struct BeatmapScreenshotAtlas {
    /// @brief 每列批次引用的 RGBA8 皮肤像素。
    std::vector<unsigned char> m_pixels;
    /// @brief 从纹理身份到图集中各素材矩形的映射。
    decltype(Common::Render::RenderSnapshot::uvMap) m_uvMap;
};

/// @brief 本次导出的编辑器快照，拥有独立的物件、分拍事件及署名。
struct BeatmapScreenshot {
    /// @brief 只取正式轨道，不包含草稿和 BGM 区。
    int m_lanes{};
    /// @brief 时间窗以秒为单位，列的读取方向与主画布一致。
    double m_begin{}, m_end{};
    /// @brief 整数秒的单列时长上限，决定 PNG 的分列边界。
    int m_columnSeconds{ DEFAULT_SCREENSHOT_COLUMN_SECONDS };
    /// @brief 纵向时间间距的百分比，100 对应原有导出刻度；轨宽不受影响。
    double m_timelineZoomPercent{ 100.0 };
    /// @brief 控制导出图的横向分拍线，轨道边界仍由皮肤绘制。
    bool m_drawBeatLines{ true };
    /// @brief 没有有效红线时使用的谱面参考节奏。
    double m_defaultBpm{ 120.0 };
    /// @brief MMM 水印开关，独立于作者水印。
    bool m_showMmm{};
    /// @brief 可选作者水印；关闭时不传出作者名。
    std::string m_author;
    /// @brief 用于渲染时间栏与中英文署名的皮肤字体。
    std::filesystem::path m_asciiFont, m_cjkFont;
    /// @brief 包含未保存修改，完整保留折线子段和颜色覆盖。
    std::vector<Logic::NoteComponent> m_notes;
    /// @brief BPM 红线及插值，分拍线由真实画布生成。
    std::vector<Logic::TimelineComponent> m_tempos;
};

/// @brief 一列已排好次序的画布几何与时间栏标签。
struct BeatmapScreenshotColumn {
    /// @brief 输出列的可读时间起止标记。
    std::string m_label;
    /// @brief 与主画布相同的无 GPU 批次；逻辑坐标是单列局部像素。
    Common::Render::RenderSnapshot m_scene;
    /// @brief 当前列内容高度，末列可能小于用户设定的列时长。
    int m_height{};
};

/// @brief 可以移交给后台编码器的完整渲染输入。
struct BeatmapScreenshotImage {
    /// @brief 区间按时间从左到右排列；每列内仍从下向上读谱。
    std::vector<BeatmapScreenshotColumn> m_columns;
    /// @brief 与列批次 UV 对应的只读皮肤；任务跨线程存活时仍拥有采样数据。
    std::shared_ptr<const BeatmapScreenshotAtlas> m_atlas;
    /// @brief 单列正式轨道宽度。
    int m_columnWidth{};
    /// @brief MMM 水印独立开关。
    bool m_showMmm{};
    /// @brief 仅在启用作者水印时非空。
    std::string m_author;
    /// @brief 署名和时间栏字体路径，由后台线程栅格化。
    std::filesystem::path m_asciiFont, m_cjkFont;
};

/// @brief 导出参数对应的实际 PNG 像素尺寸及列数。
struct BeatmapScreenshotSize {
    int m_width{}, m_height{};  ///< 包含轨道、页边距和已启用水印的像素尺寸。
    int m_columns{};            ///< 按设定时长切分的列数，含不足整列的末段。
};

/// @brief 预览纹理的数据，等比采样完整输出，内存占用独立于成品尺寸。
using BeatmapScreenshotPreview = ImagePixels;

/// @brief PNG 导出结果，供界面本地化呈现。
enum class ScreenshotResult {
    Saved,         ///< 完整的 PNG 已提交到目标文件。
    InvalidRange,  ///< 时段超出支持尺寸。
    FontError,     ///< 字体资源不可用。
    WriteError,    ///< 无法编码或写入目标文件。
    PublishError,  ///< 系统文档提供器拒绝发布 PNG。
    Cancelled,     ///< 未提交图像前收到停止请求。
};

/// @brief 计算成品尺寸；时间、缩放或像素预算无效时返回空。
/// @param watermarkLines 实际出现的署名行数，取值零至二。
/// @warning 可用于每帧重算参数：常量时间，不访问文件系统。
[[nodiscard]] std::optional<BeatmapScreenshotSize> estimateBeatmapScreenshot(
    int lanes, double begin, double end, int columnSeconds,
    double timelineZoomPercent, int watermarkLines);

/// @brief 固定用于本轮预览和保存的当前皮肤图集。
/// @warning 显式开启导出时从 UI 线程调用；会读取皮肤文件。
[[nodiscard]] std::shared_ptr<const BeatmapScreenshotAtlas>
loadBeatmapScreenshotAtlas();

/// @brief 复用正式画布生成分列批次；调用期间借用当前皮肤。
/// @param atlas 已缓存的皮肤图集；空指针时由调用线程读取当前皮肤。
/// @warning 仅在显式导出或参数变动后调用，在后台绘制前固定资源代际。
[[nodiscard]] std::optional<BeatmapScreenshotImage> prepareBeatmapScreenshot(
    const BeatmapScreenshot& chart, const Config::EditorConfig& config,
    std::shared_ptr<const BeatmapScreenshotAtlas> atlas = {});

/// @brief 在后台按 PNG 的绘制顺序生成有界的全图预览。
/// @return 取消或字体准备失败时不发布任何像素。
[[nodiscard]] std::optional<BeatmapScreenshotPreview> previewBeatmapScreenshot(
    const BeatmapScreenshotImage& image, std::stop_token stop = {});

/// @brief 为查看器绘制成品指定区域，直接采样原始图元并限制纹理边长。
/// @param region 成品像素位置，必须完全位于预计 PNG 尺寸内。
/// @param maxEdge 局部预览的最大纹理边长。
/// @warning 后台路径，允许跳过所选区域之外的成品像素。
[[nodiscard]] std::optional<ImagePixels> previewBeatmapScreenshotRegion(
    const BeatmapScreenshotImage& image, ImageRegion region, int maxEdge,
    std::stop_token stop = {});

/// @brief 将画布批次逐行写入 PNG，原图完整生成前保留已有目标文件。
/// @warning 后台导出路径；只读取自有图集、几何和字体输入。
[[nodiscard]] ScreenshotResult writeBeatmapScreenshot(
    const BeatmapScreenshotImage& image, const std::filesystem::path& path,
    std::stop_token stop = {});

}  // namespace MMM::UI
