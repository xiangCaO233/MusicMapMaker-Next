#include "logic/BeatmapSession.h"

#include "event/audio/AudioPlaybackEvent.h"
#include "event/core/EventBus.h"
#include "log/colorful-log.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/context/SessionContext.h"

#include <cmath>

namespace
{

/// @brief 使用小容差比较播放时间。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个时间值足够接近时返回 true。
bool near(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1e-6;
}

/// @brief 验证主音轨自然结束会停止会话并标记下一次播放从头开始。
/// @return 行为符合预期时返回 true。
bool testNaturalFinishRestartsFromBeginning()
{
    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    context.currentTime                = 37.5;
    context.isPlaying                  = true;

    MMM::Event::AudioFinishedEvent event;
    event.isLooping = false;
    MMM::Event::EventBus::instance().publish(event);

    if ( context.isPlaying || !context.restartPlaybackAfterFinishPending.load(
                                  std::memory_order_acquire) ) {
        XERROR("Natural audio finish did not arm playback restart");
        return false;
    }

    MMM::Logic::PlaybackController controller(context);
    controller.handleCommand(MMM::Logic::CmdSetPlayState{ true });

    if ( !context.isPlaying || !near(context.currentTime, 0.0) ||
         context.restartPlaybackAfterFinishPending.load(
             std::memory_order_acquire) ) {
        XERROR(
            "Playback did not restart from the beginning after natural finish");
        return false;
    }
    return true;
}

/// @brief 验证普通暂停恢复不会改变当前播放位置。
/// @return 行为符合预期时返回 true。
bool testNormalResumeKeepsCurrentTime()
{
    MMM::Logic::SessionContext     context;
    MMM::Logic::PlaybackController controller(context);
    context.currentTime = 8.25;

    controller.handleCommand(MMM::Logic::CmdSetPlayState{ true });

    if ( !context.isPlaying || !near(context.currentTime, 8.25) ) {
        XERROR("Normal playback resume unexpectedly rewound the timeline");
        return false;
    }
    return true;
}

/// @brief 验证手动跳转会取消自然结束后的自动回到开头。
/// @return 行为符合预期时返回 true。
bool testSeekCancelsPendingRestart()
{
    MMM::Logic::SessionContext     context;
    MMM::Logic::PlaybackController controller(context);
    context.restartPlaybackAfterFinishPending.store(true,
                                                    std::memory_order_relaxed);

    controller.handleCommand(MMM::Logic::CmdSeek{ 0.0 });

    if ( context.restartPlaybackAfterFinishPending.load(
             std::memory_order_acquire) ) {
        XERROR("Manual seek did not cancel pending playback restart");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行播放自然结束重播策略测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testNaturalFinishRestartsFromBeginning() &&
                   testNormalResumeKeepsCurrentTime() &&
                   testSeekCancelsPendingRestart()
               ? 0
               : 1;
}
