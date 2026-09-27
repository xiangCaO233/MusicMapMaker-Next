#pragma once

#include "imgui.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace MMM::Canvas
{

/// @brief 根据协作者稳定标识选择醒目的覆盖层颜色。
/// @param participantId 不随 PeerId 复用或重连变化的稳定标识。
/// @param alpha 输出透明度。
/// @return ImGui 使用的 RGBA 颜色。
/// @warning UI 热路径纯计算：每个协作者每帧调用，不得引入分配或阻塞操作。
///
/// 相同 participantId 在不同会话中保持同一调色板索引，空 ID 也得到确定颜色。
[[nodiscard]] inline ImU32 collaborationPeerColor(
    std::string_view participantId, std::uint8_t alpha)
{
    // 固定调色板在深浅皮肤上都保持较高饱和度，透明度由调用场景单独决定。
    constexpr std::array<ImVec4, 7> COLORS{
        // 相邻槽位刻意拉开色相，降低多人视野框同时出现时的混淆。
        ImVec4{ 0.20F, 0.72F, 1.00F, 1.00F },
        ImVec4{ 1.00F, 0.38F, 0.42F, 1.00F },
        ImVec4{ 0.42F, 0.90F, 0.45F, 1.00F },
        ImVec4{ 1.00F, 0.72F, 0.22F, 1.00F },
        ImVec4{ 0.75F, 0.46F, 1.00F, 1.00F },
        ImVec4{ 0.16F, 0.88F, 0.78F, 1.00F },
        ImVec4{ 1.00F, 0.46F, 0.82F, 1.00F },
    };
    // 使用 FNV-1a 对稳定参与者 ID 求值，使重连或临时 PeerId 变化后仍保持颜色。
    std::uint64_t stableHash = 1469598103934665603ULL;
    for ( const unsigned char character : participantId ) {
        // 按无符号字节处理 UTF-8，避免 char 符号性随编译器改变哈希结果。
        stableHash ^= character;
        stableHash *= 1099511628211ULL;
    }
    // 取模只选择离散颜色，不在热路径中生成或缓存动态调色板。
    ImVec4 color = COLORS[stableHash % COLORS.size()];
    // 调用方透明度覆盖调色板的占位 alpha，统一转换到 ImGui 的浮点范围。
    color.w = static_cast<float>(alpha) / 255.0F;
    // 最后使用 ImGui 当前约定的通道布局打包，避免手写位移依赖平台字节序。
    return ImGui::ColorConvertFloat4ToU32(color);
}

}  // namespace MMM::Canvas
