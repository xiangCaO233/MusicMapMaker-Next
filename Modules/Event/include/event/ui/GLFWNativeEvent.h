#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"

enum class NativeEventType {
    GLFW_CLOSE_WINDOW,
    GLFW_ICONFY_WINDOW,
    GLFW_TOGGLE_WINDOW_MAXIMIZE,
};

namespace MMM::Event
{

struct GLFWNativeEvent : public BaseEvent {
    NativeEventType type;
};

}  // namespace MMM::Event

EVENT_REGISTER_PARENTS(GLFWNativeEvent, BaseEvent)
