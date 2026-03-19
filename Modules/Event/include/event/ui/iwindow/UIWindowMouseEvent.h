#pragma once

#include "event/input/imgui/ImGuiMouseEvent.h"
#include "event/ui/UIEvent.h"

namespace MMM::Event
{
// 对于按键
struct UIWindowMouseButtonEvent : public UIEvent, public ImGuiMouseButtonEvent {
};

// 对于移动
struct UIWindowMouseMoveEvent : public UIEvent, public ImGuiMouseMoveEvent {
};

// 对于滚动
struct UIWindowMouseScrollEvent : public UIEvent, public ImGuiMouseScrollEvent {
};

}  // namespace MMM::Event

// 注册 parent 关系
EVENT_REGISTER_PARENTS(UIWindowMouseButtonEvent, UIEvent,
                       ImGuiMouseButtonEvent);
EVENT_REGISTER_PARENTS(UIWindowMouseMoveEvent, UIEvent, ImGuiMouseMoveEvent);
EVENT_REGISTER_PARENTS(UIWindowMouseScrollEvent, UIEvent,
                       ImGuiMouseScrollEvent);
