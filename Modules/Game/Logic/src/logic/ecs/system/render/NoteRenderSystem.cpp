#include "logic/ecs/system/NoteRenderSystem.h"
#include "config/skin/SkinConfig.h"
#include "logic/ecs/system/BackgroundRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"

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

    if ( cameraId == "Preview" ) {
        // 预览区逻辑：不使用主画布的轨道配置，使用预览区留白配置
        leftX        = config.visual.previewConfig.margin.left;
        rightX       = viewportWidth - config.visual.previewConfig.margin.right;
        topY         = config.visual.previewConfig.margin.top;
        bottomY      = viewportHeight - config.visual.previewConfig.margin.bottom;
        trackAreaW   = rightX - leftX;
        singleTrackW = trackAreaW / static_cast<float>(trackCount);

        // --- 修正预览区缩放比例 (renderScaleY) ---
        // 1. 计算主画布可见的逻辑像素高度 (effective scroll height)
        float mainEffectiveH =
            (config.visual.trackLayout.bottom - config.visual.trackLayout.top) *
            mainViewportHeight;
        // 2. 计算预览区可用的绘图区高度
        float previewDrawH = bottomY - topY;
        // 3. 计算 renderScaleY 使其满足 areaRatio 的定义 (显示 5 倍范围)
        renderScaleY =
            previewDrawH / (mainEffectiveH * config.visual.previewConfig.areaRatio);

        // 预览区通常不绘制背景图 (保持透明或由 UI 层处理)
        // 绘制主画布范围包围框和判定线
        auto& skin    = Config::SkinManager::instance();
        auto  boxCol  = skin.getColor("preview.boundingbox");
        auto  lineCol = skin.getColor("preview.judgeline");

        // 计算主画布判定线在预览区中的高度位置
        float mainJudgelineInPreviewY = judgmentLineY;

        // 包围框的高度在预览区中的显示大小
        float boxDrawH = mainEffectiveH * renderScaleY;
        // 包围框相对于判定线的位置
        float boxTop = mainJudgelineInPreviewY -
                       (config.visual.judgeline_pos - config.visual.trackLayout.top) *
                           mainViewportHeight * renderScaleY;

        batcher.setTexture(TextureID::None);
        // 绘制半透明背景包围框
        batcher.pushQuad(leftX,
                         boxTop + boxDrawH,
                         trackAreaW,
                         boxDrawH,
                         { boxCol.r, boxCol.g, boxCol.b, boxCol.a });
        // 绘制包围框轮廓 (不透明)
        batcher.pushStrokeRect(leftX,
                               boxTop,
                               rightX,
                               boxTop + boxDrawH,
                               2.0f,
                               { boxCol.r, boxCol.g, boxCol.b, 1.0f });
        // 绘制判定线
        batcher.pushQuad(leftX,
                         mainJudgelineInPreviewY + config.visual.judgelineWidth * 0.5f,
                         trackAreaW,
                         config.visual.judgelineWidth,
                         { lineCol.r, lineCol.g, lineCol.b, lineCol.a });
    } else {
        // 主画布逻辑
        // 绘制背景
        BackgroundRenderSystem::render(
            batcher, viewportWidth, viewportHeight, config, snapshot);

        renderTrackLayout(batcher,
                          viewportWidth,
                          viewportHeight,
                          judgmentLineY,
                          trackCount,
                          config,
                          leftX,
                          rightX,
                          topY,
                          bottomY,
                          trackAreaW,
                          singleTrackW);
    }

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

    batcher.flush();
}

}  // namespace MMM::Logic::System
