#include "logic/ecs/system/NoteRenderSystem.h"
#include "config/skin/SkinConfig.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/BackgroundRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
#include <algorithm>
#include <cmath>

namespace MMM::Logic::System
{

void NoteRenderSystem::generateSnapshot(
    entt::registry& registry, const entt::registry& timelineRegistry,
    RenderSnapshot* snapshot, const std::string& cameraId, double currentTime,
    float viewportWidth, float viewportHeight, float judgmentLineY,
    int32_t trackCount, const Config::EditorConfig& config,
    float mainViewportHeight)
{
    // 核心同步：如果预览区正在拖拽，主画布（Basic2DCanvas）渲染的时间应该是预览区当前的悬停时间
    // 但必须确保 currentTime
    // 不断更新导致死循环问题，所以这里只做渲染偏移，不改写真实的逻辑时间
    double renderTime = currentTime;
    if ( cameraId == "Basic2DCanvas" && snapshot->isPreviewDragging ) {
        renderTime = snapshot->previewHoverTime;
    }

    const auto* cache = timelineRegistry.ctx().find<ScrollCache>();
    if ( !cache ) return;

    // 将 ScrollCache 指针存入 context 供 renderPolyline 等后续使用
    registry.ctx().erase<const ScrollCache*>();
    registry.ctx().emplace<const ScrollCache*>(cache);

    Batcher batcher(snapshot);
    float   leftX = 0, rightX = 0, topY = 0, bottomY = 0, trackAreaW = 0,
            singleTrackW = 0;
    float   renderScaleY = 1.0f;

    if ( cameraId == "Timeline" ) {
        batcher.setScissor(0, 0, viewportWidth, viewportHeight);
        NoteRenderSystem::generateTimelineSnapshot(snapshot,
                                                   batcher,
                                                   renderTime,
                                                   viewportWidth,
                                                   viewportHeight,
                                                   judgmentLineY,
                                                   config,
                                                   cache);
    } else if ( cameraId == "Preview" ) {
        float lx = config.visual.previewConfig.margin.left;
        float rx = viewportWidth - config.visual.previewConfig.margin.right;
        float ty = config.visual.previewConfig.margin.top;
        float by = viewportHeight - config.visual.previewConfig.margin.bottom;
        batcher.setScissor(lx, ty, rx - lx, by - ty);

        NoteRenderSystem::generatePreviewSnapshot(snapshot,
                                                  batcher,
                                                  viewportWidth,
                                                  viewportHeight,
                                                  judgmentLineY,
                                                  trackCount,
                                                  config,
                                                  mainViewportHeight,
                                                  leftX,
                                                  rightX,
                                                  topY,
                                                  bottomY,
                                                  trackAreaW,
                                                  singleTrackW,
                                                  renderScaleY);
    } else {
        // 主画布背景不裁剪
        batcher.setScissor(0, 0, viewportWidth, viewportHeight);

        NoteRenderSystem::generateMainCanvasSnapshot(registry,
                                                     timelineRegistry,
                                                     snapshot,
                                                     batcher,
                                                     renderTime,
                                                     viewportWidth,
                                                     viewportHeight,
                                                     judgmentLineY,
                                                     trackCount,
                                                     config,
                                                     cache,
                                                     leftX,
                                                     rightX,
                                                     topY,
                                                     bottomY,
                                                     trackAreaW,
                                                     singleTrackW);

        // --- 交互：计算当前悬浮时间点所在的拍序 ---
        if ( snapshot->isHoveringCanvas || snapshot->isPreviewDragging ) {
            double hoveredTime = snapshot->isPreviewDragging
                                     ? snapshot->previewHoverTime
                                     : snapshot->hoveredTime;
            std::vector<const TimelineComponent*> bpmEvents;
            auto tlView = timelineRegistry.view<const TimelineComponent>();
            for ( auto entity : tlView ) {
                const auto& tl = tlView.get<const TimelineComponent>(entity);
                if ( tl.m_effect == ::MMM::TimingEffect::BPM ) {
                    bpmEvents.push_back(&tl);
                }
            }

            if ( !bpmEvents.empty() ) {
                std::stable_sort(
                    bpmEvents.begin(),
                    bpmEvents.end(),
                    [](const TimelineComponent* a, const TimelineComponent* b) {
                        return a->m_timestamp < b->m_timestamp;
                    });

                int64_t totalBeats = 0;
                bool    found      = false;
                for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
                    const auto* currentBPM = bpmEvents[i];
                    double      bpmTime    = currentBPM->m_timestamp;
                    double      bpmVal     = currentBPM->m_value;
                    if ( bpmVal <= 0.0 ) bpmVal = 120.0;
                    if ( bpmVal > 10000.0 ) bpmVal = 10000.0;  // 增加安全限制

                    double nextBpmTime =
                        (i + 1 < bpmEvents.size())
                            ? bpmEvents[i + 1]->m_timestamp
                            : std::numeric_limits<double>::infinity();

                    if ( hoveredTime >= bpmTime && hoveredTime < nextBpmTime ) {
                        double  beatDuration = 60.0 / bpmVal;
                        double  timeInBpm    = hoveredTime - bpmTime;
                        int64_t beatsInBpm   = static_cast<int64_t>(
                            std::floor(timeInBpm / beatDuration + 1e-6));
                        snapshot->hoveredBeatIndex =
                            static_cast<int>(totalBeats + beatsInBpm + 1);
                        found = true;
                        break;
                    } else if ( hoveredTime >= nextBpmTime ) {
                        double beatDuration = 60.0 / bpmVal;
                        double bpmDuration  = nextBpmTime - bpmTime;
                        totalBeats += static_cast<int64_t>(
                            std::round(bpmDuration / beatDuration));
                    } else {
                        // hoveredTime < bpmTime
                        break;
                    }
                }
                if ( !found ) snapshot->hoveredBeatIndex = 0;
            }
        }
    }

    NoteRenderSystem::renderNotes(registry,
                                  snapshot,
                                  cameraId,
                                  renderTime,
                                  judgmentLineY,
                                  trackCount,
                                  config,
                                  batcher,
                                  leftX,
                                  rightX,
                                  topY,
                                  bottomY,
                                  singleTrackW,
                                  renderScaleY);

    batcher.flush();
}

void NoteRenderSystem::generateTimelineSnapshot(
    RenderSnapshot* snapshot, Batcher& batcher, double currentTime,
    float viewportWidth, float viewportHeight, float judgmentLineY,
    const Config::EditorConfig& config, const ScrollCache* cache)
{
    if ( !snapshot->hasBeatmap ) return;

    batcher.setTexture(TextureID::None);

    // 绘制背景 (确保全覆盖，消除透明混合带来的边缘可疑像素)
    batcher.pushQuad(
        0, viewportHeight, viewportWidth, viewportHeight, { 0, 0, 0, 0.01f });

    auto& skin    = Config::SkinManager::instance();
    auto  tickCol = skin.getColor("timeline.tick");

    double currentAbsY = cache->getAbsY(currentTime);
    double startTime =
        cache->getTime(currentAbsY - (viewportHeight - judgmentLineY));
    double endTime = cache->getTime(currentAbsY + judgmentLineY);

    float paddingX = 30.0f;
    float lineW    = viewportWidth - paddingX * 2.0f;

    for ( const auto& seg : cache->getSegments() ) {
        if ( seg.time < startTime || seg.time > endTime ) continue;

        float y = judgmentLineY - static_cast<float>(seg.absY - currentAbsY);

        glm::vec4 color = { tickCol.r, tickCol.g, tickCol.b, 0.8f };
        if ( (seg.effects & SCROLL_EFFECT_BPM) &&
             (seg.effects & SCROLL_EFFECT_SCROLL) ) {
            color = { 1.0f, 0.5f, 0.0f, 0.8f };
        } else if ( seg.effects & SCROLL_EFFECT_BPM ) {
            color = { 1.0f, 0.2f, 0.2f, 0.8f };
        } else if ( seg.effects & SCROLL_EFFECT_SCROLL ) {
            color = { 0.2f, 1.0f, 0.2f, 0.8f };
        }

        batcher.pushQuad(paddingX, y + 1.0f, lineW, 2.0f, color);
        snapshot->timelineElements.push_back(
            { seg.time,
              y,
              seg.effects,
              seg.bpmEntity,
              seg.scrollEntity,
              seg.bpmValue,
              seg.scrollValue });
    }

    batcher.pushQuad(paddingX,
                     judgmentLineY + 1.0f,
                     lineW,
                     2.0f,
                     { 1.0f, 0.2f, 0.2f, 1.0f });
}

void NoteRenderSystem::generatePreviewSnapshot(
    RenderSnapshot* snapshot, Batcher& batcher, float viewportWidth,
    float viewportHeight, float judgmentLineY, int32_t trackCount,
    const Config::EditorConfig& config, float mainViewportHeight, float& leftX,
    float& rightX, float& topY, float& bottomY, float& trackAreaW,
    float& singleTrackW, float& renderScaleY)
{
    if ( !snapshot->hasBeatmap ) return;

    leftX        = config.visual.previewConfig.margin.left;
    rightX       = viewportWidth - config.visual.previewConfig.margin.right;
    topY         = config.visual.previewConfig.margin.top;
    bottomY      = viewportHeight - config.visual.previewConfig.margin.bottom;
    trackAreaW   = rightX - leftX;
    singleTrackW = trackAreaW / static_cast<float>(trackCount);

    float mainEffectiveH =
        (config.visual.trackLayout.bottom - config.visual.trackLayout.top) *
        mainViewportHeight;
    float previewDrawH = bottomY - topY;
    renderScaleY =
        previewDrawH / (mainEffectiveH * config.visual.previewConfig.areaRatio);

    auto& skin    = Config::SkinManager::instance();
    auto  boxCol  = skin.getColor("preview.boundingbox");
    auto  lineCol = skin.getColor("preview.judgeline");

    float mainJudgelineInPreviewY = judgmentLineY;

    // 核心修正：包围盒的高度应该代表主视窗在预览比例下的可见范围。
    // 在 Batcher 中，pushQuad(x, y, w, h) 的 y 是底边坐标。
    float boxDrawH = mainEffectiveH * renderScaleY;

    // --- 区分状态进行绘制 ---
    bool isDragging = snapshot->isPreviewDragging;

    // 1. [展示中] 始终绘制当前主视窗位置的包围盒 (除非正在拖拽)
    if ( !isDragging ) {
        // boxBottom 计算：判定线位置 + (视觉底部 - 判定线位置) * 缩放
        float boxBottom =
            mainJudgelineInPreviewY +
            (config.visual.trackLayout.bottom - config.visual.judgeline_pos) *
                mainViewportHeight * renderScaleY;

        batcher.setTexture(TextureID::None);
        batcher.pushQuad(leftX,
                         boxBottom,
                         trackAreaW,
                         boxDrawH,
                         { boxCol.r, boxCol.g, boxCol.b, boxCol.a });
        batcher.pushStrokeRect(leftX,
                               boxBottom - boxDrawH,
                               rightX,
                               boxBottom,
                               2.0f,
                               { boxCol.r, boxCol.g, boxCol.b, 1.0f });
    }

    // 2. [悬浮中/拖拽中] 绘制参考包围盒
    if ( snapshot->isPreviewHovered || isDragging ) {
        float previewHoverY =
            isDragging ? snapshot->previewHoverY
                       : snapshot->previewHoverY;  // 实际上拖拽时也是由
                                                   // mousePosition 驱动的
        float hoverBoxBottom =
            previewHoverY +
            (config.visual.trackLayout.bottom - config.visual.judgeline_pos) *
                mainViewportHeight * renderScaleY;

        auto hoverBoxCol = skin.getColor("preview.hoverbox");
        if ( hoverBoxCol.r == 1.0f && hoverBoxCol.g == 0.0f &&
             hoverBoxCol.b == 1.0f ) {
            hoverBoxCol = { 1.0f, 1.0f, 0.6f, 0.3f };
        }

        // 如果正在拖动，使用更亮的颜色
        if ( isDragging ) {
            hoverBoxCol.a = std::min(1.0f, hoverBoxCol.a * 2.0f);
        }

        batcher.setTexture(TextureID::None);
        batcher.pushQuad(
            leftX,
            hoverBoxBottom,
            trackAreaW,
            boxDrawH,
            { hoverBoxCol.r, hoverBoxCol.g, hoverBoxCol.b, hoverBoxCol.a });
        batcher.pushStrokeRect(
            leftX,
            hoverBoxBottom - boxDrawH,
            rightX,
            hoverBoxBottom,
            2.0f,
            { hoverBoxCol.r, hoverBoxCol.g, hoverBoxCol.b, 0.8f });

        // 在鼠标位置绘制临时的判定线预览
        batcher.pushQuad(leftX,
                         previewHoverY + config.visual.judgelineWidth * 0.5f,
                         trackAreaW,
                         config.visual.judgelineWidth,
                         { hoverBoxCol.r, hoverBoxCol.g, hoverBoxCol.b, 0.6f });
    }

    batcher.setTexture(TextureID::None);
    batcher.pushQuad(
        leftX,
        mainJudgelineInPreviewY + config.visual.judgelineWidth * 0.5f,
        trackAreaW,
        config.visual.judgelineWidth,
        { lineCol.r, lineCol.g, lineCol.b, lineCol.a });
}

void NoteRenderSystem::generateMainCanvasSnapshot(
    entt::registry& registry, const entt::registry& timelineRegistry,
    RenderSnapshot* snapshot, Batcher& batcher, double currentTime,
    float viewportWidth, float viewportHeight, float judgmentLineY,
    int32_t trackCount, const Config::EditorConfig& config,
    const ScrollCache* cache, float& leftX, float& rightX, float& topY,
    float& bottomY, float& trackAreaW, float& singleTrackW)
{
    BackgroundRenderSystem::render(
        batcher, viewportWidth, viewportHeight, config, snapshot);

    // 设置轨道区域裁剪
    float lx = viewportWidth * config.visual.trackLayout.left;
    float rx = viewportWidth * config.visual.trackLayout.right;
    float ty = viewportHeight * config.visual.trackLayout.top;
    float by = viewportHeight * config.visual.trackLayout.bottom;
    batcher.setScissor(lx, ty, rx - lx, by - ty);

    if ( !snapshot->hasBeatmap ) {
        batcher.setTexture(TextureID::Logo);
        float logoSize = std::min(viewportWidth, viewportHeight) * 0.4f;
        float cx       = viewportWidth * 0.5f;
        float cy       = viewportHeight * 0.5f;
        batcher.pushQuad(cx - logoSize * 0.5f,
                         cy + logoSize * 0.5f,
                         logoSize,
                         logoSize,
                         { 1.0f, 1.0f, 1.0f, 0.15f });
    } else {
        NoteRenderSystem::renderTrackLayout(batcher,
                                            viewportWidth,
                                            viewportHeight,
                                            judgmentLineY,
                                            trackCount,
                                            config,
                                            timelineRegistry,
                                            currentTime,
                                            cache,
                                            leftX,
                                            rightX,
                                            topY,
                                            bottomY,
                                            trackAreaW,
                                            singleTrackW);
    }
}

}  // namespace MMM::Logic::System
