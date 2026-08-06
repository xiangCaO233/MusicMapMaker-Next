#include "logic/session/CanvasCamera.h"
#include "common/LogicCommands.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/ActionController.h"
#include "logic/session/EditorAction.h"
#include "logic/session/InteractionController.h"
#include "logic/session/NoteAction.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/SampleAction.h"
#include "logic/session/SamplePropertyEdit.h"
#include "logic/session/SelectionState.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "logic/session/tool/DrawTool.h"
#include "logic/session/tool/GrabTool.h"
#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <cmath>
#include <limits>
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

/// @brief 配置可重复的主画布投影和线性空时间线。
/// @param context 待配置会话。
void configureObjectEditingCanvas(MMM::Logic::SessionContext& context)
{
    if ( !context.currentBeatmap ) {
        context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    }
    context.currentBeatmap->m_baseMapMetadata.track_count = 4;
    context.trackCount                                    = 4;
    context.bgmTrackCount                                 = 1;
    context.currentTime                                   = 1.0;
    context.animateTime                                   = 1.0;
    context.lastConfig.visual.trackLayout.left            = 0.1F;
    context.lastConfig.visual.trackLayout.right           = 0.5F;
    context.lastConfig.visual.judgeline_pos               = 0.5F;
    context.cameras.emplace("Basic2DCanvas",
                            MMM::Logic::CameraInfo{
                                "Basic2DCanvas",
                                1000.0F,
                                600.0F,
                                0.0F,
                            });
    auto* cache =
        context.timelineRegistry.ctx().find<MMM::Logic::System::ScrollCache>();
    if ( !cache ) {
        cache = &context.timelineRegistry.ctx()
                     .emplace<MMM::Logic::System::ScrollCache>();
    }
    cache->rebuild(context.timelineRegistry,
                   context.lastConfig,
                   context.currentBeatmap.get());
}

/// @brief 验证关闭折线编辑后只有主 Note 和 Hold 可被选中与悬停。
/// @return Note/Hold 可交互且 Flick/Polyline 被忽略时返回 true。
bool testKeyModeInteractionRestriction()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.lastConfig.settings.enablePolylineEditing = false;

    const auto createNote = [&](MMM::NoteType type) {
        const auto entity = context.noteRegistry.create();
        context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
            entity,
            MMM::Logic::NoteComponent{
                .m_type       = type,
                .m_timestamp  = 1.0,
                .m_trackIndex = static_cast<int>(entt::to_integral(entity)),
            });
        return entity;
    };
    const auto noteEntity     = createNote(MMM::NoteType::NOTE);
    const auto holdEntity     = createNote(MMM::NoteType::HOLD);
    const auto flickEntity    = createNote(MMM::NoteType::FLICK);
    const auto polylineEntity = createNote(MMM::NoteType::POLYLINE);

    MMM::Logic::InteractionController controller(context);
    controller.handleCommand(MMM::Logic::CmdSelectAll{});
    const auto isSelected = [&](entt::entity entity) {
        const auto* interaction =
            context.noteRegistry.try_get<MMM::Logic::InteractionComponent>(
                entity);
        return interaction && interaction->isSelected;
    };
    if ( !isSelected(noteEntity) || !isSelected(holdEntity) ||
         isSelected(flickEntity) || isSelected(polylineEntity) ) {
        XERROR("Key mode select-all included a non-Note/Hold object");
        return false;
    }

    controller.handleCommand(MMM::Logic::CmdSetHoveredEntity{
        flickEntity,
        static_cast<std::uint8_t>(MMM::Logic::HoverPart::Head),
        -1,
    });
    if ( context.hoveredEntity != entt::null ) {
        XERROR("Key mode allowed hovering a Flick");
        return false;
    }

    controller.handleCommand(MMM::Logic::CmdSetHoveredEntity{
        holdEntity,
        static_cast<std::uint8_t>(MMM::Logic::HoverPart::Head),
        -1,
    });
    return context.hoveredEntity == holdEntity;
}

/// @brief 验证关闭折线编辑后 Shift 拖绘只创建普通 Hold。
/// @return 跨时间和轨道拖绘仍生成起始轨道 Hold 时返回 true。
bool testKeyModeBrushCreatesOnlyHold()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.lastConfig.settings.enablePolylineEditing = false;

    MMM::Logic::DrawTool drawTool;
    drawTool.handleStartBrush(context,
                              MMM::Logic::CmdStartBrush{
                                  .cameraId    = "Basic2DCanvas",
                                  .mouseX      = 150.0F,
                                  .mouseY      = 300.0F,
                                  .isShiftDown = true,
                                  .isCtrlDown  = true,
                              });
    drawTool.handleUpdateBrush(context,
                               MMM::Logic::CmdUpdateBrush{
                                   .cameraId    = "Basic2DCanvas",
                                   .mouseX      = 250.0F,
                                   .mouseY      = 50.0F,
                                   .isShiftDown = true,
                                   .isCtrlDown  = true,
                               });
    drawTool.handleEndBrush(
        context, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });

    const auto notes = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    if ( notes.size() != 1 ) {
        XERROR("Key mode brush did not create exactly one object");
        return false;
    }
    const auto& note = notes.get<MMM::Logic::NoteComponent>(*notes.begin());
    return note.m_type == MMM::NoteType::HOLD && note.m_trackIndex == 0 &&
           note.m_duration > 0.0 && note.m_subNotes.empty();
}

/// @brief 验证关闭 BMS 编辑后 BGM 区不参与投影与画笔交互。
/// @return 玩家轨道仍可寻址且 BGM 区不会创建自动采样时返回 true。
bool testBmsEditingHidesBgmLanes()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.lastConfig.settings.enableBmsEditing = false;

    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        1000.0F, 4, 1, 0.1F, 0.5F, 0.0F, true, false);
    const auto playerLane = projection.laneAt(150.0F);
    if ( !projection.valid || projection.bgmLaneCount != 0 || !playerLane ||
         playerLane->kind != MMM::Logic::CanvasLaneKind::Player ||
         projection.laneAt(550.0F).has_value() ) {
        XERROR("Disabled BMS editing still exposed a BGM lane projection");
        return false;
    }

    MMM::Logic::DrawTool drawTool;
    drawTool.handleStartBrush(context,
                              MMM::Logic::CmdStartBrush{
                                  .cameraId = "Basic2DCanvas",
                                  .mouseX   = 550.0F,
                                  .mouseY   = 300.0F,
                              });
    const auto samples =
        context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    return !context.brushState.isActive && samples.size() == 0 &&
           context.bgmTrackCount == 1;
}

/// @brief 验证项目音频选择按资源类型决定画笔在玩家区和 BGM 区的产物。
/// @return Effect 可创建绑定 Note 与自动采样，Main 只允许创建自动采样，空选择
/// 可创建静音采样草稿时返回 true。
bool testBrushAudioResourcePlacementRules()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    MMM::Logic::InteractionController controller(context);
    MMM::Logic::DrawTool              drawTool;

    controller.handleCommand(MMM::Logic::CmdSetBrushAudioResource{
        .audioResourceId = "effect",
        .audioTrackType  = MMM::AudioTrackType::Effect,
        .volume          = 0.4F,
    });
    drawTool.handleStartBrush(context,
                              MMM::Logic::CmdStartBrush{
                                  .cameraId = "Basic2DCanvas",
                                  .mouseX   = 150.0F,
                                  .mouseY   = 300.0F,
                              });
    drawTool.handleEndBrush(
        context, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });

    auto notes = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    if ( notes.size() != 1 ) {
        XERROR("Effect brush selection did not create a player note");
        return false;
    }
    const auto& note = notes.get<MMM::Logic::NoteComponent>(*notes.begin());
    if ( note.m_trackIndex != 0 || !note.m_sampleBinding ||
         note.m_sampleBinding->m_audioResourceId != "effect" ||
         !near(note.m_sampleBinding->m_volume, 0.4) ) {
        XERROR("Effect brush selection did not bind the created note");
        return false;
    }

    drawTool.handleStartBrush(context,
                              MMM::Logic::CmdStartBrush{
                                  .cameraId = "Basic2DCanvas",
                                  .mouseX   = 550.0F,
                                  .mouseY   = 300.0F,
                              });
    drawTool.handleEndBrush(
        context, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });

    auto samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( samples.size() != 1 ) {
        XERROR("Effect brush selection did not create an automatic sample");
        return false;
    }
    const auto& effectSample =
        samples.get<MMM::Logic::SampleComponent>(*samples.begin());
    if ( effectSample.m_track != 4 ||
         effectSample.m_audioResourceId != "effect" ||
         !near(effectSample.m_volume, 0.4) ) {
        XERROR("Effect automatic sample used the wrong lane or resource");
        return false;
    }

    controller.handleCommand(MMM::Logic::CmdSetBrushAudioResource{
        .audioResourceId = "main",
        .audioTrackType  = MMM::AudioTrackType::Main,
        .volume          = 0.7F,
    });
    drawTool.handleStartBrush(context,
                              MMM::Logic::CmdStartBrush{
                                  .cameraId = "Basic2DCanvas",
                                  .mouseX   = 250.0F,
                                  .mouseY   = 300.0F,
                              });
    drawTool.handleEndBrush(
        context, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });
    if ( context.noteRegistry.view<MMM::Logic::NoteComponent>().size() != 1 ||
         context.lastActionMessage.find("主音轨") == std::string::npos ) {
        XERROR("Main brush selection was not rejected in the player lanes");
        return false;
    }

    drawTool.handleStartBrush(context,
                              MMM::Logic::CmdStartBrush{
                                  .cameraId = "Basic2DCanvas",
                                  .mouseX   = 650.0F,
                                  .mouseY   = 300.0F,
                              });
    drawTool.handleEndBrush(
        context, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });
    samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( samples.size() != 2 || context.bgmTrackCount != 2 ) {
        XERROR("Main brush selection did not create on the append BGM lane");
        return false;
    }
    bool foundMain = false;
    for ( const auto entity : samples ) {
        const auto& sample = samples.get<MMM::Logic::SampleComponent>(entity);
        foundMain          = foundMain || (sample.m_track == 5 &&
                                           sample.m_audioResourceId == "main" &&
                                           near(sample.m_volume, 0.7));
    }
    if ( !foundMain ) return false;

    controller.handleCommand(MMM::Logic::CmdSetBrushAudioResource{
        .audioResourceId = {},
        .audioTrackType  = MMM::AudioTrackType::Effect,
        .volume          = 0.55F,
    });
    drawTool.handleStartBrush(context,
                              MMM::Logic::CmdStartBrush{
                                  .cameraId = "Basic2DCanvas",
                                  .mouseX   = 550.0F,
                                  .mouseY   = 300.0F,
                              });
    if ( !context.brushState.isActive ||
         !context.brushState.createsAudioSample ||
         !context.brushState.activeAudioResourceId.empty() ) {
        XERROR("Empty audio selection did not expose a silent sample brush");
        return false;
    }
    drawTool.handleEndBrush(
        context, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });
    samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( samples.size() != 3 ) {
        XERROR("Empty audio selection did not create a silent sample draft");
        return false;
    }
    return std::ranges::any_of(samples, [&](const auto entity) {
        const auto& sample = samples.get<MMM::Logic::SampleComponent>(entity);
        return sample.m_audioResourceId.empty() &&
               sample.m_track >=
                   static_cast<std::uint32_t>(context.trackCount) &&
               near(sample.m_volume, 0.55);
    });
}

/// @brief 验证 BGM 画笔按住期间持续更新半透明采样预览的轨道与时间。
/// @return 拖到追加轨后预览和最终 Sample 均跟随指针位置时返回 true。
bool testSampleBrushFollowsPointerBeforeCommit()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    MMM::Logic::InteractionController controller(context);
    MMM::Logic::DrawTool              drawTool;
    controller.handleCommand(MMM::Logic::CmdSetBrushAudioResource{
        .audioResourceId = "effect",
        .audioTrackType  = MMM::AudioTrackType::Effect,
    });

    drawTool.handleStartBrush(context,
                              MMM::Logic::CmdStartBrush{
                                  .cameraId = "Basic2DCanvas",
                                  .mouseX   = 550.0F,
                                  .mouseY   = 300.0F,
                              });
    const double startTime = context.brushState.time;
    if ( !context.brushState.isActive ||
         !context.brushState.createsAudioSample ||
         context.brushState.track != 4 ||
         context.brushState.activeAudioResourceId != "effect" ) {
        XERROR("Sample brush did not expose its pressed preview state");
        return false;
    }

    drawTool.handleUpdateBrush(context,
                               MMM::Logic::CmdUpdateBrush{
                                   .cameraId   = "Basic2DCanvas",
                                   .mouseX     = 650.0F,
                                   .mouseY     = 250.0F,
                                   .isCtrlDown = true,
                               });
    if ( context.brushState.track != 5 ||
         near(context.brushState.time, startTime) ) {
        XERROR("Sample brush preview did not follow the held pointer");
        return false;
    }
    const double draggedTime = context.brushState.time;
    drawTool.handleEndBrush(
        context, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });

    const auto samples =
        context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( samples.size() != 1 || context.bgmTrackCount != 2 ) return false;
    const auto& sample =
        samples.get<MMM::Logic::SampleComponent>(*samples.begin());
    return sample.m_track == 5 && near(sample.m_timestamp, draggedTime);
}

/// @brief 验证绘制工具按住左键跨区时会按当前落点切换 Note 与自动采样。
/// @return 玩家区到 BGM 区及反向拖绘均生成正确物件时返回 true。
bool testBrushCrossesPlayerAndBgmLanes()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    MMM::Logic::InteractionController controller(context);
    MMM::Logic::DrawTool              drawTool;
    controller.handleCommand(MMM::Logic::CmdSetBrushAudioResource{
        .audioResourceId = "effect.wav",
        .audioTrackType  = MMM::AudioTrackType::Effect,
        .volume          = 0.6F,
    });

    drawTool.handleStartBrush(context,
                              MMM::Logic::CmdStartBrush{
                                  .cameraId = "Basic2DCanvas",
                                  .mouseX   = 150.0F,
                                  .mouseY   = 300.0F,
                              });
    drawTool.handleUpdateBrush(context,
                               MMM::Logic::CmdUpdateBrush{
                                   .cameraId   = "Basic2DCanvas",
                                   .mouseX     = 550.0F,
                                   .mouseY     = 300.0F,
                                   .isCtrlDown = true,
                               });
    if ( !context.brushState.isActive ||
         !context.brushState.createsAudioSample ||
         context.brushState.track != 4 ||
         context.brushState.activeAudioResourceId != "effect.wav" ||
         context.brushState.activeSampleBinding ) {
        XERROR("Player-to-BGM brush did not switch its preview to a sample");
        return false;
    }
    drawTool.handleEndBrush(
        context, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });

    const auto samples =
        context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( samples.size() != 1 ||
         context.noteRegistry.view<MMM::Logic::NoteComponent>().size() != 0 ) {
        XERROR("Player-to-BGM brush committed the wrong object kind");
        return false;
    }
    const auto& sample =
        samples.get<MMM::Logic::SampleComponent>(*samples.begin());
    if ( sample.m_track != 4 || sample.m_audioResourceId != "effect.wav" ||
         !near(sample.m_volume, 0.6) ) {
        XERROR("Player-to-BGM brush committed the wrong sample properties");
        return false;
    }

    drawTool.handleStartBrush(context,
                              MMM::Logic::CmdStartBrush{
                                  .cameraId = "Basic2DCanvas",
                                  .mouseX   = 550.0F,
                                  .mouseY   = 300.0F,
                              });
    drawTool.handleUpdateBrush(context,
                               MMM::Logic::CmdUpdateBrush{
                                   .cameraId   = "Basic2DCanvas",
                                   .mouseX     = 250.0F,
                                   .mouseY     = 300.0F,
                                   .isCtrlDown = true,
                               });
    if ( !context.brushState.isActive ||
         context.brushState.createsAudioSample ||
         context.brushState.track != 1 ||
         context.brushState.activeAudioResourceId.size() != 0 ||
         !context.brushState.activeSampleBinding ||
         context.brushState.activeSampleBinding->m_audioResourceId !=
             "effect.wav" ||
         !near(context.brushState.activeSampleBinding->m_volume, 0.6) ) {
        XERROR("BGM-to-player brush did not switch its preview to a Note");
        return false;
    }
    drawTool.handleEndBrush(
        context, MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" });

    const auto notes = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    if ( notes.size() != 1 ) {
        XERROR("BGM-to-player brush did not commit one Note");
        return false;
    }
    const auto& note = notes.get<MMM::Logic::NoteComponent>(*notes.begin());
    return note.m_type == MMM::NoteType::NOTE && note.m_trackIndex == 1 &&
           note.m_sampleBinding &&
           note.m_sampleBinding->m_audioResourceId == "effect.wav" &&
           near(note.m_sampleBinding->m_volume, 0.6);
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
    context.currentTime            = 10.0;
    context.animateTime            = 10.0;
    context.audioTimelineTotalTime = 100.0;
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
    context.trackCount       = 4;
    context.bgmTrackCount    = 2;
    context.isTransformDirty = false;

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
         !context.isTransformDirty ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Track count action did not atomically migrate all samples");
        return false;
    }

    context.isTransformDirty = false;
    context.actionStack.undo(context);
    if ( context.trackCount != 4 || context.bgmTrackCount != 2 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 4 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(third)
                 .m_track != 6 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(sparse)
                 .m_track != 1000 ||
         !context.isTransformDirty ) {
        XERROR("Track count action undo did not restore sample tracks");
        return false;
    }

    context.isTransformDirty = false;
    context.actionStack.redo(context);
    if ( context.trackCount != 7 || context.bgmTrackCount != 2 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 7 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(third)
                 .m_track != 9 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(sparse)
                 .m_track != 1003 ||
         !context.isTransformDirty ) {
        XERROR("Track count action redo did not restore migrated tracks");
        return false;
    }
    return true;
}

/// @brief 验证 Session 按当前 Key 数选择独立轨道与组件布局。
/// @return 切换 Key 数后直接字段均物化为对应独立布局时返回 true。
bool testSessionSelectsKeyCountLayout()
{
    MMM::Config::EditorConfig config;
    auto& fourTrack = config.visual.editableTrackLayoutForKeyCount(4);
    fourTrack.left  = 0.14F;
    fourTrack.right = 0.54F;
    config.visual.editableJudgmentLinePositionForKeyCount(4) = 0.74F;
    config.visual.editableCanvasComponentsForKeyCount(4).beatNumber.anchorX =
        0.24F;

    auto& sevenTrack = config.visual.editableTrackLayoutForKeyCount(7);
    sevenTrack.left  = 0.27F;
    sevenTrack.right = 0.87F;
    config.visual.editableJudgmentLinePositionForKeyCount(7) = 0.87F;
    config.visual.editableCanvasComponentsForKeyCount(7).beatNumber.anchorX =
        0.67F;

    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    context.trackCount                 = 4;
    session.update(0.0, config, false);
    if ( !near(context.lastConfig.visual.trackLayout.left, 0.14) ||
         !near(context.lastConfig.visual.trackLayout.right, 0.54) ||
         !near(context.lastConfig.visual.judgeline_pos, 0.74) ||
         !near(context.lastConfig.visual.canvasComponents.beatNumber.anchorX,
               0.24) ) {
        XERROR("Four-key Session did not select its independent layout");
        return false;
    }

    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateTrackCount{ .trackCount = 7 },
    });
    session.update(0.0, config, false);
    if ( context.trackCount != 7 ||
         !near(context.lastConfig.visual.trackLayout.left, 0.27) ||
         !near(context.lastConfig.visual.trackLayout.right, 0.87) ||
         !near(context.lastConfig.visual.judgeline_pos, 0.87) ||
         !near(context.lastConfig.visual.canvasComponents.beatNumber.anchorX,
               0.67) ) {
        XERROR("Seven-key Session reused another Key-count layout");
        return false;
    }

    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateEditorConfig{ .config = config },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateTrackCount{ .trackCount = 5 },
    });
    session.update(0.0, config, false);
    return context.trackCount == 5 &&
           near(context.lastConfig.visual.trackLayout.left,
                config.visual.trackLayout.left) &&
           near(context.lastConfig.visual.judgeline_pos,
                config.visual.judgeline_pos) &&
           near(context.lastConfig.visual.canvasComponents.beatNumber.anchorX,
                config.visual.canvasComponents.beatNumber.anchorX);
}

/// @brief 验证交互命令执行前已经物化当前 Key 数的轨道与判定线布局。
/// @return 画笔在专属布局中的鼠标位置生成同轨同时间预览时返回 true。
bool testQueuedBrushUsesKeyCountLayout()
{
    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    configureObjectEditingCanvas(context);
    context.currentTool = MMM::Logic::EditTool::Draw;

    auto config                     = context.lastConfig;
    config.visual.trackLayout.left  = 0.1F;
    config.visual.trackLayout.right = 0.5F;
    config.visual.judgeline_pos     = 0.5F;
    auto& fourTrackLayout = config.visual.editableTrackLayoutForKeyCount(4);
    fourTrackLayout.left  = 0.4F;
    fourTrackLayout.right = 0.8F;
    config.visual.editableJudgmentLinePositionForKeyCount(4) = 0.75F;

    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdStartBrush{
            .cameraId = "Basic2DCanvas",
            .mouseX   = 450.0F,
            .mouseY   = 450.0F,
        },
    });
    session.update(0.0, config, true);

    if ( !context.brushState.isActive || context.brushState.track != 0 ||
         !near(context.brushState.time, 1.0) ||
         !near(context.lastConfig.visual.trackLayout.left, 0.4) ||
         !near(context.lastConfig.visual.judgeline_pos, 0.75) ) {
        XERROR("Queued brush command used the legacy global canvas layout");
        return false;
    }
    return true;
}

/// @brief 验证玩家轨道数变化不会把超出 uint32 的自动采样轨道静默截断。
/// @return 溢出时轨道数、采样和撤销栈均保持原状时返回 true。
bool testTrackCountOverflowIsRejectedAtomically()
{
    MMM::Logic::SessionContext context;
    context.trackCount    = 1;
    context.bgmTrackCount = 1;
    const auto entity     = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        entity,
        MMM::Logic::SampleComponent{
            .m_track = std::numeric_limits<std::uint32_t>::max(),
        });

    MMM::Logic::InteractionController controller(context);
    controller.handleCommand(MMM::Logic::CmdUpdateTrackCount{ 2 });

    return context.trackCount == 1 &&
           context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity)
                   .m_track == std::numeric_limits<std::uint32_t>::max() &&
           context.actionStack.getUndoStackSize() == 0 &&
           !context.lastActionMessage.empty();
}

/// @brief 验证元数据入口改键失败不会覆盖任一谱面状态。
/// @return 非法轨道与溢出均原子拒绝，成功迁移仍可撤销时返回 true。
bool testMetadataTrackCountMigrationIsAtomic()
{
    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    context.currentBeatmap             = std::make_shared<MMM::BeatMap>();
    context.currentBeatmap->m_baseMapMetadata.name            = "before";
    context.currentBeatmap->m_baseMapMetadata.track_count     = 1;
    context.currentBeatmap->m_baseMapMetadata.bgm_track_count = 2;
    context.trackCount                                        = 1;
    context.bgmTrackCount                                     = 2;

    const auto first = context.sampleRegistry.create();
    const auto far   = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        first,
        MMM::Logic::SampleComponent{
            .m_track           = 1,
            .m_audioResourceId = "first",
        });
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        far,
        MMM::Logic::SampleComponent{
            .m_track           = std::numeric_limits<std::uint32_t>::max(),
            .m_audioResourceId = "far",
        });

    auto updatedMeta        = context.currentBeatmap->m_baseMapMetadata;
    updatedMeta.name        = "overflow";
    updatedMeta.track_count = 2;
    session.pushCommand(
        MMM::Logic::LogicCommand{ MMM::Logic::CmdUpdateBeatmapMetadata{
            .baseMeta = updatedMeta,
        } });
    session.update(0.0, MMM::Config::EditorConfig{}, false);
    if ( context.currentBeatmap->m_baseMapMetadata.name != "before" ||
         context.currentBeatmap->m_baseMapMetadata.track_count != 1 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 2 ||
         context.trackCount != 1 || context.bgmTrackCount != 2 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 1 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(far).m_track !=
             std::numeric_limits<std::uint32_t>::max() ||
         context.actionStack.getUndoStackSize() != 0 ||
         context.lastActionMessage.find("超出可表示范围") ==
             std::string::npos ) {
        XERROR("Metadata track-count overflow partially changed chart state");
        return false;
    }

    context.sampleRegistry.get<MMM::Logic::SampleComponent>(far).m_track = 0;
    updatedMeta.name = "invalid";
    session.pushCommand(
        MMM::Logic::LogicCommand{ MMM::Logic::CmdUpdateBeatmapMetadata{
            .baseMeta = updatedMeta,
        } });
    session.update(0.0, MMM::Config::EditorConfig{}, false);
    if ( context.currentBeatmap->m_baseMapMetadata.name != "before" ||
         context.currentBeatmap->m_baseMapMetadata.track_count != 1 ||
         context.trackCount != 1 || context.bgmTrackCount != 2 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 1 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(far).m_track !=
             0 ||
         context.actionStack.getUndoStackSize() != 0 ||
         context.lastActionMessage.find("落入玩家轨道区") ==
             std::string::npos ) {
        XERROR("Invalid automatic-sample lane partially changed metadata");
        return false;
    }

    context.sampleRegistry.get<MMM::Logic::SampleComponent>(far).m_track = 2;
    updatedMeta.name        = "after";
    updatedMeta.track_count = 3;
    session.pushCommand(
        MMM::Logic::LogicCommand{ MMM::Logic::CmdUpdateBeatmapMetadata{
            .baseMeta = updatedMeta,
        } });
    session.update(0.0, MMM::Config::EditorConfig{}, false);
    if ( context.currentBeatmap->m_baseMapMetadata.name != "after" ||
         context.currentBeatmap->m_baseMapMetadata.track_count != 3 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 2 ||
         context.trackCount != 3 || context.bgmTrackCount != 2 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 3 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(far).m_track !=
             4 ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Metadata track-count migration did not preserve BGM lanes");
        return false;
    }

    context.actionStack.undo(context);
    return context.currentBeatmap->m_baseMapMetadata.name == "after" &&
           context.currentBeatmap->m_baseMapMetadata.track_count == 1 &&
           context.currentBeatmap->m_baseMapMetadata.bgm_track_count == 2 &&
           context.trackCount == 1 && context.bgmTrackCount == 2 &&
           context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                   .m_track == 1 &&
           context.sampleRegistry.get<MMM::Logic::SampleComponent>(far)
                   .m_track == 2;
}

/// @brief 验证替换元数据时同步迁移自动采样，并保留当前 BGM 轨道数。
/// @return 执行、Undo 和 Redo 均恢复轨道与元数据时返回 true。
bool testReplaceBeatmapMetadataMigratesSamples()
{
    MMM::Logic::SessionContext context;
    context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    context.currentBeatmap->m_baseMapMetadata.track_count     = 4;
    context.currentBeatmap->m_baseMapMetadata.bgm_track_count = 3;
    context.trackCount                                        = 4;
    context.bgmTrackCount                                     = 3;

    const auto first = context.sampleRegistry.create();
    const auto far   = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        first,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "first",
        });
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        far,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 2.0,
            .m_track           = 10,
            .m_audioResourceId = "far",
        });

    auto source                           = std::make_shared<MMM::BeatMap>();
    source->m_baseMapMetadata.track_count = 7;
    source->m_baseMapMetadata.bgm_track_count = 99;

    MMM::Logic::ActionController controller(context);
    controller.handleCommand(MMM::Logic::CmdReplaceBeatmapData{
        .sourceBeatmap   = source,
        .replaceMetadata = true,
    });
    if ( context.trackCount != 7 || context.bgmTrackCount != 3 ||
         context.currentBeatmap->m_baseMapMetadata.track_count != 7 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 3 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 7 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(far).m_track !=
             13 ||
         context.currentBeatmap->m_audioSamples.size() != 2 ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Metadata replacement did not preserve BGM-relative samples");
        return false;
    }

    context.actionStack.undo(context);
    if ( context.trackCount != 4 || context.bgmTrackCount != 3 ||
         context.currentBeatmap->m_baseMapMetadata.track_count != 4 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 3 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                 .m_track != 4 ||
         context.sampleRegistry.get<MMM::Logic::SampleComponent>(far).m_track !=
             10 ) {
        XERROR("Metadata replacement undo did not restore sample tracks");
        return false;
    }

    context.actionStack.redo(context);
    return context.trackCount == 7 && context.bgmTrackCount == 3 &&
           context.sampleRegistry.get<MMM::Logic::SampleComponent>(first)
                   .m_track == 7 &&
           context.sampleRegistry.get<MMM::Logic::SampleComponent>(far)
                   .m_track == 13;
}

/// @brief 验证替换元数据造成自动采样绝对轨道溢出时整项拒绝。
/// @return 元数据、采样与撤销栈均未改变时返回 true。
bool testReplaceBeatmapMetadataOverflowIsRejected()
{
    MMM::Logic::SessionContext context;
    context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    context.currentBeatmap->m_baseMapMetadata.track_count     = 1;
    context.currentBeatmap->m_baseMapMetadata.bgm_track_count = 1;
    context.trackCount                                        = 1;
    context.bgmTrackCount                                     = 1;
    const auto entity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        entity,
        MMM::Logic::SampleComponent{
            .m_track = std::numeric_limits<std::uint32_t>::max(),
        });

    auto source                           = std::make_shared<MMM::BeatMap>();
    source->m_baseMapMetadata.track_count = 2;
    MMM::Logic::ActionController controller(context);
    controller.handleCommand(MMM::Logic::CmdReplaceBeatmapData{
        .sourceBeatmap   = source,
        .replaceMetadata = true,
    });

    return context.trackCount == 1 &&
           context.currentBeatmap->m_baseMapMetadata.track_count == 1 &&
           context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity)
                   .m_track == std::numeric_limits<std::uint32_t>::max() &&
           context.actionStack.getUndoStackSize() == 0 &&
           !context.lastActionMessage.empty();
}

/// @brief 验证权威远端替换废弃旧实体历史与全部玩家物件交互状态。
/// @return 替换后只保留权威对象且旧撤销和实体缓存不可再访问时返回 true。
bool testAuthoritativeReplacementInvalidatesEntityState()
{
    MMM::Logic::SessionContext context;
    context.currentBeatmap                = std::make_shared<MMM::BeatMap>();
    const auto                staleEntity = context.noteRegistry.create();
    MMM::Logic::NoteComponent staleNote;
    staleNote.m_timestamp = 1.0;
    context.actionStack.pushAndExecute(std::make_unique<MMM::Logic::NoteAction>(
                                           MMM::Logic::NoteAction::Type::Create,
                                           staleEntity,
                                           std::nullopt,
                                           staleNote),
                                       context);
    context.sortedNoteEntities.push_back(staleEntity);
    context.sortedNoteMaxEndPrefix.push_back(1.0);
    context.dragRenderPinnedEntities.push_back(staleEntity);
    context.brushState.isActive = true;
    context.brushState.polylineSegments.push_back({});
    context.eraserState.isActive = true;
    context.eraserState.targetObjectKind =
        MMM::Logic::ChartObjectKind::PlayerNote;
    context.eraserState.targetEntities.insert(staleEntity);

    auto  source     = std::make_shared<MMM::BeatMap>();
    auto& note       = source->m_noteData.notes.emplace_back();
    note.m_timestamp = 2500.0;
    note.m_track     = 3;
    source->sync();

    MMM::Logic::ActionController controller(context);
    controller.handleCommand(MMM::Logic::CmdReplaceBeatmapData{
        .sourceBeatmap       = source,
        .replaceObjects      = true,
        .authoritativeRemote = true,
    });

    const auto view =
        context.noteRegistry.view<const MMM::Logic::NoteComponent>();
    if ( view.size() != 1U ||
         !near(view.get<const MMM::Logic::NoteComponent>(*view.begin())
                   .m_timestamp,
               2.5) ||
         context.actionStack.getUndoStackSize() != 0U ||
         !context.actionStack.isDirty() || context.brushState.isActive ||
         !context.brushState.polylineSegments.empty() ||
         context.eraserState.isActive ||
         !context.eraserState.targetEntities.empty() ||
         !context.dragRenderPinnedEntities.empty() ||
         !context.sortedNoteEntities.empty() ||
         !context.sortedNoteMaxEndPrefix.empty() ||
         !context.isNoteOrderDirty ) {
        XERROR("Authoritative replacement retained stale entity state");
        return false;
    }

    context.actionStack.undo(context);
    const auto afterUndo =
        context.noteRegistry.view<const MMM::Logic::NoteComponent>();
    return afterUndo.size() == 1U &&
           near(afterUndo
                    .get<const MMM::Logic::NoteComponent>(*afterUndo.begin())
                    .m_timestamp,
                2.5);
}

/// @brief 验证 Polyline 子物件经过多轮领域对象与 ECS 往返仍保持身份和顺序。
/// @return 子物件不会成为独立根对象且全部字段稳定时返回 true。
bool testPolylineSubNoteIdentitySurvivesRepeatedEcsSync()
{
    auto  beatmap        = std::make_shared<MMM::BeatMap>();
    auto& hold           = beatmap->m_noteData.holds.emplace_back();
    hold.m_timestamp     = 1000.0;
    hold.m_duration      = 375.0;
    hold.m_track         = 1;
    hold.m_isSubNote     = true;
    hold.m_sampleBinding = MMM::AudioSampleBinding{ "hold.wav", 0.4F };
    hold.m_metadata.note_properties[MMM::NoteMetadataType::MMM]["child"] =
        "hold";
    auto& flick       = beatmap->m_noteData.flicks.emplace_back();
    flick.m_timestamp = 1375.0;
    flick.m_track     = 1;
    flick.m_dtrack    = 2;
    flick.m_isSubNote = true;
    flick.m_metadata.note_properties[MMM::NoteMetadataType::MMM]["child"] =
        "flick";
    auto& polyline = beatmap->m_noteData.polylines.emplace_back();
    polyline.m_subNotes.emplace_back(hold);
    polyline.m_subNotes.emplace_back(flick);
    polyline.m_subHolds.emplace_back(hold);
    polyline.m_subFlicks.emplace_back(flick);
    beatmap->sync();

    MMM::Logic::SessionContext context;
    for ( std::size_t round = 0; round < 32U; ++round ) {
        MMM::Logic::SessionUtils::loadBeatmap(context, beatmap);
        context.m_needsNotesSync = true;
        MMM::Logic::SessionUtils::syncBeatmap(context);
        if ( beatmap->m_noteData.notes.size() != 0U ||
             beatmap->m_noteData.holds.size() != 1U ||
             beatmap->m_noteData.flicks.size() != 1U ||
             beatmap->m_noteData.polylines.size() != 1U ) {
            XERROR("Polyline ECS round trip amplified child objects");
            return false;
        }
        const auto& restoredHold     = beatmap->m_noteData.holds.front();
        const auto& restoredFlick    = beatmap->m_noteData.flicks.front();
        const auto& restoredPolyline = beatmap->m_noteData.polylines.front();
        if ( !restoredHold.m_isSubNote || !restoredFlick.m_isSubNote ||
             restoredPolyline.m_subNotes.size() != 2U ||
             &restoredPolyline.m_subNotes[0].get() != &restoredHold ||
             &restoredPolyline.m_subNotes[1].get() != &restoredFlick ||
             restoredPolyline.m_subHolds.size() != 1U ||
             restoredPolyline.m_subFlicks.size() != 1U ||
             !near(restoredHold.m_duration, 375.0) ||
             restoredFlick.m_dtrack != 2 || !restoredHold.m_sampleBinding ||
             restoredHold.m_sampleBinding->m_audioResourceId != "hold.wav" ||
             !near(restoredHold.m_sampleBinding->m_volume, 0.4) ||
             restoredHold.m_metadata.note_properties
                     .at(MMM::NoteMetadataType::MMM)
                     .at("child") != "hold" ) {
            XERROR("Polyline ECS round trip lost child semantics");
            return false;
        }
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

/// @brief 验证显式增删持久 BGM 轨可撤销，并禁止删除占用中的末尾轨。
/// @return 元数据同步、Undo/Redo 和末轨占用保护均正确时返回 true。
bool testExplicitBgmTrackCountAction()
{
    MMM::Logic::SessionContext context;
    context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    context.currentBeatmap->m_baseMapMetadata.track_count     = 4;
    context.currentBeatmap->m_baseMapMetadata.bgm_track_count = 2;
    context.trackCount                                        = 4;
    context.bgmTrackCount                                     = 2;
    MMM::Logic::InteractionController controller(context);

    controller.handleCommand(MMM::Logic::CmdUpdateBgmTrackCount{ 3 });
    if ( context.bgmTrackCount != 3 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 3 ||
         !context.isTransformDirty ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Explicit BGM lane add did not persist as one editor action");
        return false;
    }

    context.isTransformDirty = false;
    context.actionStack.undo(context);
    if ( context.bgmTrackCount != 2 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 2 ||
         !context.isTransformDirty ) {
        XERROR("Explicit BGM lane add undo did not restore metadata");
        return false;
    }

    context.isTransformDirty = false;
    context.actionStack.redo(context);
    if ( context.bgmTrackCount != 3 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 3 ||
         !context.isTransformDirty ) {
        XERROR("Explicit BGM lane add redo did not restore metadata");
        return false;
    }

    const auto occupiedEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        occupiedEntity,
        MMM::Logic::SampleComponent{
            .m_track           = 6,
            .m_audioResourceId = "occupied.wav",
        });
    controller.handleCommand(MMM::Logic::CmdUpdateBgmTrackCount{ 2 });
    if ( context.bgmTrackCount != 3 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 3 ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Occupied final BGM lane was removed");
        return false;
    }

    context.sampleRegistry.destroy(occupiedEntity);
    controller.handleCommand(MMM::Logic::CmdUpdateBgmTrackCount{ 2 });
    if ( context.bgmTrackCount != 2 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 2 ||
         context.actionStack.getUndoStackSize() != 2 ) {
        XERROR("Empty final BGM lane was not removed");
        return false;
    }

    context.actionStack.undo(context);
    if ( context.bgmTrackCount != 3 ||
         context.currentBeatmap->m_baseMapMetadata.bgm_track_count != 3 ) {
        XERROR("Explicit BGM lane removal undo did not restore metadata");
        return false;
    }
    context.actionStack.redo(context);
    return context.bgmTrackCount == 2 &&
           context.currentBeatmap->m_baseMapMetadata.bgm_track_count == 2;
}

/// @brief 验证自动采样精确属性编辑接受 Main/Effect 并原子更新全部字段。
/// @return 资源与数值校验、BGM 相对轨换算及 Undo/Redo 均正确时返回 true。
bool testSamplePropertyEditValidationAndAction()
{
    MMM::Logic::SampleComponent before{
        .m_timestamp       = 2.5,
        .m_offsetMs        = 0,
        .m_track           = 4,
        .m_audioResourceId = "old.wav",
        .m_volume          = 1.0F,
    };
    const MMM::AudioResource mainResource{
        .m_id   = "main.wav",
        .m_path = "main.wav",
        .m_type = MMM::AudioTrackType::Main,
    };
    const MMM::AudioResource effectResource{
        .m_id   = "effect.wav",
        .m_path = "effect.wav",
        .m_type = MMM::AudioTrackType::Effect,
    };

    auto mainEdit = MMM::Logic::resolveSamplePropertyEdit(
        before, 4, &mainResource, 2, -125, 0.75F);
    if ( !mainEdit.m_sample ||
         mainEdit.m_issue != MMM::Logic::SamplePropertyEditIssue::None ||
         mainEdit.m_sample->m_timestamp != before.m_timestamp ||
         mainEdit.m_sample->m_track != 6 ||
         mainEdit.m_sample->m_offsetMs != -125 ||
         mainEdit.m_sample->m_audioResourceId != "main.wav" ||
         !near(mainEdit.m_sample->m_volume, 0.75) ) {
        XERROR("Main sample property edit did not preserve and map fields");
        return false;
    }

    const auto effectEdit = MMM::Logic::resolveSamplePropertyEdit(
        before, 4, &effectResource, 0, 80, 1.25F);
    MMM::AudioResource unsupportedResource = effectResource;
    unsupportedResource.m_type = static_cast<MMM::AudioTrackType>(99);
    if ( !effectEdit.m_sample ||
         effectEdit.m_sample->m_audioResourceId != "effect.wav" ||
         effectEdit.m_sample->m_track != 4 ||
         MMM::Logic::resolveSamplePropertyEdit(before, 4, nullptr, 0, 0, 1.0F)
                 .m_issue !=
             MMM::Logic::SamplePropertyEditIssue::MissingResource ||
         MMM::Logic::resolveSamplePropertyEdit(
             before, 4, &unsupportedResource, 0, 0, 1.0F)
                 .m_issue !=
             MMM::Logic::SamplePropertyEditIssue::UnsupportedResourceType ||
         MMM::Logic::resolveSamplePropertyEdit(
             before, 4, &mainResource, -1, 0, 1.0F)
                 .m_issue !=
             MMM::Logic::SamplePropertyEditIssue::InvalidBgmLane ||
         MMM::Logic::resolveSamplePropertyEdit(
             before,
             4,
             &mainResource,
             0,
             0,
             std::numeric_limits<float>::infinity())
                 .m_issue !=
             MMM::Logic::SamplePropertyEditIssue::InvalidVolume ) {
        XERROR("Sample property edit resource or numeric validation failed");
        return false;
    }

    MMM::Logic::SessionContext context;
    context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    context.trackCount     = 4;
    context.bgmTrackCount  = 1;
    const auto entity      = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(entity, before);
    context.actionStack.pushAndExecute(
        std::make_unique<MMM::Logic::SampleAction>(
            MMM::Logic::SampleAction::Type::Update,
            entity,
            before,
            *mainEdit.m_sample),
        context);
    const auto& edited =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity);
    if ( edited.m_track != 6 || edited.m_audioResourceId != "main.wav" ||
         edited.m_offsetMs != -125 || !near(edited.m_volume, 0.75) ||
         context.bgmTrackCount != 3 ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Sample property edit was not one atomic persistent action");
        return false;
    }

    context.actionStack.undo(context);
    const auto& restored =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity);
    if ( restored.m_track != 4 || restored.m_audioResourceId != "old.wav" ||
         restored.m_offsetMs != 0 || !near(restored.m_volume, 1.0) ||
         context.bgmTrackCount != 1 ) {
        XERROR("Sample property edit undo did not restore all fields");
        return false;
    }

    context.actionStack.redo(context);
    const auto& redone =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity);
    return redone.m_track == 6 && redone.m_audioResourceId == "main.wav" &&
           redone.m_offsetMs == -125 && near(redone.m_volume, 0.75) &&
           context.bgmTrackCount == 3;
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

/// @brief 验证 Note 采样绑定音量在领域对象、ECS、Action 与 HitEvent
/// 间完整往返。
/// @return 加载、更新、撤销、重做及同步均保留资源标识和物件音量时返回 true。
bool testNoteSampleBindingRoundTrip()
{
    auto beatmap                           = std::make_shared<MMM::BeatMap>();
    beatmap->m_baseMapMetadata.track_count = 4;
    MMM::Note note;
    note.m_timestamp = 1000.0;
    note.m_track     = 1;
    note.setSampleBinding({ "effect-id", 0.25F });
    beatmap->m_noteData.notes.push_back(std::move(note));

    MMM::Logic::SessionContext context;
    MMM::Logic::SessionUtils::loadBeatmap(context, beatmap);
    auto view = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    if ( view.size() != 1 ) {
        XERROR("Note sample binding setup did not create one ECS note");
        return false;
    }

    const auto entity = *view.begin();
    const auto before =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(entity);
    if ( !before.m_sampleBinding ||
         before.m_sampleBinding->m_audioResourceId != "effect-id" ||
         !near(before.m_sampleBinding->m_volume, 0.25) ) {
        XERROR("Domain note sample binding did not load into ECS");
        return false;
    }

    auto after = before;
    after.m_sampleBinding =
        MMM::AudioSampleBinding{ "replacement-effect", 0.75F };
    context.actionStack.pushAndExecute(
        std::make_unique<MMM::Logic::NoteAction>(
            MMM::Logic::NoteAction::Type::Update, entity, before, after),
        context);
    const auto bindingAfterExecute =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(entity)
            .m_sampleBinding;
    if ( !bindingAfterExecute ||
         bindingAfterExecute->m_audioResourceId != "replacement-effect" ||
         !near(bindingAfterExecute->m_volume, 0.75) ) {
        XERROR("Note action execute dropped sample binding volume");
        return false;
    }

    context.actionStack.undo(context);
    const auto bindingAfterUndo =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(entity)
            .m_sampleBinding;
    if ( !bindingAfterUndo ||
         bindingAfterUndo->m_audioResourceId != "effect-id" ||
         !near(bindingAfterUndo->m_volume, 0.25) ) {
        XERROR("Note action undo dropped sample binding volume");
        return false;
    }

    context.actionStack.redo(context);
    MMM::Logic::SessionUtils::syncBeatmap(context);
    const auto& domainBinding =
        beatmap->m_noteData.notes.front().getSampleBinding();
    MMM::Logic::SessionUtils::ensureHitEvents(context);
    if ( !domainBinding ||
         domainBinding->m_audioResourceId != "replacement-effect" ||
         !near(domainBinding->m_volume, 0.75) ||
         context.hitEvents.size() != 1 ||
         !context.hitEvents.front().sampleBinding ||
         context.hitEvents.front().sampleBinding->m_audioResourceId !=
             "replacement-effect" ||
         !near(context.hitEvents.front().sampleBinding->m_volume, 0.75) ) {
        XERROR("ECS note sample binding did not sync to domain and HitEvent");
        return false;
    }
    return true;
}

/// @brief 验证主画布音量指令原子更新玩家绑定、Polyline 子绑定和自动采样。
/// @return 三类目标均支持 Undo/Redo，非法音量不写入时返回 true。
bool testObjectSampleVolumeCommand()
{
    MMM::Logic::SessionContext context;
    context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    context.trackCount     = 4;
    context.bgmTrackCount  = 1;
    MMM::Logic::InteractionController controller(context);

    MMM::Logic::NoteComponent note;
    note.m_sampleBinding = MMM::AudioSampleBinding{ "head.wav", 0.5F };
    note.m_type          = MMM::NoteType::POLYLINE;
    note.m_subNotes.push_back(MMM::Logic::NoteComponent::SubNote{
        .type          = MMM::NoteType::NOTE,
        .timestamp     = 1.0,
        .duration      = 0.0,
        .trackIndex    = 0,
        .dtrack        = 0,
        .sampleBinding = MMM::AudioSampleBinding{ "node.wav", 0.4F },
    });
    const auto noteEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(noteEntity, note);

    const auto sampleEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "sample.wav",
            .m_volume          = 0.6F,
        });

    controller.handleCommand(MMM::Logic::CmdUpdateObjectSampleVolume{
        .entity = noteEntity,
        .kind   = MMM::Logic::ChartObjectKind::PlayerNote,
        .volume = 0.75F,
    });
    controller.handleCommand(MMM::Logic::CmdUpdateObjectSampleVolume{
        .entity   = noteEntity,
        .kind     = MMM::Logic::ChartObjectKind::PlayerNote,
        .subIndex = 0,
        .volume   = 1.25F,
    });
    controller.handleCommand(MMM::Logic::CmdUpdateObjectSampleVolume{
        .entity = sampleEntity,
        .kind   = MMM::Logic::ChartObjectKind::AudioSample,
        .volume = 0.25F,
    });
    controller.handleCommand(MMM::Logic::CmdUpdateObjectSampleVolume{
        .entity = sampleEntity,
        .kind   = MMM::Logic::ChartObjectKind::AudioSample,
        .volume = -0.25F,
    });

    const auto& editedNote =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(noteEntity);
    const auto& editedSample =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(sampleEntity);
    if ( !editedNote.m_sampleBinding ||
         !near(editedNote.m_sampleBinding->m_volume, 0.75) ||
         !editedNote.m_subNotes.front().sampleBinding ||
         !near(editedNote.m_subNotes.front().sampleBinding->m_volume, 1.25) ||
         !near(editedSample.m_volume, 0.25) ||
         context.actionStack.getUndoStackSize() != 3 ) {
        XERROR("Object sample volume command did not update typed targets");
        return false;
    }

    context.actionStack.undo(context);
    context.actionStack.undo(context);
    context.actionStack.undo(context);
    const auto& restoredNote =
        context.noteRegistry.get<MMM::Logic::NoteComponent>(noteEntity);
    const auto& restoredSample =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(sampleEntity);
    if ( !restoredNote.m_sampleBinding ||
         !near(restoredNote.m_sampleBinding->m_volume, 0.5) ||
         !restoredNote.m_subNotes.front().sampleBinding ||
         !near(restoredNote.m_subNotes.front().sampleBinding->m_volume, 0.4) ||
         !near(restoredSample.m_volume, 0.6) ) {
        XERROR("Object sample volume undo did not restore all typed targets");
        return false;
    }

    context.actionStack.redo(context);
    context.actionStack.redo(context);
    context.actionStack.redo(context);
    return near(context.noteRegistry.get<MMM::Logic::NoteComponent>(noteEntity)
                    .m_sampleBinding->m_volume,
                0.75) &&
           near(context.noteRegistry.get<MMM::Logic::NoteComponent>(noteEntity)
                    .m_subNotes.front()
                    .sampleBinding->m_volume,
                1.25) &&
           near(context.sampleRegistry
                    .get<MMM::Logic::SampleComponent>(sampleEntity)
                    .m_volume,
                0.25);
}

/// @brief 验证物件音量命令经 BeatmapSession 队列分发后真正写入组件。
/// @return 队列命令更新自动采样且生成一个撤销步骤时返回 true。
bool testObjectSampleVolumeCommandRoutesThroughSession()
{
    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    const auto                 entity  = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        entity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "effect.wav",
            .m_volume          = 1.0F,
        });

    session.pushCommand(
        MMM::Logic::LogicCommand{ MMM::Logic::CmdUpdateObjectSampleVolume{
            .entity = entity,
            .kind   = MMM::Logic::ChartObjectKind::AudioSample,
            .volume = 0.4F,
        } });
    session.update(0.0, MMM::Config::EditorConfig{}, false);

    return near(context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity)
                    .m_volume,
                0.4) &&
           context.actionStack.getUndoStackSize() == 1;
}

/// @brief 验证不同 ECS 注册表中重叠的实体 ID 不会被 DrawTool 混淆。
/// @return 悬停自动采样时只删除 Sample 并可通过一次 Undo 恢复时返回 true。
bool testSampleEraseTargetsTypedRegistry()
{
    MMM::Logic::SessionContext context;
    const auto                 noteEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(noteEntity);
    const auto sampleEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "effect.wav",
        });
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
         context.sampleRegistry.valid(sampleEntity) ||
         context.actionStack.getUndoStackSize() != 1 ||
         !context.eraserState.targetEntities.empty() ) {
        XERROR("Sample eraser did not delete only the typed sample target");
        return false;
    }
    context.actionStack.undo(context);
    return context.noteRegistry.valid(noteEntity) &&
           context.sampleRegistry.valid(sampleEntity) &&
           context.sampleRegistry.get<MMM::Logic::SampleComponent>(sampleEntity)
                   .m_audioResourceId == "effect.wav";
}

/// @brief 验证自动采样悬浮检视包含锚点、实际触发点和音频字段。
/// @return offset handle 检视快照完整保留资源、音量、偏移与两类时间点时返回
/// true。
bool testSampleHoverInspectDetails()
{
    auto beatmap                           = std::make_shared<MMM::BeatMap>();
    beatmap->m_baseMapMetadata.track_count = 4;
    beatmap->m_baseMapMetadata.bgm_track_count = 1;
    MMM::Note note;
    note.m_timestamp = 1250.0;
    note.m_track     = 0;
    note.setSampleBinding({ "wrong-note-effect.wav", 0.9F });
    beatmap->m_noteData.notes.push_back(std::move(note));
    beatmap->m_audioSamples.push_back({
        .m_timestamp       = 1250.0,
        .m_offsetMs        = -125,
        .m_track           = 4,
        .m_audioResourceId = "detail-effect.wav",
        .m_volume          = 0.35F,
    });

    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    MMM::Logic::SessionUtils::loadBeatmap(context, beatmap);
    configureObjectEditingCanvas(context);
    const auto sampleView =
        context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    const auto noteView =
        context.noteRegistry.view<MMM::Logic::NoteComponent>();
    if ( sampleView.size() != 1 || noteView.size() != 1 ) {
        XERROR("Sample hover inspect setup did not load overlapping objects");
        return false;
    }
    const auto entity     = *sampleView.begin();
    const auto noteEntity = *noteView.begin();
    if ( entity != noteEntity ) {
        XERROR("Sample hover inspect setup did not overlap ECS entity IDs");
        return false;
    }
    auto* interaction =
        context.sampleRegistry.try_get<MMM::Logic::InteractionComponent>(
            entity);
    if ( !interaction ) {
        interaction =
            &context.sampleRegistry.emplace<MMM::Logic::InteractionComponent>(
                entity);
    }
    interaction->isHovered = true;
    interaction->hoveredPart =
        static_cast<std::uint8_t>(MMM::Logic::HoverPart::SampleOffset);
    context.hoveredEntity     = entity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::AudioSample;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::SampleOffset);
    context.mouseCameraId   = "Basic2DCanvas";
    context.isMouseInCanvas = true;
    context.lastMousePos    = { 550.0F, 300.0F };

    const auto config = context.lastConfig;
    session.update(0.0, config, true);
    const auto bufferIt = context.syncBuffers.find("Basic2DCanvas");
    if ( bufferIt == context.syncBuffers.end() || !bufferIt->second ) {
        XERROR("Sample hover inspect did not publish a main-canvas snapshot");
        return false;
    }
    const auto* snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot ) return false;
    const auto& inspect = snapshot->hoverInspect;
    if ( !inspect.show ||
         inspect.kind != MMM::Logic::HoverInspectKind::AudioSampleTrigger ||
         !inspect.showAudioSample || !inspect.showAudioPreview ||
         inspect.entity != entity ||
         inspect.objectKind != MMM::Logic::ChartObjectKind::AudioSample ||
         !inspect.head.show || !inspect.end.show || !inspect.showTrack ||
         inspect.audioResourceId != "detail-effect.wav" ||
         !near(inspect.volume, 0.35) || inspect.offsetMs != -125 ||
         inspect.track != 4 || !near(inspect.head.time, 1.25) ||
         !near(inspect.end.time, 1.125) ) {
        XERROR("Sample hover inspect omitted audio or effective-time details");
        return false;
    }
    return true;
}

/// @brief 验证悬浮检视与常用分拍编辑手势只生成单轨单拍临时预览。
/// @return 悬浮、拖动与绘制状态均使用正确轨道、拍区间和分拍来源时返回 true。
bool testHoverSubdivisionPreviewUsesInspectedTrackAndBeat()
{
    auto beatmap                           = std::make_shared<MMM::BeatMap>();
    beatmap->m_baseMapMetadata.track_count = 6;
    beatmap->m_baseMapMetadata.preference_bpm = 120.0;

    MMM::Timing timing;
    timing.m_timestamp             = 0.0;
    timing.m_bpm                   = 120.0;
    timing.m_beat_length           = 500.0;
    timing.m_timingEffect          = MMM::TimingEffect::BPM;
    timing.m_timingEffectParameter = 120.0;
    beatmap->m_timings.push_back(timing);

    MMM::Note note;
    note.m_timestamp = 1166.6666666667;
    note.m_track     = 5;
    beatmap->m_noteData.notes.push_back(note);

    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    MMM::Logic::SessionUtils::loadBeatmap(context, beatmap);
    configureObjectEditingCanvas(context);
    context.currentBeatmap->m_baseMapMetadata.track_count = 6;
    context.trackCount                                    = 6;
    context.lastConfig.settings.beatDivisor               = 4;

    const auto view = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    if ( view.size() != 1 ) {
        XERROR("Hover subdivision preview setup did not load one Note");
        return false;
    }
    const auto entity = *view.begin();
    auto&      interaction =
        context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(entity);
    interaction.isHovered = true;
    interaction.hoveredPart =
        static_cast<std::uint8_t>(MMM::Logic::HoverPart::Head);
    context.hoveredEntity     = entity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::PlayerNote;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::Head);
    context.mouseCameraId   = "Basic2DCanvas";
    context.isMouseInCanvas = true;
    context.lastMousePos    = { 350.0F, 200.0F };

    auto config = context.lastConfig;
    session.update(0.0, config, true);
    const auto bufferIt = context.syncBuffers.find("Basic2DCanvas");
    if ( bufferIt == context.syncBuffers.end() || !bufferIt->second ) {
        XERROR("Hover subdivision preview did not publish a canvas snapshot");
        return false;
    }
    const auto* snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot ) return false;
    const auto& preview = snapshot->hoverSubdivisionPreview;
    if ( !preview.show || preview.track != 5 || preview.numerator != 1 ||
         preview.denominator != 3 || !near(preview.beatStartTime, 1.0) ||
         !near(preview.beatEndTime, 1.5) || !near(preview.beatDuration, 0.5) ) {
        XERROR("Hover subdivision preview escaped the inspected track or beat");
        return false;
    }

    context.isDragging        = true;
    context.draggedEntity     = entity;
    context.draggedObjectKind = MMM::Logic::ChartObjectKind::PlayerNote;
    context.draggedPart       = MMM::Logic::HoverPart::Head;
    session.update(0.0, config, true);
    snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot || snapshot->hoverSubdivisionPreview.show ) {
        XERROR("Dragging did not restore the configured beat grid preview");
        return false;
    }

    std::uint32_t commonBeatDivisorMask = 0U;
    MMM::Config::setCommonBeatDivisorEnabled(commonBeatDivisorMask, 3, true);
    MMM::Config::setCommonBeatDivisorEnabled(commonBeatDivisorMask, 5, true);
    config.settings.objectPlacementSnap = true;
    config.settings.objectPlacementSnapMode =
        MMM::Config::ObjectPlacementSnapMode::CommonBeatDivisors;
    config.settings.commonBeatDivisorMask = commonBeatDivisorMask;
    session.update(0.0, config, true);
    snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot || !snapshot->hoverSubdivisionPreview.show ||
         snapshot->hoverSubdivisionPreview.track != 5 ||
         snapshot->hoverSubdivisionPreview.commonBeatDivisorMask !=
             commonBeatDivisorMask ||
         !near(snapshot->hoverSubdivisionPreview.focusTime,
               note.m_timestamp / 1000.0) ||
         !near(snapshot->hoverSubdivisionPreview.beatStartTime, 1.0) ||
         !near(snapshot->hoverSubdivisionPreview.beatEndTime, 1.5) ) {
        XERROR("Common-divisor drag preview did not follow the dragged Note");
        return false;
    }
    context.isDragging    = false;
    context.draggedEntity = entt::null;

    config.settings.beatDivisor = 6;
    session.update(0.0, config, true);
    snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot || snapshot->hoverSubdivisionPreview.show ) {
        XERROR("Compatible current beat grid still enabled hover preview");
        return false;
    }

    interaction.isHovered                 = false;
    context.hoveredEntity                 = entt::null;
    context.brushState.isActive           = true;
    context.brushState.createsAudioSample = false;
    context.brushState.type               = MMM::NoteType::HOLD;
    context.brushState.time               = 1.0;
    context.brushState.duration           = 0.1;
    context.brushState.track              = 4;
    session.update(0.0, config, true);
    snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot || !snapshot->hoverSubdivisionPreview.show ||
         snapshot->hoverSubdivisionPreview.track != 4 ||
         snapshot->hoverSubdivisionPreview.commonBeatDivisorMask !=
             commonBeatDivisorMask ||
         !near(snapshot->hoverSubdivisionPreview.focusTime, 1.1) ||
         !near(snapshot->hoverSubdivisionPreview.beatStartTime, 1.0) ||
         !near(snapshot->hoverSubdivisionPreview.beatEndTime, 1.5) ) {
        XERROR("Common-divisor draw preview did not follow the brush tip");
        return false;
    }

    context.brushState.createsAudioSample = true;
    session.update(0.0, config, true);
    snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot || snapshot->hoverSubdivisionPreview.show ) {
        XERROR(
            "BGM sample brush unexpectedly enabled player subdivision preview");
        return false;
    }
    return true;
}

/// @brief 验证绑定采样的玩家物件会向主画布公开独立试听字段。
/// @return 悬浮信息保留实体类型、资源 ID 和物件音量时返回 true。
bool testBoundNoteHoverInspectAudioPreview()
{
    auto beatmap                           = std::make_shared<MMM::BeatMap>();
    beatmap->m_baseMapMetadata.track_count = 4;
    MMM::Note note;
    note.m_timestamp = 1500.0;
    note.m_track     = 2;
    note.setSampleBinding({ "bound-effect.wav", 0.45F });
    beatmap->m_noteData.notes.push_back(std::move(note));

    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    MMM::Logic::SessionUtils::loadBeatmap(context, beatmap);
    configureObjectEditingCanvas(context);
    const auto view = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    if ( view.size() != 1 ) {
        XERROR("Bound Note hover inspect setup did not load one player note");
        return false;
    }
    const auto entity = *view.begin();
    auto&      interaction =
        context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(entity);
    interaction.isHovered = true;
    interaction.hoveredPart =
        static_cast<std::uint8_t>(MMM::Logic::HoverPart::Head);
    context.hoveredEntity     = entity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::PlayerNote;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::Head);
    context.mouseCameraId   = "Basic2DCanvas";
    context.isMouseInCanvas = true;
    context.lastMousePos    = { 375.0F, 300.0F };

    const auto config = context.lastConfig;
    session.update(0.0, config, true);
    const auto bufferIt = context.syncBuffers.find("Basic2DCanvas");
    if ( bufferIt == context.syncBuffers.end() || !bufferIt->second ) {
        XERROR(
            "Bound Note hover inspect did not publish a main-canvas snapshot");
        return false;
    }
    const auto* snapshot = bufferIt->second->pullLatestSnapshot();
    if ( !snapshot ) return false;
    const auto& inspect = snapshot->hoverInspect;
    if ( !inspect.show || !inspect.showAudioPreview ||
         inspect.showAudioSample || inspect.entity != entity ||
         inspect.objectKind != MMM::Logic::ChartObjectKind::PlayerNote ||
         inspect.audioResourceId != "bound-effect.wav" ||
         !near(inspect.volume, 0.45) ) {
        XERROR("Bound Note hover inspect omitted audio preview details");
        return false;
    }
    return true;
}

/// @brief 验证自动采样在 BGM 区拖到追加轨后只提交一次可撤销更新。
/// @return 锚点、轨道、BGM 数量及 Undo/Redo 均正确时返回 true。
bool testSampleAnchorDragUsesSingleAction()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);

    const auto entity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        entity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "effect.wav",
        });
    context.sampleRegistry.emplace<MMM::Logic::InteractionComponent>(entity);
    context.hoveredEntity     = entity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::AudioSample;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::SampleAnchor);

    MMM::Logic::GrabTool tool;
    tool.handleStartDrag(context,
                         MMM::Logic::CmdStartDrag{
                             entity,
                             "Basic2DCanvas",
                             false,
                             MMM::Logic::ChartObjectKind::AudioSample,
                         });
    tool.handleUpdateDrag(context,
                          MMM::Logic::CmdUpdateDrag{
                              "Basic2DCanvas",
                              650.0F,
                              50.0F,
                              true,
                          });
    tool.handleEndDrag(context, MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    const auto& moved =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity);
    if ( !near(moved.m_timestamp, 1.5) || moved.m_track != 5 ||
         context.bgmTrackCount != 2 ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Sample anchor drag did not commit one append-lane action");
        return false;
    }

    context.actionStack.undo(context);
    const auto& restored =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity);
    if ( !near(restored.m_timestamp, 1.0) || restored.m_track != 4 ||
         context.bgmTrackCount != 1 ) {
        XERROR("Sample anchor drag undo did not restore the original sample");
        return false;
    }
    context.actionStack.redo(context);
    const auto& redone =
        context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity);
    return near(redone.m_timestamp, 1.5) && redone.m_track == 5 &&
           context.bgmTrackCount == 2;
}

/// @brief 验证拖放命令不能绕过项目资源表创建悬空自动采样。
/// @return 当前项目中无法解析资源时不创建实体或撤销动作。
bool testAudioResourceDropRejectsMissingProjectResource()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    MMM::Logic::InteractionController controller(context);

    controller.handleCommand(MMM::Logic::CmdCreateAudioSample{
        .audioResourceId = "main-track-id",
        .cameraId        = "Basic2DCanvas",
        .mouseX          = 650.0F,
        .mouseY          = 50.0F,
        .isCtrlDown      = true,
    });

    return context.sampleRegistry.view<MMM::Logic::SampleComponent>().empty() &&
           context.actionStack.getUndoStackSize() == 0 &&
           !context.lastActionMessage.empty();
}

/// @brief 验证实际触发 handle 可产生有符号 offset 并完整撤销。
/// @return 负 offset、Undo 与 Redo 均正确时返回 true。
bool testSampleOffsetHandleDrag()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);

    const auto entity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        entity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "effect.wav",
        });
    context.sampleRegistry.emplace<MMM::Logic::InteractionComponent>(entity);
    context.hoveredEntity     = entity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::AudioSample;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::SampleOffset);

    MMM::Logic::GrabTool tool;
    tool.handleStartDrag(context,
                         MMM::Logic::CmdStartDrag{
                             entity,
                             "Basic2DCanvas",
                             false,
                             MMM::Logic::ChartObjectKind::AudioSample,
                         });
    tool.handleUpdateDrag(context,
                          MMM::Logic::CmdUpdateDrag{
                              "Basic2DCanvas",
                              582.0F,
                              425.0F,
                              true,
                          });
    tool.handleEndDrag(context, MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    if ( context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity)
                 .m_offsetMs != -250 ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Sample offset handle did not commit a signed offset");
        return false;
    }
    context.actionStack.undo(context);
    if ( context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity)
             .m_offsetMs != 0 ) {
        XERROR("Sample offset handle undo did not restore zero offset");
        return false;
    }
    context.actionStack.redo(context);
    return context.sampleRegistry.get<MMM::Logic::SampleComponent>(entity)
               .m_offsetMs == -250;
}

/// @brief 验证 Note 与自动采样的转换规则严格区分 Effect、Main 和物件类型。
/// @return 仅规格允许的两个方向能构造转换结果时返回 true。
bool testCrossAreaConversionRules()
{
    MMM::AudioResource effect{
        .m_id   = "effect.wav",
        .m_type = MMM::AudioTrackType::Effect,
    };
    MMM::AudioResource main{
        .m_id   = "main.ogg",
        .m_type = MMM::AudioTrackType::Main,
    };
    MMM::Logic::SampleComponent sample{
        .m_timestamp       = 1.25,
        .m_track           = 4,
        .m_audioResourceId = "effect.wav",
        .m_volume          = 0.35F,
    };

    const auto note = MMM::Logic::makePlayerNoteFromSample(sample, 2, &effect);
    auto       offsetSample = sample;
    offsetSample.m_offsetMs = 1;
    if ( !note || note->m_type != MMM::NoteType::NOTE ||
         note->m_trackIndex != 2 || !note->m_sampleBinding ||
         note->m_sampleBinding->m_audioResourceId != "effect.wav" ||
         !near(note->m_sampleBinding->m_volume, 0.35) ||
         MMM::Logic::makePlayerNoteFromSample(sample, 2, &main) ||
         MMM::Logic::makePlayerNoteFromSample(offsetSample, 2, &effect) ) {
        XERROR("Sample-to-Note conversion accepted an invalid source");
        return false;
    }

    auto emptySample              = sample;
    emptySample.m_audioResourceId = {};
    const auto unboundNote =
        MMM::Logic::makePlayerNoteFromSample(emptySample, 1, nullptr);
    if ( !unboundNote || unboundNote->m_type != MMM::NoteType::NOTE ||
         unboundNote->m_trackIndex != 1 || unboundNote->m_sampleBinding ) {
        XERROR("Silent sample draft did not convert to an unbound player Tap");
        return false;
    }

    const auto convertedSample =
        MMM::Logic::makeAudioSampleFromPlayerNote(*note, 5, &effect);
    auto hold    = *note;
    hold.m_type  = MMM::NoteType::HOLD;
    auto unbound = *note;
    unbound.m_sampleBinding.reset();
    const auto silentSample =
        MMM::Logic::makeAudioSampleFromPlayerNote(unbound, 6, nullptr);
    if ( !convertedSample || convertedSample->m_track != 5 ||
         convertedSample->m_offsetMs != 0 ||
         convertedSample->m_audioResourceId != "effect.wav" ||
         !near(convertedSample->m_volume, 0.35) || !silentSample ||
         silentSample->m_track != 6 ||
         !silentSample->m_audioResourceId.empty() ||
         !near(silentSample->m_volume, 1.0) ||
         MMM::Logic::makeAudioSampleFromPlayerNote(*note, 5, &main) ||
         MMM::Logic::makeAudioSampleFromPlayerNote(hold, 5, &effect) ) {
        XERROR("Note-to-Sample conversion accepted an invalid source");
        return false;
    }
    return true;
}

/// @brief 验证空采样草稿拖回玩家轨道后成为可撤销的未绑定 Tap。
/// @return 转换、Undo 与 Redo 均保持空资源语义时返回 true。
bool testSilentSampleDragConvertsToUnboundNote()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);

    const auto sampleEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp = 1.0,
            .m_track     = 4,
        });
    context.sampleRegistry.emplace<MMM::Logic::InteractionComponent>(
        sampleEntity, MMM::Logic::InteractionComponent{ .isSelected = true });
    context.hoveredEntity     = sampleEntity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::AudioSample;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::SampleAnchor);

    MMM::Logic::GrabTool tool;
    tool.handleStartDrag(context,
                         MMM::Logic::CmdStartDrag{
                             sampleEntity,
                             "Basic2DCanvas",
                             false,
                             MMM::Logic::ChartObjectKind::AudioSample,
                         });
    tool.handleUpdateDrag(context,
                          MMM::Logic::CmdUpdateDrag{
                              "Basic2DCanvas",
                              150.0F,
                              300.0F,
                              true,
                          });
    tool.handleEndDrag(context, MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    auto notes   = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    auto samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( !samples.empty() || notes.size() != 1 ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Silent sample drag did not commit one cross-area conversion");
        return false;
    }
    const auto  noteEntity = *notes.begin();
    const auto& note       = notes.get<MMM::Logic::NoteComponent>(noteEntity);
    if ( note.m_type != MMM::NoteType::NOTE || note.m_trackIndex != 0 ||
         note.m_sampleBinding ) {
        XERROR("Silent sample drag created a bound or non-Tap player object");
        return false;
    }

    context.actionStack.undo(context);
    auto restoredSamples =
        context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( restoredSamples.size() != 1 ||
         !context.noteRegistry.view<MMM::Logic::NoteComponent>().empty() ||
         !restoredSamples
              .get<MMM::Logic::SampleComponent>(*restoredSamples.begin())
              .m_audioResourceId.empty() ) {
        XERROR("Silent sample conversion undo did not restore the draft");
        return false;
    }

    context.actionStack.redo(context);
    notes   = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    return samples.empty() && notes.size() == 1 &&
           !notes.get<MMM::Logic::NoteComponent>(*notes.begin())
                .m_sampleBinding;
}

/// @brief 验证选取工具按住已选物件时复用统一抓取并允许跨入 BGM 区。
/// @return 未绑定 Note 经选取工具命令路由转换为静音采样且可撤销时返回 true。
bool testMarqueeToolEntityDragCrossesCanvasAreas()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.currentTool = MMM::Logic::EditTool::Marquee;

    const auto noteEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        noteEntity,
        MMM::Logic::NoteComponent{
            .m_timestamp  = 1.0,
            .m_trackIndex = 0,
        });
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(noteEntity);

    MMM::Logic::InteractionController controller(context);
    controller.handleCommand(MMM::Logic::CmdSelectEntity{
        noteEntity,
        true,
        MMM::Logic::ChartObjectKind::PlayerNote,
    });
    controller.handleCommand(MMM::Logic::CmdSetHoveredEntity{
        noteEntity,
        static_cast<std::uint8_t>(MMM::Logic::HoverPart::Head),
        -1,
        MMM::Logic::ChartObjectKind::PlayerNote,
    });
    controller.handleCommand(MMM::Logic::CmdStartDrag{
        noteEntity,
        "Basic2DCanvas",
        false,
        MMM::Logic::ChartObjectKind::PlayerNote,
    });
    controller.handleCommand(MMM::Logic::CmdUpdateDrag{
        "Basic2DCanvas",
        550.0F,
        300.0F,
        true,
    });
    controller.handleCommand(MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    const auto samples =
        context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( !context.noteRegistry.view<MMM::Logic::NoteComponent>().empty() ||
         samples.size() != 1 || context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Marquee tool did not commit one cross-area conversion");
        return false;
    }
    const auto& sample =
        samples.get<MMM::Logic::SampleComponent>(*samples.begin());
    if ( sample.m_track != 4 || !sample.m_audioResourceId.empty() ) {
        XERROR("Marquee tool produced the wrong BGM sample");
        return false;
    }

    context.actionStack.undo(context);
    return context.noteRegistry.view<MMM::Logic::NoteComponent>().size() == 1 &&
           context.sampleRegistry.view<MMM::Logic::SampleComponent>().empty();
}

/// @brief 验证未绑定 Tap 拖入 BGM 轨道后成为可撤销的空采样草稿。
/// @return 转换、Undo 与 Redo 均保持未绑定音频语义时返回 true。
bool testUnboundNoteDragConvertsToSilentSample()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);

    const auto noteEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        noteEntity,
        MMM::Logic::NoteComponent{
            .m_type       = MMM::NoteType::NOTE,
            .m_timestamp  = 1.0,
            .m_trackIndex = 0,
        });
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        noteEntity, MMM::Logic::InteractionComponent{ .isSelected = true });
    context.hoveredEntity     = noteEntity;
    context.hoveredObjectKind = MMM::Logic::ChartObjectKind::PlayerNote;
    context.hoveredPart =
        static_cast<std::int32_t>(MMM::Logic::HoverPart::Head);

    MMM::Logic::GrabTool tool;
    tool.handleStartDrag(context,
                         MMM::Logic::CmdStartDrag{
                             noteEntity,
                             "Basic2DCanvas",
                             false,
                             MMM::Logic::ChartObjectKind::PlayerNote,
                         });
    tool.handleUpdateDrag(context,
                          MMM::Logic::CmdUpdateDrag{
                              "Basic2DCanvas",
                              550.0F,
                              300.0F,
                              true,
                          });
    tool.handleEndDrag(context, MMM::Logic::CmdEndDrag{ "Basic2DCanvas" });

    auto notes   = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    auto samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( !notes.empty() || samples.size() != 1 ||
         context.actionStack.getUndoStackSize() != 1 ) {
        XERROR("Unbound Note drag did not commit one cross-area conversion");
        return false;
    }
    const auto& sample =
        samples.get<MMM::Logic::SampleComponent>(*samples.begin());
    if ( sample.m_track != 4 || !sample.m_audioResourceId.empty() ||
         !near(sample.m_volume, 1.0) ) {
        XERROR("Unbound Note drag created a bound or misplaced sample");
        return false;
    }

    context.actionStack.undo(context);
    notes   = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( notes.size() != 1 || !samples.empty() ||
         notes.get<MMM::Logic::NoteComponent>(*notes.begin())
             .m_sampleBinding ) {
        XERROR("Unbound Note conversion undo did not restore the Tap");
        return false;
    }

    context.actionStack.redo(context);
    notes   = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    return notes.empty() && samples.size() == 1 &&
           samples.get<MMM::Logic::SampleComponent>(*samples.begin())
               .m_audioResourceId.empty();
}

/// @brief 验证跨 Registry 的同值实体 ID 在复合转换及 Undo 中不会串对象。
/// @return 目标与来源选中状态均按领域恢复时返回 true。
bool testCompositeConversionUsesTypedIdentity()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    const auto sampleEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "effect.wav",
        });
    context.sampleRegistry.emplace<MMM::Logic::InteractionComponent>(
        sampleEntity, MMM::Logic::InteractionComponent{ .isSelected = true });
    const auto noteEntity = context.noteRegistry.create();
    if ( noteEntity != sampleEntity ) {
        XERROR("Typed identity test did not obtain overlapping entity IDs");
        return false;
    }

    MMM::Logic::NoteComponent note;
    note.m_timestamp     = 1.0;
    note.m_trackIndex    = 2;
    note.m_sampleBinding = MMM::AudioSampleBinding{ "effect.wav", 1.0F };
    std::vector<std::unique_ptr<MMM::Logic::IEditorAction>> actions;
    actions.push_back(std::make_unique<MMM::Logic::BatchNoteAction>(
        std::vector<MMM::Logic::BatchNoteAction::Entry>{
            {
                .entity        = noteEntity,
                .before        = std::nullopt,
                .after         = note,
                .afterSelected = true,
            },
        }));
    actions.push_back(std::make_unique<MMM::Logic::BatchSampleAction>(
        std::vector<MMM::Logic::BatchSampleAction::Entry>{
            {
                .entity = sampleEntity,
                .before = context.sampleRegistry
                              .get<MMM::Logic::SampleComponent>(sampleEntity),
                .after  = std::nullopt,
                .beforeSelected = true,
            },
        }));
    context.actionStack.pushAndExecute(
        std::make_unique<MMM::Logic::CompositeEditorAction>(std::move(actions),
                                                            "测试跨区转换"),
        context);
    if ( context.sampleRegistry.valid(sampleEntity) ||
         !context.noteRegistry.valid(noteEntity) ||
         !context.noteRegistry.get<MMM::Logic::InteractionComponent>(noteEntity)
              .isSelected ||
         !context.selectedNoteEntities.contains(noteEntity) ||
         !context.selectedSampleEntities.empty() ) {
        XERROR("Composite conversion mixed overlapping Registry identities");
        return false;
    }

    context.actionStack.undo(context);
    if ( !context.sampleRegistry.valid(sampleEntity) ||
         context.noteRegistry.valid(noteEntity) ||
         !context.sampleRegistry
              .get<MMM::Logic::InteractionComponent>(sampleEntity)
              .isSelected ||
         !context.selectedSampleEntities.contains(sampleEntity) ||
         !context.selectedNoteEntities.empty() ) {
        XERROR("Composite conversion undo did not restore typed selection");
        return false;
    }
    context.actionStack.redo(context);
    return !context.sampleRegistry.valid(sampleEntity) &&
           context.noteRegistry.valid(noteEntity) &&
           context.selectedNoteEntities.contains(noteEntity) &&
           context.selectedSampleEntities.empty();
}

/// @brief 验证主画布框选按带类型身份选中 Sample，且 Preview 忽略 Sample。
/// @return 同值 Note 未被串选且 Preview 不处理自动采样时返回 true。
bool testMarqueeSelectsTypedSamplesOnlyOnMainCanvas()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    context.cameras.emplace(
        "Preview", MMM::Logic::CameraInfo{ "Preview", 1000.0F, 600.0F, 0.0F });

    const auto noteEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        noteEntity,
        MMM::Logic::NoteComponent{
            .m_timestamp  = 1.0,
            .m_trackIndex = 0,
        });
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(noteEntity);
    const auto sampleEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 1.0,
            .m_track           = 4,
            .m_audioResourceId = "effect.wav",
        });
    context.sampleRegistry.emplace<MMM::Logic::InteractionComponent>(
        sampleEntity);
    if ( noteEntity != sampleEntity ) {
        XERROR("Marquee identity test did not obtain overlapping entity IDs");
        return false;
    }

    context.sortedNoteEntities       = { noteEntity };
    context.sortedNoteMaxEndPrefix   = { 1.0 };
    context.sortedSampleEntities     = { sampleEntity };
    context.sortedSampleMaxEndPrefix = { 1.0 };
    context.marqueeBoxes             = {
        MMM::Logic::MarqueeBox{
            .startTime  = 0.9,
            .endTime    = 1.1,
            .startTrack = 4.05F,
            .endTrack   = 4.95F,
            .cameraId   = "Basic2DCanvas",
        },
    };
    context.isMarqueeSelectionDirty = true;

    MMM::Logic::InteractionController controller(context);
    controller.updateMarqueeSelection();
    if ( !context.sampleRegistry
              .get<MMM::Logic::InteractionComponent>(sampleEntity)
              .isSelected ||
         context.noteRegistry.get<MMM::Logic::InteractionComponent>(noteEntity)
             .isSelected ||
         !context.selectedNoteEntities.empty() ||
         !context.selectedSampleEntities.contains(sampleEntity) ) {
        XERROR("Main-canvas marquee confused typed sample and note identity");
        return false;
    }

    context.marqueeBoxes = {
        MMM::Logic::MarqueeBox{
            .startTime  = 0.9,
            .endTime    = 1.1,
            .startTrack = 4.05F,
            .endTrack   = 4.95F,
            .cameraId   = "Preview",
        },
    };
    context.isMarqueeSelectionDirty = true;
    controller.updateMarqueeSelection(true);
    return !context.sampleRegistry
                .get<MMM::Logic::InteractionComponent>(sampleEntity)
                .isSelected &&
           context.selectedNoteEntities.empty() &&
           context.selectedSampleEntities.empty();
}

/// @brief 验证混合 Note/Sample 跨会话粘贴共用时间锚点和相对 BGM 轨道。
/// @return 镜像仅作用 Note，且一次 Undo 同时移除两类新物件时返回 true。
bool testMixedChartObjectClipboardAcrossSessions()
{
    MMM::Logic::SessionContext source;
    configureObjectEditingCanvas(source);
    const auto noteEntity = source.noteRegistry.create();
    source.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        noteEntity,
        MMM::Logic::NoteComponent{
            .m_timestamp  = 2.0,
            .m_trackIndex = 1,
        });
    source.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        noteEntity, MMM::Logic::InteractionComponent{ .isSelected = true });
    const auto sampleEntity = source.sampleRegistry.create();
    source.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 3.0,
            .m_offsetMs        = -80,
            .m_track           = 6,
            .m_audioResourceId = "stem.ogg",
            .m_volume          = 0.4F,
        });
    source.sampleRegistry.emplace<MMM::Logic::InteractionComponent>(
        sampleEntity, MMM::Logic::InteractionComponent{ .isSelected = true });

    MMM::Logic::ActionController sourceController(source);
    sourceController.handleCommand(MMM::Logic::CmdCopy{});

    MMM::Logic::SessionContext target;
    configureObjectEditingCanvas(target);
    target.trackCount                                    = 6;
    target.currentBeatmap->m_baseMapMetadata.track_count = 6;
    target.animateTime                                   = 10.0;
    MMM::Logic::ActionController targetController(target);
    targetController.handleCommand(MMM::Logic::CmdPaste{
        .m_mirrored            = true,
        .m_selectPastedObjects = true,
    });

    auto notes   = target.noteRegistry.view<MMM::Logic::NoteComponent>();
    auto samples = target.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( notes.size() != 1 || samples.size() != 1 ||
         target.actionStack.getUndoStackSize() != 1 ||
         target.bgmTrackCount != 3 ) {
        XERROR("Mixed clipboard paste did not create one atomic object batch");
        return false;
    }
    const auto  pastedNoteEntity   = *notes.begin();
    const auto  pastedSampleEntity = *samples.begin();
    const auto& note = notes.get<MMM::Logic::NoteComponent>(pastedNoteEntity);
    const auto& sample =
        samples.get<MMM::Logic::SampleComponent>(pastedSampleEntity);
    if ( !near(note.m_timestamp, 10.0) || note.m_trackIndex != 4 ||
         !near(sample.m_timestamp, 11.0) || sample.m_track != 8 ||
         sample.m_offsetMs != -80 || sample.m_audioResourceId != "stem.ogg" ||
         !near(sample.m_volume, 0.4) ||
         !target.noteRegistry
              .get<MMM::Logic::InteractionComponent>(pastedNoteEntity)
              .isSelected ||
         !target.sampleRegistry
              .get<MMM::Logic::InteractionComponent>(pastedSampleEntity)
              .isSelected ) {
        XERROR("Mixed clipboard paste changed timing, lane, mirror or fields");
        return false;
    }

    target.actionStack.undo(target);
    if ( !target.noteRegistry.view<MMM::Logic::NoteComponent>().empty() ||
         !target.sampleRegistry.view<MMM::Logic::SampleComponent>().empty() ) {
        XERROR("One undo did not remove both mixed pasted object kinds");
        return false;
    }
    target.actionStack.redo(target);
    return target.noteRegistry.view<MMM::Logic::NoteComponent>().size() == 1 &&
           target.sampleRegistry.view<MMM::Logic::SampleComponent>().size() ==
               1;
}

/// @brief 验证本会话混合剪切粘贴会原子删除来源并创建目标物件。
/// @return 一次 Undo 恢复两类来源物件并移除粘贴副本时返回 true。
bool testMixedChartObjectLocalCut()
{
    MMM::Logic::SessionContext context;
    configureObjectEditingCanvas(context);
    const auto noteEntity = context.noteRegistry.create();
    context.noteRegistry.emplace<MMM::Logic::NoteComponent>(
        noteEntity,
        MMM::Logic::NoteComponent{
            .m_timestamp  = 2.0,
            .m_trackIndex = 1,
        });
    context.noteRegistry.emplace<MMM::Logic::InteractionComponent>(
        noteEntity, MMM::Logic::InteractionComponent{ .isSelected = true });
    const auto sampleEntity = context.sampleRegistry.create();
    context.sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 3.0,
            .m_track           = 5,
            .m_audioResourceId = "effect.wav",
        });
    context.sampleRegistry.emplace<MMM::Logic::InteractionComponent>(
        sampleEntity, MMM::Logic::InteractionComponent{ .isSelected = true });

    MMM::Logic::ActionController controller(context);
    controller.handleCommand(MMM::Logic::CmdCut{});
    context.animateTime = 6.0;
    controller.handleCommand(MMM::Logic::CmdPaste{});

    auto notes   = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    auto samples = context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    if ( notes.size() != 1 || samples.size() != 1 ||
         context.actionStack.getUndoStackSize() != 1 ||
         !near(notes.get<MMM::Logic::NoteComponent>(*notes.begin()).m_timestamp,
               6.0) ||
         !near(samples.get<MMM::Logic::SampleComponent>(*samples.begin())
                   .m_timestamp,
               7.0) ) {
        XERROR("Local mixed cut did not atomically replace source objects");
        return false;
    }

    context.actionStack.undo(context);
    auto restoredNotes = context.noteRegistry.view<MMM::Logic::NoteComponent>();
    auto restoredSamples =
        context.sampleRegistry.view<MMM::Logic::SampleComponent>();
    return restoredNotes.size() == 1 && restoredSamples.size() == 1 &&
           near(restoredNotes
                    .get<MMM::Logic::NoteComponent>(*restoredNotes.begin())
                    .m_timestamp,
                2.0) &&
           near(restoredSamples
                    .get<MMM::Logic::SampleComponent>(*restoredSamples.begin())
                    .m_timestamp,
                3.0);
}

}  // namespace

/// @brief 运行主画布二维相机换算测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testKeyModeInteractionRestriction() &&
                   testKeyModeBrushCreatesOnlyHold() &&
                   testBmsEditingHidesBgmLanes() &&
                   testBrushAudioResourcePlacementRules() &&
                   testSampleBrushFollowsPointerBeforeCommit() &&
                   testBrushCrossesPlayerAndBgmLanes() &&
                   testTrackProjectionUsesCameraOffset() &&
                   testUnifiedLaneProjection() &&
                   testResizePreservesNormalizedOffset() &&
                   testPanCommandUsesLogicalPixels() &&
                   testTrackCountActionMigratesAllSamples() &&
                   testSessionSelectsKeyCountLayout() &&
                   testQueuedBrushUsesKeyCountLayout() &&
                   testTrackCountOverflowIsRejectedAtomically() &&
                   testMetadataTrackCountMigrationIsAtomic() &&
                   testReplaceBeatmapMetadataMigratesSamples() &&
                   testReplaceBeatmapMetadataOverflowIsRejected() &&
                   testAuthoritativeReplacementInvalidatesEntityState() &&
                   testPolylineSubNoteIdentitySurvivesRepeatedEcsSync() &&
                   testAppendLaneExpandsPersistentCount() &&
                   testExplicitBgmTrackCountAction() &&
                   testSamplePropertyEditValidationAndAction() &&
                   testSampleRegistryLoadAndSync() &&
                   testNoteSampleBindingRoundTrip() &&
                   testObjectSampleVolumeCommand() &&
                   testObjectSampleVolumeCommandRoutesThroughSession() &&
                   testSampleEraseTargetsTypedRegistry() &&
                   testSampleHoverInspectDetails() &&
                   testHoverSubdivisionPreviewUsesInspectedTrackAndBeat() &&
                   testBoundNoteHoverInspectAudioPreview() &&
                   testSampleAnchorDragUsesSingleAction() &&
                   testAudioResourceDropRejectsMissingProjectResource() &&
                   testSampleOffsetHandleDrag() &&
                   testCrossAreaConversionRules() &&
                   testSilentSampleDragConvertsToUnboundNote() &&
                   testMarqueeToolEntityDragCrossesCanvasAreas() &&
                   testUnboundNoteDragConvertsToSilentSample() &&
                   testCompositeConversionUsesTypedIdentity() &&
                   testMarqueeSelectsTypedSamplesOnlyOnMainCanvas() &&
                   testMixedChartObjectClipboardAcrossSessions() &&
                   testMixedChartObjectLocalCut()
               ? 0
               : 1;
}
