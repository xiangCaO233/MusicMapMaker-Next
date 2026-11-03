#pragma once

namespace MMM::UI
{
class UIManager;
/// @brief 绘制移动工作台双列入口，不拥有谱面、菜单或工具内容。
/// @param manager 只在本帧借用，所有编辑动作继续由公共菜单处理器执行。
/// @param scale 触摸尺寸倍率；主题留白和文字字号保持当前设置。
/// @warning 每帧只访问固定按钮表和本地配置；不得加锁或遍历谱面。
void drawMobileWorkbench(UIManager& manager, float scale);
}  // namespace MMM::UI
