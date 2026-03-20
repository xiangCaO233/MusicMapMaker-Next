#pragma once

#include "event/input/MouseEvent.h"

namespace MMM::Event
{
// 对于按键
struct GLFWMouseButtonEvent : public MouseButtonEvent {
};

// 对于移动
struct GLFWMouseMoveEvent : public MouseMoveEvent {
};

// 对于滚动
struct GLFWMouseScrollEvent : public MouseScrollEvent {
};

}  // namespace MMM::Event

// 注册 parent 关系
EVENT_REGISTER_PARENTS(GLFWMouseButtonEvent, MouseButtonEvent);
EVENT_REGISTER_PARENTS(GLFWMouseMoveEvent, MouseMoveEvent);
EVENT_REGISTER_PARENTS(GLFWMouseScrollEvent, MouseScrollEvent);
