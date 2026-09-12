#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"
#include <string>

namespace MMM::Event
{

/// @brief 标识由某个命名画布产生的交互事件。
/// @note 派生事件使用画布名称路由，避免直接持有 UI 对象或渲染资源。
struct CanvasInteractiveEvent : public BaseEvent {
    /// @brief 交互来源画布的稳定名称。
    /// @note 发布者与订阅者必须使用相同命名约定才能完成路由。
    std::string canvasName;
};

}  // namespace MMM::Event

// 注册基础事件关系，支持只关心任意事件或任意画布交互的订阅者。
EVENT_REGISTER_PARENTS(CanvasInteractiveEvent, BaseEvent);
