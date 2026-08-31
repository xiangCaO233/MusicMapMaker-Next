#include "canvas/TimelineTableSnapshotState.h"

#include <cstdint>

namespace
{
/// @brief 验证表格打开请求会等待正确谱面快照，而不会因旧路径键差异被吞掉。
/// @return 所有状态迁移符合预期时返回 true。
bool testTimelineTableSnapshotState()
{
    constexpr std::uintptr_t ACTIVE_BEATMAP_ID = 0x1234U;
    constexpr std::uintptr_t STALE_BEATMAP_ID  = 0x5678U;
    using MMM::Canvas::TimelineTableSnapshotStatus;
    using MMM::Canvas::resolveTimelineTableSnapshotStatus;

    // 菜单点击发生在快照准备之前时必须保留打开状态。
    if ( resolveTimelineTableSnapshotStatus(
             true, ACTIVE_BEATMAP_ID, false, false, 0) !=
         TimelineTableSnapshotStatus::AwaitingSnapshot ) {
        return false;
    }
    // 活动会话切换后，旧 Timeline 快照同样只能等待，不能误绘制或关闭请求。
    if ( resolveTimelineTableSnapshotStatus(
             true, ACTIVE_BEATMAP_ID, true, true, STALE_BEATMAP_ID) !=
         TimelineTableSnapshotStatus::AwaitingSnapshot ) {
        return false;
    }
    // 新快照携带同一谱面实例标识后，Timing 表和批注表均可立即绘制。
    if ( resolveTimelineTableSnapshotStatus(
             true, ACTIVE_BEATMAP_ID, true, true, ACTIVE_BEATMAP_ID) !=
         TimelineTableSnapshotStatus::Ready ) {
        return false;
    }
    // 活动谱面被关闭后，独立表格应随会话一起关闭。
    return resolveTimelineTableSnapshotStatus(false, 0, true, true, 0) ==
           TimelineTableSnapshotStatus::Close;
}
}  // namespace

/// @brief 运行 Timeline 独立表格快照状态回归测试。
/// @return 测试通过时返回 0。
int main()
{
    return testTimelineTableSnapshotState() ? 0 : 1;
}
