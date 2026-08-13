#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"

namespace MMM::Event
{

enum class NativeEventType {
    GLFW_CLOSE_WINDOW,
    GLFW_ICONFY_WINDOW,
    GLFW_TOGGLE_WINDOW_MAXIMIZE,
    GLFW_WINDOW_RESIZED,
    GLFW_WINDOW_CONTENT_SCALE_CHANGED,
    GLFW_WINDOW_FOCUS_CHANGED,
};

struct GLFWNativeEvent : public BaseEvent {
    NativeEventType type;

    // 状态数据 (主要用于系统触发的回调同步)
    bool hasStateChange = false;
    bool isMaximized    = false;
    /// @brief 焦点状态事件中原生窗口当前是否拥有输入焦点。
    bool isFocused = true;
};

}  // namespace MMM::Event

EVENT_REGISTER_PARENTS(GLFWNativeEvent, BaseEvent)
