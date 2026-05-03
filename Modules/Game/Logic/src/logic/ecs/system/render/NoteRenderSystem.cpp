#include "logic/ecs/system/NoteRenderSystem.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/BackgroundRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>

#include "logic/ecs/system/HitFXSystem.h"

namespace MMM::Logic::System
{

void NoteRenderSystem::generateSnapshot(
    entt::registry& registry, const entt::registry& timelineRegistry,
    RenderSnapshot* snapshot, const std::string& cameraId, double currentTime,
    float viewportWidth, float viewportHeight, float judgmentLineY,
    int32_t trackCount, const Config::EditorConfig& config,
    float mainViewportHeight, HitFXSystem* hitFXSystem)
{
    // 核心同步：如果预览区正在拖拽，主画布（Basic2DCanvas）渲染的时间应该是预览区当前的悬停时间
    double renderTime = currentTime;
    if ( cameraId == "Basic2DCanvas" && snapshot->isPreviewDragging ) {
        renderTime = snapshot->previewHoverTime;
    }

    const auto* cache = timelineRegistry.ctx().find<ScrollCache>();
    if ( !cache ) return;

    // 将 ScrollCache 指针存入 context 供 renderPolyline 等后续使用
    registry.ctx().erase<const ScrollCache*>();
    registry.ctx().emplace<const ScrollCache*>(cache);

    // 拷贝缓存段供 UI 线程计算时间映射
    snapshot->scrollSegments = cache->getSegments();

    Batcher batcher(snapshot);
    float   leftX = 0, rightX = 0, topY = 0, bottomY = 0, trackAreaW = 0,
            singleTrackW = 0;
    float   renderScaleY = 1.0f;

    // --- Phase 1: 静态布局与打击特效预生成 ---
    // 我们需要打击特效绘制在音符上方，但它的顶点位置是相对于判定线的（静态的，不随时间偏移）。
    // 因此，我们在设置 staticVertexCount
    // 之前生成它的顶点，但将其命令延迟到最后插入。
    uint32_t fxCmdStart = static_cast<uint32_t>(snapshot->cmds.size());
    if ( hitFXSystem &&
         (cameraId == "Basic2DCanvas" || cameraId == "Preview") ) {
        // 提前计算轨道参数
        float tempLX = 0, tempRX = 0;
        if ( cameraId == "Basic2DCanvas" ) {
            tempLX = viewportWidth * config.visual.trackLayout.left;
            tempRX = viewportWidth * config.visual.trackLayout.right;
        } else {
            tempLX = config.visual.previewConfig.margin.left;
            tempRX = viewportWidth - config.visual.previewConfig.margin.right;
        }
        float tempSTW = (tempRX - tempLX) / static_cast<float>(trackCount);

        hitFXSystem->generateSnapshot(batcher,
                                      renderTime,
                                      config,
                                      trackCount,
                                      judgmentLineY,
                                      tempLX,
                                      tempSTW);
    }
    uint32_t fxCmdEnd = static_cast<uint32_t>(snapshot->cmds.size());

    // 提取并暂存打击特效命令
    std::vector<UI::BrushDrawCmd> deferredHitCmds;
    if ( fxCmdEnd > fxCmdStart ) {
        deferredHitCmds.assign(snapshot->cmds.begin() + fxCmdStart,
                               snapshot->cmds.end());
        snapshot->cmds.erase(snapshot->cmds.begin() + fxCmdStart,
                             snapshot->cmds.end());
    }

    // 正常生成基础布局
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
                                                  renderTime,
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
        batcher.setScissor(0, 0, viewportWidth, viewportHeight);
        renderScaleY = 1.0f;

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
                                                     singleTrackW,
                                                     renderScaleY);

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
                    if ( bpmVal <= 0.0 ) {
                        bpmVal = 120.0;
                        if ( auto session =
                                 EditorEngine::instance().getActiveSession() ) {
                            if ( auto beatmap =
                                     session->getContext().currentBeatmap ) {
                                if ( beatmap->m_baseMapMetadata.preference_bpm >
                                     0.0 ) {
                                    bpmVal = beatmap->m_baseMapMetadata
                                                 .preference_bpm;
                                }
                            }
                        }
                    }
                    if ( bpmVal > 10000.0 ) bpmVal = 10000.0;

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
                        break;
                    }
                }
                if ( !found ) snapshot->hoveredBeatIndex = 0;
            }
        }
    }

    // 记录静态边界 (此时 snapshot->vertices 包含了特效和布局的顶点)
    snapshot->staticVertexCount =
        static_cast<uint32_t>(snapshot->vertices.size());
    snapshot->staticCmdCount = static_cast<uint32_t>(snapshot->cmds.size());

    // --- Phase 2: 动态内容生成 (拍线、音符等) ---
    // 这些内容会受到 UI 线程 yOffset 补偿的影响，从而消除亚帧抖动
    snapshot->trackCount   = trackCount;
    snapshot->renderScaleY = renderScaleY;

    if ( cameraId != "Timeline" ) {
        // 先绘制拍线，使其在物件下方
        bool shouldDrawBeatLines   = config.visual.drawBeatLines;
        bool shouldDrawTimingLines = false;

        if ( cameraId == "Preview" ) {
            // 预览区逻辑：若全局开启，则由预览区具体开关决定；若全局关闭，则强制关闭
            shouldDrawBeatLines   = config.visual.drawBeatLines &&
                                    config.visual.previewConfig.drawBeatLines;
            shouldDrawTimingLines = config.visual.previewConfig.drawTimingLines;
        }

        if ( shouldDrawBeatLines ) {
            NoteRenderSystem::drawBeatLines(batcher,
                                            viewportHeight,
                                            judgmentLineY,
                                            config,
                                            timelineRegistry,
                                            renderTime,
                                            cache,
                                            leftX,
                                            topY,
                                            bottomY,
                                            trackAreaW,
                                            renderScaleY);
        }

        if ( shouldDrawTimingLines ) {
            NoteRenderSystem::drawTimingLines(batcher,
                                              viewportHeight,
                                              judgmentLineY,
                                              config,
                                              renderTime,
                                              cache,
                                              leftX,
                                              topY,
                                              bottomY,
                                              trackAreaW,
                                              renderScaleY);
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
                                      -viewportHeight * 0.5f,
                                      viewportHeight * 1.5f,
                                      singleTrackW,
                                      renderScaleY);
    }

    for ( const auto& box : snapshot->marqueeBoxes ) {
        NoteRenderSystem::renderMarqueeBox(batcher,
                                           box,
                                           judgmentLineY,
                                           leftX,
                                           singleTrackW,
                                           renderScaleY,
                                           cache,
                                           renderTime,
                                           viewportWidth,
                                           viewportHeight);
    }

    // 记录动态顶点数量
    snapshot->dynamicVertexCount =
        static_cast<uint32_t>(snapshot->vertices.size()) -
        snapshot->staticVertexCount;

    // --- Phase 3: 置顶层渲染 (静态或动态) ---
    // 将之前生成的打击特效命令插入到最后，使其绘制在物件上方
    if ( !deferredHitCmds.empty() ) {
        snapshot->cmds.insert(snapshot->cmds.end(),
                              deferredHitCmds.begin(),
                              deferredHitCmds.end());
    }

    // 预览区包围盒：用户要求作为静态元素且在最上层
    if ( cameraId == "Preview" ) {
        auto& skin       = Config::SkinManager::instance();
        auto  boxCol     = skin.getColor("preview.boundingbox");
        auto  lineCol    = skin.getColor("preview.judgeline");
        bool  isDragging = snapshot->isPreviewDragging;

        float mainEffectiveH =
            (config.visual.trackLayout.bottom - config.visual.trackLayout.top) *
            mainViewportHeight;
        float boxDrawH = mainEffectiveH * renderScaleY;

        // 1. [展示中] 始终绘制当前主视窗位置的包围盒 (除非正在拖拽)
        if ( !isDragging ) {
            float boxBottom =
                judgmentLineY + (config.visual.trackLayout.bottom -
                                 config.visual.judgeline_pos) *
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
            float hoverBoxBottom =
                snapshot->previewHoverY + (config.visual.trackLayout.bottom -
                                           config.visual.judgeline_pos) *
                                              mainViewportHeight * renderScaleY;

            auto hoverBoxCol = skin.getColor("preview.hoverbox");
            if ( hoverBoxCol.r == 1.0f && hoverBoxCol.g == 0.0f &&
                 hoverBoxCol.b == 1.0f ) {
                hoverBoxCol = { 1.0f, 1.0f, 0.6f, 0.3f };
            }
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
            batcher.pushQuad(
                leftX,
                snapshot->previewHoverY + 2.0f * 0.5f,
                trackAreaW,
                2.0f,
                { hoverBoxCol.r, hoverBoxCol.g, hoverBoxCol.b, 0.6f });
        }

        // 3. 绘制预览区判定红线 (最上层静态)
        batcher.setTexture(TextureID::None);
        batcher.pushQuad(leftX,
                         judgmentLineY + 2.0f * 0.5f,
                         trackAreaW,
                         2.0f,
                         { lineCol.r, lineCol.g, lineCol.b, lineCol.a });
    }

    batcher.flush();
}

void NoteRenderSystem::renderMarqueeBox(
    Batcher& batcher, const RenderSnapshot::MarqueeBoxSnapshot& box,
    float judgmentLineY, float leftX, float singleTrackW, float renderScaleY,
    const ScrollCache* cache, double renderTime, float viewportWidth,
    float viewportHeight)
{
    float x1 = leftX + box.startTrack * singleTrackW;
    float x2 = leftX + box.endTrack * singleTrackW;

    double currentAbsY = cache->getAbsY(renderTime);
    double startAbsY   = cache->getAbsY(box.startTime);
    double endAbsY     = cache->getAbsY(box.endTime);

    float y1 = judgmentLineY -
               static_cast<float>(startAbsY - currentAbsY) * renderScaleY;
    float y2 = judgmentLineY -
               static_cast<float>(endAbsY - currentAbsY) * renderScaleY;

    float left   = std::min(x1, x2);
    float right  = std::max(x1, x2);
    float top    = std::min(y1, y2);
    float bottom = std::max(y1, y2);
    float w      = right - left;
    float h      = bottom - top;

    if ( w < 1.0f || h < 1.0f ) return;

    auto& settings = Config::AppConfig::instance().getEditorSettings();
    float borderW  = settings.marqueeThickness;
    float cornerR  = settings.marqueeRounding;

    // 重置 scissor 到全屏，确保框选矩形不被轨道裁剪掉
    batcher.setScissor(0, 0, viewportWidth, viewportHeight);
    batcher.setTexture(TextureID::None);

    // 绘制半透明填充
    glm::vec4 fillCol   = { 0.2f, 0.6f, 1.0f, 0.15f };
    glm::vec4 strokeCol = { 0.3f, 0.7f, 1.0f, 0.85f };

    batcher.pushRoundedQuad(left, bottom, w, h, cornerR, fillCol);
    batcher.pushRoundedStrokeRect(
        left, bottom, w, h, cornerR, borderW, strokeCol);
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

    // 2. 绘制当前时间红线 (静态)
    batcher.pushQuad(paddingX,
                     judgmentLineY + 1.0f,
                     lineW,
                     2.0f,
                     { 1.0f, 0.2f, 0.2f, 1.0f });

    // 记录静态边界
    snapshot->staticVertexCount =
        static_cast<uint32_t>(snapshot->vertices.size());
    snapshot->staticCmdCount = static_cast<uint32_t>(snapshot->cmds.size());

    // 3. 绘制 Ticks (动态)
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
        snapshot->timelineElements.push_back({ seg.time,
                                               y,
                                               seg.effects,
                                               seg.bpmEntity,
                                               seg.scrollEntity,
                                               seg.bpmValue,
                                               seg.scrollValue });
    }
}

void NoteRenderSystem::generatePreviewSnapshot(
    RenderSnapshot* snapshot, Batcher& batcher, double currentTime,
    float viewportWidth, float viewportHeight, float judgmentLineY,
    int32_t trackCount, const Config::EditorConfig& config,
    float mainViewportHeight, float& leftX, float& rightX, float& topY,
    float& bottomY, float& trackAreaW, float& singleTrackW, float& renderScaleY)
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

    // 记录静态边界
    snapshot->staticVertexCount =
        static_cast<uint32_t>(snapshot->vertices.size());
    snapshot->staticCmdCount = static_cast<uint32_t>(snapshot->cmds.size());
}

void NoteRenderSystem::generateMainCanvasSnapshot(
    entt::registry& registry, const entt::registry& timelineRegistry,
    RenderSnapshot* snapshot, Batcher& batcher, double currentTime,
    float viewportWidth, float viewportHeight, float judgmentLineY,
    int32_t trackCount, const Config::EditorConfig& config,
    const ScrollCache* cache, float& leftX, float& rightX, float& topY,
    float& bottomY, float& trackAreaW, float& singleTrackW, float renderScaleY)
{
    BackgroundRenderSystem::render(
        batcher, viewportWidth, viewportHeight, config, snapshot);

    // 设置轨道区域裁剪
    float lx = viewportWidth * config.visual.trackLayout.left;
    float rx = viewportWidth * config.visual.trackLayout.right;
    // 扩展垂直方向的裁剪区域，给予上下各 0.5 倍视口的余量
    batcher.setScissor(
        lx, -viewportHeight * 0.5f, rx - lx, viewportHeight * 2.0f);

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
                                            singleTrackW,
                                            renderScaleY);
    }
}

}  // namespace MMM::Logic::System
