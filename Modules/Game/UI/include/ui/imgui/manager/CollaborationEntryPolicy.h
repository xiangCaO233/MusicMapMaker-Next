#pragma once

namespace MMM::UI
{
/// @brief 判断协作入口是否满足项目打开要求。
/// @param isHost 是否准备开启房间。
/// @param hasProject 当前是否已经打开项目。
/// @return 房主已打开项目或当前为访客连接流程时返回 true。
[[nodiscard]] constexpr bool isCollaborationProjectRequirementSatisfied(
    bool isHost, bool hasProject)
{
    return !isHost || hasProject;
}
}  // namespace MMM::UI
