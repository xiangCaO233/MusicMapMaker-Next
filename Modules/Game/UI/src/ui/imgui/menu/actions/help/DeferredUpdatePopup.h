#pragma once

#include "ui/utils/UIWidgetUtils.h"
#include <imgui.h>

namespace MMM::UI::Detail
{
/// @brief 在现有弹层结束后消费一次更新通知请求。
/// @param id 固定弹窗身份，由实际模态正文使用同一个值。
/// @param pending 尚未展示的事件；忙碌时保留，成功请求后立即清除。
/// @return 本帧确实发出一次打开请求时返回真。
/// @details 启动 PGO 同意弹窗与更新通知共享宿主，不能在同一层互相抢占。
/// 后台“发现更新”是持续状态，只有完成事件能设置 pending。
/// 外部关闭后不自动重开；用户主动再次检查仍可产生新事件。
/// @warning 每帧只检查 ImGui 状态并修改布尔值，不等待或轮询网络。
inline bool openDeferredUpdatePopup(const char* id, bool& pending)
{
    // 不抢占同层或嵌套弹层，保持请求等待下一个可展示帧。
    if ( !pending || ImGui::IsPopupOpen(nullptr,
                                        ImGuiPopupFlags_AnyPopupId |
                                            ImGuiPopupFlags_AnyPopupLevel) )
        return false;
    // 先消费再打开，窗口未能持续显示也不能把终态变成逐帧反馈音。
    pending = false;
    FeedbackOpenPopup(id);
    return true;
}
}  // namespace MMM::UI::Detail
