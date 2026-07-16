#pragma once

namespace MMM::UI
{

/// @brief 判断存在活动 ImGui 控件时是否仍允许空格切换编辑器播放状态。
/// @param shiftPressed Shift 是否按下。
/// @param isMainCanvasMarqueeSelecting 主画布是否正在框选。
/// @param isDraggingNote 主画布是否正在拖拽物件。
/// @param isDrawingBrush 主画布是否正在使用画笔绘制。
/// @param isTimelineTimingDragging 时间线是否正在通过抓取工具拖动 Timing。
/// @param isTimelineMarqueeSelecting 时间线是否正在框选 Timing。
/// @return 当前活动交互允许空格切换播放时返回 true。
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
