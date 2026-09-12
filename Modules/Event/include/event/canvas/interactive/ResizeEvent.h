#pragma once

#include "event/EventDef.h"
#include "event/canvas/CanvasInteractiveEvent.h"
#include <glm/ext/vector_float2.hpp>

namespace MMM::Event
{

/// @brief 描述画布交互区域尺寸发生变化的事件。
/// @note 新旧尺寸同时保留，供订阅者按差值更新投影或缓存。
struct CanvasResizeEvent : public CanvasInteractiveEvent {
    /// @brief 调整前的画布尺寸。
    /// @note 分量沿用画布坐标系的宽度与高度单位。
    glm::vec2 lastSize;
    /// @brief 调整后的画布尺寸。
    /// @note 订阅者应以该值作为最终状态，而不是重复累加尺寸差。
    glm::vec2 newSize;
};

}  // namespace MMM::Event

// 将尺寸事件纳入画布交互事件的多态分发链。
EVENT_REGISTER_PARENTS(CanvasResizeEvent, CanvasInteractiveEvent);
