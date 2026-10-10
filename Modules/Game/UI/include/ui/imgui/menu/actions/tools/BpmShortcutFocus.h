#pragma once

#include "ui/imgui/WindowIdUtils.h"
#include "ui/imgui/menu/actions/tools/BpmPlaybackRouting.h"

#include <imgui_internal.h>

namespace MMM::UI
{

/// @brief 判断 BPM 测量工具根窗口或其子窗口是否拥有键盘焦点。
/// @param context 当前 ImGui 上下文，可为空。
/// @return 当前导航焦点属于 BPM 工具时返回 true。
/// @warning UI 热路径：仅沿焦点窗口父链检查稳定 ID，不访问文件或复制所有权。
/// @note 主菜单动作共用此判定，使空格与历史快捷键遵循相同焦点边界。
/// @note 没有导航窗口时保留编辑器现有快捷键行为。
/// @note 不缓存窗口指针，窗口关闭后的下一帧不会沿用旧焦点。
inline bool isBpmMeasurementToolFocused(const ImGuiContext* context)
{
    // 子窗口拥有焦点时，父链仍保留工具根窗口身份。
    const ImGuiWindow* window = context ? context->NavWindow : nullptr;
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

}  // namespace MMM::UI
