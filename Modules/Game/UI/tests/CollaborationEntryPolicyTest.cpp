#include "ui/imgui/manager/CollaborationEntryPolicy.h"

#include "log/colorful-log.h"

namespace
{
/// @brief 验证只有房主开启房间需要已打开项目。
/// @return 房主和访客的项目门槛符合协作流程时返回 true。
[[nodiscard]] bool testProjectRequirement()
{
    using MMM::UI::isCollaborationProjectRequirementSatisfied;

    const bool hostWithoutProject =
        isCollaborationProjectRequirementSatisfied(true, false);
    const bool hostWithProject =
        isCollaborationProjectRequirementSatisfied(true, true);
    const bool guestWithoutProject =
        isCollaborationProjectRequirementSatisfied(false, false);
    if ( hostWithoutProject || !hostWithProject || !guestWithoutProject ) {
        XERROR("Collaboration project requirement policy was incorrect");
        return false;
    }
    return true;
}

/// @brief 验证访客必须先清空本机状态且不能复用既有活动会话。
/// @return 数据隔离策略符合预期时返回 true。
[[nodiscard]] bool testGuestSessionIsolation()
{
    using MMM::UI::mayBindExistingSessionForCollaboration;
    using MMM::UI::needsLocalStateCloseBeforeGuestJoin;

    if ( needsLocalStateCloseBeforeGuestJoin(false, false) ||
         !needsLocalStateCloseBeforeGuestJoin(true, false) ||
         !needsLocalStateCloseBeforeGuestJoin(false, true) ||
         !needsLocalStateCloseBeforeGuestJoin(true, true) ||
         mayBindExistingSessionForCollaboration(false) ||
         !mayBindExistingSessionForCollaboration(true) ) {
        XERROR("Collaboration guest session isolation policy was incorrect");
        return false;
    }
    return true;
}

/// @brief 验证会话从访客切换为房主时一定解除遗留的离线只读状态。
/// @return 只有断线访客只读且房主始终可编辑时返回 true。
[[nodiscard]] bool testSessionReadOnlyPolicy()
{
    using MMM::UI::shouldCollaborationSessionBeReadOnly;

    const bool disconnectedGuest =
        shouldCollaborationSessionBeReadOnly(true, false);
    const bool connectedGuest =
        shouldCollaborationSessionBeReadOnly(true, true);
    const bool disconnectedHost =
        shouldCollaborationSessionBeReadOnly(false, false);
    const bool connectedHost =
        shouldCollaborationSessionBeReadOnly(false, true);
    if ( !disconnectedGuest || connectedGuest || disconnectedHost ||
         connectedHost ) {
        XERROR("Collaboration session read-only policy was incorrect");
        return false;
    }
    return true;
}
}  // namespace

/// @brief 运行协作入口项目要求回归测试。
/// @return 全部断言通过时返回 0。
int main()
{
    return testProjectRequirement() && testGuestSessionIsolation() &&
                   testSessionReadOnlyPolicy()
               ? 0
               : 1;
}
