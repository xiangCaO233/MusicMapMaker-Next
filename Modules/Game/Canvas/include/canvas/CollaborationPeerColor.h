#pragma once

#include "imgui.h"

#include <array>
#include <cstdint>

namespace MMM::Canvas
{

/// @brief 根据协作客户端标识选择稳定且醒目的覆盖层颜色。
/// @param peerId 远端参与者标识。
/// @param alpha 输出透明度。
/// @return ImGui 使用的 RGBA 颜色。
/// @warning UI 热路径纯计算：每个协作者每帧调用，不得引入分配或阻塞操作。
[[nodiscard]] inline ImU32 collaborationPeerColor(std::uint64_t peerId,
                                                  std::uint8_t  alpha)
{
    constexpr std::array<ImVec4, 7> COLORS{
        ImVec4{ 0.20F, 0.72F, 1.00F, 1.00F },
        ImVec4{ 1.00F, 0.38F, 0.42F, 1.00F },
        ImVec4{ 0.42F, 0.90F, 0.45F, 1.00F },
        ImVec4{ 1.00F, 0.72F, 0.22F, 1.00F },
        ImVec4{ 0.75F, 0.46F, 1.00F, 1.00F },
        ImVec4{ 0.16F, 0.88F, 0.78F, 1.00F },
        ImVec4{ 1.00F, 0.46F, 0.82F, 1.00F },
    };
    ImVec4 color = COLORS[peerId % COLORS.size()];
    color.w      = static_cast<float>(alpha) / 255.0F;
    return ImGui::ColorConvertFloat4ToU32(color);
}

}  // namespace MMM::Canvas
