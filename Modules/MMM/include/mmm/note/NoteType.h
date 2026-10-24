#pragma once

namespace MMM
{
/// @brief 模型、渲染与教学 DTO 共用的玩家物件类型。
/// @details 保持原枚举顺序及默认 int 底层类型，避免改变既有数据布局。
/// 仅表达物件几何种类，不依赖模型实例、采样绑定或格式元数据。
enum class NoteType {
    /// @brief 普通瞬时物件。
    NOTE,
    /// @brief 具有持续时间的长条物件。
    HOLD,
    /// @brief 带横向滑动方向的瞬时物件。
    FLICK,
    /// @brief 由多个节点组成的折线物件。
    POLYLINE,
};
}  // namespace MMM
