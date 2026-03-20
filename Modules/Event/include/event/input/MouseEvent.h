#pragma once

#include "InputEvent.h"
#include <glm/ext/vector_float2.hpp>

namespace MMM::Event
{

///@brief 鼠标按钮事件 (点击/松开)
struct MouseButtonEvent : public InputEvent {
    Input::MouseButton button{ Input::MouseButton::Left };
    Input::Action      action{ Input::Action::Release };

    ///@brief 触发点击时的鼠标坐标 (通常相对于窗口或画布)
    glm::vec2 pos{ 0.0f, 0.0f };
};

///@brief 鼠标移动事件
struct MouseMoveEvent : public InputEvent {
    ///@brief 当前坐标
    glm::vec2 pos{ 0.0f, 0.0f };

    ///@brief 相对上一帧的增量 (对于自由视角摄像机控制非常重要)
    glm::vec2 delta{ 0.0f, 0.0f };
};

///@brief 鼠标滚轮事件
struct MouseScrollEvent : public InputEvent {
    ///@brief 滚轮偏移量 (通常 Y 是上下滚动，X 是左右触控板滑动)
    glm::vec2 offset{ 0.0f, 0.0f };

    ///@brief 触发滚动时的鼠标坐标
    glm::vec2 pos{ 0.0f, 0.0f };
};

}  // namespace MMM::Event

// 注册 parent 关系
EVENT_REGISTER_PARENTS(MouseButtonEvent, InputEvent);
EVENT_REGISTER_PARENTS(MouseMoveEvent, InputEvent);
EVENT_REGISTER_PARENTS(MouseScrollEvent, InputEvent);
