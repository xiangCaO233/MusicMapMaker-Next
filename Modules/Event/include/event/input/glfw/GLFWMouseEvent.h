#pragma once

#include "event/input/MouseEvent.h"

namespace MMM::Event
{
/// @brief 标记由 GLFW 回调翻译得到的鼠标按钮事件。
/// @note 按钮、动作与坐标载荷继承自 MouseButtonEvent。
struct GLFWMouseButtonEvent : public MouseButtonEvent {};

/// @brief 标记由 GLFW 回调翻译得到的鼠标移动事件。
/// @note 当前位置与移动增量载荷继承自 MouseMoveEvent。
struct GLFWMouseMoveEvent : public MouseMoveEvent {};

/// @brief 标记由 GLFW 回调翻译得到的鼠标滚轮事件。
/// @note 滚动偏移与指针位置载荷继承自 MouseScrollEvent。
struct GLFWMouseScrollEvent : public MouseScrollEvent {};

}  // namespace MMM::Event

// 注册来源事件与通用鼠标事件之间的分发关系。
EVENT_REGISTER_PARENTS(GLFWMouseButtonEvent, MouseButtonEvent);
EVENT_REGISTER_PARENTS(GLFWMouseMoveEvent, MouseMoveEvent);
EVENT_REGISTER_PARENTS(GLFWMouseScrollEvent, MouseScrollEvent);
