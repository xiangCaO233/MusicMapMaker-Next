#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"
#include <array>

namespace MMM::Event
{

/// @brief 请求图形后端更新下一帧使用的清屏颜色。
/// @note 事件仅传递颜色值，不持有渲染器或 Vulkan 资源。
struct ClearColorUpdateEvent : public BaseEvent {
    /// @brief 按 RGBA 顺序保存的清屏颜色分量。
    /// @note 数值直接来自 UI 主题颜色，并由渲染后端原样采用。
    std::array<float, 4> clear_color_value;
};

}  // namespace MMM::Event

// 允许通用事件订阅者观察清屏颜色更新。
EVENT_REGISTER_PARENTS(ClearColorUpdateEvent, BaseEvent)
