#include "logic/session/tool/DrawTool.h"
#include "logic/session/NoteAction.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include <algorithm>

namespace MMM::Logic
{

void DrawTool::handleStartBrush(SessionContext& ctx, const CmdStartBrush& cmd)
{
    ctx.brushState.isActive = true;
    ctx.brushState.duration = 0.0;

    // 计算初始吸附位置
    auto itCamera = ctx.cameras.find(cmd.cameraId);
    if ( itCamera == ctx.cameras.end() ) return;

    // 获取 BPM 事件供磁吸使用
    std::vector<const TimelineComponent*> bpmEvents;
    auto tlView = ctx.timelineRegistry.view<const TimelineComponent>();
    for ( auto entity : tlView ) {
        const auto& tl = tlView.get<const TimelineComponent>(entity);
        if ( tl.m_effect == ::MMM::TimingEffect::BPM ) {
            bpmEvents.push_back(&tl);
        }
    }
    std::stable_sort(
        bpmEvents.begin(),
        bpmEvents.end(),
        [](const TimelineComponent* a, const TimelineComponent* b) {
            return a->m_timestamp < b->m_timestamp;
        });

    // 计算逻辑时间
    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) return;

    float judgmentLineY =
        itCamera->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;
    double currentAbsY = cache->getAbsY(ctx.visualTime);
    double deltaY      = (judgmentLineY - cmd.mouseY);

    // 处理预览区缩放
    float renderScaleY = 1.0f;
    if ( cmd.cameraId == "Preview" || cmd.cameraId == "PreviewCanvas" ) {
        auto  itMain             = ctx.cameras.find("Basic2DCanvas");
        float mainViewportHeight = itMain != ctx.cameras.end()
                                       ? itMain->second.viewportHeight
                                       : itCamera->second.viewportHeight;
        float mainEffectiveH     = (ctx.lastConfig.visual.trackLayout.bottom -
                                    ctx.lastConfig.visual.trackLayout.top) *
                                   mainViewportHeight;
        float previewDrawH =
            itCamera->second.viewportHeight -
            (ctx.lastConfig.visual.previewConfig.margin.top +
             ctx.lastConfig.visual.previewConfig.margin.bottom);
        renderScaleY =
            previewDrawH /
            (mainEffectiveH * ctx.lastConfig.visual.previewConfig.areaRatio);
    }
    deltaY /= renderScaleY;

    double rawTime = cache->getTime(currentAbsY + deltaY);

    auto snap = SessionUtils::getSnapResult(rawTime,
                                            cmd.mouseY,
                                            itCamera->second,
                                            ctx.lastConfig,
                                            bpmEvents,
                                            ctx.timelineRegistry,
                                            ctx.visualTime,
                                            ctx.cameras);

    ctx.brushState.time = snap.isSnapped ? snap.snappedTime : rawTime;

    if ( cmd.isShiftDown ) {
        ctx.brushState.type          = ::MMM::NoteType::HOLD;
        ctx.brushState.holdStartTime = ctx.brushState.time;
    } else {
        ctx.brushState.type = ::MMM::NoteType::NOTE;
    }

    // 计算轨道
    float leftX =
        itCamera->second.viewportWidth * ctx.lastConfig.visual.trackLayout.left;
    float rightX = itCamera->second.viewportWidth *
                   ctx.lastConfig.visual.trackLayout.right;
    if ( cmd.cameraId == "Preview" ) {
        leftX  = ctx.lastConfig.visual.previewConfig.margin.left;
        rightX = itCamera->second.viewportWidth -
                 ctx.lastConfig.visual.previewConfig.margin.right;
    }
    float trackAreaW   = rightX - leftX;
    float singleTrackW = trackAreaW / static_cast<float>(ctx.trackCount);
    int   track =
        static_cast<int>(std::floor((cmd.mouseX - leftX) / singleTrackW));
    ctx.brushState.track = std::clamp(track, 0, ctx.trackCount - 1);
}

void DrawTool::handleUpdateBrush(SessionContext& ctx, const CmdUpdateBrush& cmd)
{
    if ( !ctx.brushState.isActive ) return;

    // 同 StartBrush 的逻辑
    auto itCamera = ctx.cameras.find(cmd.cameraId);
    if ( itCamera == ctx.cameras.end() ) return;

    std::vector<const TimelineComponent*> bpmEvents;
    auto tlView = ctx.timelineRegistry.view<const TimelineComponent>();
    for ( auto entity : tlView ) {
        const auto& tl = tlView.get<const TimelineComponent>(entity);
        if ( tl.m_effect == ::MMM::TimingEffect::BPM ) bpmEvents.push_back(&tl);
    }
    std::stable_sort(
        bpmEvents.begin(),
        bpmEvents.end(),
        [](const TimelineComponent* a, const TimelineComponent* b) {
            return a->m_timestamp < b->m_timestamp;
        });

    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) return;

    float judgmentLineY =
        itCamera->second.viewportHeight * ctx.lastConfig.visual.judgeline_pos;
    double currentAbsY = cache->getAbsY(ctx.visualTime);
    double deltaY      = (judgmentLineY - cmd.mouseY);

    float renderScaleY = 1.0f;
    if ( cmd.cameraId == "Preview" || cmd.cameraId == "PreviewCanvas" ) {
        auto  itMain             = ctx.cameras.find("Basic2DCanvas");
        float mainViewportHeight = itMain != ctx.cameras.end()
                                       ? itMain->second.viewportHeight
                                       : itCamera->second.viewportHeight;
        float mainEffectiveH     = (ctx.lastConfig.visual.trackLayout.bottom -
                                    ctx.lastConfig.visual.trackLayout.top) *
                                   mainViewportHeight;
        float previewDrawH =
            itCamera->second.viewportHeight -
            (ctx.lastConfig.visual.previewConfig.margin.top +
             ctx.lastConfig.visual.previewConfig.margin.bottom);
        renderScaleY =
            previewDrawH /
            (mainEffectiveH * ctx.lastConfig.visual.previewConfig.areaRatio);
    }
    deltaY /= renderScaleY;

    double rawTime = cache->getTime(currentAbsY + deltaY);
    auto   snap    = SessionUtils::getSnapResult(rawTime,
                                                 cmd.mouseY,
                                                 itCamera->second,
                                                 ctx.lastConfig,
                                                 bpmEvents,
                                                 ctx.timelineRegistry,
                                                 ctx.visualTime,
                                                 ctx.cameras);

    double currentPosTime = snap.isSnapped ? snap.snappedTime : rawTime;

    if ( cmd.isShiftDown ) {
        if ( ctx.brushState.type == ::MMM::NoteType::NOTE ) {
            // 切换为 Hold，记录起始位置
            ctx.brushState.type          = ::MMM::NoteType::HOLD;
            ctx.brushState.holdStartTime = currentPosTime;
            ctx.brushState.time          = currentPosTime;
        }
        // 更新持续时间
        ctx.brushState.duration =
            std::max(0.0, currentPosTime - ctx.brushState.holdStartTime);
    } else {
        // 切换回 Note
        ctx.brushState.type     = ::MMM::NoteType::NOTE;
        ctx.brushState.time     = currentPosTime;
        ctx.brushState.duration = 0.0;
    }

    float leftX =
        itCamera->second.viewportWidth * ctx.lastConfig.visual.trackLayout.left;
    float rightX = itCamera->second.viewportWidth *
                   ctx.lastConfig.visual.trackLayout.right;
    if ( cmd.cameraId == "Preview" ) {
        leftX  = ctx.lastConfig.visual.previewConfig.margin.left;
        rightX = itCamera->second.viewportWidth -
                 ctx.lastConfig.visual.previewConfig.margin.right;
    }
    float trackAreaW   = rightX - leftX;
    float singleTrackW = trackAreaW / static_cast<float>(ctx.trackCount);
    int   track =
        static_cast<int>(std::floor((cmd.mouseX - leftX) / singleTrackW));
    ctx.brushState.track = std::clamp(track, 0, ctx.trackCount - 1);
}

void DrawTool::handleEndBrush(SessionContext& ctx, const CmdEndBrush& cmd)
{
    if ( !ctx.brushState.isActive ) return;

    // 创建正式音符
    NoteComponent note;
    note.m_timestamp  = ctx.brushState.time;
    note.m_duration   = ctx.brushState.duration;
    note.m_trackIndex = ctx.brushState.track;
    note.m_type       = ctx.brushState.type;

    auto action = std::make_unique<NoteAction>(
        NoteAction::Type::Create, entt::null, std::nullopt, note);
    ctx.actionStack.pushAndExecute(std::move(action), ctx);

    ctx.brushState.isActive = false;
}

void DrawTool::handleStartErase(SessionContext& ctx, const CmdStartErase& cmd)
{
    ctx.eraserState.isActive = true;
    ctx.eraserState.targetEntities.clear();
    if ( ctx.hoveredEntity != entt::null ) {
        ctx.eraserState.targetEntities.insert(ctx.hoveredEntity);
    }
}

void DrawTool::handleUpdateErase(SessionContext& ctx, const CmdUpdateErase& cmd)
{
    if ( !ctx.eraserState.isActive ) return;

    // 每帧只标记当前鼠标正下方的物件，移开就取消
    ctx.eraserState.targetEntities.clear();
    if ( ctx.hoveredEntity != entt::null ) {
        ctx.eraserState.targetEntities.insert(ctx.hoveredEntity);
    }
}

void DrawTool::handleEndErase(SessionContext& ctx, const CmdEndErase& cmd)
{
    if ( !ctx.eraserState.isActive ) return;

    // 松开时只删除此刻仍被标记的物件（即鼠标正下方那个）
    if ( !ctx.eraserState.targetEntities.empty() ) {
        for ( auto entity : ctx.eraserState.targetEntities ) {
            if ( ctx.noteRegistry.valid(entity) ) {
                auto before = ctx.noteRegistry.get<NoteComponent>(entity);
                auto action = std::make_unique<NoteAction>(
                    NoteAction::Type::Delete, entity, before, std::nullopt);
                ctx.actionStack.pushAndExecute(std::move(action), ctx);
            }
        }
    }

    ctx.eraserState.isActive = false;
    ctx.eraserState.targetEntities.clear();
}

}  // namespace MMM::Logic
