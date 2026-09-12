#pragma once

#include "MMMInput.h"
#include "event/EventDef.h"
#include "event/core/BaseEvent.h"

namespace MMM::Event
{

/// @brief 输入设备事件共享的修饰键与动作载荷。
/// @note 具体设备事件通过继承补充按键、鼠标或文本输入数据。
struct InputEvent : public BaseEvent {
    /// @brief 事件产生时处于按下状态的修饰键位掩码。
    Input::Modifiers mods;

    /// @brief 按下、释放或重复等输入动作类型。
    /// @note 鼠标移动没有固有动作，但保留在基类可统一处理离散输入事件。
    Input::Action action;
};

}  // namespace MMM::Event

// 注册继承关系，使按具体事件发布的消息也能由基础输入订阅者接收。
EVENT_REGISTER_PARENTS(InputEvent, BaseEvent);
