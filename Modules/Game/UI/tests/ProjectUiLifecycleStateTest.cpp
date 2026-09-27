#include "ui/project/ProjectUiLifecycleState.h"

/// @file ProjectUiLifecycleStateTest.cpp
/// @brief UI 项目生命周期归约器对正常切换、失败恢复和原地迁移的编译期测试。
/// @details 每个场景从明确快照开始并按事件顺序归约，重点防止项目替换期间
/// 短暂进入无项目工作区而引发布局闪烁。

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
    // 左折叠保持参数包给出的事件先后顺序。
    ((state = reduceProjectUiLifecycleState(state, kinds)), ...);
    return state;
}

/// @brief 检查切换期间关闭旧项目不会进入无项目工作区。
/// @return 状态符合预期时返回 true。
/// @note closedDuringTransition 用于记录旧项目已经离场但新项目尚未完成。
constexpr bool checkClosedDuringTransitionKeepsWorkspace()
{
    // 替换项目先开始再关闭旧项目，此时加载工作区必须继续存在。
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
/// @note Opened 必须同时清除 transition 与 closedDuringTransition。
constexpr bool checkOpenedCompletesTransition()
{
    // 新项目打开成功清除全部过渡标记并保持活动项目。
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
/// @note 失败前未收到 Closed，因此 hasActiveProject 应保持 true。
constexpr bool checkFailureBeforeCloseKeepsActiveProject()
{
    // 路径校验等早期失败发生时，旧项目尚可继续使用。
    const auto state =
        reduceAll(ProjectUiLifecycleState{ .hasActiveProject = true },
                  ProjectUiLifecycleKind::OpenStarted,
                  ProjectUiLifecycleKind::OpenFailed);
    return state.hasActiveProject && !state.transitionInProgress &&
           !state.closedDuringTransition;
}

/// @brief 检查旧项目已关闭后打开失败会进入真正的无项目状态。
/// @return 状态符合预期时返回 true。
/// @note 此序列是替换项目流程中唯一需要清除活动项目的失败分支。
constexpr bool checkFailureAfterCloseClearsActiveProject()
{
    // 旧项目已关闭后再失败，无法恢复原项目，只能进入无项目状态。
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
    // 首次打开失败没有旧项目可回退，保持欢迎工作区。
    const auto state = reduceAll(ProjectUiLifecycleState{},
                                 ProjectUiLifecycleKind::OpenStarted,
                                 ProjectUiLifecycleKind::OpenFailed);
    return shouldApplyNoProjectWorkspace(state);
}

/// @brief 检查路径校验阶段失败不会清除仍然活动的旧项目。
/// @return 状态符合预期时返回 true。
constexpr bool checkRejectedOpenKeepsActiveProject()
{
    // 未进入过渡期的拒绝事件不得影响现有活动项目。
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
    // 普通关闭没有后继加载，应立即应用无项目工作区。
    const auto state =
        reduceAll(ProjectUiLifecycleState{ .hasActiveProject = true },
                  ProjectUiLifecycleKind::Closed);
    return shouldApplyNoProjectWorkspace(state);
}

/// @brief 检查项目根目录原地迁移不会改变项目活动状态。
/// @return 状态符合预期时返回 true。
/// @note RootChanged 只通知路径身份变化，不代表重新打开项目。
constexpr bool checkRootChangeKeepsActiveProject()
{
    // 临时项目转正只替换根路径，不关闭逻辑会话。
    const auto state =
        reduceAll(ProjectUiLifecycleState{ .hasActiveProject = true },
                  ProjectUiLifecycleKind::RootChanged);
    return state.hasActiveProject && !state.transitionInProgress &&
           !state.closedDuringTransition;
}
}  // namespace

/// @brief 覆盖项目切换空窗与打开失败时的 UI 生命周期状态。
/// @return 所有状态断言通过时返回 0。
/// @note 全部 static_assert 同时保证归约器继续满足 constexpr 契约。
int main()
{
    // 编译期执行完整事件矩阵，运行期无需外部项目资源。
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
