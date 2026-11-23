#pragma once

namespace MMM::Common
{

/// @brief 用于全局快捷键路由的活动会话手势状态。
/// @details 三个标志来自同一次发布；调用方不借用可变会话或持有会话锁。
struct EditorGestureState {
    /// @brief 框选工具已开始拖动选择区域。
    bool m_selectingMarquee{ false };
    /// @brief 框选工具正在拖动有效谱面物件。
    bool m_draggingNote{ false };
    /// @brief 绘图工具的画笔会话处于活动状态。
    bool m_drawingBrush{ false };
};

}  // namespace MMM::Common
