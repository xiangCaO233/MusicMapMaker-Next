#pragma once

#include "canvas/ComposeHoldTarget.h"
#include "canvas/ComposeTargetEligibility.h"
#include "common/render/RenderSnapshot.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace MMM::Canvas
{
/// @brief 折线教学中的一个固定拍位与玩家轨道。
struct ComposePolylineWaypoint {
    /// @brief 玩家轨道下标。
    int track{ 0 };
    /// @brief 渲染器已经绘出的精确分拍时间。
    double time{ 0.0 };
};

/// @brief 五段 Hold/Flick 折线路径；最后两个节点用于延长末段 Hold。
struct ComposePolylineTarget {
    /// @brief 路径从起点到终点依次经过的七个检查点。
    /// 前五个检查点依次形成 Hold、Flick、Hold、Flick。
    /// 第五到第七个检查点都在同轨，合成最后一段 Hold。
    /// 检查点保存时间而非屏幕 y，滚动时可重新投到真实拍线。
    std::array<ComposePolylineWaypoint, 7> waypoints{};
};

/// @brief 从真实可见拍线中选择五段折线的完整教学路径。
/// @param lines 当前玩家区实际显示的拍线，顺序可被变速效果打乱。
/// @param trackCount 玩家轨道数，至少为两条。
/// @param noteHeight Note 的实际高度，用于避免相邻目标框重叠。
/// @param top 玩家区可见上界，已扣除画布补间偏移。
/// @param bottom 玩家区可见下界，已扣除画布补间偏移。
/// @param seed 目标轨道和起始候选的稳定随机来源。
/// @param placedHold 本轮已绘制的 Hold；优先换轨，双轨时避开其时间区间。
/// @return 找到五条有足够间隔的合法拍线时返回七个有序检查点。
/// @note 返回值不存储渲染快照中的指针；调用方可跨帧保存结果。
/// @note 只有时间严格递增的纵向段才可通过 DrawTool 最终规范化。
/// @note 屏幕 y 可以因 Scroll/SV 非单调，不能拿 y 代替谱面时间排序。
/// @note 种子只选择候选起点和轨道，不改变五段路线的结构约束。
/// @note 三轨以上整条路线避开旧 Hold 轨道，包含横移经过的端点。
/// @note 双轨没有两条空轨，五个拍位必须全部处于 Hold 的同一时间侧。
/// @warning 仅在目标生成或失效后调用；不进入每帧排序或全谱扫描。
inline std::optional<ComposePolylineTarget> chooseComposePolylineTarget(
    std::span<const Common::Render::PlayerBeatLineSnapshot> lines,
    int trackCount, float noteHeight, float top, float bottom,
    std::uint64_t                    seed,
    std::optional<ComposeHoldTarget> placedHold = std::nullopt)
{
    if ( trackCount < 2 || lines.size() < 5U || !std::isfinite(noteHeight) ||
         noteHeight <= 0.0F )
        return std::nullopt;
    // 多轨时整个路线避开旧 Hold 所在轨；双轨必须保留两轨，改为换时间段。
    const bool validHold       = placedHold && placedHold->track >= 0 &&
                                 placedHold->track < trackCount &&
                                 placedHold->startTime < placedHold->endTime;
    const bool avoidHoldTrack  = validHold && trackCount > 2;
    const int  availableTracks = trackCount - (avoidHoldTrack ? 1 : 0);
    /// @brief 把候选序号映射回真实轨号，排除已经占用的 Hold 轨道。
    /// @note 先在紧凑候选集取模，再跳过旧轨，保留每条空轨的选中机会。
    const auto availableTrackAt = [&](int index) {
        if ( avoidHoldTrack && index >= placedHold->track ) ++index;
        return index;
    };

    // 从轮转起点寻找一条完整时间正向路径；时间增长不要求屏幕 y 单调。
    // 每次只在线性可见拍线中寻找下一拍，避免按每帧 UI 顺序排序。
    // 起始位置受种子影响，顺序失败后尝试其它可见拍位。
    // 检查点数量固定，局部指针数组分配在栈上而不是共享渲染数据。
    for ( std::size_t startOffset = 0; startOffset < lines.size();
          ++startOffset ) {
        const auto& first = lines[(seed + startOffset) % lines.size()];
        if ( !isComposeTargetBeatLine(
                 first.time, first.y, noteHeight, top, bottom) )
            continue;
        // 双轨没有两条空轨，整条折线只能落在旧 Hold 的前侧或后侧。
        // 只检查端点时间不足以保护中间段，因此后续拍线须保持在同一侧。
        const bool beforeHold = validHold && trackCount == 2 &&
                                first.time < placedHold->startTime - 1e-7;
        const bool afterHold  = validHold && trackCount == 2 &&
                                first.time > placedHold->endTime + 1e-7;
        // 起点在 Hold 内部时不能仅靠后继筛选来修正整段跨越。
        if ( validHold && trackCount == 2 && !beforeHold && !afterHold )
            continue;
        // 这些指针只在本次候选搜索期间使用，返回前全部转成时间值。
        std::array<const Common::Render::PlayerBeatLineSnapshot*, 5> chosen{};
        chosen[0]     = &first;
        bool complete = true;
        for ( std::size_t index = 1; index < chosen.size(); ++index ) {
            const Common::Render::PlayerBeatLineSnapshot* next = nullptr;
            // 当前阶段只关心时间上最近的合法后继，不要求输入已排序。
            // 纵向目标框过近会遮住操作方向，所以同时施加像素间隔。
            for ( const auto& candidate : lines ) {
                // 端点必须完整可见，且与前一个目标框至少相隔两个 Note 高度。
                if ( !isComposeTargetBeatLine(candidate.time,
                                              candidate.y,
                                              noteHeight,
                                              top,
                                              bottom) ||
                     candidate.time <= chosen[index - 1]->time + 1e-7 ||
                     (beforeHold &&
                      candidate.time >= placedHold->startTime - 1e-7) ||
                     (afterHold &&
                      candidate.time <= placedHold->endTime + 1e-7) ||
                     std::abs(candidate.y - chosen[index - 1]->y) <
                         noteHeight * 2.0F )
                    continue;
                if ( !next || candidate.time < next->time ) next = &candidate;
            }
            if ( !next ) {
                // 单个起点不够五拍仍可能有其它起点；继续外层候选搜索。
                complete = false;
                break;
            }
            chosen[index] = next;
        }
        if ( !complete ) continue;

        // 横向两次跨轨夹在三段时间正向 Hold 之间。
        // 末尾同轨再经过一拍，使画笔的临时零长度 Hold 真正延长。
        // 第二轨通过非零偏移选取，双轨谱面自然使用唯一的另一个轨道。
        const int sourceIndex = static_cast<int>(seed % availableTracks);
        const int otherIndex =
            (sourceIndex + 1 +
             static_cast<int>((seed >> 16U) % (availableTracks - 1))) %
            availableTracks;
        // 两次横移复用同一对空轨；纵向段不会落回旧 Hold 轨道。
        const int sourceTrack = availableTrackAt(sourceIndex);
        const int otherTrack  = availableTrackAt(otherIndex);
        return ComposePolylineTarget{
            .waypoints = {
                ComposePolylineWaypoint{ sourceTrack, chosen[0]->time },
                ComposePolylineWaypoint{ sourceTrack, chosen[1]->time },
                ComposePolylineWaypoint{ otherTrack, chosen[1]->time },
                ComposePolylineWaypoint{ otherTrack, chosen[2]->time },
                ComposePolylineWaypoint{ sourceTrack, chosen[2]->time },
                ComposePolylineWaypoint{ sourceTrack, chosen[3]->time },
                ComposePolylineWaypoint{ sourceTrack, chosen[4]->time },
            },
        };
    }
    return std::nullopt;
}
}  // namespace MMM::Canvas
