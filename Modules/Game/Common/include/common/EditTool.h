#pragma once

namespace MMM::Logic
{

/// @brief 编辑工具类型。
enum class EditTool {
    Move,         ///< 移动工具。
    Marquee,      ///< 矩形选取工具。
    Draw,         ///< 绘制工具。
    ColorBrush,   ///< 配色笔刷工具。
    ColorEraser,  ///< 配色橡皮工具。
    Layout,       ///< 画布布局调整工具。
};

}  // namespace MMM::Logic
