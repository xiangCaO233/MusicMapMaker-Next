#include "canvas/Basic2DCanvas.h"
#include "canvas/Basic2DCanvasInteraction.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/system/ScrollCache.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fmt/format.h>
#include <utility>

namespace MMM::Canvas
{

Basic2DCanvas::Basic2DCanvas(
    const std::string& name, uint32_t w, uint32_t h,
    std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer,
    const std::string&                        cameraId)
    : IUIView(name)
    , IRenderableView(name)
    , m_canvasName(name)
    , m_cameraId(cameraId.empty() ? name : cameraId)
    , m_syncBuffer(std::move(syncBuffer))
{
    m_targetWidth  = w;
    m_targetHeight = h;

    m_interaction =
        std::make_unique<Basic2DCanvasInteraction>(m_canvasName, m_cameraId);
}

Basic2DCanvas::~Basic2DCanvas() {}

void Basic2DCanvas::update(UI::UIManager* sourceManager)
{
    std::string windowName =
        fmt::format("{}###{}", TR("canvas.editor"), m_canvasName);
    UI::LayoutContext lctx(m_layoutCtx, windowName);
    RenderContext     rctx(
        this, m_canvasName.c_str(), m_targetWidth, m_targetHeight);

    // 拉取快照
    if ( m_syncBuffer ) {
        m_currentSnapshot = m_syncBuffer->pullLatestSnapshot();
    }

    if ( m_currentSnapshot ) {
        // 更新背景纹理
        updateBackgroundTexture();

        // --- 亚帧时间补偿 (直接修改动态顶点 Y 坐标) ---
        // 当播放中时，逻辑线程生成快照的时刻 (snapshotSysTime) 与 UI
        // 线程实际渲染的时刻之间存在时间差。
        // 在 effectTiming 段落中，由于可见物体更多导致逻辑线程帧率下降，
        // 这个时间差被放大为可见的周期性停顿。
        // 通过直接修改动态顶点（拍线、音符等）的 Y 坐标来补偿，
        // 而静态顶点（轨道底板、判定区）保持不变。
        float newYOffset = 0.0f;

        if ( m_currentSnapshot->isPlaying &&
             m_currentSnapshot->snapshotSysTime > 0.0 ) {
            double now =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            double dt = now - m_currentSnapshot->snapshotSysTime;
            if ( dt > 0.0 && dt < 0.1 ) {
                double      scrollSpeed  = 500.0;
                double      snapshotTime = m_currentSnapshot->currentTime;
                const auto& segs         = m_currentSnapshot->scrollSegments;
                if ( !segs.empty() ) {
                    auto it = std::upper_bound(
                        segs.begin(),
                        segs.end(),
                        snapshotTime,
                        [](double                              val,
                           const Logic::System::ScrollSegment& seg) {
                            return val < seg.time;
                        });
                    if ( it != segs.begin() ) {
                        --it;
                    }
                    scrollSpeed = it->speed;
                }
                newYOffset = static_cast<float>(
                    dt * m_currentSnapshot->playbackSpeed * scrollSpeed);
            }
        }

        // 应用顶点级 Y 偏移 (仅修改动态顶点: 拍线、音符等)
        uint32_t startVtx = m_currentSnapshot->staticVertexCount;
        auto&    vertices = m_currentSnapshot->vertices;
        uint32_t endVtx   = m_currentSnapshot->dynamicVertexCount > 0
                                ? (startVtx + m_currentSnapshot->dynamicVertexCount)
                                : static_cast<uint32_t>(vertices.size());

        // 如果是同一个快照被复用，先撤销上一帧的偏移
        if ( m_lastOffsetSnapshot == m_currentSnapshot &&
             std::abs(m_lastAppliedYOffset) > 0.0001f ) {
            for ( size_t i = startVtx; i < endVtx && i < vertices.size(); ++i ) {
                vertices[i].pos.y -= m_lastAppliedYOffset;
            }
        }

        // 应用新偏移
        if ( std::abs(newYOffset) > 0.0001f ) {
            for ( size_t i = startVtx; i < endVtx && i < vertices.size(); ++i ) {
                vertices[i].pos.y += newYOffset;
            }
        }

        m_lastOffsetSnapshot = m_currentSnapshot;
        m_lastAppliedYOffset = newYOffset;
    } else {
        m_lastOffsetSnapshot = nullptr;
        m_lastAppliedYOffset = 0.0f;
    }

    // 交互统一交给 Interaction 处理
    m_interaction->update(
        sourceManager, m_currentSnapshot, m_logicalWidth, m_logicalHeight);
}

bool Basic2DCanvas::isDirty() const
{
    return true;
}

void Basic2DCanvas::resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                               uint32_t h) const
{
    Event::CanvasResizeEvent e;
    e.canvasName = m_cameraId;
    e.lastSize   = { oldW, oldH };
    e.newSize    = { w, h };
    Event::EventBus::instance().publish(e);
}

bool Basic2DCanvas::needReload()
{
    return std::exchange(m_needReload, false);
}

}  // namespace MMM::Canvas
