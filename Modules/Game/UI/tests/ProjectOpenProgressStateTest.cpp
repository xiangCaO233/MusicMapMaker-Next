#include "ui/project/ProjectOpenProgressState.h"

#include <cassert>
#include <limits>
#include <string>

namespace
{
using MMM::Event::ProjectOpenProgressStage;
using MMM::UI::ProjectOpenProgressState;
using MMM::UI::applyProjectOpenProgress;
using MMM::UI::beginProjectOpenProgress;
using MMM::UI::finishProjectOpenProgress;

/// @brief 检查打开事件可以初始化项目加载进度。
void checkBeginInitializesProgress()
{
    ProjectOpenProgressState state;
    beginProjectOpenProgress(state, "project");
    assert(state.active);
    assert(state.stage == ProjectOpenProgressStage::Validating);
    assert(state.fraction == 0.0F);
    assert(state.detail == "project");
}

/// @brief 检查先到达的跨线程进度不会被稍后的打开事件覆盖。
void checkBeginPreservesQueuedProgress()
{
    ProjectOpenProgressState state;
    applyProjectOpenProgress(
        state, ProjectOpenProgressStage::LoadingBeatmaps, 0.75F, "chart.osu");
    beginProjectOpenProgress(state, "project");
    assert(state.stage == ProjectOpenProgressStage::LoadingBeatmaps);
    assert(state.fraction == 0.75F);
    assert(state.detail == "chart.osu");
}

/// @brief 检查异常和越界进度会被约束到可绘制范围。
void checkFractionIsSanitized()
{
    ProjectOpenProgressState state;
    applyProjectOpenProgress(
        state, ProjectOpenProgressStage::PreparingAudio, 1.5F, "hit.wav");
    assert(state.fraction == 1.0F);

    applyProjectOpenProgress(state,
                             ProjectOpenProgressStage::PreparingAudio,
                             std::numeric_limits<float>::quiet_NaN(),
                             "hit.wav");
    assert(state.fraction == 0.0F);
}

/// @brief 检查项目加载结束后状态栏不再展示旧进度。
void checkFinishClearsProgress()
{
    ProjectOpenProgressState state;
    applyProjectOpenProgress(
        state, ProjectOpenProgressStage::Finalizing, 0.98F, "project");
    finishProjectOpenProgress(state);
    assert(!state.active);
    assert(state.fraction == 1.0F);
    assert(state.detail.empty());
}
}  // namespace

/// @brief 覆盖项目打开状态栏进度的初始化、跨线程顺序和结束行为。
/// @return 所有断言通过时返回 0。
int main()
{
    checkBeginInitializesProgress();
    checkBeginPreservesQueuedProgress();
    checkFractionIsSanitized();
    checkFinishClearsProgress();
    return 0;
}
