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
}  // namespace

/// @brief 运行协作入口项目要求回归测试。
/// @return 全部断言通过时返回 0。
int main()
{
    return testProjectRequirement() ? 0 : 1;
}
