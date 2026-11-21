#pragma once

#include "common/render/RenderSnapshot.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"

#include <filesystem>
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
/// @brief 本次导出的编辑器快照，拥有独立的物件、分拍事件及署名。
struct BeatmapScreenshot {
    /// @brief 只取正式轨道，不包含草稿和 BGM 区。
    int m_lanes{};
    /// @brief 时间窗以秒为单位，列的读取方向与主画布一致。
    double m_begin{}, m_end{};
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
    /// @brief 当前列内容高度，末列可能少于 60 秒。
    int m_height{};
};

/// @brief 可以移交给后台编码器的完整渲染输入。
struct BeatmapScreenshotImage {
    /// @brief 区间按时间从左到右排列；每列内仍从下向上读谱。
    std::vector<BeatmapScreenshotColumn> m_columns;
    /// @brief 与列批次 UV 对应的 RGBA8 皮肤图集。
    std::vector<unsigned char> m_atlas;
    /// @brief 单列正式轨道宽度。
    int m_columnWidth{};
    /// @brief MMM 水印独立开关。
    bool m_showMmm{};
    /// @brief 仅在启用作者水印时非空。
    std::string m_author;
    /// @brief 署名和时间栏字体路径，由后台线程栅格化。
    std::filesystem::path m_asciiFont, m_cjkFont;
};

/// @brief PNG 导出结果，供界面本地化呈现。
enum class ScreenshotResult {
    Saved,         ///< 完整的 PNG 已提交到目标文件。
    InvalidRange,  ///< 时段超出支持尺寸。
    FontError,     ///< 字体资源不可用。
    WriteError,    ///< 无法编码或写入目标文件。
    PublishError,  ///< 系统文档提供器拒绝发布 PNG。
    Cancelled,     ///< 未提交图像前收到停止请求。
};

/// @brief 校验时间、轨数和像素预算，不访问文件系统。
[[nodiscard]] bool validScreenshotRange(int lanes, double begin, double end);

/// @brief 复用正式画布生成分列批次；调用期间借用当前皮肤。
/// @warning 只在显式导出时从 UI 线程调用，在后台编码前固定资源代际。
[[nodiscard]] std::optional<BeatmapScreenshotImage> prepareBeatmapScreenshot(
    const BeatmapScreenshot& chart, const Config::EditorConfig& config);

/// @brief 将画布批次逐行写入 PNG，原图完整生成前保留已有目标文件。
/// @warning 后台导出路径；只读取自有图集、几何和字体输入。
[[nodiscard]] ScreenshotResult writeBeatmapScreenshot(
    const BeatmapScreenshotImage& image, const std::filesystem::path& path,
    std::stop_token stop = {});

}  // namespace MMM::UI
