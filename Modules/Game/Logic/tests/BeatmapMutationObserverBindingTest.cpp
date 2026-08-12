#include "common/LogicCommands.h"
#include "config/EditorConfig.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "logic/BeatmapSession.h"
#include "logic/ProjectController.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/beatmap/BeatmapMutationObserver.h"

#include "log/colorful-log.h"

#include <atomic>
#include <cmath>
#include <memory>

namespace
{
/// @brief 记录谱面观察者收到的通知次数与最后一次变化类别。
class CountingMutationObserver final : public MMM::IBeatmapMutationObserver
{
public:
    /// @copydoc MMM::IBeatmapMutationObserver::onBeatmapMutated
    void onBeatmapMutated(const MMM::BeatMap&,
                          MMM::BeatmapMutationFlags flags) override
    {
        ++m_notificationCount;
        m_lastFlags = flags;
    }

    /// @copydoc MMM::IBeatmapMutationObserver::onBeatmapSynchronized
    void onBeatmapSynchronized(const MMM::BeatMap&) override
    {
        ++m_synchronizationCount;
    }

    /// @brief 返回已经收到的通知次数。
    [[nodiscard]] int notificationCount() const { return m_notificationCount; }

    /// @brief 返回最后一次通知的变化类别。
    [[nodiscard]] MMM::BeatmapMutationFlags lastFlags() const
    {
        return m_lastFlags;
    }

    /// @brief 返回已经收到的权威基线同步次数。
    [[nodiscard]] int synchronizationCount() const
    {
        return m_synchronizationCount;
    }

private:
    /// @brief 已经收到的通知次数。
    int m_notificationCount{ 0 };
    /// @brief 最后一次通知的变化类别。
    MMM::BeatmapMutationFlags m_lastFlags{ MMM::BeatmapMutationFlags::None };
    /// @brief 已经收到的权威基线同步次数。
    int m_synchronizationCount{ 0 };
};

/// @brief 创建可载入会话的最小谱面。
/// @return 最小谱面共享对象。
[[nodiscard]] std::shared_ptr<MMM::BeatMap> makeBeatmap()
{
    auto beatmap                           = std::make_shared<MMM::BeatMap>();
    beatmap->m_baseMapMetadata.name        = "Collaboration Snapshot";
    beatmap->m_baseMapMetadata.track_count = 4;
    return beatmap;
}

/// @brief 判断会话中是否存在指定时间与轨道的非折线根 Note。
/// @param session 待检查会话。
/// @param timestamp 时间戳，单位秒。
/// @param track 玩家轨道索引。
/// @return 找到匹配物件时返回 true。
[[nodiscard]] bool hasRootNote(const MMM::Logic::BeatmapSession& session,
                               double timestamp, int track)
{
    const auto& context = session.getContext();
    const auto  notes =
        context.noteRegistry.view<const MMM::Logic::NoteComponent>();
    for ( const auto entity : notes ) {
        const auto& note = notes.get<const MMM::Logic::NoteComponent>(entity);
        if ( !note.m_isSubNote && note.m_type == MMM::NoteType::NOTE &&
             std::abs(note.m_timestamp - timestamp) < 1e-9 &&
             note.m_trackIndex == track ) {
            return true;
        }
    }
    return false;
}

/// @brief 判断会话中是否存在已经提交的折线根物件。
/// @param session 待检查会话。
/// @return 找到至少包含两个子段的折线时返回 true。
[[nodiscard]] bool hasCommittedPolyline(
    const MMM::Logic::BeatmapSession& session)
{
    const auto& context = session.getContext();
    const auto  notes =
        context.noteRegistry.view<const MMM::Logic::NoteComponent>();
    for ( const auto entity : notes ) {
        const auto& note = notes.get<const MMM::Logic::NoteComponent>(entity);
        if ( !note.m_isSubNote && note.m_type == MMM::NoteType::POLYLINE &&
             note.m_subNotes.size() >= 2U ) {
            return true;
        }
    }
    return false;
}

/// @brief 验证访客绑定观察者时不会把房主快照回传为本地编辑。
/// @return 禁止初始快照且显式请求仍能发布完整快照时返回 true。
[[nodiscard]] bool testOptionalInitialSnapshot()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = makeBeatmap() },
    });
    session.update(0.0, config, false);
    auto observer = std::make_shared<CountingMutationObserver>();
    session.setMutationObserver(observer, false);
    session.update(0.0, config, false);
    if ( observer->notificationCount() != 0 ) {
        XERROR("Guest observer echoed the received host snapshot");
        return false;
    }

    session.setMutationObserver(observer, true);
    session.update(0.0, config, false);
    if ( observer->notificationCount() != 1 ||
         observer->lastFlags() != MMM::BeatmapMutationFlags::All ) {
        XERROR("Requested mutation snapshot was not published exactly once");
        return false;
    }
    return true;
}

/// @brief 验证 Timeline 窗口使用的单个和批量 Timing 命令都会发布协作变化。
/// @return 两次编辑分别以 Timelines 类型通知观察者时返回 true。
[[nodiscard]] bool testTimelineCommandsPublishMutations()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = makeBeatmap() },
    });
    session.update(0.0, config, false);

    auto observer = std::make_shared<CountingMutationObserver>();
    session.setMutationObserver(observer, false);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdCreateTimelineEvent{
            .time  = 1.25,
            .type  = MMM::TimingEffect::SCROLL,
            .value = 1.5,
        },
    });
    session.update(0.0, config, false);
    if ( observer->notificationCount() != 1 ||
         observer->lastFlags() != MMM::BeatmapMutationFlags::Timelines ) {
        XERROR("Single Timeline command did not publish a mutation");
        return false;
    }

    MMM::Logic::CmdCreateTimelineEvents batch;
    batch.events.push_back(MMM::Logic::CmdCreateTimelineEvents::Entry{
        .time  = 2.5,
        .type  = MMM::TimingEffect::HS,
        .value = 0.75,
    });
    session.pushCommand(MMM::Logic::LogicCommand{ std::move(batch) });
    session.update(0.0, config, false);
    if ( observer->notificationCount() != 2 ||
         observer->lastFlags() != MMM::BeatmapMutationFlags::Timelines ) {
        XERROR("Batch Timeline command did not publish a mutation");
        return false;
    }

    session.pushCommand(MMM::Logic::LogicCommand{ MMM::Logic::CmdUndo{} });
    session.update(0.0, config, false);
    session.pushCommand(MMM::Logic::LogicCommand{ MMM::Logic::CmdRedo{} });
    session.update(0.0, config, false);
    if ( observer->notificationCount() != 4 ||
         observer->lastFlags() != MMM::BeatmapMutationFlags::Timelines ) {
        XERROR("Timeline undo/redo did not publish mutations");
        return false;
    }
    return true;
}

/// @brief 验证折线子音符注释只发布 Annotations 类别并受独立权限门禁。
/// @return 注释、撤销与权限拒绝均保持精确类别和父子组件一致时返回 true。
[[nodiscard]] bool testSubNoteAnnotationPermissionAndMutationFlag()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    auto                       beatmap = makeBeatmap();
    auto&                      hold = beatmap->m_noteData.holds.emplace_back();
    hold.m_timestamp                = 1000.0;
    hold.m_duration                 = 500.0;
    hold.m_track                    = 1;
    hold.m_isSubNote                = true;
    auto& flick                     = beatmap->m_noteData.flicks.emplace_back();
    flick.m_timestamp               = 1500.0;
    flick.m_track                   = 1;
    flick.m_dtrack                  = 1;
    flick.m_isSubNote               = true;
    auto& polyline       = beatmap->m_noteData.polylines.emplace_back();
    polyline.m_timestamp = hold.m_timestamp;
    polyline.m_track     = hold.m_track;
    polyline.m_subNotes.emplace_back(hold);
    polyline.m_subNotes.emplace_back(flick);
    polyline.m_subHolds.emplace_back(hold);
    polyline.m_subFlicks.emplace_back(flick);
    beatmap->sync();
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = std::move(beatmap) },
    });
    session.update(0.0, config, false);

    entt::entity polylineEntity = entt::null;
    const auto view = session.getContext()
                          .noteRegistry.view<const MMM::Logic::NoteComponent>();
    for ( const auto entity : view ) {
        const auto& note = view.get<const MMM::Logic::NoteComponent>(entity);
        if ( !note.m_isSubNote && note.m_type == MMM::NoteType::POLYLINE ) {
            polylineEntity = entity;
            break;
        }
    }
    if ( polylineEntity == entt::null ) return false;

    auto observer = std::make_shared<CountingMutationObserver>();
    session.setMutationObserver(observer, false);
    session.setCollaborationAllowedMutationFlags(
        MMM::BeatmapMutationFlags::Annotations);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdSetNoteAnnotation{
            .entity     = polylineEntity,
            .subIndex   = 0,
            .annotation = "检查折线起段",
        },
    });
    session.update(0.0, config, false);
    const auto& annotated =
        session.getContext().noteRegistry.get<const MMM::Logic::NoteComponent>(
            polylineEntity);
    if ( annotated.m_subNotes.front().annotation != "检查折线起段" ||
         observer->notificationCount() != 1 ||
         observer->lastFlags() != MMM::BeatmapMutationFlags::Annotations ) {
        XERROR("Sub-note annotation did not publish exact mutation category");
        return false;
    }

    session.pushCommand(MMM::Logic::LogicCommand{ MMM::Logic::CmdUndo{} });
    session.update(0.0, config, false);
    if ( !session.getContext()
              .noteRegistry.get<const MMM::Logic::NoteComponent>(polylineEntity)
              .m_subNotes.front()
              .annotation.empty() ||
         observer->notificationCount() != 2 ||
         observer->lastFlags() != MMM::BeatmapMutationFlags::Annotations ) {
        XERROR("Sub-note annotation undo did not preserve mutation category");
        return false;
    }

    session.setCollaborationAllowedMutationFlags(
        MMM::BeatmapMutationFlags::Objects);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdSetNoteAnnotation{
            .entity     = polylineEntity,
            .subIndex   = -1,
            .annotation = "不应写入",
        },
    });
    session.update(0.0, config, false);
    return session.getContext()
               .noteRegistry
               .get<const MMM::Logic::NoteComponent>(polylineEntity)
               .m_annotation.empty() &&
           observer->notificationCount() == 2;
}

/// @brief 验证远端纯物件替换不会覆盖正在绘制的本地折线草稿。
/// @return 远端物件即时出现且松键后本地折线与远端物件同时存在时返回 true。
[[nodiscard]] bool testRemoteSynchronizationPreservesActiveBrush()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    auto                       initial = makeBeatmap();
    auto& initialNote       = initial->m_noteData.notes.emplace_back();
    initialNote.m_timestamp = 500.0;
    initialNote.m_track     = 2;
    initial->sync();
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = std::move(initial) },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateViewport{
            .cameraId = "Basic2DCanvas",
            .width    = 400.0F,
            .height   = 600.0F,
        },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdChangeTool{ .tool = MMM::Logic::EditTool::Draw },
    });
    session.update(0.0, config, false);

    auto observer = std::make_shared<CountingMutationObserver>();
    session.setMutationObserver(observer, false);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdStartBrush{
            .cameraId    = "Basic2DCanvas",
            .mouseX      = 150.0F,
            .mouseY      = 300.0F,
            .isShiftDown = true,
            .isCtrlDown  = true,
        },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateBrush{
            .cameraId    = "Basic2DCanvas",
            .mouseX      = 250.0F,
            .mouseY      = 150.0F,
            .isShiftDown = true,
            .isCtrlDown  = true,
        },
    });
    session.update(0.1, config, false);

    auto  remote           = makeBeatmap();
    auto& remoteNote       = remote->m_noteData.notes.emplace_back();
    remoteNote.m_timestamp = 1000.0;
    remoteNote.m_track     = 3;
    remote->sync();
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdReplaceBeatmapData{
            .sourceBeatmap          = std::move(remote),
            .replaceObjects         = true,
            .notifyMutationObserver = false,
            .authoritativeRemote    = true,
        },
    });
    session.update(0.2, config, false);
    if ( observer->synchronizationCount() != 1 ||
         !session.getContext().brushState.isActive ||
         session.getContext().brushState.polylineSegments.empty() ||
         !hasRootNote(session, 1.0, 3) || hasRootNote(session, 0.5, 2) ) {
        XERROR(
            "Remote synchronization did not preserve the active Polyline "
            "draft: syncs={}, active={}, segments={}, added={}, deleted={}",
            observer->synchronizationCount(),
            session.getContext().brushState.isActive,
            session.getContext().brushState.polylineSegments.size(),
            hasRootNote(session, 1.0, 3),
            !hasRootNote(session, 0.5, 2));
        return false;
    }

    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" },
    });
    session.update(0.3, config, false);
    if ( observer->notificationCount() != 1 ||
         observer->lastFlags() != MMM::BeatmapMutationFlags::Objects ||
         observer->synchronizationCount() != 1 ||
         !hasRootNote(session, 1.0, 3) || hasRootNote(session, 0.5, 2) ||
         !hasCommittedPolyline(session) ) {
        XERROR(
            "Active Polyline did not commit on top of the remote baseline: "
            "mutations={}, syncs={}, added={}, deleted={}, polyline={}",
            observer->notificationCount(),
            observer->synchronizationCount(),
            hasRootNote(session, 1.0, 3),
            !hasRootNote(session, 0.5, 2),
            hasCommittedPolyline(session));
        return false;
    }
    return true;
}

/// @brief 验证房间掉线后编辑命令被统一拦截且提示在一个离线周期只发布一次。
/// @return 只读状态保持谱面不变，解除只读后编辑恢复时返回 true。
[[nodiscard]] bool testOfflineCollaborationSessionIsReadOnly()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = makeBeatmap() },
    });
    session.update(0.0, config, false);

    std::atomic_int blockedEvents{ 0 };
    auto&           eventBus = MMM::Event::EventBus::instance();
    const auto      subscription =
        eventBus.subscribe<MMM::Event::CollaborationOfflineEditBlockedEvent>(
            [&blockedEvents](
                const MMM::Event::CollaborationOfflineEditBlockedEvent&) {
                blockedEvents.fetch_add(1, std::memory_order_relaxed);
            });

    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateTrackCount{ .trackCount = 6 },
    });
    session.setCollaborationOfflineReadOnly(true);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateTrackCount{ .trackCount = 7 },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdCreateTimelineEvent{
            .time  = 1.0,
            .type  = MMM::TimingEffect::SCROLL,
            .value = 2.0,
        },
    });
    session.update(0.0, config, false);
    const bool stayedReadOnly =
        session.getContext().trackCount == 4 &&
        blockedEvents.load(std::memory_order_relaxed) == 1;

    session.setCollaborationOfflineReadOnly(false);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateTrackCount{ .trackCount = 7 },
    });
    session.update(0.0, config, false);
    const bool resumedEditing = session.getContext().trackCount == 7;

    eventBus.unsubscribe<MMM::Event::CollaborationOfflineEditBlockedEvent>(
        subscription);
    if ( !stayedReadOnly || !resumedEditing ) {
        XERROR(
            "Offline collaboration session edit gate failed: readOnly={}, "
            "resumed={}, events={}",
            stayedReadOnly,
            resumedEditing,
            blockedEvents.load(std::memory_order_relaxed));
        return false;
    }
    return true;
}

/// @brief 验证协作细分权限在命令入队和逻辑消费边界拦截未授权类别。
/// @return Timing 可编辑而元数据被拒绝，切换权限后结果反转时返回 true。
[[nodiscard]] bool testCollaborationMutationPermissionsAreLocalGate()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = makeBeatmap() },
    });
    session.update(0.0, config, false);
    const auto initialTimelineCount =
        session.getContext()
            .timelineRegistry.view<const MMM::Logic::TimelineComponent>()
            .size();

    std::atomic_int blockedEvents{ 0 };
    auto&           eventBus = MMM::Event::EventBus::instance();
    const auto      subscription =
        eventBus.subscribe<MMM::Event::CollaborationPermissionEditBlockedEvent>(
            [&blockedEvents](
                const MMM::Event::CollaborationPermissionEditBlockedEvent&) {
                blockedEvents.fetch_add(1, std::memory_order_relaxed);
            });

    session.setCollaborationAllowedMutationFlags(
        MMM::BeatmapMutationFlags::Timelines);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateTrackCount{ .trackCount = 7 },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdCreateTimelineEvent{
            .time  = 1.0,
            .type  = MMM::TimingEffect::SCROLL,
            .value = 2.0,
        },
    });
    session.update(0.0, config, false);
    const bool timingOnlyApplied =
        session.getContext().trackCount == 4 &&
        session.getContext()
                .timelineRegistry.view<const MMM::Logic::TimelineComponent>()
                .size() == initialTimelineCount + 1U &&
        blockedEvents.load(std::memory_order_relaxed) == 1;

    session.setCollaborationAllowedMutationFlags(
        MMM::BeatmapMutationFlags::Metadata |
        MMM::BeatmapMutationFlags::AudioSamples);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdCreateTimelineEvent{
            .time  = 2.0,
            .type  = MMM::TimingEffect::SCROLL,
            .value = 3.0,
        },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateTrackCount{ .trackCount = 7 },
    });
    session.update(0.0, config, false);
    const bool metadataOnlyApplied =
        session.getContext().trackCount == 7 &&
        session.getContext()
                .timelineRegistry.view<const MMM::Logic::TimelineComponent>()
                .size() == initialTimelineCount + 1U &&
        blockedEvents.load(std::memory_order_relaxed) == 2;

    session.setCollaborationAllowedMutationFlags(
        MMM::BeatmapMutationFlags::All);
    eventBus.unsubscribe<MMM::Event::CollaborationPermissionEditBlockedEvent>(
        subscription);
    if ( !timingOnlyApplied || !metadataOnlyApplied ) {
        XERROR(
            "Collaboration mutation permission gate failed: timingOnly={}, "
            "metadataOnly={}, tracks={}, timelines={}, events={}",
            timingOnlyApplied,
            metadataOnlyApplied,
            session.getContext().trackCount,
            session.getContext()
                .timelineRegistry.view<const MMM::Logic::TimelineComponent>()
                .size(),
            blockedEvents.load(std::memory_order_relaxed));
        return false;
    }
    return true;
}

/// @brief 验证访客连接门闩会清空旧打开请求并拦截所有后续本机项目请求。
/// @return 门闩解除前没有项目动作进入逻辑线程时返回 true。
[[nodiscard]] bool testGuestConnectionBlocksLocalProjectOpening()
{
    auto& controller = MMM::Logic::ProjectController::instance();
    controller.setLocalProjectOpeningBlockedByCollaboration(false);
    controller.cancelPendingProjectSwitch();
    controller.requestOpenProject("/tmp/mmm-collaboration-pending-project");
    if ( !controller.hasPendingProjectAction() ) {
        XERROR("Local project request was not queued before collaboration");
        return false;
    }

    std::atomic_int blockedEvents{ 0 };
    auto&           eventBus = MMM::Event::EventBus::instance();
    const auto      subscription =
        eventBus.subscribe<MMM::Event::CollaborationProjectOpenBlockedEvent>(
            [&blockedEvents](
                const MMM::Event::CollaborationProjectOpenBlockedEvent&) {
                blockedEvents.fetch_add(1, std::memory_order_relaxed);
            });

    controller.setLocalProjectOpeningBlockedByCollaboration(true);
    controller.requestOpenProject("/tmp/mmm-collaboration-blocked-project");
    const bool blocked =
        controller.isLocalProjectOpeningBlockedByCollaboration() &&
        !controller.hasPendingProjectAction() &&
        blockedEvents.load(std::memory_order_relaxed) == 1;

    controller.setLocalProjectOpeningBlockedByCollaboration(false);
    controller.cancelPendingProjectSwitch();
    eventBus.unsubscribe<MMM::Event::CollaborationProjectOpenBlockedEvent>(
        subscription);
    if ( !blocked ) {
        XERROR("Guest collaboration did not isolate local project requests");
        return false;
    }
    return true;
}
}  // namespace

/// @brief 运行协作谱面观察者绑定回归测试。
/// @return 全部断言通过时返回 0。
int main()
{
    return testOptionalInitialSnapshot() &&
                   testTimelineCommandsPublishMutations() &&
                   testSubNoteAnnotationPermissionAndMutationFlag() &&
                   testRemoteSynchronizationPreservesActiveBrush() &&
                   testOfflineCollaborationSessionIsReadOnly() &&
                   testCollaborationMutationPermissionsAreLocalGate() &&
                   testGuestConnectionBlocksLocalProjectOpening()
               ? 0
               : 1;
}
