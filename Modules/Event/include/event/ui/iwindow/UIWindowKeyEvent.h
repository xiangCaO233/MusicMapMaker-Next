#pragma once

#include "event/input/imgui/ImGuiKeyEvent.h"
#include "event/ui/UIEvent.h"

namespace MMM
{
namespace Event
{
/// @brief 表示当前 UI 窗口接收到的 ImGui 键盘按下事件。
/// @note 多重继承同时保留 UI 路由信息和规范化键盘输入载荷。
struct UIWindowKeyPressEvent : public UIEvent, public ImGuiKeyEvent {};
}  // namespace Event
}  // namespace MMM

// 同时注册到 UI 与 ImGui 键盘事件分支。
// 订阅者可按关注层级接收消息，无需识别具体窗口事件类型。
// 事件本身不决定快捷键是否消费，该策略由窗口订阅者实现。
EVENT_REGISTER_PARENTS(UIWindowKeyPressEvent, UIEvent, ImGuiKeyEvent);
