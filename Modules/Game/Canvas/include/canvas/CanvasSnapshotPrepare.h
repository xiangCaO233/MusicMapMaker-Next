#pragma once

#include "common/render/RenderSnapshotBuffer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace MMM::Canvas
{

/// @brief UI 并行准备阶段生成的画布快照消费结果。
struct PreparedCanvasSnapshot {
    /// @brief 当前帧应展示的渲染快照。
    Common::Render::RenderSnapshot* snapshot{ nullptr };

    /// @brief 当前帧应用偏移后的快照指针。
    Common::Render::RenderSnapshot* offsetSnapshot{ nullptr };

    /// @brief 当前帧实际应用到动态顶点的 Y 偏移。
    float appliedYOffset{ 0.0f };
};

/// @brief 计算当前稳态时钟秒数。
/// @return steady_clock 当前时间，单位秒。
inline double currentSteadySeconds()
{
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// @brief 对快照动态顶点和 Timeline 交互坐标应用或还原 UI 侧播放插值偏移。
/// @param snapshot 待修改的渲染快照。
/// @param yOffset 需要叠加到动态顶点上的 Y 偏移。
/// @warning 后台线程路径：只允许在快照仍由 UI 侧持有、尚未归还逻辑线程前调用。
inline void applyDynamicVertexYOffset(Common::Render::RenderSnapshot* snapshot,
                                      float                           yOffset)
{
    if ( !snapshot || std::abs(yOffset) <= 0.0001f ) {
        return;
    }

    if ( snapshot->dynamicVertexCount > 0 ) {
        const uint32_t startVtx = snapshot->staticVertexCount;
        auto&          vertices = snapshot->vertices;
        const uint32_t endVtx =
            std::min(startVtx + snapshot->dynamicVertexCount,
                     static_cast<uint32_t>(vertices.size()));

        for ( size_t i = startVtx; i < endVtx; ++i ) {
            vertices[i].pos.y += yOffset;
        }
    }

    for ( auto& element : snapshot->timelineElements ) {
        element.y += yOffset;
    }

    for ( auto& marker : snapshot->annotationMarkers ) {
        marker.canvasY += yOffset;
    }
}

/// @brief 拉取并准备画布渲染快照。
/// @param syncBuffer 逻辑线程写入的同步缓冲区。
/// @param lastOffsetSnapshot 上一帧应用过偏移的快照。
/// @param lastAppliedYOffset 上一帧应用的 Y 偏移。
/// @param scaleByRenderScaleY 是否按预览画布垂直缩放倍率修正偏移。
/// @return 准备好的画布快照消费结果。
/// @warning 后台线程路径：只允许消费 BeatmapSyncBuffer
/// 并修改当前读快照的动态顶点。
inline PreparedCanvasSnapshot prepareCanvasSnapshot(
    Common::Render::RenderSnapshotBuffer* syncBuffer,
    Common::Render::RenderSnapshot*       lastOffsetSnapshot,
    float lastAppliedYOffset, bool scaleByRenderScaleY)
{
    PreparedCanvasSnapshot prepared;
    if ( !syncBuffer ) {
        return prepared;
    }

    // 先还原上一帧偏移，再拉取新快照；resize 重建可能让旧读缓冲被
    // 逻辑线程回收复用，不能只靠指针相等判断是否仍是同一帧数据。
    applyDynamicVertexYOffset(lastOffsetSnapshot, -lastAppliedYOffset);

    prepared.snapshot = syncBuffer->pullLatestSnapshot();
    if ( !prepared.snapshot ) {
        return prepared;
    }

    float        newYOffset = 0.0f;
    const double dt =
        prepared.snapshot->playbackInterpolationElapsed(currentSteadySeconds());
    if ( dt > 0.0 ) {
        newYOffset =
            static_cast<float>(prepared.snapshot->getInterpolatedOffset(dt));
        if ( scaleByRenderScaleY ) {
            newYOffset *= prepared.snapshot->renderScaleY;
        }
    }

    applyDynamicVertexYOffset(prepared.snapshot, newYOffset);

    prepared.offsetSnapshot = prepared.snapshot;
    prepared.appliedYOffset = newYOffset;
    return prepared;
}

}  // namespace MMM::Canvas
