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

/// @brief 将当前或待恢复工具约束到软件模式允许的交互集合。
/// @param tool 用户请求或项目工作区保存的工具。
/// @param professionalMode 当前软件级专业编辑开关。
/// @return 配色工具在普通模式下回退为 Move，其他工具保持原值。
/// @details 工具栏隐藏只是显示规则；命令与工作区恢复必须采用相同门禁。
/// @warning UI 与逻辑热路径只判断枚举和布尔值，不访问配置或会话锁。
inline EditTool resolveEditToolForMode(EditTool tool, bool professionalMode)
{
    // 基础工具和布局可独立使用，不能随高级工具一起被禁用。
    return !professionalMode && (tool == EditTool::ColorBrush ||
                                 tool == EditTool::ColorEraser)
               ? EditTool::Move
               : tool;
}

}  // namespace MMM::Logic
