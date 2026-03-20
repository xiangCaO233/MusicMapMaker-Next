#pragma once

#include "event/input/imgui/ImGuiKeyEvent.h"
#include "event/ui/UIEvent.h"

namespace MMM
{
namespace Event
{
struct UIWindowKeyPressEvent : public UIEvent, public ImGuiKeyEvent {
};
}  // namespace Event
}  // namespace MMM

// 注册parent关系
EVENT_REGISTER_PARENTS(UIWindowKeyPressEvent, UIEvent, ImGuiKeyEvent);
