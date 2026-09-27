#pragma once

namespace MMM::Logic
{

/// @brief 编辑工具类型。
/// @note 枚举描述当前交互策略，不直接拥有选择集或画布状态。
/// @note 切换工具时由会话层负责结束旧工具的临时手势。
enum class EditTool {
    // Move 与 Marquee 处理已有物件的直接操作和区域选择。
    Move,     ///< 移动工具。
    Marquee,  ///< 矩形选取工具。
    // Draw、ColorBrush 与 ColorEraser产生可提交的内容变更。
    Draw,         ///< 绘制工具。
    ColorBrush,   ///< 配色笔刷工具。
    ColorEraser,  ///< 配色橡皮工具。
    // Layout 只调整画布组件布局，不改变谱面物件内容。
    Layout,  ///< 画布布局调整工具。
};

}  // namespace MMM::Logic
