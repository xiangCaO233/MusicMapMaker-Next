#pragma once

#include "ui/utils/UIWidgetUtils.h"
#include <imgui.h>
#include <imgui_internal.h>

namespace MMM::UI::Detail
{
/// @brief 在现有弹层结束后尝试打开更新弹窗。
/// @param id 固定弹窗身份，由实际模态正文使用同一个值。
/// @param pending 更新通知还没有完成展示。
/// @details 已有弹层时让出弹窗栈，避免抢占启动授权等用户操作。
/// @warning 每帧只检查并更新 ImGui 弹窗状态，不等待或轮询网络。
inline void openDeferredUpdatePopup(const char* id, bool pending)
{
    // 已打开的更新窗口也在栈中，继续提交正文即可，无需重新打开。
    if ( !pending || ImGui::IsPopupOpen(nullptr,
                                        ImGuiPopupFlags_AnyPopupId |
                                            ImGuiPopupFlags_AnyPopupLevel) )
        return;
    ImGui::OpenPopup(id);
}

/// @brief 更新弹窗跨过首次打开帧且可见后，消费通知并播放一次提示音。
/// @param pending 尚未确认展示的通知，由调用方在新检查结果到达时置位。
/// @return 本帧确认展示时返回真。
/// @pre 当前窗口处于成功的 BeginPopupModal 和 EndPopup 之间。
/// @warning 每帧只读当前窗口标志，展示确认时仅触发已预加载的提示音。
inline bool confirmDeferredUpdatePopup(bool& pending)
{
    // 欢迎页等视图可能在首帧稍后取得焦点并移除弹窗；保留事件供下一帧重试。
    // ImGui 允许在隐藏的测量帧提交正文，因此还需要确认窗口实际可绘制。
    if ( !pending || ImGui::IsWindowAppearing() ||
         ImGui::GetCurrentWindowRead()->Hidden )
        return false;

    // 用户之后主动关闭时无需重开，后台的持续终态也不会再次播放提示音。
    pending = false;
    PlayPopupOpenFeedback();
    return true;
}
}  // namespace MMM::UI::Detail
