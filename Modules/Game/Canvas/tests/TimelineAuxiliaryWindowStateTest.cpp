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
    // Timeline 与时间点表都隐藏时没有消费者，不应准备快照。
    if ( shouldPrepareTimelineSnapshot(false, state) ) return false;
    // 主 Timeline 可见时始终是自身快照消费者。
    if ( !shouldPrepareTimelineSnapshot(true, state) ) return false;

    // 独立时间点表打开后，即使主 Timeline 隐藏也必须继续准备快照。
    state.timingPointsTableOpen = true;
    if ( !shouldPrepareTimelineSnapshot(false, state) ) return false;

    // 关闭最后一个消费者后应立即停止无用的快照准备。
    state.timingPointsTableOpen = false;
    return !shouldPrepareTimelineSnapshot(false, state);
}
}  // namespace

/// @brief 运行 Timeline 自身快照唤醒回归测试。
/// @return 测试通过时返回 0。
int main()
{
    // 单一测试覆盖消费者从零到一再回到零的完整状态变化。
    return testHiddenTimelineSnapshotConsumers() ? 0 : 1;
}
