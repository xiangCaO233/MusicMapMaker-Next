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
/// @details 三个布尔量共同区分稳定有项目、稳定无项目以及替换项目的过渡期，
/// 避免旧项目关闭事件导致工作区在新项目加载期间闪回欢迎页。
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
/// @note 该纯函数不访问项目对象，可直接用于事件顺序的确定性测试。
constexpr ProjectUiLifecycleState reduceProjectUiLifecycleState(
    ProjectUiLifecycleState state, ProjectUiLifecycleKind kind)
{
    switch ( kind ) {
    case ProjectUiLifecycleKind::OpenStarted:
        // 开始切换时仍保留旧项目 UI，直到后续事件给出明确终态。
        state.transitionInProgress   = true;
        state.closedDuringTransition = false;
        break;
    case ProjectUiLifecycleKind::Opened:
        // 新项目完成后切换到稳定有项目状态，并清除过渡标记。
        state.hasActiveProject       = true;
        state.transitionInProgress   = false;
        state.closedDuringTransition = false;
        break;
    case ProjectUiLifecycleKind::Closed:
        if ( state.transitionInProgress ) {
            // 替换项目中的关闭只记录旧项目已离场，不立即应用无项目布局。
            state.closedDuringTransition = true;
        } else {
            // 独立关闭操作直接进入稳定无项目状态。
            state.hasActiveProject = false;
        }
        break;
    case ProjectUiLifecycleKind::OpenFailed:
        if ( state.transitionInProgress ) {
            // 旧项目已关闭时无法恢复其 UI；否则继续保留原活动项目。
            if ( state.closedDuringTransition ) {
                state.hasActiveProject = false;
            }
            state.transitionInProgress   = false;
            state.closedDuringTransition = false;
        }
        break;
    case ProjectUiLifecycleKind::RootChanged:
        // 原地迁移只更新外部路径快照，不改变项目是否活动。
        break;
    }
    return state;
}

/// @brief 判断当前是否应应用真正的无项目工作区。
/// @param state 当前 UI 项目生命周期状态。
/// @return 没有项目且不在切换中时返回 true。
/// @note 过渡期即使暂时没有活动项目也必须维持加载工作区。
constexpr bool shouldApplyNoProjectWorkspace(
    const ProjectUiLifecycleState& state)
{
    return !state.transitionInProgress && !state.hasActiveProject;
}

}  // namespace MMM::UI
