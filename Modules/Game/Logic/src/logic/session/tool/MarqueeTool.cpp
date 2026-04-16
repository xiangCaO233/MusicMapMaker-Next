#include "logic/session/tool/MarqueeTool.h"
#include "logic/BeatmapSession.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/context/SessionContext.h"

namespace MMM::Logic
{

void MarqueeTool::handleStartMarquee(SessionContext&        ctx,
                                     const CmdStartMarquee& cmd)
{
    ctx.isSelecting         = true;
    ctx.hasMarqueeSelection = true;
    ctx.marqueeIsAdditive   = cmd.isCtrlDown;

    // 如果不是加选模式，先清除当前所有选中框和实体选中
    if ( !ctx.marqueeIsAdditive ) {
        ctx.marqueeBoxes.clear();
        auto view = ctx.noteRegistry.view<InteractionComponent>();
        for ( auto entity : view ) {
            ctx.noteRegistry.get<InteractionComponent>(entity).isSelected =
                false;
        }
    }

    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( cache ) {
        auto it = ctx.cameras.find(cmd.cameraId);
        if ( it != ctx.cameras.end() ) {
            MarqueeBox newBox;
            newBox.cameraId = cmd.cameraId;

            float judgmentLineY =
                it->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;

            float renderScaleY = 1.0f;
            if ( cmd.cameraId == "Preview" ) {
                auto  itMain             = ctx.cameras.find("Basic2DCanvas");
                float mainViewportHeight = itMain != ctx.cameras.end()
                                               ? itMain->second.viewportHeight
                                               : it->second.viewportHeight;

                float mainEffectiveH =
                    (ctx.lastConfig.visual.trackLayout.bottom -
                     ctx.lastConfig.visual.trackLayout.top) *
                    mainViewportHeight;
                float ty = ctx.lastConfig.visual.previewConfig.margin.top;
                float by = it->second.viewportHeight -
                           ctx.lastConfig.visual.previewConfig.margin.bottom;
                float previewDrawH = by - ty;

                renderScaleY = previewDrawH /
                               (mainEffectiveH *
                                ctx.lastConfig.visual.previewConfig.areaRatio);
            } else {
                renderScaleY = ctx.lastConfig.visual.noteScaleY;
            }

            double currentAbsY = cache->getAbsY(ctx.visualTime);
            double targetAbsY =
                currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
            newBox.startTime = cache->getTime(targetAbsY);
            newBox.endTime   = newBox.startTime;

            float leftX      = it->second.viewportWidth *
                               ctx.lastConfig.visual.trackLayout.left;
            float rightX     = it->second.viewportWidth *
                               ctx.lastConfig.visual.trackLayout.right;
            float trackAreaW = rightX - leftX;
            newBox.startTrack =
                (cmd.mouseX - leftX) / (trackAreaW / ctx.trackCount);
            newBox.endTrack = newBox.startTrack;

            ctx.marqueeBoxes.push_back(std::move(newBox));
        }
    }
}

void MarqueeTool::handleUpdateMarquee(SessionContext&         ctx,
                                      const CmdUpdateMarquee& cmd)
{
    if ( !ctx.isSelecting || ctx.marqueeBoxes.empty() ) return;
    auto& currentBox = ctx.marqueeBoxes.back();

    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( cache ) {
        auto it = ctx.cameras.find(currentBox.cameraId);
        if ( it != ctx.cameras.end() ) {
            float judgmentLineY =
                it->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;

            float renderScaleY = 1.0f;
            if ( currentBox.cameraId == "Preview" ) {
                auto  itMain             = ctx.cameras.find("Basic2DCanvas");
                float mainViewportHeight = itMain != ctx.cameras.end()
                                               ? itMain->second.viewportHeight
                                               : it->second.viewportHeight;

                float mainEffectiveH =
                    (ctx.lastConfig.visual.trackLayout.bottom -
                     ctx.lastConfig.visual.trackLayout.top) *
                    mainViewportHeight;
                float ty = ctx.lastConfig.visual.previewConfig.margin.top;
                float by = it->second.viewportHeight -
                           ctx.lastConfig.visual.previewConfig.margin.bottom;
                float previewDrawH = by - ty;

                renderScaleY = previewDrawH /
                               (mainEffectiveH *
                                ctx.lastConfig.visual.previewConfig.areaRatio);
            } else {
                renderScaleY = ctx.lastConfig.visual.noteScaleY;
            }

            double currentAbsY = cache->getAbsY(ctx.visualTime);
            double targetAbsY =
                currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
            currentBox.endTime = cache->getTime(targetAbsY);

            float leftX      = it->second.viewportWidth *
                               ctx.lastConfig.visual.trackLayout.left;
            float rightX     = it->second.viewportWidth *
                               ctx.lastConfig.visual.trackLayout.right;
            float trackAreaW = rightX - leftX;
            currentBox.endTrack =
                (cmd.mouseX - leftX) / (trackAreaW / ctx.trackCount);
        }
    }
}

void MarqueeTool::handleEndMarquee(SessionContext&      ctx,
                                   const CmdEndMarquee& cmd)
{
    ctx.isSelecting = false;
    // 如果框的大小极小，可以认为是一个无效框，清理掉
    if ( !ctx.marqueeBoxes.empty() ) {
        auto& lastBox = ctx.marqueeBoxes.back();
        if ( std::abs(lastBox.endTime - lastBox.startTime) < 0.001 &&
             std::abs(lastBox.endTrack - lastBox.startTrack) < 0.1 ) {
            ctx.marqueeBoxes.pop_back();
        }
    }
}

void MarqueeTool::handleRemoveMarqueeAt(SessionContext&           ctx,
                                        const CmdRemoveMarqueeAt& cmd)
{
    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) return;

    auto it = ctx.cameras.find(cmd.cameraId);
    if ( it == ctx.cameras.end() ) return;

    // 1. 计算点击位置对应的逻辑时间与轨道
    float judgmentLineY =
        it->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;

    float renderScaleY = 1.0f;
    if ( cmd.cameraId == "Preview" ) {
        auto  itMain             = ctx.cameras.find("Basic2DCanvas");
        float mainViewportHeight = itMain != ctx.cameras.end()
                                       ? itMain->second.viewportHeight
                                       : it->second.viewportHeight;
        float mainEffectiveH     = (ctx.lastConfig.visual.trackLayout.bottom -
                                    ctx.lastConfig.visual.trackLayout.top) *
                                   mainViewportHeight;
        float ty           = ctx.lastConfig.visual.previewConfig.margin.top;
        float by           = it->second.viewportHeight -
                             ctx.lastConfig.visual.previewConfig.margin.bottom;
        float previewDrawH = by - ty;
        renderScaleY =
            previewDrawH /
            (mainEffectiveH * ctx.lastConfig.visual.previewConfig.areaRatio);
    } else {
        renderScaleY = ctx.lastConfig.visual.noteScaleY;
    }

    double currentAbsY = cache->getAbsY(ctx.visualTime);
    double targetAbsY =
        currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
    double clickTime = cache->getTime(targetAbsY);

    float leftX =
        it->second.viewportWidth * ctx.lastConfig.visual.trackLayout.left;
    float rightX =
        it->second.viewportWidth * ctx.lastConfig.visual.trackLayout.right;
    float trackAreaW = rightX - leftX;
    float clickTrack = (cmd.mouseX - leftX) / (trackAreaW / ctx.trackCount);

    // 2. 逆序查找被点击的框
    for ( int i = static_cast<int>(ctx.marqueeBoxes.size()) - 1; i >= 0; --i ) {
        auto& box = ctx.marqueeBoxes[i];
        if ( box.cameraId != cmd.cameraId ) continue;

        double minTime  = std::min(box.startTime, box.endTime);
        double maxTime  = std::max(box.startTime, box.endTime);
        float  minTrack = std::min(box.startTrack, box.endTrack);
        float  maxTrack = std::max(box.startTrack, box.endTrack);

        if ( clickTime >= minTime && clickTime <= maxTime &&
             clickTrack >= minTrack && clickTrack <= maxTrack ) {
            ctx.marqueeBoxes.erase(ctx.marqueeBoxes.begin() + i);

            // 移除后需要重算一次所有物件的选中状态
            // 如果框选列表空了，清除所有选中（除非处于追加模式，但移除框通常意味着我们想改变选中结果）
            if ( ctx.marqueeBoxes.empty() ) {
                auto view = ctx.noteRegistry.view<InteractionComponent>();
                for ( auto entity : view ) {
                    ctx.noteRegistry.get<InteractionComponent>(entity)
                        .isSelected = false;
                }
            } else {
                ctx.marqueeIsAdditive =
                    false;  // Reset additive to false to ensure a clean rebuild
                            // of selection
                auto view = ctx.noteRegistry.view<InteractionComponent>();
                for ( auto entity : view ) {
                    ctx.noteRegistry.get<InteractionComponent>(entity)
                        .isSelected = false;
                }
                ctx.hasMarqueeSelection = true;
            }
            return;
        }
    }
}

}  // namespace MMM::Logic
