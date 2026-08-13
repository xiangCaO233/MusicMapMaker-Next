#pragma once

#include <chrono>

namespace MMM::Logic
{

/// @brief 无限制模式下空闲 Session 的低频维护轮询间隔。
inline constexpr auto UNLIMITED_IDLE_SESSION_POLL_INTERVAL =
    std::chrono::microseconds(500);

/// @brief 在不限制实时循环频率的前提下跳过空闲 Session 的重复加锁轮次。
class UnlimitedIdleUpdateGate
{
public:
    /// @brief 门控使用的单调时钟类型。
    using Clock = std::chrono::steady_clock;

    /// @brief 判断本轮是否需要进入持锁的 Session 更新路径。
    /// @param now 当前逻辑循环时刻。
    /// @param hasPendingCommands 是否存在尚未消费的 Session 命令。
    /// @return 无限播放推进、待处理命令或维护期限任一满足时返回 true。
    /// @warning 逻辑热路径：每个 Unlimited update 调用；只做常量级状态读取
    /// 和时间比较，禁止加入锁、等待、分配或文件系统访问。
    [[nodiscard]] bool shouldPoll(Clock::time_point now,
                                  bool hasPendingCommands) const noexcept
    {
        return m_pollContinuously || hasPendingCommands ||
               now >= m_nextIdlePoll;
    }

    /// @brief 提交一次持锁轮询后的无限播放推进状态并安排下一次维护。
    /// @param now 本次轮询完成时刻。
    /// @param hasUnlimitedWork Session 是否仍有播放时钟需要逐轮次推进。
    /// @warning 逻辑热路径：只更新逻辑线程私有状态，不执行等待或系统调用。
    void completePoll(Clock::time_point now, bool hasUnlimitedWork) noexcept
    {
        m_pollContinuously = hasUnlimitedWork;
        m_nextIdlePoll =
            hasUnlimitedWork ? now : now + UNLIMITED_IDLE_SESSION_POLL_INTERVAL;
    }

    /// @brief 累加尚未传递给 Session 的真实逻辑时间。
    /// @param elapsedSeconds 本轮外层逻辑循环经过的秒数。
    /// @warning 逻辑热路径：每个 update 调用；只执行常量级浮点加法。
    void accumulateElapsedSeconds(double elapsedSeconds) noexcept
    {
        if ( elapsedSeconds > 0.0 ) {
            m_pendingElapsedSeconds += elapsedSeconds;
        }
    }

    /// @brief 取出并清零门控期间累计的 Session 时间步。
    /// @return 自上一次实际 Session 更新后经过的秒数。
    /// @warning 逻辑热路径：仅在进入持锁 Session 更新路径时调用。
    [[nodiscard]] double consumeElapsedSeconds() noexcept
    {
        const double elapsedSeconds = m_pendingElapsedSeconds;
        m_pendingElapsedSeconds     = 0.0;
        return elapsedSeconds;
    }

    /// @brief 丢弃没有 Session 时累计的时间，避免新会话继承旧时间步。
    /// @warning 无会话低频等待路径：只清零逻辑线程私有状态。
    void discardElapsedSeconds() noexcept { m_pendingElapsedSeconds = 0.0; }

    /// @brief 强制下一轮重新检查 Session，用于会话列表切换或重新初始化。
    /// @warning 低频状态切换路径：只修改逻辑线程私有状态。
    void requestPoll() noexcept
    {
        m_pollContinuously = true;
        m_nextIdlePoll     = Clock::time_point::min();
    }

private:
    /// @brief 上一次轮询后是否仍有播放时钟需要逐 update 推进。
    bool m_pollContinuously{ true };

    /// @brief 完全空闲时允许再次进入 Session 锁区的最早时刻。
    Clock::time_point m_nextIdlePoll{ Clock::time_point::min() };

    /// @brief 门控跳过期间尚未传递给 Session 的累计时间，单位秒。
    double m_pendingElapsedSeconds{ 0.0 };
};

}  // namespace MMM::Logic
