#pragma once

#include "MMMInput.h"
#include "event/EventDef.h"
#include "event/core/BaseEvent.h"

namespace MMM::Event
{

struct InputEvent : public BaseEvent {
    ///@brief 修饰键位掩码
    Input::Modifiers mods;

    ///@brief 动作类型
    ///@note 虽然鼠标移动没有 action，但按键和点击都有，放在基类可以统一判断
    Input::Action action;
};

}  // namespace MMM::Event

// 注册parent关系
EVENT_REGISTER_PARENTS(InputEvent, BaseEvent);
