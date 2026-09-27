#pragma once

#include "event/input/KeyEvent.h"

namespace MMM::Event
{
/// @brief 标记由 GLFW 原生回调翻译得到的键盘事件。
/// @note 按键、动作和修饰键载荷全部继承自 KeyEvent。
struct GLFWKeyEvent : public KeyEvent {};
}  // namespace MMM::Event

// 保留来源类型的同时允许 KeyEvent 订阅者统一处理键盘输入。
EVENT_REGISTER_PARENTS(GLFWKeyEvent, KeyEvent);
