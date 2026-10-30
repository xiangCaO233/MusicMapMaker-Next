#pragma once
#include <imgui.h>
namespace MMM::UI
{
/// @brief 独立的触摸按钮外观：48dp 下限、小圆角和按下后亮度衰减。
/// @param selected 是否以选中底色表达当前状态，不借助悬浮提示传达状态。
/// @warning UI 热路径：动画数据保存在当前窗口的
/// ImGuiStorage，不创建任务或计时线程。
bool MobileButton(const char* label, ImVec2 size = {}, bool selected = false);
/// @brief 图标与常驻文字上下排列的导航按钮，不依赖悬浮解释入口。
/// @param id 稳定隐藏身份，图标与可见文字变化不会重置反馈动画。
/// @warning 每帧只测量两个短标签，不重新加载皮肤字体。
bool MobileNavigationButton(const char* id, const char* icon, const char* label,
                            ImVec2 size, bool selected = false);
/// @brief 图标与常驻文字横排的操作按钮，适用于开关、收起和确认操作。
/// @param id 独立于可见文字的稳定身份，状态文案改变时保留点击反馈。
/// @warning 每帧只测量短标签，动画与导航按钮共用窗口局部存储。
bool MobileActionButton(const char* id, const char* icon, const char* label,
                        ImVec2 size = {}, bool selected = false);
}  // namespace MMM::UI
