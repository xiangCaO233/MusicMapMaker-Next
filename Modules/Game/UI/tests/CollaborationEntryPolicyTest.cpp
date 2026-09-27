#include "ui/imgui/manager/CollaborationEntryPolicy.h"

#include "log/colorful-log.h"

/// @file CollaborationEntryPolicyTest.cpp
/// @brief 协作入口的项目门槛、本地状态隔离和断线只读策略回归测试。
/// @details 测试枚举全部布尔输入组合，确保房主与访客两种角色不会共用错误的
/// 项目要求或离线编辑权限。

namespace
{
/// @brief 验证只有房主开启房间需要已打开项目。
/// @return 房主和访客的项目门槛符合协作流程时返回 true。
/// @note 访客无项目是合法场景，因为项目将由房间快照提供。
[[nodiscard]] bool testProjectRequirement()
{
    using MMM::UI::isCollaborationProjectRequirementSatisfied;

    // 房主需要用当前项目创建房间，因此无项目时必须拒绝。
    const bool hostWithoutProject =
        isCollaborationProjectRequirementSatisfied(true, false);
    // 房主有项目时允许进入创建流程。
    const bool hostWithProject =
        isCollaborationProjectRequirementSatisfied(true, true);
    // 访客将从房间同步项目，加入前不要求本地项目。
    const bool guestWithoutProject =
        isCollaborationProjectRequirementSatisfied(false, false);
    if ( hostWithoutProject || !hostWithProject || !guestWithoutProject ) {
        // 聚合失败便于 CTest 日志直接定位到项目门槛策略。
        XERROR("Collaboration project requirement policy was incorrect");
        return false;
    }
    return true;
}

/// @brief 验证访客必须先清空本机状态且不能复用既有活动会话。
/// @return 数据隔离策略符合预期时返回 true。
/// @note 两个输入分别代表本地项目与本地编辑会话是否存在。
[[nodiscard]] bool testGuestSessionIsolation()
{
    using MMM::UI::mayBindExistingSessionForCollaboration;
    using MMM::UI::needsLocalStateCloseBeforeGuestJoin;

    // 本地项目和已有会话任一存在，访客加入前都必须清理本地状态。
    if ( needsLocalStateCloseBeforeGuestJoin(false, false) ||
         !needsLocalStateCloseBeforeGuestJoin(true, false) ||
         !needsLocalStateCloseBeforeGuestJoin(false, true) ||
         !needsLocalStateCloseBeforeGuestJoin(true, true) ||
         mayBindExistingSessionForCollaboration(false) ||
         !mayBindExistingSessionForCollaboration(true) ) {
        // 只有房主允许把现有编辑会话直接绑定到协作房间。
        XERROR("Collaboration guest session isolation policy was incorrect");
        return false;
    }
    return true;
}

/// @brief 验证会话从访客切换为房主时一定解除遗留的离线只读状态。
/// @return 只有断线访客只读且房主始终可编辑时返回 true。
/// @note 四种角色和连接组合全部显式计算，避免遗漏切换边界。
[[nodiscard]] bool testSessionReadOnlyPolicy()
{
    using MMM::UI::shouldCollaborationSessionBeReadOnly;

    // 断线访客缺少同步权威，必须进入只读状态。
    const bool disconnectedGuest =
        shouldCollaborationSessionBeReadOnly(true, false);
    // 在线访客可按服务端权限继续编辑。
    const bool connectedGuest =
        shouldCollaborationSessionBeReadOnly(true, true);
    // 房主无论连接状态都不继承访客的离线只读标记。
    const bool disconnectedHost =
        shouldCollaborationSessionBeReadOnly(false, false);
    const bool connectedHost =
        shouldCollaborationSessionBeReadOnly(false, true);
    if ( !disconnectedGuest || connectedGuest || disconnectedHost ||
         connectedHost ) {
        // 聚合四种组合可防止角色切换后残留旧状态。
        XERROR("Collaboration session read-only policy was incorrect");
        return false;
    }
    return true;
}
}  // namespace

/// @brief 运行协作入口项目要求回归测试。
/// @return 全部断言通过时返回 0。
/// @note 返回单一退出码，具体失败策略由各场景日志标识。
int main()
{
    return testProjectRequirement() && testGuestSessionIsolation() &&
                   testSessionReadOnlyPolicy()
               ? 0
               : 1;
}
