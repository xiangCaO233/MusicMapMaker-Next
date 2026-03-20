#pragma once

#include "event/input/MouseEvent.h"

namespace MMM::Event
{
// 对于按键
struct ImGuiMouseButtonEvent : public MouseButtonEvent {
};

// 对于移动
struct ImGuiMouseMoveEvent : public MouseMoveEvent {
};

// 对于滚动
struct ImGuiMouseScrollEvent : public MouseScrollEvent {
};

}  // namespace MMM::Event

// 注册 parent 关系
EVENT_REGISTER_PARENTS(ImGuiMouseButtonEvent, MouseButtonEvent);
EVENT_REGISTER_PARENTS(ImGuiMouseMoveEvent, MouseMoveEvent);
EVENT_REGISTER_PARENTS(ImGuiMouseScrollEvent, MouseScrollEvent);
