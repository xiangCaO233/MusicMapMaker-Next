#include "logic/ecs/system/NoteRenderSystem.h"
#include "config/skin/SkinConfig.h"
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

    // 将 ScrollCache 指针存入 context
    registry.ctx().erase<const ScrollCache*>();
    registry.ctx().emplace<const ScrollCache*>(cache);

    Batcher batcher(snapshot);
    float   leftX, rightX, topY, bottomY, trackAreaW, singleTrackW;
    float   renderScaleY = 1.0f;

    if ( cameraId == "Timeline" ) {
        if ( !snapshot->hasBeatmap ) {
            batcher.flush();
            return;
        }

        // ================== 时间线标尺逻辑 ==================
        leftX        = 0;
        rightX       = viewportWidth;
        topY         = 0;
        bottomY      = viewportHeight;
        trackAreaW   = viewportWidth;
        singleTrackW = viewportWidth;

        batcher.setTexture(TextureID::None);
        auto& skin    = Config::SkinManager::instance();
        auto  tickCol = skin.getColor("timeline.tick");

        double currentAbsY = cache->getAbsY(currentTime);

        // 绘制刻度线
        // 我们从当前视图底部对应的时间开始，向上遍历到顶部对应的时间
        double startTime =
            cache->getTime(currentAbsY - (viewportHeight - judgmentLineY));
        double endTime = cache->getTime(currentAbsY + judgmentLineY);

        for ( const auto& seg : cache->getSegments() ) {
            if ( seg.time < startTime || seg.time > endTime ) continue;

            // 计算在视口中的 Y (基于判定线位置)
            float y =
                judgmentLineY - static_cast<float>(seg.absY - currentAbsY);

            // 绘制横向刻度线 (BPM/Scroll 变化点)
            batcher.pushQuad(0,
                             y + 1.0f,
                             viewportWidth,
                             2.0f,
                             { tickCol.r, tickCol.g, tickCol.b, 0.8f });
        }

        // 绘制当前时间指示线 (亮红)
        batcher.pushQuad(0,
                         judgmentLineY + 1.0f,
                         viewportWidth,
                         2.0f,
                         { 1.0f, 0.2f, 0.2f, 1.0f });

    } else if ( cameraId == "Preview" ) {
        if ( !snapshot->hasBeatmap ) {
            batcher.flush();
            return;
        }

        // ================== 预览区逻辑 ==================
        leftX      = config.visual.previewConfig.margin.left;
        rightX     = viewportWidth - config.visual.previewConfig.margin.right;
        topY       = config.visual.previewConfig.margin.top;
        bottomY    = viewportHeight - config.visual.previewConfig.margin.bottom;
        trackAreaW = rightX - leftX;
        singleTrackW = trackAreaW / static_cast<float>(trackCount);

        float mainEffectiveH =
            (config.visual.trackLayout.bottom - config.visual.trackLayout.top) *
            mainViewportHeight;
        float previewDrawH = bottomY - topY;
        renderScaleY = previewDrawH /
                       (mainEffectiveH * config.visual.previewConfig.areaRatio);

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
    } else {
        // ================== 主画布逻辑 ==================
        BackgroundRenderSystem::render(
            batcher, viewportWidth, viewportHeight, config, snapshot);

        if ( !snapshot->hasBeatmap ) {
            // 未加载谱面时，主画布只绘制 Logo
            batcher.setTexture(TextureID::Logo);
            float logoSize = std::min(viewportWidth, viewportHeight) * 0.4f;
            float cx       = viewportWidth * 0.5f;
            float cy       = viewportHeight * 0.5f;
            // 绘制半透明居中 Logo
            batcher.pushQuad(cx - logoSize * 0.5f,
                             cy - logoSize * 0.5f,
                             logoSize,
                             logoSize,
                             { 1.0f, 1.0f, 1.0f, 0.15f });
        } else {
            renderTrackLayout(batcher,
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

    if ( snapshot->hasBeatmap ) {
        // 统一绘制音符
        renderNotes(registry,
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

    batcher.flush();
}

}  // namespace MMM::Logic::System
