#pragma once

#include "event/input/KeyEvent.h"

namespace MMM::Event
{
struct GLFWKeyEvent : public KeyEvent {
};
}  // namespace MMM::Event

// 注册parent关系
EVENT_REGISTER_PARENTS(GLFWKeyEvent, KeyEvent);
