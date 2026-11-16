#pragma once

#include <array>
#include <cstddef>

namespace MMM::Config
{
/// @brief 独立的音符视觉部件；身份不复用拾取协议或纹理编号。
/// @note Body 仅调整横截面，谱面时间与轨道决定的连接跨度不缩放。
enum class NoteSizePart {
    /// @brief 根头部保留旧字段，避免头部配置出现两个不同来源。
    Head,  ///< 单键与长条、滑键、折线的起始头部。
    /// @brief 纵向连接体只取 X 作为厚度，持续时间不参与缩放。
    HoldBody,  ///< 独立长条的纵向连接体。
    /// @brief 结束端点独立设置两轴，不改变长条结束时刻。
    HoldTail,  ///< 长条及折线末端的长条尾部。
    /// @brief 横向连接体只取 Y 作为厚度，跨度由起终轨道决定。
    FlickBody,  ///< 独立滑键的横向连接体。
    /// @brief 箭头方向取自谱面，两个方向共用用户尺寸。
    FlickTail,  ///< 左右滑键箭头，包含折线末端箭头。
    /// @brief 只用于中间节点，首节点继续使用 Head。
    PolylineNode,  ///< 折线中间节点。
    /// @brief 折线纵段取 X、横段取 Y，与独立载体分别保存。
    PolylineBody,  ///< 折线载体与节点间的连接段。
    Count          ///< 固定容量边界，不表示可编辑部件。
};

/// @brief 一类部件的独立二维尺寸，数值相对轨宽与基础纹理比例。
struct NotePartScale {
    /// @brief 缺省值是确定尺寸，不表示动态继承头部。
    float x{ 0.95F };  ///< 横轴比例，不继承头部当前比例。
    float y{ 0.95F };  ///< 纵轴比例，不影响连接段的时间跨度。
    /// @brief 值语义比较供配置变化检测与迁移回归使用。
    bool operator==(const NotePartScale&) const = default;
};

/// @brief 非头部的固定数组；头部继续使用旧字段以兼容既有配置。
using NotePartScales =
    std::array<NotePartScale,
               static_cast<std::size_t>(NoteSizePart::Count) - 1>;
/// @brief 持久化键按数组顺序排列，不使用翻译文本作为配置协议。
inline constexpr std::array NOTE_PART_SIZE_KEYS{
    "holdBody",  "holdTail",     "flickBody",
    "flickTail", "polylineNode", "polylineBody"
};
}  // namespace MMM::Config
