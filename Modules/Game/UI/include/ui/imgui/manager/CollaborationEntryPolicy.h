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

/// @brief 判断访客入房前是否必须先关闭本机项目和谱面画布。
/// @param hasProject 当前是否打开本机项目。
/// @param hasBeatmapCanvas 当前是否存在非欢迎页谱面画布。
/// @return 任一本机编辑状态存在时返回 true。
[[nodiscard]] constexpr bool needsLocalStateCloseBeforeGuestJoin(
    bool hasProject, bool hasBeatmapCanvas)
{
    return hasProject || hasBeatmapCanvas;
}

/// @brief 判断协作建立时是否允许绑定当前活动谱面会话。
/// @param isHost 当前客户端是否为房主。
/// @return 只有房主可绑定已打开的本机会话；访客必须等待远端快照创建新会话。
[[nodiscard]] constexpr bool mayBindExistingSessionForCollaboration(bool isHost)
{
    return isHost;
}
}  // namespace MMM::UI
