#pragma once

#include "event/input/imgui/ImGuiMouseEvent.h"
#include "event/ui/UIEvent.h"

namespace MMM::Event
{
/// @brief 当前 UI 窗口接收到的鼠标按钮事件。
/// @note 同时携带 UI 来源信息与 ImGui 鼠标按钮载荷。
struct UIWindowMouseButtonEvent : public UIEvent,
                                  public ImGuiMouseButtonEvent {};

/// @brief 当前 UI 窗口接收到的鼠标移动事件。
/// @note 同时携带 UI 来源信息与 ImGui 鼠标移动载荷。
struct UIWindowMouseMoveEvent : public UIEvent, public ImGuiMouseMoveEvent {};

/// @brief 当前 UI 窗口接收到的鼠标滚动事件。
/// @note 同时携带 UI 来源信息与 ImGui 鼠标滚动载荷。
struct UIWindowMouseScrollEvent : public UIEvent,
                                  public ImGuiMouseScrollEvent {};

}  // namespace MMM::Event

// 为每类窗口鼠标事件注册 UI 与 ImGui 输入两条分发链。
EVENT_REGISTER_PARENTS(UIWindowMouseButtonEvent, UIEvent,
                       ImGuiMouseButtonEvent);
EVENT_REGISTER_PARENTS(UIWindowMouseMoveEvent, UIEvent, ImGuiMouseMoveEvent);
EVENT_REGISTER_PARENTS(UIWindowMouseScrollEvent, UIEvent,
                       ImGuiMouseScrollEvent);
