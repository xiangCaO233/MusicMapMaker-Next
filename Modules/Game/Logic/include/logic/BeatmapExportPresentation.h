#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace MMM::Logic
{

/// @brief 导出对话框命名所需的活动谱面字段。
/// @note 各字段独立拥有存储，文件名清理与格式化在会话临界区外执行。
struct BeatmapExportNaming {
    /// @brief 谱面内部名称，用于没有当前文件名时的退路。
    std::string name;
    /// @brief 先选 Unicode 标题，再选普通标题和内部名称。
    std::string preferredTitle;
    /// @brief 谱面版本或难度，空值由文件命名规则提供默认值。
    std::string version;
    /// @brief 谱面的操作轨道数，不含自动采样轨。
    std::int32_t trackCount{};
};

/// @brief 对话框请求的兼容性检查范围。
enum class BeatmapExportFormat : std::uint8_t {
    Bms,     ///< BMS 与 PMS 共享音量和物件检查。
    Osu,     ///< osu! 关注 Jump、HS、负 Scroll 和长条折线。
    Imd,     ///< RM/IMD 需要检查写不出的 timing、谱面及物件元数据。
    Malody,  ///< Malody 的 Key/Slide 差异与上架扩展选项。
};

/// @brief 兼容性提示所需的值字段，不持有会话、谱面或视图。
/// @note timing、音量及元数据标记只按请求格式计算，其余保持默认。
struct BeatmapExportCompatibility {
    /// @brief 从当前谱面记录的实际操作轨道数。
    std::int32_t trackCount{};
    /// @brief 谱面是否包含 Flick。
    bool hasFlick{};
    /// @brief 谱面是否包含折线物件。
    bool hasPolyline{};
    /// @brief BMS 之外的轨迹效果是否需要提示保留范围。
    bool hasBmsTimingDifferences{};
    /// @brief 自动采样或玩家物件绑定是否使用非默认音量。
    bool hasNonDefaultSampleVolume{};
    /// @brief osu! 不表达的 Jump 或 HS。
    bool hasJumpOrHs{};
    /// @brief osu! 导出器会跳过的负 Scroll。
    bool hasNegativeScroll{};
    /// @brief 在 RM/IMD 导出中不能保留的 Jump、HS 或 Scroll。
    bool hasImdTimingDifferences{};
    /// @brief 当前有超过 RM 基础和谱面扩展能力的字段。
    bool hasImdMapMetadataDifferences{};
    /// @brief 当前有超过 RM 物件元数据能力的字段。
    bool hasImdNoteMetadataDifferences{};
    /// @brief 原有 Malody mode 协议值，非法数据保持空值。
    std::optional<int> malodyMode;
};

/// @brief 为导出选择器捕获活动谱面的命名信息。
/// @return 无活动谱面时为空。
/// @warning 低频文件选择路径，只复制命名字段，不执行文件操作。
[[nodiscard]] std::optional<BeatmapExportNaming> captureBeatmapExportNaming();

/// @brief 在同一会话版本中分析目标格式需要的兼容性信息。
/// @param format 目标格式，用于避免扫描无关的字段和物件。
/// @return 无活动谱面时为空。结果可在切换或关闭会话后继续格式化。
/// @warning 用户主动导出时使用；部分格式需要完整遍历 timing 或物件。
[[nodiscard]] std::optional<BeatmapExportCompatibility>
captureBeatmapExportCompatibility(BeatmapExportFormat format);

}  // namespace MMM::Logic
