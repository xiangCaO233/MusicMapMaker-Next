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

    /// @brief 返回已经收到的通知次数。
    [[nodiscard]] int notificationCount() const { return m_notificationCount; }

    /// @brief 返回最后一次通知的变化类别。
    [[nodiscard]] MMM::BeatmapMutationFlags lastFlags() const
    {
        return m_lastFlags;
    }

private:
    /// @brief 已经收到的通知次数。
    int m_notificationCount{ 0 };
    /// @brief 最后一次通知的变化类别。
    MMM::BeatmapMutationFlags m_lastFlags{ MMM::BeatmapMutationFlags::None };
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
}  // namespace

/// @brief 运行协作谱面观察者绑定回归测试。
/// @return 全部断言通过时返回 0。
int main()
{
    return testOptionalInitialSnapshot() ? 0 : 1;
}
