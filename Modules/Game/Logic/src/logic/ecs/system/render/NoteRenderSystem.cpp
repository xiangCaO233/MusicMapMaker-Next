#include "logic/ecs/system/NoteRenderSystem.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/BackgroundRenderSystem.h"
#include "logic/ecs/system/CanvasComponentRenderSystem.h"
#include "logic/ecs/system/SampleRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>

#include "logic/ecs/system/HitFXSystem.h"

namespace MMM::Logic::System
{

namespace
{

/// @brief UI 播放补间窗口，单位为 steady-clock 秒。
/// @warning 快照热路径常量；需与 CanvasSnapshotPrepare 保持同步。
constexpr double UI_PLAYBACK_INTERPOLATION_WINDOW_SECONDS = 0.1;

/// @brief Timeline UI 补间安全窗口，单位为 steady-clock 秒。
/// @warning Snapshot hot path constant：Timeline 使用屏幕 Y 补偿，窗口取接近
/// 辅助视图快照间隔的保守值，避免密集 Timing 过度关闭补间。
constexpr double TIMELINE_UI_PLAYBACK_INTERPOLATION_WINDOW_SECONDS =
    1.0 / 120.0;

/// @brief Timeline 专业模式轨道数量。
constexpr int PROFESSIONAL_TIMELINE_LANE_COUNT = 4;

/// @brief 判断当前拖动中的玩家物件是否已进入 BGM 轨道区。
/// @param registry 玩家物件注册表。
/// @param playerTrackCount 玩家轨道数量。
/// @return 至少一个拖动物件或其 Flick 端点进入 BGM 区时返回 true。
/// @warning 主画布快照热路径：仅在拖动期间遍历已固定的局部实体列表，
/// 禁止退化为完整 Registry 扫描。
bool hasDraggedNoteAcrossPlayerBoundary(entt::registry& registry,
                                        std::int32_t    playerTrackCount)
{
    if ( playerTrackCount <= 0 ) return false;
    const auto* pinned = registry.ctx().find<DragRenderPinnedEntities>();
    if ( !pinned || !pinned->entities ) return false;

    const auto crossesBoundary = [playerTrackCount](::MMM::NoteType type,
                                                    std::int32_t    track,
                                                    std::int32_t    dtrack) {
        std::int64_t rightTrack = track;
        if ( type == ::MMM::NoteType::FLICK ) {
            rightTrack = std::max<std::int64_t>(
                rightTrack, static_cast<std::int64_t>(track) + dtrack);
        }
        return rightTrack >= playerTrackCount;
    };

    for ( const auto entity : *pinned->entities ) {
        const auto* note = registry.try_get<const NoteComponent>(entity);
        if ( !note ) continue;
        const auto* interaction =
            registry.try_get<const InteractionComponent>(entity);
        if ( !interaction || !interaction->isDragging ) continue;
        if ( crossesBoundary(
                 note->m_type, note->m_trackIndex, note->m_dtrack) ) {
            return true;
        }
        for ( const auto& subNote : note->m_subNotes ) {
            if ( crossesBoundary(
                     subNote.type, subNote.trackIndex, subNote.dtrack) ) {
                return true;
            }
        }
    }
    return false;
}

/// @brief 获取专业模式中指定 Timing 类型所属的轨道索引。
int professionalTimelineLane(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return 0;
    case ::MMM::TimingEffect::SCROLL: return 1;
    case ::MMM::TimingEffect::JUMP: return 2;
    case ::MMM::TimingEffect::HS: return 3;
    }
    return 0;
}

/// @brief 获取 Timeline Timing 类型的快照效果掩码。
uint32_t timelineEffectMask(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return SCROLL_EFFECT_BPM;
    case ::MMM::TimingEffect::SCROLL: return SCROLL_EFFECT_SCROLL;
    case ::MMM::TimingEffect::JUMP: return SCROLL_EFFECT_JUMP;
    case ::MMM::TimingEffect::HS: return SCROLL_EFFECT_HS;
    }
    return 0;
}

/// @brief 获取 Timeline 元素中指定 Timing 类型的 marker 几何槽。
TimelineInteractiveElement::MarkerGeometry& markerGeometryForEffect(
    TimelineInteractiveElement& element, ::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return element.bpmMarker;
    case ::MMM::TimingEffect::SCROLL: return element.scrollMarker;
    case ::MMM::TimingEffect::JUMP: return element.jumpMarker;
    case ::MMM::TimingEffect::HS: return element.hsMarker;
    }
    return element.scrollMarker;
}

/// @brief 绘制时间线/预览用的小型判定框。
/// @warning 热路径：每个 Timeline/Preview 快照生成时执行；只推送固定数量几何。
void drawJudgmentGuideBox(Batcher& batcher, float leftX, float centerY,
                          float width, float height)
{
    if ( width <= 0.5f || height <= 0.5f ) return;

    auto& skin        = Config::SkinManager::instance();
    auto  fillColor   = skin.getColor("preview.judgment_guide.fill");
    auto  borderColor = skin.getColor("preview.judgment_guide.border");
    constexpr float strokeWidth = 2.0f;

    const float topY    = centerY - height * 0.5f;
    const float bottomY = centerY + height * 0.5f;
    batcher.setTexture(TextureID::None);
    batcher.pushQuad(leftX,
                     bottomY,
                     width,
                     height,
                     { fillColor.r, fillColor.g, fillColor.b, fillColor.a });
    batcher.pushStrokeRect(
        leftX,
        topY,
        leftX + width,
        bottomY,
        strokeWidth,
        { borderColor.r, borderColor.g, borderColor.b, borderColor.a });
}

}  // namespace

/// @brief 生成指定画布的批量渲染快照。
/// @warning 热路径：每帧/每 update 执行；禁止引入文件系统访问、
/// registry 全量无缓存扫描或阻塞同步；Timeline 与活跃主画布 Move
/// 工具会复制 ScrollSegment 供 UI 精确时间映射。
void NoteRenderSystem::generateSnapshot(
    entt::registry& registry, entt::registry& sampleRegistry,
    const std::vector<entt::entity>&             sortedSampleEntities,
    const std::vector<double>&                   sortedSampleMaxEndPrefix,
    const entt::registry&                        timelineRegistry,
    const std::vector<const TimelineComponent*>& bpmEvents,
    RenderSnapshot* snapshot, const std::string& cameraId, double currentTime,
    float viewportWidth, float viewportHeight, float judgmentLineY,
    int32_t trackCount, int32_t bgmTrackCount,
    const Config::EditorConfig& config, float mainViewportHeight,
    HitFXSystem* hitFXSystem)
{
    const bool isMainCanvas = SessionUtils::isMainCanvasCameraId(cameraId);
    const auto normalizeInteractionHitboxScale = [](float scale) {
        if ( !std::isfinite(scale) ) {
            return Config::VisualConfig::DEFAULT_INTERACTION_HITBOX_SCALE;
        }
        return std::clamp(scale,
                          Config::VisualConfig::MIN_INTERACTION_HITBOX_SCALE,
                          Config::VisualConfig::MAX_INTERACTION_HITBOX_SCALE);
    };
    snapshot->interactionHitboxScaleX =
        normalizeInteractionHitboxScale(config.visual.interactionHitboxScaleX);
    snapshot->interactionHitboxScaleY =
        normalizeInteractionHitboxScale(config.visual.interactionHitboxScaleY);

    // 核心同步：如果预览区正在拖拽，主画布渲染的时间应该是预览区当前的悬停时间
    double renderTime = currentTime;
    if ( isMainCanvas && snapshot->isPreviewDragging ) {
        renderTime = snapshot->previewHoverTime;
    }

    const auto* cache = timelineRegistry.ctx().find<ScrollCache>();
    if ( !cache ) return;

    // 将 ScrollCache 指针存入 context 供 renderPolyline 等后续使用
    if ( auto** cacheSlot = registry.ctx().find<const ScrollCache*>() ) {
        *cacheSlot = cache;
    } else {
        registry.ctx().emplace<const ScrollCache*>(cache);
    }

    const double interpolationWindow =
        cameraId == "Timeline"
            ? TIMELINE_UI_PLAYBACK_INTERPOLATION_WINDOW_SECONDS
            : UI_PLAYBACK_INTERPOLATION_WINDOW_SECONDS;
    const double interpolationDuration =
        std::abs(snapshot->playbackSpeed) * interpolationWindow;
    const bool supportsUiPlaybackInterpolation =
        isMainCanvas || cameraId == "Preview" || cameraId == "Timeline";
    const bool canUsePlaybackInterpolation =
        snapshot->isPlaying && !snapshot->isPreviewDragging &&
        supportsUiPlaybackInterpolation && std::isfinite(renderTime) &&
        std::isfinite(interpolationDuration) &&
        cache->canInterpolateLinearly(renderTime, interpolationDuration);
    if ( canUsePlaybackInterpolation ) {
        const double interpolationSpeed =
            cache->getSpeedAt(renderTime) * snapshot->playbackSpeed;
        double interpolationYOffsetScale = 1.0;
        if ( cameraId == "Timeline" ) {
            interpolationYOffsetScale = cache->getHsAt(renderTime);
            if ( !std::isfinite(interpolationYOffsetScale) ) {
                interpolationYOffsetScale = 1.0;
            }
        }
        snapshot->allowUiPlaybackInterpolation =
            std::isfinite(interpolationSpeed) &&
            std::isfinite(interpolationYOffsetScale);
        snapshot->uiInterpolationAbsYSpeed =
            snapshot->allowUiPlaybackInterpolation ? interpolationSpeed : 0.0;
        snapshot->uiInterpolationYOffsetScale =
            snapshot->allowUiPlaybackInterpolation ? interpolationYOffsetScale
                                                   : 1.0;
    } else {
        snapshot->allowUiPlaybackInterpolation = false;
        snapshot->uiInterpolationAbsYSpeed     = 0.0;
        snapshot->uiInterpolationYOffsetScale  = 1.0;
    }

    // Timeline 右键创建事件与主画布 Move 工具空白拖动需要完整映射；
    // 普通播放快照只携带线性补间速度。
    if ( cameraId == "Timeline" ||
         (isMainCanvas && snapshot->acceptsInteraction &&
          snapshot->currentTool == EditTool::Move) ) {
        cache->copyAnimatedSegmentsTo(snapshot->scrollSegments);
    }

    Batcher batcher(snapshot);
    float   leftX = 0, rightX = 0, topY = 0, bottomY = 0, trackAreaW = 0,
            singleTrackW = 0;
    float   renderScaleY = 1.0f;

    // --- Phase 1: 静态布局与打击特效预生成 ---
    // 打击特效顶点不随谱面滚动，因此在静态顶点边界前生成，
    // 绘制命令再按皮肤布局模式插入对应覆盖层。
    uint32_t fxCmdStart = static_cast<uint32_t>(snapshot->cmds.size());
    if ( hitFXSystem && trackCount > 0 &&
         (isMainCanvas || cameraId == "Preview") ) {
        // 提前计算轨道参数
        float tempLX = 0, tempRX = 0, tempTY = 0, tempBY = viewportHeight;
        if ( isMainCanvas ) {
            const auto projection = calculatePlayerTrackProjection(
                viewportWidth,
                trackCount,
                config.visual.trackLayout.left,
                config.visual.trackLayout.right,
                snapshot->canvasHorizontalOffsetX);
            tempLX = projection.leftX;
            tempRX = projection.rightX;
            tempTY = viewportHeight * config.visual.trackLayout.top;
            tempBY = viewportHeight * config.visual.trackLayout.bottom;
        } else {
            tempLX = config.visual.previewConfig.margin.left;
            tempRX = viewportWidth - config.visual.previewConfig.margin.right;
            tempTY = config.visual.previewConfig.margin.top;
            tempBY = viewportHeight - config.visual.previewConfig.margin.bottom;
        }
        float tempSTW = (tempRX - tempLX) / static_cast<float>(trackCount);

        hitFXSystem->generateSnapshot(batcher,
                                      renderTime,
                                      config,
                                      trackCount,
                                      judgmentLineY,
                                      tempLX,
                                      tempTY,
                                      tempBY,
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
                                                   bpmEvents,
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
                                                     bgmTrackCount,
                                                     cache,
                                                     leftX,
                                                     rightX,
                                                     topY,
                                                     bottomY,
                                                     trackAreaW,
                                                     singleTrackW,
                                                     renderScaleY);

        if ( snapshot->isHoveringCanvas || snapshot->isPreviewDragging ) {
            double hoveredTime         = snapshot->isPreviewDragging
                                             ? snapshot->previewHoverTime
                                             : snapshot->hoveredTime;
            snapshot->hoveredBeatIndex = SessionUtils::calculateBeatIndex(
                hoveredTime, bpmEvents, snapshot->fallbackBpm);
        }
    }

    // 整轨光效属于轨道覆盖层：在静态布局之后、拍线和物件之前绘制，
    // 避免半透明渐变覆盖物件本身；固定尺寸特效继续在 Phase 3 置顶。
    if ( Config::SkinManager::instance().getHitEffectLayoutMode() ==
             Config::HitEffectLayoutMode::TrackFill &&
         !deferredHitCmds.empty() ) {
        batcher.flush();
        snapshot->cmds.insert(snapshot->cmds.end(),
                              deferredHitCmds.begin(),
                              deferredHitCmds.end());
        deferredHitCmds.clear();
    }

    // 记录静态边界 (此时 snapshot->vertices 包含了特效和布局的顶点)
    if ( cameraId != "Timeline" ) {
        snapshot->staticVertexCount =
            static_cast<uint32_t>(snapshot->vertices.size());
        snapshot->staticCmdCount = static_cast<uint32_t>(snapshot->cmds.size());
    }

    // --- Phase 2: 动态内容生成 (拍线、音符等) ---
    // 这些内容会受到 UI 线程 yOffset 补偿的影响，从而消除亚帧抖动
    snapshot->trackCount   = trackCount;
    snapshot->renderScaleY = renderScaleY;

    if ( cameraId != "Timeline" ) {
        batcher.setScissor(leftX, topY, trackAreaW, bottomY - topY);
        // 先绘制拍线，使其在物件下方
        const bool beatLinesHidden       = config.visual.beatLineDisplayMode ==
                                           Config::BeatLineDisplayMode::Hidden;
        bool       shouldDrawBeatLines   = !beatLinesHidden;
        bool       shouldDrawTimingLines = false;
        bool       revealBeatLinesNearCursor =
            config.visual.beatLineDisplayMode ==
            Config::BeatLineDisplayMode::NearCursor;

        if ( cameraId == "Preview" ) {
            // 预览区保留自身开关；自动渐隐只应用到具备精确编辑光标的主画布。
            shouldDrawBeatLines =
                !beatLinesHidden && config.visual.previewConfig.drawBeatLines;
            revealBeatLinesNearCursor = false;
            shouldDrawTimingLines = config.visual.previewConfig.drawTimingLines;
        }

        if ( shouldDrawBeatLines ) {
            NoteRenderSystem::drawBeatLines(batcher,
                                            viewportHeight,
                                            judgmentLineY,
                                            config,
                                            bpmEvents,
                                            renderTime,
                                            cache,
                                            leftX,
                                            topY,
                                            bottomY,
                                            trackAreaW,
                                            renderScaleY,
                                            revealBeatLinesNearCursor,
                                            1.0F,
                                            isMainCanvas);
        }

        if ( isMainCanvas && shouldDrawBeatLines ) {
            const auto laneProjection =
                calculateCanvasLaneProjection(viewportWidth,
                                              trackCount,
                                              bgmTrackCount,
                                              config.visual.trackLayout.left,
                                              config.visual.trackLayout.right,
                                              snapshot->canvasHorizontalOffsetX,
                                              true,
                                              config.settings.enableBmsEditing);
            const float visibleLeft = std::max(0.0F, laneProjection.bgmLeftX);
            const float visibleRight =
                std::min(viewportWidth, laneProjection.bgmRightX);
            if ( visibleRight > visibleLeft ) {
                batcher.setScissor(visibleLeft,
                                   topY,
                                   visibleRight - visibleLeft,
                                   bottomY - topY);
                NoteRenderSystem::drawBeatLines(batcher,
                                                viewportHeight,
                                                judgmentLineY,
                                                config,
                                                bpmEvents,
                                                renderTime,
                                                cache,
                                                visibleLeft,
                                                topY,
                                                bottomY,
                                                visibleRight - visibleLeft,
                                                renderScaleY,
                                                revealBeatLinesNearCursor,
                                                0.28F,
                                                false);
                batcher.setScissor(leftX, topY, trackAreaW, bottomY - topY);
            }
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

        float noteRenderRightX = rightX;
        if ( isMainCanvas &&
             hasDraggedNoteAcrossPlayerBoundary(registry, trackCount) ) {
            const auto laneProjection =
                calculateCanvasLaneProjection(viewportWidth,
                                              trackCount,
                                              bgmTrackCount,
                                              config.visual.trackLayout.left,
                                              config.visual.trackLayout.right,
                                              snapshot->canvasHorizontalOffsetX,
                                              true,
                                              config.settings.enableBmsEditing);
            noteRenderRightX = laneProjection.bgmRightX;
            batcher.setScissor(0.0F, topY, viewportWidth, bottomY - topY);
        } else {
            batcher.setScissor(leftX, topY, trackAreaW, bottomY - topY);
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
                                      noteRenderRightX,
                                      topY,
                                      bottomY,
                                      singleTrackW,
                                      renderScaleY);
        if ( isMainCanvas ) {
            const auto laneProjection =
                calculateCanvasLaneProjection(viewportWidth,
                                              trackCount,
                                              bgmTrackCount,
                                              config.visual.trackLayout.left,
                                              config.visual.trackLayout.right,
                                              snapshot->canvasHorizontalOffsetX,
                                              true,
                                              config.settings.enableBmsEditing);
            SampleRenderSystem::renderSamples(sampleRegistry,
                                              sortedSampleEntities,
                                              sortedSampleMaxEndPrefix,
                                              snapshot,
                                              batcher,
                                              laneProjection,
                                              cache,
                                              config,
                                              renderTime,
                                              judgmentLineY,
                                              viewportWidth,
                                              topY,
                                              bottomY,
                                              renderScaleY);
        }
        if ( cameraId == "Preview" ) {
            float lx = config.visual.previewConfig.margin.left;
            float rx = viewportWidth - config.visual.previewConfig.margin.right;
            float ty = config.visual.previewConfig.margin.top;
            float by =
                viewportHeight - config.visual.previewConfig.margin.bottom;
            batcher.setScissor(lx, ty, rx - lx, by - ty);
        } else {
            batcher.setScissor(leftX,
                               -viewportHeight * 0.5f,
                               trackAreaW,
                               viewportHeight * 2.0f);
        }
        if ( isMainCanvas && config.visual.debugDrawHitboxes ) {
            NoteRenderSystem::debugRenderHitboxes(batcher, snapshot);
        }
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
    if ( cameraId != "Timeline" ) {
        snapshot->dynamicVertexCount =
            static_cast<uint32_t>(snapshot->vertices.size()) -
            snapshot->staticVertexCount;
    }

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

        // 3. 绘制预览区判定框 (最上层静态)
        batcher.flush();
        drawJudgmentGuideBox(batcher, leftX, judgmentLineY, trackAreaW, 18.0f);
    }

    batcher.flush();

    if ( isMainCanvas ) {
        const CanvasComponentRenderContext componentContext{
            .currentTime    = renderTime,
            .viewportWidth  = viewportWidth,
            .viewportHeight = viewportHeight,
            .judgmentLineY  = judgmentLineY,
            .visibleTop     = topY,
            .visibleBottom  = bottomY,
            .renderScaleY   = renderScaleY,
            .beatDivisor    = config.settings.beatDivisor,
            .trackCount     = trackCount,
            .trackLeft      = config.visual.trackLayout.left,
            .trackRight     = config.visual.trackLayout.right,
            .trackKps       = hitFXSystem ? hitFXSystem->trackKps()
                                          : std::span<const std::uint32_t>{},
            .bpmEvents      = bpmEvents,
            .scrollCache    = cache,
        };
        CanvasComponentRenderSystem::render(
            snapshot, componentContext, config.visual.canvasComponents);
    }
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
    RenderSnapshot*                              snapshot,
    const std::vector<const TimelineComponent*>& bpmEvents, Batcher& batcher,
    double currentTime, float viewportWidth, float viewportHeight,
    float judgmentLineY, const Config::EditorConfig& config,
    const ScrollCache* cache)
{
    if ( !snapshot->hasBeatmap ) return;

    batcher.setTexture(TextureID::None);

    // 绘制背景 (确保全覆盖，消除透明混合带来的边缘可疑像素)
    batcher.pushQuad(
        0, viewportHeight, viewportWidth, viewportHeight, { 0, 0, 0, 0.01f });

    auto&      skin             = Config::SkinManager::instance();
    auto       tickCol          = skin.getColor("timeline.tick");
    const bool professionalMode = Config::AppConfig::instance()
                                      .getEditorSettings()
                                      .timelineProfessionalMode;

    double currentAbsY = cache->getVisualAnchorAbsY(currentTime);

    float paddingX = 30.0f;
    float lineW    = std::max(1.0f, viewportWidth - paddingX * 2.0f);

    if ( professionalMode ) {
        constexpr glm::vec4 laneColors[PROFESSIONAL_TIMELINE_LANE_COUNT] = {
            { 1.0f, 0.28f, 0.28f, 0.20f },
            { 0.28f, 1.0f, 0.38f, 0.18f },
            { 0.34f, 0.55f, 1.0f, 0.18f },
            { 1.0f, 0.87f, 0.28f, 0.18f },
        };
        const float laneWidth =
            viewportWidth /
            static_cast<float>(PROFESSIONAL_TIMELINE_LANE_COUNT);
        batcher.setTexture(TextureID::None);
        for ( int lane = 0; lane < PROFESSIONAL_TIMELINE_LANE_COUNT; ++lane ) {
            const float laneX = laneWidth * static_cast<float>(lane);
            batcher.pushQuad(laneX,
                             viewportHeight,
                             lane == PROFESSIONAL_TIMELINE_LANE_COUNT - 1
                                 ? viewportWidth - laneX
                                 : laneWidth,
                             viewportHeight,
                             laneColors[lane]);
            if ( lane > 0 ) {
                batcher.pushQuad(laneX,
                                 viewportHeight,
                                 1.0f,
                                 viewportHeight,
                                 { 1.0f, 1.0f, 1.0f, 0.16f });
            }
        }
    }

    // 记录静态边界
    snapshot->staticVertexCount =
        static_cast<uint32_t>(snapshot->vertices.size());
    snapshot->staticCmdCount = static_cast<uint32_t>(snapshot->cmds.size());

    auto getBeatLineConfig = [&](int denominator) {
        if ( denominator <= 0 ) denominator = 1;
        std::string   key = "beat_lines.beat_" + std::to_string(denominator);
        Config::Color c   = skin.getColor(key);
        if ( c.r == 1.0f && c.g == 0.0f && c.b == 1.0f && c.a == 1.0f ) {
            c   = skin.getColor("beat_lines.default");
            key = "beat_lines_width.default";
        } else {
            key = "beat_lines_width.beat_" + std::to_string(denominator);
        }

        float width =
            skin.getValue(key, skin.getValue("beat_lines_width.default", 2.0f));
        if ( config.visual.overrideBeatLineColors ) {
            const auto& overrideColor =
                config.visual.beatLineColors[Config::beatLineColorPaletteSlot(
                    denominator)];
            c = { overrideColor[0],
                  overrideColor[1],
                  overrideColor[2],
                  overrideColor[3] };
        }
        return std::pair{
            glm::vec4(c.r, c.g, c.b, c.a * config.visual.beatLineAlpha), width
        };
    };

    // 3. 绘制 Timeline 自身的分拍线
    /// @brief 绘制 Timeline 自身的分拍线；bpmEvents 由 SessionContext
    /// 脏标记缓存维护，避免热路径完整遍历和排序。
    int beatDivisor = config.settings.beatDivisor;
    if ( beatDivisor <= 0 ) beatDivisor = 4;

    if ( !bpmEvents.empty() ) {
        double topAbsY       = currentAbsY + judgmentLineY;
        double bottomAbsY    = currentAbsY + judgmentLineY - viewportHeight;
        auto   visibleRanges = cache->getTimeRangesForAbsYWindow(
            std::min(topAbsY, bottomAbsY), std::max(topAbsY, bottomAbsY));

        batcher.setTexture(TextureID::None);
        for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
            const auto* currentBPM = bpmEvents[i];
            double      bpmTime    = currentBPM->m_timestamp;
            double      bpmVal     = currentBPM->m_value;
            if ( bpmVal <= 0.0 ) {
                bpmVal = snapshot->fallbackBpm;
            }
            if ( bpmVal > 10000.0 ) bpmVal = 10000.0;
            if ( bpmVal <= 0.0 ) bpmVal = 120.0;

            double nextBpmTime  = (i + 1 < bpmEvents.size())
                                      ? bpmEvents[i + 1]->m_timestamp
                                      : std::numeric_limits<double>::infinity();
            double beatDuration = 60.0 / bpmVal;
            double stepDuration = beatDuration / beatDivisor;

            for ( const auto& [startTime, endTime] : visibleRanges ) {
                if ( nextBpmTime <= startTime ) continue;
                double segmentStartTime = bpmTime;
                if ( i == 0 && config.visual.drawBeatLinesBeforeFirstTiming ) {
                    segmentStartTime = startTime;
                }
                if ( segmentStartTime >= endTime ) continue;

                double  startCalcTime = std::max(segmentStartTime, startTime);
                int64_t stepOffset    = 0;
                if ( startCalcTime > bpmTime ) {
                    stepOffset = static_cast<int64_t>(std::ceil(
                        (startCalcTime - bpmTime) / stepDuration - 1e-4));
                } else if ( startCalcTime < bpmTime ) {
                    stepOffset = static_cast<int64_t>(std::floor(
                        (startCalcTime - bpmTime) / stepDuration + 1e-4));
                }

                double t = bpmTime + stepOffset * stepDuration;
                while ( t < startCalcTime - 1e-4 ) {
                    stepOffset++;
                    t = bpmTime + stepOffset * stepDuration;
                }
                while ( t < nextBpmTime && t <= endTime ) {
                    int beatIndex = static_cast<int>(stepOffset % beatDivisor);
                    if ( beatIndex < 0 ) beatIndex += beatDivisor;
                    int denominator = 1;
                    if ( beatIndex != 0 ) {
                        int gcd     = std::gcd(beatIndex, beatDivisor);
                        denominator = beatDivisor / gcd;
                    }

                    auto [color, width] = getBeatLineConfig(denominator);
                    float y = judgmentLineY -
                              static_cast<float>(
                                  cache->getDisplayDelta(t, currentAbsY, t));
                    if ( y >= 0.0f && y <= viewportHeight ) {
                        color.a *= 0.75f;
                        const float beatLineX =
                            professionalMode ? 0.0f : paddingX;
                        const float beatLineW =
                            professionalMode ? viewportWidth : lineW;
                        batcher.setTexture(TextureID::None);
                        if ( snapshot->isSnapped &&
                             std::abs(t - snapshot->snappedTime) < 1e-6 ) {
                            glm::vec4 glowCol = color;
                            glowCol.a *= 0.6f;
                            batcher.pushQuad(beatLineX,
                                             y + (width + 4.0f) * 0.5f,
                                             beatLineW,
                                             width + 4.0f,
                                             glowCol);
                            glowCol.a *= 0.5f;
                            batcher.pushQuad(beatLineX,
                                             y + (width + 10.0f) * 0.5f,
                                             beatLineW,
                                             width + 10.0f,
                                             glowCol);
                            glowCol.a *= 0.5f;
                            batcher.pushQuad(beatLineX,
                                             y + (width + 20.0f) * 0.5f,
                                             beatLineW,
                                             width + 20.0f,
                                             glowCol);
                        }
                        batcher.pushQuad(beatLineX,
                                         y + width * 0.5f,
                                         beatLineW,
                                         width,
                                         color);
                    }

                    stepOffset++;
                    t = bpmTime + stepOffset * stepDuration;
                }
            }
        }
    }

    // 5. 绘制 Timing 事件为普通 Note 形状。
    const float professionalLaneWidth =
        viewportWidth / static_cast<float>(PROFESSIONAL_TIMELINE_LANE_COUNT);
    float noteW =
        professionalMode ? std::max(1.0f, professionalLaneWidth - 2.0f) : lineW;
    float noteH = noteW * 0.36f;
    if ( auto uvIt =
             snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Note));
         uvIt != snapshot->uvMap.end() && uvIt->second.w > 0.0f ) {
        noteH = noteW * (uvIt->second.w / uvIt->second.z);
    }
    float noteX = paddingX;

    int markerRows =
        std::max(1, static_cast<int>(std::ceil(viewportHeight)) + 1);
    const int markerLaneCount =
        professionalMode ? PROFESSIONAL_TIMELINE_LANE_COUNT : 1;
    std::vector<uint8_t> occupiedMarkerRows(
        static_cast<size_t>(markerRows * markerLaneCount), 0);
    auto occupyMarkerRow = [&](int lane, float y) {
        if ( y < 0.0f || y > viewportHeight ) return false;
        int row            = static_cast<int>(std::floor(y));
        row                = std::clamp(row, 0, markerRows - 1);
        lane               = std::clamp(lane, 0, markerLaneCount - 1);
        const size_t index = static_cast<size_t>(lane * markerRows + row);
        if ( occupiedMarkerRows[index] != 0 ) {
            return false;
        }
        occupiedMarkerRows[index] = 1;
        return true;
    };

    auto markerColorForEffect = [&](::MMM::TimingEffect effect) {
        switch ( effect ) {
        case ::MMM::TimingEffect::BPM:
            return glm::vec4{ 1.0f, 0.2f, 0.2f, 0.8f };
        case ::MMM::TimingEffect::SCROLL:
            return glm::vec4{ 0.2f, 1.0f, 0.2f, 0.8f };
        case ::MMM::TimingEffect::JUMP:
            return glm::vec4{ 0.2f, 0.45f, 1.0f, 0.8f };
        case ::MMM::TimingEffect::HS:
            return glm::vec4{ 1.0f, 0.85f, 0.2f, 0.8f };
        }
        return glm::vec4{ tickCol.r, tickCol.g, tickCol.b, 0.8f };
    };

    auto writeMarkerGeometry =
        [&](TimelineInteractiveElement::MarkerGeometry& geometry,
            uint32_t                                    markerVertexOffset,
            uint32_t                                    markerIndexOffset) {
            geometry.hasMarkerGeometry  = true;
            geometry.markerVertexOffset = markerVertexOffset;
            geometry.markerVertexCount  = static_cast<uint32_t>(
                snapshot->vertices.size() - markerVertexOffset);
            geometry.markerIndexOffset = markerIndexOffset;
            geometry.markerIndexCount  = static_cast<uint32_t>(
                snapshot->indices.size() - markerIndexOffset);
        };

    for ( const auto& seg : cache->getSegments() ) {
        if ( seg.effects == 0 ) continue;

        const double segmentAbsY = seg.absY * cache->getAnimatedZoomScale();
        float y = judgmentLineY -
                  static_cast<float>((segmentAbsY - currentAbsY) * seg.hs);

        TimelineInteractiveElement el;
        el.time         = seg.time;
        el.y            = y;
        el.effects      = seg.effects;
        el.bpmEntity    = seg.bpmEntity;
        el.scrollEntity = seg.scrollEntity;
        el.jumpEntity   = seg.jumpEntity;
        el.hsEntity     = seg.hsEntity;
        el.bpmValue     = seg.bpmValue;
        el.scrollValue  = seg.scrollValue;
        el.jumpValue    = seg.jumpValue;
        el.hsValue      = seg.hsValue;
        snapshot->timelineElements.push_back(el);
        size_t interactiveElementIdx = snapshot->timelineElements.size() - 1;

        if ( professionalMode ) {
            constexpr ::MMM::TimingEffect professionalEffects[] = {
                ::MMM::TimingEffect::BPM,
                ::MMM::TimingEffect::SCROLL,
                ::MMM::TimingEffect::JUMP,
                ::MMM::TimingEffect::HS,
            };
            for ( auto effect : professionalEffects ) {
                if ( (seg.effects & timelineEffectMask(effect)) == 0 ) {
                    continue;
                }

                const int lane = professionalTimelineLane(effect);
                if ( !occupyMarkerRow(lane, y) ) {
                    continue;
                }

                noteX = professionalLaneWidth * static_cast<float>(lane) +
                        (professionalLaneWidth - noteW) * 0.5f;
                batcher.setTexture(TextureID::Note);
                const uint32_t markerVertexOffset =
                    static_cast<uint32_t>(snapshot->vertices.size());
                const uint32_t markerIndexOffset =
                    static_cast<uint32_t>(snapshot->indices.size());
                batcher.pushFilledQuad(noteX,
                                       y + noteH * 0.5f,
                                       noteW,
                                       noteH,
                                       { 1.0f, 1.0f },
                                       config.visual.noteFillMode,
                                       markerColorForEffect(effect));

                auto& element =
                    snapshot->timelineElements[interactiveElementIdx];
                auto& geometry = markerGeometryForEffect(element, effect);
                writeMarkerGeometry(
                    geometry, markerVertexOffset, markerIndexOffset);
                if ( !element.hasMarkerGeometry ) {
                    element.hasMarkerGeometry  = geometry.hasMarkerGeometry;
                    element.markerVertexOffset = geometry.markerVertexOffset;
                    element.markerVertexCount  = geometry.markerVertexCount;
                    element.markerIndexOffset  = geometry.markerIndexOffset;
                    element.markerIndexCount   = geometry.markerIndexCount;
                }
            }
            continue;
        }

        if ( !occupyMarkerRow(0, y) ) continue;

        glm::vec4 color = { tickCol.r, tickCol.g, tickCol.b, 0.8f };
        if ( (seg.effects & SCROLL_EFFECT_BPM) &&
             (seg.effects & SCROLL_EFFECT_SCROLL) ) {
            color = { 1.0f, 0.5f, 0.0f, 0.8f };
        } else if ( seg.effects & SCROLL_EFFECT_BPM ) {
            color = markerColorForEffect(::MMM::TimingEffect::BPM);
        } else if ( seg.effects & SCROLL_EFFECT_JUMP ) {
            color = markerColorForEffect(::MMM::TimingEffect::JUMP);
        } else if ( seg.effects & SCROLL_EFFECT_HS ) {
            color = markerColorForEffect(::MMM::TimingEffect::HS);
        } else if ( seg.effects & SCROLL_EFFECT_SCROLL ) {
            color = markerColorForEffect(::MMM::TimingEffect::SCROLL);
        }

        batcher.setTexture(TextureID::Note);
        const uint32_t markerVertexOffset =
            static_cast<uint32_t>(snapshot->vertices.size());
        const uint32_t markerIndexOffset =
            static_cast<uint32_t>(snapshot->indices.size());
        batcher.pushFilledQuad(noteX,
                               y + noteH * 0.5f,
                               noteW,
                               noteH,
                               { 1.0f, 1.0f },
                               config.visual.noteFillMode,
                               color);
        auto& element = snapshot->timelineElements[interactiveElementIdx];
        element.hasMarkerGeometry  = true;
        element.markerVertexOffset = markerVertexOffset;
        element.markerVertexCount  = static_cast<uint32_t>(
            snapshot->vertices.size() - markerVertexOffset);
        element.markerIndexOffset = markerIndexOffset;
        element.markerIndexCount =
            static_cast<uint32_t>(snapshot->indices.size() - markerIndexOffset);
        if ( seg.effects & SCROLL_EFFECT_BPM ) {
            writeMarkerGeometry(
                element.bpmMarker, markerVertexOffset, markerIndexOffset);
        }
        if ( seg.effects & SCROLL_EFFECT_SCROLL ) {
            writeMarkerGeometry(
                element.scrollMarker, markerVertexOffset, markerIndexOffset);
        }
        if ( seg.effects & SCROLL_EFFECT_JUMP ) {
            writeMarkerGeometry(
                element.jumpMarker, markerVertexOffset, markerIndexOffset);
        }
        if ( seg.effects & SCROLL_EFFECT_HS ) {
            writeMarkerGeometry(
                element.hsMarker, markerVertexOffset, markerIndexOffset);
        }
    }

    snapshot->dynamicVertexCount =
        static_cast<uint32_t>(snapshot->vertices.size()) -
        snapshot->staticVertexCount;

    // 6. 绘制当前时间判定框，作为时间线最上层覆盖物。
    batcher.flush();
    drawJudgmentGuideBox(batcher, paddingX, judgmentLineY, lineW, 18.0f);
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
    int32_t bgmTrackCount, const ScrollCache* cache, float& leftX,
    float& rightX, float& topY, float& bottomY, float& trackAreaW,
    float& singleTrackW, float renderScaleY)
{
    BackgroundRenderSystem::render(
        batcher, viewportWidth, viewportHeight, config, snapshot);

    if ( !snapshot->hasBeatmap ) {
        // Logo 属于完整画布占位内容，不应继承轨道布局的水平裁剪范围。
        batcher.setScissor(0.0f, 0.0f, viewportWidth, viewportHeight);
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
        // 谱面布局只允许在轨道水平范围内生成基础绘制命令。
        const auto projection =
            calculatePlayerTrackProjection(viewportWidth,
                                           trackCount,
                                           config.visual.trackLayout.left,
                                           config.visual.trackLayout.right,
                                           snapshot->canvasHorizontalOffsetX);
        const float lx = projection.leftX;
        const float rx = projection.rightX;
        // 扩展垂直方向的裁剪区域，给予上下各 0.5 倍视口的余量。
        batcher.setScissor(
            lx, -viewportHeight * 0.5f, rx - lx, viewportHeight * 2.0f);
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
        const auto laneProjection =
            calculateCanvasLaneProjection(viewportWidth,
                                          trackCount,
                                          bgmTrackCount,
                                          config.visual.trackLayout.left,
                                          config.visual.trackLayout.right,
                                          snapshot->canvasHorizontalOffsetX,
                                          true,
                                          config.settings.enableBmsEditing);
        SampleRenderSystem::renderLaneLayout(batcher,
                                             laneProjection,
                                             bgmTrackCount,
                                             viewportWidth,
                                             topY,
                                             bottomY);
    }
}

void NoteRenderSystem::debugRenderHitboxes(Batcher&        batcher,
                                           RenderSnapshot* snapshot)
{
    if ( !snapshot ) return;

    batcher.setTexture(TextureID::None);
    for ( const auto& rawHitbox : snapshot->hitboxes ) {
        const auto hb =
            scaleInteractionHitbox(rawHitbox,
                                   snapshot->interactionHitboxScaleX,
                                   snapshot->interactionHitboxScaleY);
        if ( hb.entity == entt::null || hb.w <= 0.0f || hb.h <= 0.0f ) continue;

        glm::vec4 color{ 0.2f, 0.9f, 1.0f, 0.8f };
        switch ( hb.part ) {
        case HoverPart::Head: color = { 0.25f, 1.0f, 0.25f, 0.85f }; break;
        case HoverPart::HoldBody: color = { 0.0f, 0.8f, 1.0f, 0.75f }; break;
        case HoverPart::HoldEnd: color = { 1.0f, 0.65f, 0.1f, 0.85f }; break;
        case HoverPart::FlickArrow: color = { 1.0f, 0.25f, 1.0f, 0.85f }; break;
        case HoverPart::PolylineNode:
            color = { 1.0f, 1.0f, 0.15f, 0.85f };
            break;
        case HoverPart::SampleAnchor:
            color = { 0.25f, 0.85f, 1.0f, 0.9f };
            break;
        case HoverPart::SampleOffset: color = { 1.0f, 0.5f, 0.2f, 0.9f }; break;
        case HoverPart::None: color = { 1.0f, 1.0f, 1.0f, 0.45f }; break;
        }

        batcher.pushQuad(hb.x,
                         hb.y + hb.h,
                         hb.w,
                         hb.h,
                         { color.r, color.g, color.b, 0.08f });
        batcher.pushStrokeRect(
            hb.x, hb.y, hb.x + hb.w, hb.y + hb.h, 2.0f, color);
    }
}

}  // namespace MMM::Logic::System
