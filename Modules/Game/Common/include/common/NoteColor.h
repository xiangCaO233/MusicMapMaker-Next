#pragma once

#include <cstddef>

namespace MMM::Logic
{

/// @brief 可覆盖的音符局部配色槽位。
enum class NoteColorSlot {
    Tap,
    Head,
    Hold,
    End,
    FlickArrow,
    Node,
};

/// @brief 音符配色槽位数量。
inline constexpr std::size_t NOTE_COLOR_SLOT_COUNT = 6;

}  // namespace MMM::Logic
