#pragma once

#include "logic/BeatmapSyncBuffer.h"
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace MMM::Canvas
{

/// @brief UI 并行准备阶段生成的画布快照消费结果。
struct PreparedCanvasSnapshot {
    /// @brief 当前帧应展示的渲染快照。
    Logic::RenderSnapshot* snapshot{ nullptr };

    /// @brief 当前帧应用偏移后的快照指针。
    Logic::RenderSnapshot* offsetSnapshot{ nullptr };

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

/// @brief 拉取并准备画布渲染快照。
/// @param syncBuffer 逻辑线程写入的同步缓冲区。
/// @param lastOffsetSnapshot 上一帧应用过偏移的快照。
/// @param lastAppliedYOffset 上一帧应用的 Y 偏移。
/// @param scaleByRenderScaleY 是否按预览画布垂直缩放倍率修正偏移。
/// @return 准备好的画布快照消费结果。
/// @warning 后台线程路径：只允许消费 BeatmapSyncBuffer
/// 并修改当前读快照的动态顶点。
inline PreparedCanvasSnapshot prepareCanvasSnapshot(
    Logic::BeatmapSyncBuffer* syncBuffer,
    Logic::RenderSnapshot* lastOffsetSnapshot, float lastAppliedYOffset,
    bool scaleByRenderScaleY)
{
    PreparedCanvasSnapshot prepared;
    if ( !syncBuffer ) {
        return prepared;
    }

    prepared.snapshot = syncBuffer->pullLatestSnapshot();
    if ( !prepared.snapshot ) {
        return prepared;
    }

    float newYOffset = 0.0f;
    if ( prepared.snapshot->isPlaying &&
         prepared.snapshot->snapshotSysTime > 0.0 ) {
        const double dt =
            currentSteadySeconds() - prepared.snapshot->snapshotSysTime;
        if ( dt > 0.0 && dt < 0.1 ) {
            newYOffset = static_cast<float>(
                prepared.snapshot->getInterpolatedOffset(dt));
            if ( scaleByRenderScaleY ) {
                newYOffset *= prepared.snapshot->renderScaleY;
            }
        }
    }

    const uint32_t startVtx = prepared.snapshot->staticVertexCount;
    auto&          vertices = prepared.snapshot->vertices;
    const uint32_t endVtx =
        prepared.snapshot->dynamicVertexCount > 0
            ? (startVtx + prepared.snapshot->dynamicVertexCount)
            : static_cast<uint32_t>(vertices.size());

    if ( lastOffsetSnapshot == prepared.snapshot &&
         std::abs(lastAppliedYOffset) > 0.0001f ) {
        for ( size_t i = startVtx; i < endVtx && i < vertices.size(); ++i ) {
            vertices[i].pos.y -= lastAppliedYOffset;
        }
    }

    if ( std::abs(newYOffset) > 0.0001f ) {
        for ( size_t i = startVtx; i < endVtx && i < vertices.size(); ++i ) {
            vertices[i].pos.y += newYOffset;
        }
    }

    prepared.offsetSnapshot = prepared.snapshot;
    prepared.appliedYOffset = newYOffset;
    return prepared;
}

}  // namespace MMM::Canvas
