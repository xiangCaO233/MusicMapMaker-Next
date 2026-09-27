#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"

namespace MMM::Event
{

/// @brief 原生窗口命令与状态通知的类别。
/// @note 同一枚举同时覆盖 UI 发出的命令和 GLFW 回调产生的通知。
enum class NativeEventType {
    GLFW_CLOSE_WINDOW,
    GLFW_ICONFY_WINDOW,
    GLFW_TOGGLE_WINDOW_MAXIMIZE,
    GLFW_WINDOW_RESIZED,
    GLFW_WINDOW_CONTENT_SCALE_CHANGED,
    GLFW_WINDOW_FOCUS_CHANGED,
};

/// @brief 在 UI 与 GLFW 窗口后端之间传递原生窗口操作。
/// @note hasStateChange 用于区分状态通知与待执行命令，防止通知反向执行。
struct GLFWNativeEvent : public BaseEvent {
    /// @brief 本次窗口操作或状态变化的类别。
    NativeEventType type;

    /// @brief 是否为 GLFW 回调产生的状态通知。
    bool hasStateChange = false;
    /// @brief 最大化状态通知中窗口当前是否最大化。
    bool isMaximized = false;
    /// @brief 焦点状态事件中原生窗口当前是否拥有输入焦点。
    bool isFocused = true;
};

}  // namespace MMM::Event

// 注册基础事件关系，供窗口生命周期的通用监听器接收通知。
EVENT_REGISTER_PARENTS(GLFWNativeEvent, BaseEvent)
