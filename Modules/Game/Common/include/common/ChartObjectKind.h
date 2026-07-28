#pragma once

#include <cstdint>

namespace MMM::Logic
{

/// @brief 标识独立 ECS 注册表中的谱面物件领域。
enum class ChartObjectKind : std::uint8_t {
    PlayerNote = 0,  ///< 需要玩家操作的 Note。
    AudioSample,     ///< 无需玩家操作的自动采样。
};

}  // namespace MMM::Logic
