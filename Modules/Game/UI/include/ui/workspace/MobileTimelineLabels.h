#pragma once
#include <imgui.h>
namespace MMM::UI
{
/// @brief 移动专业时间线的五泳道标签，无鼠标悬浮依赖。
/// @details 始终水平显示，窄栏沿用往复滚动节奏，区域高度随字号和主题留白计算。
/// @warning 每帧只测量五个短字符串，不分配资源或访问逻辑线程。
void drawMobileTimelineLaneLabels(ImVec2 origin, ImVec2 size);
}  // namespace MMM::UI
