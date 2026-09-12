#pragma once

#include <cstddef>

namespace MMM::Logic
{

/// @brief 可覆盖的音符局部配色槽位。
/// @note 槽位描述渲染部件语义，与主题中实际颜色值分离。
enum class NoteColorSlot {
    // Tap、Head、Hold 与 End 覆盖音符主体的生命周期部件。
    Tap,
    Head,
    Hold,
    End,
    // FlickArrow 与 Node 覆盖方向提示和折点等附加部件。
    FlickArrow,
    Node,
};

/// @brief 音符配色槽位数量。
/// @note 新增 NoteColorSlot 时必须同步更新该常量和所有定长配色容器。
inline constexpr std::size_t NOTE_COLOR_SLOT_COUNT = 6;

}  // namespace MMM::Logic
