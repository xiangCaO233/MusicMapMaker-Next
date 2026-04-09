#include "logic/ecs/system/NoteRenderSystem.h"
#include "config/skin/SkinConfig.h"
#include "imgui.h"
#include "logic/ecs/system/BackgroundRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
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
        NoteRenderSystem::generateTimelineSnapshot(snapshot,
                                                   batcher,
                                                   currentTime,
                                                   viewportWidth,
                                                   viewportHeight,
                                                   judgmentLineY,
                                                   config,
                                                   cache);
    } else if ( cameraId == "Preview" ) {
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
        NoteRenderSystem::generateMainCanvasSnapshot(registry,
                                                     timelineRegistry,
                                                     snapshot,
                                                     batcher,
                                                     currentTime,
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
    }

    // 统一绘制音符
    if ( snapshot->hasBeatmap && cameraId != "Timeline" ) {
        NoteRenderSystem::renderNotes(registry,
                                      snapshot,
                                      cameraId,
                                      currentTime,
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
    }

    /*
    // --- 调试：绘制 Hitbox ---
    NoteRenderSystem::debugRenderHitboxes(batcher, snapshot);
    */

    batcher.flush();
}

/*
void NoteRenderSystem::debugRenderHitboxes(Batcher&        batcher,
                                           RenderSnapshot* snapshot)
{
    batcher.setTexture(TextureID::None);
    for ( const auto& box : snapshot->hitboxes ) {
        glm::vec4 color;
        switch ( box.part ) {
        case HoverPart::Head: color = { 1.0f, 0.0f, 0.0f, 0.8f }; break;
        case HoverPart::HoldBody: color = { 0.0f, 1.0f, 0.0f, 0.8f }; break;
        case HoverPart::PolylineNode: color = { 0.0f, 0.0f, 1.0f, 0.8f }; break;
        case HoverPart::FlickArrow: color = { 1.0f, 1.0f, 0.0f, 0.8f }; break;
        default: color = { 1.0f, 1.0f, 1.0f, 0.8f }; break;
        }
        batcher.pushStrokeRect(
            box.x, box.y, box.x + box.w, box.y + box.h, 2.0f, color);
    }
}
*/

void NoteRenderSystem::generateTimelineSnapshot(
    RenderSnapshot* snapshot, Batcher& batcher, double currentTime,
    float viewportWidth, float viewportHeight, float judgmentLineY,
    const Config::EditorConfig& config, const ScrollCache* cache)
{
    if ( !snapshot->hasBeatmap ) return;

    batcher.setTexture(TextureID::None);
    auto& skin    = Config::SkinManager::instance();
    auto  tickCol = skin.getColor("timeline.tick");

    double currentAbsY = cache->getAbsY(currentTime);
    double startTime =
        cache->getTime(currentAbsY - (viewportHeight - judgmentLineY));
    double endTime = cache->getTime(currentAbsY + judgmentLineY);

    for ( const auto& seg : cache->getSegments() ) {
        if ( seg.time < startTime || seg.time > endTime ) continue;

        float y = judgmentLineY - static_cast<float>(seg.absY - currentAbsY);
        float paddingX = 30.0f;
        float lineW    = viewportWidth - paddingX * 2.0f;

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
        snapshot->timelineElements.push_back({ seg.time, y, seg.effects });
    }

    batcher.pushQuad(0,
                     judgmentLineY + 1.0f,
                     viewportWidth,
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
    float boxDrawH                = mainEffectiveH * renderScaleY;
    float boxTop =
        mainJudgelineInPreviewY -
        (config.visual.judgeline_pos - config.visual.trackLayout.top) *
            mainViewportHeight * renderScaleY;

    batcher.setTexture(TextureID::None);
    batcher.pushQuad(leftX,
                     boxTop + boxDrawH,
                     trackAreaW,
                     boxDrawH,
                     { boxCol.r, boxCol.g, boxCol.b, boxCol.a });
    batcher.pushStrokeRect(leftX,
                           boxTop,
                           rightX,
                           boxTop + boxDrawH,
                           2.0f,
                           { boxCol.r, boxCol.g, boxCol.b, 1.0f });
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
