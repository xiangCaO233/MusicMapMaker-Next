#pragma once

#include "common/render/HoverRenderData.h"
#include <cstdint>
#include <entt/entity/entity.hpp>

namespace MMM::Common::Walkthrough
{

/// @brief 一次已结束的编辑手势，供逻辑状态和教学渲染共同消费。
/// @details 独立于整帧快照，避免会话只保存手势时依赖字体、顶点和批次定义。
/// 所有字段按值保存；实体句柄仅在所属谱面会话内有效，不持有注册表所有权。
struct EditEvent {
    /// @brief 手势来源，None 表示尚未发生可供教学验收的操作。
    enum class Kind : std::uint8_t { None, Drag, Brush };
    /// @brief 每次确认手势后递增，用于区分预览和已落地操作。
    std::uint64_t revision{ 0 };
    /// @brief 操作开始时锁定的源实体，刷写时允许为空。
    entt::entity sourceEntity{ entt::null };
    /// @brief 手势命中的物件部位，不能通过当前悬浮状态倒推。
    Render::HoverPart part{ Render::HoverPart::None };
    /// @brief 折线子项下标，未命中子项时为负。
    int subIndex{ -1 };
    /// @brief 已结束的手势类别。
    Kind kind{ Kind::None };
};

}  // namespace MMM::Common::Walkthrough
