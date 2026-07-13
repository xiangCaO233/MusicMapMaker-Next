#pragma once

namespace MMM::UI
{

/// @brief UI 线程需要消费的项目生命周期信号。
enum class ProjectUiLifecycleKind {
    /// @brief 已开始关闭旧项目并加载新项目。
    OpenStarted,

    /// @brief 新项目已经完成加载。
    Opened,

    /// @brief 当前项目已经关闭。
    Closed,

    /// @brief 新项目打开失败。
    OpenFailed,

    /// @brief 当前项目原地迁移到了新的正式根目录。
    RootChanged
};

/// @brief UI 使用的项目生命周期状态，不持有逻辑线程项目实例。
struct ProjectUiLifecycleState {
    /// @brief 当前是否仍有可展示的项目 UI 快照。
    bool hasActiveProject{ false };

    /// @brief 当前是否正在关闭旧项目并加载新项目。
    bool transitionInProgress{ false };

    /// @brief 本次切换期间是否已经关闭旧项目。
    bool closedDuringTransition{ false };
};

/// @brief 将一个项目生命周期信号归约到 UI 本地状态。
/// @param state 信号到来前的状态。
/// @param kind 本次生命周期信号。
/// @return 应用信号后的新状态。
constexpr ProjectUiLifecycleState reduceProjectUiLifecycleState(
    ProjectUiLifecycleState state, ProjectUiLifecycleKind kind)
{
    switch ( kind ) {
    case ProjectUiLifecycleKind::OpenStarted:
        state.transitionInProgress   = true;
        state.closedDuringTransition = false;
        break;
    case ProjectUiLifecycleKind::Opened:
        state.hasActiveProject       = true;
        state.transitionInProgress   = false;
        state.closedDuringTransition = false;
        break;
    case ProjectUiLifecycleKind::Closed:
        if ( state.transitionInProgress ) {
            state.closedDuringTransition = true;
        } else {
            state.hasActiveProject = false;
        }
        break;
    case ProjectUiLifecycleKind::OpenFailed:
        if ( state.transitionInProgress ) {
            if ( state.closedDuringTransition ) {
                state.hasActiveProject = false;
            }
            state.transitionInProgress   = false;
            state.closedDuringTransition = false;
        }
        break;
    case ProjectUiLifecycleKind::RootChanged: break;
    }
    return state;
}

/// @brief 判断当前是否应应用真正的无项目工作区。
/// @param state 当前 UI 项目生命周期状态。
/// @return 没有项目且不在切换中时返回 true。
constexpr bool shouldApplyNoProjectWorkspace(
    const ProjectUiLifecycleState& state)
{
    return !state.transitionInProgress && !state.hasActiveProject;
}

}  // namespace MMM::UI
