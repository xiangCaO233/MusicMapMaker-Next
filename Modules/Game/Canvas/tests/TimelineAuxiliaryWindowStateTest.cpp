#include "canvas/TimelineAuxiliaryWindowState.h"

namespace
{
/// @brief 验证 Timeline 隐藏时仅时间点表格会唤醒 Timeline 快照消费。
/// @return 所有可见状态组合符合预期时返回 true。
bool testHiddenTimelineSnapshotConsumers()
{
    using MMM::Canvas::shouldPrepareTimelineSnapshot;
    using MMM::Canvas::TimelineAuxiliaryWindowState;

    TimelineAuxiliaryWindowState state;
    if ( shouldPrepareTimelineSnapshot(false, state) ) return false;
    if ( !shouldPrepareTimelineSnapshot(true, state) ) return false;

    state.timingPointsTableOpen = true;
    if ( !shouldPrepareTimelineSnapshot(false, state) ) return false;

    state.timingPointsTableOpen = false;
    return !shouldPrepareTimelineSnapshot(false, state);
}
}  // namespace

/// @brief 运行 Timeline 自身快照唤醒回归测试。
/// @return 测试通过时返回 0。
int main()
{
    return testHiddenTimelineSnapshotConsumers() ? 0 : 1;
}
