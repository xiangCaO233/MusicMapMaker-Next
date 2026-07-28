#include "logic/session/CanvasCamera.h"
#include "log/colorful-log.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/InteractionController.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/SampleAction.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "logic/session/tool/DrawTool.h"
#include "mmm/beatmap/BeatMap.h"

#include <cmath>
#include <memory>

namespace
{

/// @brief 使用小容差比较逻辑像素或时间值。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
bool near(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1e-6;
}

/// @brief 验证横向相机偏移被渲染和拾取共用的轨道投影正确应用。
/// @return 投影边界、轨道宽度和拾取结果正确时返回 true。
bool testTrackProjectionUsesCameraOffset()
{
    const auto projection = MMM::Logic::calculatePlayerTrackProjection(
        1000.0F, 4, 0.2F, 0.6F, 50.0F);
    if ( !projection.valid || !near(projection.leftX, 250.0) ||
         !near(projection.rightX, 650.0) ||
         !near(projection.singleTrackWidth, 100.0) ||
         !projection.contains(250.0F) || projection.contains(200.0F) ||
         projection.trackAt(349.0F, 4) != 0 ||
         projection.trackAt(350.0F, 4) != 1 ||
         projection.trackAt(649.0F, 4) != 3 ) {
        XERROR("Canvas track projection ignored horizontal camera offset");
        return false;
    }
    return true;
}

/// @brief 验证玩家轨道与 BGM 轨道共用统一地址和连续横向投影。
/// @return 边界、绝对轨道及运行时追加轨映射均正确时返回 true。
bool testUnifiedLaneProjection()
{
    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        1000.0F, 4, 2, 0.1F, 0.5F, 0.0F);
    const auto playerLane = projection.laneAt(499.0F);
    const auto firstBgm   = projection.laneAt(500.0F);
    const auto appendBgm  = projection.laneAt(700.0F);
    const auto outside    = projection.laneAt(800.0F);
    const auto visible    = projection.visibleBgmRange(550.0F, 650.0F);
    if ( !projection.valid || projection.bgmLaneCount != 3 || !playerLane ||
         *playerLane !=
             MMM::Logic::CanvasLaneAddress{ MMM::Logic::CanvasLaneKind::Player,
                                            3 } ||
         !firstBgm || firstBgm->absoluteTrack(4) != 4 || !appendBgm ||
         *appendBgm !=
             MMM::Logic::CanvasLaneAddress{ MMM::Logic::CanvasLaneKind::Bgm,
                                            2 } ||
         outside || !visible || visible->first != 0 || visible->second != 2 ) {
        XERROR("Unified canvas lane projection did not map BGM lanes");
        return false;
    }
    return true;
}

/// @brief 验证逻辑视口 Resize 后横向位移保持相同比例。
/// @return 偏移随逻辑宽度等比例换算时返回 true。
bool testResizePreservesNormalizedOffset()
{
    const float resized =
        MMM::Logic::resizeCanvasHorizontalOffset(120.0F, 1200.0F, 600.0F);
    const float unchanged =
        MMM::Logic::resizeCanvasHorizontalOffset(120.0F, 0.0F, 600.0F);
    if ( !near(resized, 60.0) || !near(unchanged, 120.0) ) {
        XERROR("Canvas horizontal offset was not stable across resize");
        return false;
    }
    return true;
}

/// @brief 验证二维平移按逻辑像素连续修改横向相机和纵向时间。
/// @return 横向偏移与无吸附纵向换算均正确时返回 true。
bool testPanCommandUsesLogicalPixels()
{
    MMM::Logic::SessionContext context;
    context.currentTime        = 10.0;
    context.animateTime        = 10.0;
    context.mainAudioTotalTime = 100.0;
    context.cameras.emplace(
        "Canvas_7",
        MMM::Logic::CameraInfo{ "Canvas_7", 1000.0F, 600.0F, 50.0F });
    context.timelineRegistry.ctx().emplace<MMM::Logic::System::ScrollCache>();

    MMM::Logic::PlaybackController controller(context);
    controller.handleCommand(MMM::Logic::CmdPanCanvas{
        .cameraId       = "Canvas_7",
        .deltaX         = 25.0F,
        .deltaY         = 100.0F,
        .viewportWidth  = 1000.0F,
        .viewportHeight = 600.0F,
        .renderScaleY   = 2.0F,
    });

    const auto cameraIt = context.cameras.find("Canvas_7");
    if ( cameraIt == context.cameras.end() ||
         !near(cameraIt->second.horizontalOffsetX, 75.0) ||
         !near(context.currentTime, 10.1) ||
         !near(context.animateTime,
               context.currentTime +
                   context.lastConfig.visual.getEffectiveVisualOffset()) ||
         context.animateTimeAnimationActive ) {
        XERROR("Canvas pan command did not apply continuous two-axis movement");
        return false;
    }

    controller.handleCommand(MMM::Logic::CmdPanCanvas{
        .cameraId       = "Canvas_7",
        .viewportWidth  = 500.0F,
        .viewportHeight = 600.0F,
    });
    if ( !near(cameraIt->second.horizontalOffsetX, 37.5) ) {
        XERROR("Canvas pan command did not preserve offset after resize");
        return false;
    }
    return true;
}

/// @brief 验证改键数按 BGM 相对索引原子迁移全部自动采样。
/// @return 稠密、稀疏轨道及 Undo/Redo 均保持相对索引时返回 true。
bool testTrackCountActionMigratesAllSamples()
{
    MMM::Logic::SessionContext context;
    context.trackCount    = 4;
    context.bgmTrackCount = 2;

    const auto first  = context.sampleRegistry.create();
    const auto third  = context.sampleRegistry.create();
    const auto sparse = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        first, MMM::Logic::SampleComponent{ .m_track = 4 });
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        third, MMM::Logic::SampleComponent{ .m_track = 6 });
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sparse, MMM::Logic::SampleComponent{ .m_track = 1000 });

    MMM::Logic::InteractionController controller(context);
    controller.handleCommand(MMM::Logic::CmdUpdateTrackCount{ 7 });
    if ( context.trackCount != 7 || context.bgmTrackCount != 2 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 7 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(third)
                 .m_track != 9 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(sparse)
                 .m_track != 1003 ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Track count action did not atomically migrate all samples");
        return false;
    }

    context.actionStack.undo(context);
    if ( context.trackCount != 4 || context.bgmTrackCount != 2 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 4 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(third)
                 .m_track != 6 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(sparse)
                 .m_track != 1000 ) {
        XERROR("Track count action undo did not restore sample tracks");
        return false;
    }

    context.actionStack.redo(context);
    if ( context.trackCount != 7 || context.bgmTrackCount != 2 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 7 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(third)
                 .m_track != 9 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(sparse)
                 .m_track != 1003 ) {
        XERROR("Track count action redo did not restore migrated tracks");
        return false;
    }
    return true;
}

/// @brief 验证在运行时追加空轨放置采样会持久扩展 BGM 轨道数。
/// @return 扩展及 Undo/Redo 均恢复完整状态时返回 true。
bool testAppendLaneExpandsPersistentCount()
{
    MMM::Logic::SessionContext context;
    context.trackCount                 = 4;
    context.bgmTrackCount              = 2;
    const auto                  entity = context.sampleRegistry.create();
    MMM::Logic::SampleComponent before;
    before.m_track = 4;
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(entity, before);

    auto after    = before;
    after.m_track = 6;
    auto action   = std::make_unique<MMM::Logic::SampleAction>(
        MMM::Logic::SampleAction::Type::Update, entity, before, after);
    context.actionStack.pushAndExecute(std::move(action), context);
    if ( context.bgmTrackCount != 3 ) {
        XERROR("Runtime append lane did not expand persistent BGM count");
        return false;
    }

    context.actionStack.undo(context);
    if ( context.bgmTrackCount != 2 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity)
                 .m_track != 4 ) {
        XERROR("Append lane undo did not restore BGM count");
        return false;
    }

    context.actionStack.redo(context);
    if ( context.bgmTrackCount != 3 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity)
                 .m_track != 6 ) {
        XERROR("Append lane redo did not restore expanded BGM count");
        return false;
    }
    return true;
}

/// @brief 验证 BeatMap 自动采样由独立 Registry 完整载入并同步回领域对象。
/// @return 时间单位、偏移、轨道、资源、音量和 BGM 轨道数均往返时返回 true。
bool testSampleRegistryLoadAndSync()
{
    auto beatmap                           = std::make_shared<MMM::BeatMap>();
    beatmap->m_baseMapMetadata.track_count = 4;
    beatmap->m_baseMapMetadata.bgm_track_count = 2;
    beatmap->m_audioSamples.push_back({
        .m_timestamp       = 1250.0,
        .m_offsetMs        = -250,
        .m_track           = 4,
        .m_audioResourceId = "stem.ogg",
        .m_volume          = 0.75F,
    });

    MMM::Logic::SessionContext context;
    MMM::Logic::SessionUtils::loadBeatmap(context, beatmap);
    const auto sampleView =
        context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( sampleView.size() != 1 ||
         !context.noteRegistry.view<MMM::Logic::NoteComponent>().empty() ||
         !context.hitEvents.empty() ) {
        XERROR("Audio samples were not isolated from player-note ECS");
        return false;
    }

    const auto entity = *sampleView.begin();
    auto&      sample =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity);
    if ( !near(sample.m_timestamp, 1.25) || sample.m_offsetMs != -250 ||
         sample.m_track != 4 || sample.m_audioResourceId != "stem.ogg" ||
         !near(sample.m_volume, 0.75) ) {
        XERROR("Audio sample load did not preserve all fields");
        return false;
    }

    sample.m_timestamp         = 2.5;
    sample.m_offsetMs          = 125;
    sample.m_track             = 6;
    sample.m_audioResourceId   = "effect.wav";
    sample.m_volume            = 0.5F;
    context.bgmTrackCount      = 3;
    context.m_needsSamplesSync = true;
    MMM::Logic::SessionUtils::syncBeatmap(context);

    if ( beatmap->m_audioSamples.size() != 1 ||
         !near(beatmap->m_audioSamples.front().m_timestamp, 2500.0) ||
         beatmap->m_audioSamples.front().m_offsetMs != 125 ||
         beatmap->m_audioSamples.front().m_track != 6 ||
         beatmap->m_audioSamples.front().m_audioResourceId != "effect.wav" ||
         !near(beatmap->m_audioSamples.front().m_volume, 0.5) ||
         beatmap->m_baseMapMetadata.bgm_track_count != 3 ||
         context.m_needsSamplesSync ) {
        XERROR("Audio sample ECS did not synchronize back to BeatMap");
        return false;
    }
    return true;
}

/// @brief 验证不同 ECS 注册表中重叠的实体 ID 不会被 DrawTool 混淆。
/// @return 悬停自动采样时橡皮擦未将同 ID 的玩家物件加入目标时返回 true。
bool testSampleHoverDoesNotTargetOverlappingNote()
{
    MMM::Logic::SessionContext context;
    const auto                 noteEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(noteEntity);
    const auto sampleEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(sampleEntity);
    if ( noteEntity != sampleEntity ) {
        XERROR("Regression setup did not create overlapping ECS entity IDs");
        return false;
    }

    context.hoveredEntity     = sampleEntity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::AudioSample;

    MMM::Logic::DrawTool drawTool;
    drawTool.handleStartErase(context, MMM::Logic::CmdStartErase{});
    drawTool.handleUpdateErase(context, MMM::Logic::CmdUpdateErase{});
    drawTool.handleEndErase(context, MMM::Logic::CmdEndErase{});

    if ( !context.noteRegistry.valid(noteEntity) ||
         !context.eraserState.targetEntities.empty() ) {
        XERROR("Sample hover leaked into the player-note eraser domain");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行主画布二维相机换算测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testTrackProjectionUsesCameraOffset() &&
                   testUnifiedLaneProjection() &&
                   testResizePreservesNormalizedOffset() &&
                   testPanCommandUsesLogicalPixels() &&
                   testTrackCountActionMigratesAllSamples() &&
                   testAppendLaneExpandsPersistentCount() &&
                   testSampleRegistryLoadAndSync() &&
                   testSampleHoverDoesNotTargetOverlappingNote()
               ? 0
               : 1;
}
