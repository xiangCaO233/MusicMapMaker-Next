#include "common/LogicCommands.h"
#include "config/EditorConfig.h"
#include "logic/BeatmapSession.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/beatmap/BeatmapMutationObserver.h"

#include "log/colorful-log.h"

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

/// @brief 验证远端权威替换会等待本地画笔手势松开后再执行。
/// @return 拖绘过程中没有同步且结束后按顺序发布本地操作并同步时返回 true。
[[nodiscard]] bool testRemoteSynchronizationWaitsForBrushEnd()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = makeBeatmap() },
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
    if ( observer->synchronizationCount() != 0 ) {
        XERROR("Remote synchronization interrupted an active brush gesture");
        return false;
    }

    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" },
    });
    session.update(0.3, config, false);
    session.update(0.4, config, false);
    if ( observer->notificationCount() != 1 ||
         observer->lastFlags() != MMM::BeatmapMutationFlags::Objects ||
         observer->synchronizationCount() != 1 ) {
        XERROR(
            "Brush operation was not published before deferred remote sync: "
            "mutations={}, syncs={}",
            observer->notificationCount(),
            observer->synchronizationCount());
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
                   testRemoteSynchronizationWaitsForBrushEnd()
               ? 0
               : 1;
}
