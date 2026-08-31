#pragma once

#include <cstdint>

namespace MMM::Canvas
{

/// @brief 独立 Timeline 表格对当前渲染快照的处理状态。
enum class TimelineTableSnapshotStatus : std::uint8_t {
    Close,             ///< 活动谱面已不存在，应关闭表格。
    AwaitingSnapshot,  ///< 快照尚未追上活动谱面，应保留打开请求。
    Ready,             ///< 快照与活动谱面一致，可以绘制表格。
};

/// @brief 判定独立表格能否消费 Timeline 快照。
/// @param hasActiveBeatmap 当前活动会话是否持有谱面。
/// @param activeBeatmapInstanceId 当前活动谱面实例标识。
/// @param hasSnapshot Timeline 是否已取得快照。
/// @param snapshotHasBeatmap 快照是否包含谱面。
/// @param snapshotBeatmapInstanceId 快照对应的谱面实例标识。
/// @return 无活动谱面时关闭；快照尚未追上时等待；身份一致时可绘制。
/// @warning UI 热路径：每个打开的表格每帧调用，只允许常量整数比较。
constexpr TimelineTableSnapshotStatus resolveTimelineTableSnapshotStatus(
    bool hasActiveBeatmap, std::uintptr_t activeBeatmapInstanceId,
    bool hasSnapshot, bool snapshotHasBeatmap,
    std::uintptr_t snapshotBeatmapInstanceId)
{
    if ( !hasActiveBeatmap || activeBeatmapInstanceId == 0 ) {
        return TimelineTableSnapshotStatus::Close;
    }
    // 菜单先打开窗口、快照后一帧到达时保留请求，避免点击后被立即吞掉。
    if ( !hasSnapshot || !snapshotHasBeatmap ||
         snapshotBeatmapInstanceId != activeBeatmapInstanceId ) {
        return TimelineTableSnapshotStatus::AwaitingSnapshot;
    }
    return TimelineTableSnapshotStatus::Ready;
}

}  // namespace MMM::Canvas
