#pragma once

#include "InputEvent.h"
#include <glm/ext/vector_float2.hpp>

namespace MMM::Event
{

/// @brief 鼠标按钮按下或释放事件。
struct MouseButtonEvent : public InputEvent {
    /// @brief 发生动作的鼠标按钮。
    Input::MouseButton button{ Input::MouseButton::Left };
    /// @brief 按钮当前执行的输入动作。
    Input::Action action{ Input::Action::Release };

    /// @brief 触发按钮动作时相对于当前输入区域的鼠标坐标。
    glm::vec2 pos{ 0.0f, 0.0f };
};

/// @brief 鼠标指针位置变化事件。
struct MouseMoveEvent : public InputEvent {
    /// @brief 指针在当前输入区域中的最新坐标。
    glm::vec2 pos{ 0.0f, 0.0f };

    /// @brief 相对上一次采样位置的移动增量。
    glm::vec2 delta{ 0.0f, 0.0f };
};

/// @brief 鼠标滚轮或触控板滚动事件。
struct MouseScrollEvent : public InputEvent {
    /// @brief 滚动偏移量，其中 Y 为纵向、X 为横向滚动。
    glm::vec2 offset{ 0.0f, 0.0f };

    /// @brief 触发滚动时指针在当前输入区域中的坐标。
    glm::vec2 pos{ 0.0f, 0.0f };
};

}  // namespace MMM::Event

// 注册通用输入继承关系，支持按设备无关接口订阅鼠标事件。
EVENT_REGISTER_PARENTS(MouseButtonEvent, InputEvent);
EVENT_REGISTER_PARENTS(MouseMoveEvent, InputEvent);
EVENT_REGISTER_PARENTS(MouseScrollEvent, InputEvent);
