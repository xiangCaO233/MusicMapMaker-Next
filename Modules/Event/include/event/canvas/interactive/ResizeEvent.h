#pragma once

#include "event/EventDef.h"
#include "event/canvas/CanvasInteractiveEvent.h"
#include <glm/ext/vector_float2.hpp>

namespace MMM::Event
{

struct CanvasResizeEvent : public CanvasInteractiveEvent {
    glm::vec2 lastSize;
    glm::vec2 newSize;
};

}  // namespace MMM::Event

// 注册parent关系
EVENT_REGISTER_PARENTS(CanvasResizeEvent, CanvasInteractiveEvent);