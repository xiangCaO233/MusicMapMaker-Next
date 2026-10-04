#pragma once

#include "imgui.h"
#include "imgui_internal.h"

namespace MMM::Canvas
{
/// @brief 判断谱面画布的原始输入是否被模态窗口独占。
/// @return 存在打开的模态窗口时返回真，与鼠标是否位于弹窗内无关。
/// @details 仅用于底层画布入口，不能用于模态窗口自身的控件。
/// ImGui 的按钮命中会自动遵守模态规则，原始鼠标和键盘查询则不会。
/// @warning UI 热路径：每帧读取弹窗栈，不分配资源、不访问会话或等待锁。
[[nodiscard]] inline bool isCanvasInputBlockedByModal()
{
    // 弹窗范围之外同样必须阻挡，不能只用当前鼠标几何或焦点判断。
    return ImGui::GetTopMostPopupModal() != nullptr;
}

/// @brief 查询画布及其普通子窗口的鼠标归属。
/// @return 没有模态阻挡且鼠标属于画布窗口时返回真。
/// @details 排除弹窗父子关系，防止把画布打开的弹窗当作画布子控件。
/// @warning UI 热路径：只查询当前 ImGui 窗口状态。
[[nodiscard]] inline bool isCanvasWindowHovered()
{
    // 活动控件回退只服务于画布拖动，不能绕过弹窗层级。
    return !isCanvasInputBlockedByModal() &&
           ImGui::IsWindowHovered(
               ImGuiHoveredFlags_RootAndChildWindows |
               ImGuiHoveredFlags_NoPopupHierarchy |
               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
}

/// @brief 查询画布及其普通子窗口的键盘焦点。
/// @return 没有模态阻挡且画布拥有键盘焦点时返回真。
/// @details 弹窗获得焦点不能触发底层画布抢焦点或编辑快捷键。
/// @warning UI 热路径：只查询当前 ImGui 窗口状态。
[[nodiscard]] inline bool isCanvasWindowFocused()
{
    return !isCanvasInputBlockedByModal() &&
           ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows |
                                  ImGuiFocusedFlags_NoPopupHierarchy);
}
}  // namespace MMM::Canvas
