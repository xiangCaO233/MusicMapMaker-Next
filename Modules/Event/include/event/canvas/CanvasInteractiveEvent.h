#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"
#include <string>

namespace MMM::Event
{

struct CanvasInteractiveEvent : public BaseEvent {
    ///@brief 画布名称
    std::string canvasName;
};

}  // namespace MMM::Event

// 注册parent关系
EVENT_REGISTER_PARENTS(CanvasInteractiveEvent, BaseEvent);
