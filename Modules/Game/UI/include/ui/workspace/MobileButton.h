#pragma once
#include <imgui.h>
namespace MMM::UI
{
/// @brief 独立的触摸按钮外观：48dp 下限、小圆角和按下后亮度衰减。
/// @param selected 是否以选中底色表达当前状态，不借助悬浮提示传达状态。
/// @warning UI 热路径：动画数据保存在当前窗口的
/// ImGuiStorage，不创建任务或计时线程。
bool MobileButton(const char* label, ImVec2 size = {}, bool selected = false);
}  // namespace MMM::UI
