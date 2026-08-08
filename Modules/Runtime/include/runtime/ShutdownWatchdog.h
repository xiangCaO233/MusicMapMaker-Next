#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace MMM::Runtime
{

/// @brief 独立于共享线程池的应用退出看门狗。
///
/// 看门狗不能投递到被监控的线程池，否则线程池耗尽或关闭死锁时无法触发兜底。
class ShutdownWatchdog final
{
public:
    /// @brief 超时后执行的终止回调。
    using TimeoutHandler = std::function<void()>;

    /// @brief 创建未启动的退出看门狗。
    /// @param timeoutHandler 超时后执行的回调。
    explicit ShutdownWatchdog(TimeoutHandler timeoutHandler);

    /// @brief 停止看门狗线程。
    ~ShutdownWatchdog();

    ShutdownWatchdog(const ShutdownWatchdog&)            = delete;
    ShutdownWatchdog(ShutdownWatchdog&&)                 = delete;
    ShutdownWatchdog& operator=(const ShutdownWatchdog&) = delete;
    ShutdownWatchdog& operator=(ShutdownWatchdog&&)      = delete;

    /// @brief 启动或重新启动退出倒计时。
    /// @param timeout 正常清理允许占用的最长时间。
    /// @warning 退出低频路径：会创建一个独立于共享线程池的监控线程。
    void arm(std::chrono::milliseconds timeout);

    /// @brief 标记正常退出完成并等待看门狗线程结束。
    /// @warning 退出低频路径：只等待已收到停止信号的看门狗线程。
    void complete();

private:
    /// @brief 看门狗等待循环。
    /// @param stopToken 外部停止令牌。
    /// @param timeout 本次等待时限。
    void waitForTimeout(std::stop_token           stopToken,
                        std::chrono::milliseconds timeout);

    /// @brief 超时后执行的终止回调。
    TimeoutHandler m_timeoutHandler;

    /// @brief 保护正常完成标志。
    std::mutex m_mutex;

    /// @brief 响应正常完成和停止请求的等待条件。
    std::condition_variable_any m_condition;

    /// @brief 当前倒计时是否已由正常退出完成。
    bool m_completed{ true };

    /// @brief 独立监控线程；不得迁移到被监控的共享线程池。
    std::jthread m_thread;
};

}  // namespace MMM::Runtime
