#pragma once

#include "imgui.h"
#include "imgui_internal.h"

namespace MMM::UI
{

/// @brief 消费播放快捷键附带的 ImGui 控件导航激活。
/// @param context 当前 ImGui 上下文，可为空。
/// @warning UI 热路径：仅在播放快捷键被消费时写入一次导航状态。
inline void consumePlaybackShortcutNavigationActivation(ImGuiContext* context)
{
    if ( !context ) return;
    context->NavActivateId        = 0;
    context->NavActivateDownId    = 0;
    context->NavActivatePressedId = 0;
    context->NavActivateFlags     = ImGuiActivateFlags_None;
}

/// @brief 判断存在活动 ImGui 控件时是否仍允许快捷键切换编辑器播放状态。
/// @param shiftPressed Shift 是否按下。
/// @param isMainCanvasMarqueeSelecting 主画布是否正在框选。
/// @param isDraggingNote 主画布是否正在拖拽物件。
/// @param isDrawingBrush 主画布是否正在使用画笔绘制。
/// @param isTimelineTimingDragging 时间线是否正在通过抓取工具拖动 Timing。
/// @param isTimelineMarqueeSelecting 时间线是否正在框选 Timing。
/// @return 当前活动交互允许快捷键切换播放时返回 true。
constexpr bool shouldAllowPlaybackToggleWhileItemActive(
    bool shiftPressed, bool isMainCanvasMarqueeSelecting, bool isDraggingNote,
    bool isDrawingBrush, bool isTimelineTimingDragging,
    bool isTimelineMarqueeSelecting)
{
    return (!shiftPressed &&
            (isMainCanvasMarqueeSelecting || isDraggingNote ||
             isTimelineTimingDragging || isTimelineMarqueeSelecting)) ||
           (shiftPressed && isDrawingBrush);
}

}  // namespace MMM::UI
