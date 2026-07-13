#include "ui/project/ProjectUiLifecycleState.h"

namespace
{
using MMM::UI::ProjectUiLifecycleKind;
using MMM::UI::ProjectUiLifecycleState;
using MMM::UI::reduceProjectUiLifecycleState;
using MMM::UI::shouldApplyNoProjectWorkspace;

/// @brief 连续归约多个项目生命周期信号。
/// @param state 初始状态。
/// @param kinds 按发生顺序排列的生命周期信号。
/// @return 应用全部信号后的状态。
template<typename... Kinds>
constexpr ProjectUiLifecycleState reduceAll(ProjectUiLifecycleState state,
                                            Kinds... kinds)
{
    ((state = reduceProjectUiLifecycleState(state, kinds)), ...);
    return state;
}

/// @brief 检查切换期间关闭旧项目不会进入无项目工作区。
/// @return 状态符合预期时返回 true。
constexpr bool checkClosedDuringTransitionKeepsWorkspace()
{
    const auto state =
        reduceAll(ProjectUiLifecycleState{ .hasActiveProject = true },
                  ProjectUiLifecycleKind::OpenStarted,
                  ProjectUiLifecycleKind::Closed);
    return state.hasActiveProject && state.transitionInProgress &&
           state.closedDuringTransition &&
           !shouldApplyNoProjectWorkspace(state);
}

/// @brief 检查新项目加载完成后结束切换并保持项目活动状态。
/// @return 状态符合预期时返回 true。
constexpr bool checkOpenedCompletesTransition()
{
    const auto state =
        reduceAll(ProjectUiLifecycleState{ .hasActiveProject = true },
                  ProjectUiLifecycleKind::OpenStarted,
                  ProjectUiLifecycleKind::Closed,
                  ProjectUiLifecycleKind::Opened);
    return state.hasActiveProject && !state.transitionInProgress &&
           !state.closedDuringTransition &&
           !shouldApplyNoProjectWorkspace(state);
}

/// @brief 检查旧项目尚未关闭时打开失败会恢复原项目状态。
/// @return 状态符合预期时返回 true。
constexpr bool checkFailureBeforeCloseKeepsActiveProject()
{
    const auto state =
        reduceAll(ProjectUiLifecycleState{ .hasActiveProject = true },
                  ProjectUiLifecycleKind::OpenStarted,
                  ProjectUiLifecycleKind::OpenFailed);
    return state.hasActiveProject && !state.transitionInProgress &&
           !state.closedDuringTransition;
}

/// @brief 检查旧项目已关闭后打开失败会进入真正的无项目状态。
/// @return 状态符合预期时返回 true。
constexpr bool checkFailureAfterCloseClearsActiveProject()
{
    const auto state =
        reduceAll(ProjectUiLifecycleState{ .hasActiveProject = true },
                  ProjectUiLifecycleKind::OpenStarted,
                  ProjectUiLifecycleKind::Closed,
                  ProjectUiLifecycleKind::OpenFailed);
    return !state.hasActiveProject && !state.transitionInProgress &&
           !state.closedDuringTransition &&
           shouldApplyNoProjectWorkspace(state);
}

/// @brief 检查无项目打开失败后仍保持无项目状态。
/// @return 状态符合预期时返回 true。
constexpr bool checkInitialOpenFailureStaysEmpty()
{
    const auto state = reduceAll(ProjectUiLifecycleState{},
                                 ProjectUiLifecycleKind::OpenStarted,
                                 ProjectUiLifecycleKind::OpenFailed);
    return shouldApplyNoProjectWorkspace(state);
}

/// @brief 检查路径校验阶段失败不会清除仍然活动的旧项目。
/// @return 状态符合预期时返回 true。
constexpr bool checkRejectedOpenKeepsActiveProject()
{
    const auto state =
        reduceAll(ProjectUiLifecycleState{ .hasActiveProject = true },
                  ProjectUiLifecycleKind::OpenFailed);
    return state.hasActiveProject && !state.transitionInProgress &&
           !state.closedDuringTransition;
}

/// @brief 检查没有切换流程的普通关闭会进入无项目状态。
/// @return 状态符合预期时返回 true。
constexpr bool checkCloseOnlyClearsActiveProject()
{
    const auto state =
        reduceAll(ProjectUiLifecycleState{ .hasActiveProject = true },
                  ProjectUiLifecycleKind::Closed);
    return shouldApplyNoProjectWorkspace(state);
}

/// @brief 检查项目根目录原地迁移不会改变项目活动状态。
/// @return 状态符合预期时返回 true。
constexpr bool checkRootChangeKeepsActiveProject()
{
    const auto state =
        reduceAll(ProjectUiLifecycleState{ .hasActiveProject = true },
                  ProjectUiLifecycleKind::RootChanged);
    return state.hasActiveProject && !state.transitionInProgress &&
           !state.closedDuringTransition;
}
}  // namespace

/// @brief 覆盖项目切换空窗与打开失败时的 UI 生命周期状态。
/// @return 所有状态断言通过时返回 0。
int main()
{
    static_assert(checkClosedDuringTransitionKeepsWorkspace());
    static_assert(checkOpenedCompletesTransition());
    static_assert(checkFailureBeforeCloseKeepsActiveProject());
    static_assert(checkFailureAfterCloseClearsActiveProject());
    static_assert(checkInitialOpenFailureStaysEmpty());
    static_assert(checkRejectedOpenKeepsActiveProject());
    static_assert(checkCloseOnlyClearsActiveProject());
    static_assert(checkRootChangeKeepsActiveProject());
    return 0;
}
