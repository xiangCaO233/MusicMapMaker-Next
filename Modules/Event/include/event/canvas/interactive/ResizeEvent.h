#pragma once

#include "event/EventDef.h"
#include "event/canvas/CanvasInteractiveEvent.h"
#include <glm/ext/vector_float2.hpp>

namespace MMM::Event
{

struct CanvasResizeEvent : public CanvasInteractiveEvent {
    ///@brief resize前尺寸
    glm::vec2 lastSize;
    ///@brief resize后尺寸
    glm::vec2 newSize;
};

}  // namespace MMM::Event

// 注册parent关系
EVENT_REGISTER_PARENTS(CanvasResizeEvent, CanvasInteractiveEvent);
