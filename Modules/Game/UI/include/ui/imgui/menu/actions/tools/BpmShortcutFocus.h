#pragma once

#include "ui/imgui/WindowIdUtils.h"
#include "ui/imgui/menu/actions/tools/BpmPlaybackRouting.h"

#include <imgui_internal.h>

namespace MMM::UI
{

/// @brief 检查窗口是否属于 BPM 测量工具或其子布局。
/// @param window 当前事件命中的窗口，允许没有窗口。
/// @return 窗口本身或任意父级是 BPM 工具时为真。
/// @warning UI 热路径：只沿父窗口链匹配稳定 ID，不缓存窗口指针。
/// @details 控制面板、波形和频谱可能分别处在嵌套的子窗口内。
/// 拖放入口使用鼠标命中结果，快捷键入口使用当前导航窗口。
inline bool isBpmMeasurementToolWindow(const ImGuiWindow* window)
{
    while ( window ) {
        // 动态标题只取稳定 ID，避免本地化文本影响快捷键所有权。
        if ( window->Name &&
             isBpmMeasurementToolStableWindowId(
                 WindowIdUtils::stableWindowId(window->Name)) ) {
            return true;
        }
        window = window->ParentWindow;
    }
    return false;
}

/// @brief 检查键盘焦点是否位于 BPM 测量工具中。
/// @param context 来自输入处理帧的 ImGui 上下文。
/// @return 无导航目标时返回 false，由调用方保留主编辑器快捷键行为。
/// @warning UI 热路径：只检查当前帧导航窗口的父链。
inline bool isBpmMeasurementToolFocused(const ImGuiContext* context)
{
    return isBpmMeasurementToolWindow(context ? context->NavWindow : nullptr);
}

}  // namespace MMM::UI
