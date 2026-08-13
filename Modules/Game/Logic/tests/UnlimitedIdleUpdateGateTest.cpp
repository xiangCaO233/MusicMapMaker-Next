#include "logic/UnlimitedIdleUpdateGate.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/session/context/SessionContext.h"

#include <chrono>
#include <cmath>

namespace
{

/// @brief 验证播放时钟不会被视觉维护门控限制更新频率。
bool testPlaybackWorkRemainsUnlimited()
{
    MMM::Logic::UnlimitedIdleUpdateGate gate;
    const auto now = MMM::Logic::UnlimitedIdleUpdateGate::Clock::time_point{};

    if ( !gate.shouldPoll(now, false) ) {
        XERROR("Initial playback poll was unexpectedly throttled");
        return false;
    }

    gate.completePoll(now, true);
    if ( !gate.shouldPoll(now, false) ||
         !gate.shouldPoll(now + std::chrono::nanoseconds(1), false) ) {
        XERROR("Playback work was throttled in Unlimited mode");
        return false;
    }
    return true;
}

/// @brief 验证视觉动画活跃但没有新命令时不会持续争抢 Session 锁。
bool testVisualMaintenanceUsesDeadlinePolling()
{
    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();
    context.animateTimeAnimationActive = true;
    context.isDragging                 = true;
    if ( !session.needsRealtimeUpdate() || session.needsUnlimitedPolling() ) {
        XERROR("Visual interaction was classified as unlimited playback work");
        return false;
    }

    MMM::Logic::UnlimitedIdleUpdateGate gate;
    const auto now = MMM::Logic::UnlimitedIdleUpdateGate::Clock::time_point{};

    // 视觉动画不属于必须逐逻辑轮次推进的播放工作。
    gate.completePoll(now, false);
    if ( gate.shouldPoll(now + std::chrono::nanoseconds(1), false) ) {
        XERROR("Visual maintenance continuously entered the session lock");
        return false;
    }
    return true;
}

/// @brief 验证本地播放和同音轨跟随仍逐逻辑轮次推进。
bool testPlaybackStateUsesUnlimitedPolling()
{
    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();

    context.isPlaying = true;
    if ( !session.needsUnlimitedPolling() ) {
        XERROR("Active playback was not classified as unlimited work");
        return false;
    }

    context.isPlaying                   = false;
    context.isAudioTimelineSyncFollower = true;
    if ( !session.needsUnlimitedPolling() ) {
        XERROR("Playback follower was not classified as unlimited work");
        return false;
    }
    return true;
}

/// @brief 验证完全空闲时跳过轮询，但期限到达后仍执行维护更新。
bool testIdleWorkWaitsOutsideSessionLock()
{
    MMM::Logic::UnlimitedIdleUpdateGate gate;
    const auto now = MMM::Logic::UnlimitedIdleUpdateGate::Clock::time_point{};
    gate.completePoll(now, false);

    if ( gate.shouldPoll(now +
                             MMM::Logic::UNLIMITED_IDLE_SESSION_POLL_INTERVAL -
                             std::chrono::nanoseconds(1),
                         false) ) {
        XERROR("Idle session entered the lock path before its poll deadline");
        return false;
    }
    if ( !gate.shouldPoll(
             now + MMM::Logic::UNLIMITED_IDLE_SESSION_POLL_INTERVAL, false) ) {
        XERROR("Idle session maintenance was skipped at its poll deadline");
        return false;
    }
    return true;
}

/// @brief 验证空闲期间到达的新命令会在下一逻辑轮次立即绕过门控。
bool testPendingCommandWakesIdlePollImmediately()
{
    MMM::Logic::UnlimitedIdleUpdateGate gate;
    const auto now = MMM::Logic::UnlimitedIdleUpdateGate::Clock::time_point{};
    gate.completePoll(now, false);

    if ( !gate.shouldPoll(now + std::chrono::nanoseconds(1), true) ) {
        XERROR("Pending session command did not wake the idle poll");
        return false;
    }
    return true;
}

/// @brief 验证门控跳过的外层轮次不会丢失视觉动画时间步。
bool testSkippedPollsAccumulateSessionDeltaTime()
{
    MMM::Logic::UnlimitedIdleUpdateGate gate;
    gate.accumulateElapsedSeconds(0.0001);
    gate.accumulateElapsedSeconds(0.0004);

    const double elapsedSeconds = gate.consumeElapsedSeconds();
    if ( std::abs(elapsedSeconds - 0.0005) > 1e-12 ||
         gate.consumeElapsedSeconds() != 0.0 ) {
        XERROR("Skipped poll delta time was not preserved exactly once");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行 Unlimited 空闲 Session 锁门控回归测试。
int main()
{
    return testPlaybackWorkRemainsUnlimited() &&
                   testVisualMaintenanceUsesDeadlinePolling() &&
                   testPlaybackStateUsesUnlimitedPolling() &&
                   testIdleWorkWaitsOutsideSessionLock() &&
                   testPendingCommandWakesIdlePollImmediately() &&
                   testSkippedPollsAccumulateSessionDeltaTime()
               ? 0
               : 1;
}
