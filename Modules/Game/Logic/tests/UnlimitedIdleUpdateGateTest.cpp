#include "logic/UnlimitedIdleUpdateGate.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/session/context/SessionContext.h"

#include <chrono>
#include <cmath>

namespace
{

/// @brief 验证播放时钟不会被视觉维护门控限制更新频率。
/// @return 首次及持续播放轮询均未被节流时返回 true。
/// @note 直接传入可控时刻，不依赖真实线程调度或操作系统定时精度。
bool testPlaybackWorkRemainsUnlimited()
{
    MMM::Logic::UnlimitedIdleUpdateGate gate;
    // 统一从零时刻起算，用相对纳秒推进表达逻辑轮次，无需实际等待。
    const auto now = MMM::Logic::UnlimitedIdleUpdateGate::Clock::time_point{};

    // false 表示没有待处理命令；首次通过应来自门控初态而非命令唤醒。
    if ( !gate.shouldPoll(now, false) ) {
        XERROR("Initial playback poll was unexpectedly throttled");
        return false;
    }

    // 上一次轮询确认存在播放工作后，即使时钟尚未推进也必须允许再次查询。
    gate.completePoll(now, true);
    // 播放结果才是持续轮询依据，不需要每轮都有新输入命令。
    // 同时覆盖同刻和下一纳秒，避免只在某个时间比较边界偶然通过。
    if ( !gate.shouldPoll(now, false) ||
         !gate.shouldPoll(now + std::chrono::nanoseconds(1), false) ) {
        XERROR("Playback work was throttled in Unlimited mode");
        return false;
    }
    return true;
}

/// @brief 验证视觉动画活跃但没有新命令时不会持续争抢 Session 锁。
/// @return 会话分类与门控均允许视觉维护采用期限轮询时返回 true。
/// @note 实际锁争用和帧率不在此测量，只验证进入维护路径的策略。
bool testVisualMaintenanceUsesDeadlinePolling()
{
    MMM::Logic::BeatmapSession session;
    // 直接设置最小上下文，排除输入事件和音频线程对分类结果的干扰。
    auto& context                      = session.getContextMutable();
    context.animateTimeAnimationActive = true;
    // 动画与拖动同时存在仍不等价于播放，避免视觉忙碌永久占用无限轮询。
    context.isDragging = true;
    // 需要实时视觉更新不等于需要 Unlimited 轮询，两种需求不能合并成一个标志。
    if ( !session.needsRealtimeUpdate() || session.needsUnlimitedPolling() ) {
        XERROR("Visual interaction was classified as unlimited playback work");
        return false;
    }

    MMM::Logic::UnlimitedIdleUpdateGate gate;
    const auto now = MMM::Logic::UnlimitedIdleUpdateGate::Clock::time_point{};

    // 视觉动画不属于必须逐逻辑轮次推进的播放工作。
    // completePoll 的 false 来自上述会话分类，而非关闭所有视觉更新。
    gate.completePoll(now, false);
    // 刚结束维护的一纳秒后仍在期限之前，应允许外层循环处理其它工作。
    if ( gate.shouldPoll(now + std::chrono::nanoseconds(1), false) ) {
        XERROR("Visual maintenance continuously entered the session lock");
        return false;
    }
    return true;
}

/// @brief 验证本地播放和同音轨跟随仍逐逻辑轮次推进。
/// @return 两种播放来源都被识别为 Unlimited 工作时返回 true。
/// @note 本例只验证状态分类，不启动音频设备或实际播放时钟。
bool testPlaybackStateUsesUnlimitedPolling()
{
    MMM::Logic::BeatmapSession session;
    auto&                      context = session.getContextMutable();

    // 活动播放直接提供持续推进需求，不依赖拖动或动画标志。
    context.isPlaying = true;
    if ( !session.needsUnlimitedPolling() ) {
        XERROR("Active playback was not classified as unlimited work");
        return false;
    }

    // 清除本地播放后单独启用跟随，防止跟随分支被已有播放状态掩盖。
    context.isPlaying                   = false;
    context.isAudioTimelineSyncFollower = true;
    // 跟随者即使不持有本地播放权，也需要连续推进自身的可视时间。
    if ( !session.needsUnlimitedPolling() ) {
        XERROR("Playback follower was not classified as unlimited work");
        return false;
    }
    return true;
}

/// @brief 验证完全空闲时跳过轮询，但期限到达后仍执行维护更新。
/// @return 期限前拒绝、期限上允许轮询时返回 true。
/// @note 使用生产间隔常量，测试不复制固定毫秒值或引入睡眠。
bool testIdleWorkWaitsOutsideSessionLock()
{
    // 门控只决定是否进入 Session 路径，不在内部阻塞等待期限到达。
    MMM::Logic::UnlimitedIdleUpdateGate gate;
    const auto now = MMM::Logic::UnlimitedIdleUpdateGate::Clock::time_point{};
    gate.completePoll(now, false);

    // 取期限前一纳秒检查严格边界，普通远离期限的采样无法区分比较符号错误。
    if ( gate.shouldPoll(now +
                             MMM::Logic::UNLIMITED_IDLE_SESSION_POLL_INTERVAL -
                             std::chrono::nanoseconds(1),
                         false) ) {
        XERROR("Idle session entered the lock path before its poll deadline");
        return false;
    }
    // 恰好到期即允许维护，不应额外再等一个外层轮次或一个完整间隔。
    if ( !gate.shouldPoll(
             now + MMM::Logic::UNLIMITED_IDLE_SESSION_POLL_INTERVAL, false) ) {
        XERROR("Idle session maintenance was skipped at its poll deadline");
        return false;
    }
    return true;
}

/// @brief 验证空闲期间到达的新命令会在下一逻辑轮次立即绕过门控。
/// @return 待处理命令在维护期限之前获得轮询机会时返回 true。
/// @note 此处验证查询策略，不模拟命令队列的入队通知和线程唤醒机制。
bool testPendingCommandWakesIdlePollImmediately()
{
    MMM::Logic::UnlimitedIdleUpdateGate gate;
    const auto now = MMM::Logic::UnlimitedIdleUpdateGate::Clock::time_point{};
    gate.completePoll(now, false);

    // 与纯空闲用例相同的短时间间隔，仅把待处理命令标志切为 true。
    // 命令必须覆盖旧的空闲分类，否则快速交互会被维护节流延迟。
    // 这里只检查一次唤醒，不假定命令已消费后仍会永久保持高频轮询。
    if ( !gate.shouldPoll(now + std::chrono::nanoseconds(1), true) ) {
        XERROR("Pending session command did not wake the idle poll");
        return false;
    }
    return true;
}

/// @brief 验证门控跳过的外层轮次不会丢失视觉动画时间步。
/// @return 多轮时间累计一次性消费，随后清零时返回 true。
/// @note 累积的是秒数增量，不是绝对时间戳或轮询次数。
bool testSkippedPollsAccumulateSessionDeltaTime()
{
    MMM::Logic::UnlimitedIdleUpdateGate gate;
    // 用不相等的两个时间步避免错误地重复使用最后一个增量也得到期望值。
    gate.accumulateElapsedSeconds(0.0001);
    // 中间没有调用 consume，模拟一次被门控跳过的维护机会。
    gate.accumulateElapsedSeconds(0.0004);

    // 下一次实际更新应一次取得跳过期间全部时间，保持动画推进速度不变。
    const double elapsedSeconds = gate.consumeElapsedSeconds();
    // 第二次消费必须为零，防止同一段经过时间被重复应用到视觉状态。
    // 首次和使用容差比较，允许二进制浮点加法的末位误差。
    if ( std::abs(elapsedSeconds - 0.0005) > 1e-12 ||
         gate.consumeElapsedSeconds() != 0.0 ) {
        XERROR("Skipped poll delta time was not preserved exactly once");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行 Unlimited 空闲 Session 锁门控回归测试。
/// @return 全部策略与时间累计用例通过时返回 0，否则返回 1。
/// @note 不以本测试替代真实多线程压力验证，也不证明锁持有时长上限。
/// @note 测试时刻完全由输入指定，运行耗时不影响期限断言的结果。
int main()
{
    // 每个用例创建独立门控，避免前一用例的期限或累计时间污染后续断言。
    // 基础播放分类先验证，再覆盖空闲期限、命令抢占及跳帧时间保存。
    // 用例通过日志报告具体失败，主入口仅汇总为测试运行器退出码。
    return testPlaybackWorkRemainsUnlimited() &&
                   testVisualMaintenanceUsesDeadlinePolling() &&
                   testPlaybackStateUsesUnlimitedPolling() &&
                   testIdleWorkWaitsOutsideSessionLock() &&
                   testPendingCommandWakesIdlePollImmediately() &&
                   testSkippedPollsAccumulateSessionDeltaTime()
               ? 0
               : 1;
}
