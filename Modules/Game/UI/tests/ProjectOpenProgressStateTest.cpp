#include "ui/project/ProjectOpenProgressState.h"

#include <cassert>
#include <limits>
#include <string>

/// @file ProjectOpenProgressStateTest.cpp
/// @brief 项目打开进度快照的初始化、事件重排、数值清洗和结束状态测试。
/// @details 测试直接驱动纯状态辅助函数，验证 UI 不会因跨线程事件顺序或异常
/// 浮点数显示倒退、越界进度及过期详情。

namespace
{
using MMM::Event::ProjectOpenProgressStage;
using MMM::UI::ProjectOpenProgressState;
using MMM::UI::applyProjectOpenProgress;
using MMM::UI::beginProjectOpenProgress;
using MMM::UI::finishProjectOpenProgress;

/// @brief 检查打开事件可以初始化项目加载进度。
/// @post 状态处于验证阶段、进度为零且保留项目名称。
void checkBeginInitializesProgress()
{
    // 默认构造状态模拟尚未收到任何项目事件的 UI。
    ProjectOpenProgressState state;
    // 开始事件建立活动快照并写入初始详情。
    beginProjectOpenProgress(state, "project");
    // 所有初始字段都需要同时成立，避免状态栏显示残留值。
    assert(state.active);
    assert(state.stage == ProjectOpenProgressStage::Validating);
    assert(state.fraction == 0.0F);
    assert(state.detail == "project");
}

/// @brief 检查先到达的跨线程进度不会被稍后的打开事件覆盖。
/// @post 较新的阶段、比例和详情全部保持不变。
void checkBeginPreservesQueuedProgress()
{
    ProjectOpenProgressState state;
    // 模拟队列中具体进度先于 OpenStarted 被 UI 消费。
    applyProjectOpenProgress(
        state, ProjectOpenProgressStage::LoadingBeatmaps, 0.75F, "chart.osu");
    // 晚到的开始事件不得把具体加载阶段重置为验证阶段。
    beginProjectOpenProgress(state, "project");
    assert(state.stage == ProjectOpenProgressStage::LoadingBeatmaps);
    assert(state.fraction == 0.75F);
    assert(state.detail == "chart.osu");
}

/// @brief 检查异常和越界进度会被约束到可绘制范围。
/// @post 大于一的数值变为一，NaN 变为零。
void checkFractionIsSanitized()
{
    // 两次更新复用同一状态，验证清洗逻辑不会依赖初始 active 值。
    ProjectOpenProgressState state;
    // 上界外输入按完成比例显示。
    applyProjectOpenProgress(
        state, ProjectOpenProgressStage::PreparingAudio, 1.5F, "hit.wav");
    assert(state.fraction == 1.0F);

    // NaN 不可交给 ImGui 进度控件，状态层必须归一为零。
    applyProjectOpenProgress(state,
                             ProjectOpenProgressStage::PreparingAudio,
                             std::numeric_limits<float>::quiet_NaN(),
                             "hit.wav");
    assert(state.fraction == 0.0F);
}

/// @brief 检查项目加载结束后状态栏不再展示旧进度。
/// @post active 为 false、比例为一且详情为空。
void checkFinishClearsProgress()
{
    ProjectOpenProgressState state;
    // 先构造接近完成但仍活动的加载状态。
    applyProjectOpenProgress(
        state, ProjectOpenProgressStage::Finalizing, 0.98F, "project");
    // 完成事件清除详情，防止下次加载短暂展示旧项目名。
    finishProjectOpenProgress(state);
    assert(!state.active);
    assert(state.fraction == 1.0F);
    assert(state.detail.empty());
}
}  // namespace

/// @brief 覆盖项目打开状态栏进度的初始化、跨线程顺序和结束行为。
/// @return 所有断言通过时返回 0。
/// @note assert 失败会使 CTest 直接报告对应状态契约被破坏。
int main()
{
    // 按生命周期顺序执行独立状态场景。
    // 每个函数自行构造状态，避免用例间共享进度详情。
    checkBeginInitializesProgress();
    checkBeginPreservesQueuedProgress();
    checkFractionIsSanitized();
    checkFinishClearsProgress();
    return 0;
}
