#pragma once
#include "common/render/NoteRenderData.h"
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <vector>

namespace MMM::Logic
{
/// @brief 一笔显式根物件配色，完整值载荷独立于触控和桌面 UI。
/// @details 起笔锁定颜色；空列表绝不借用当前选择或悬浮。
/// 投影子项由逻辑端提升到根，采样不属于此命令领域。
struct NotePaletteStroke {
    /// @brief 原谱面实例身份，切谱后的同号句柄不能修改新谱面。
    std::uintptr_t m_instance{};
    /// @brief 带代数的音符句柄；仅释放时转移，不保存快照引用。
    std::vector<entt::entity> m_targets;
    /// @brief 起笔时锁定的画笔覆盖，后续改调色盘不污染已排队笔画。
    Common::Render::NoteColorOverrides m_colors;
    /// @brief 为真仅清配色，不删除物件、批注或采样绑定。
    bool m_clear{};
};
}  // namespace MMM::Logic
