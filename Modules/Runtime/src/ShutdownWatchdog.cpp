#include "runtime/ShutdownWatchdog.h"

#include <utility>

namespace MMM::Runtime
{

/// @brief 保存超时处理器，但暂不启动监控线程。
/// @param timeoutHandler 倒计时自然到期后调用的处理器。
/// @warning 处理器由看门狗线程调用，必须自行满足线程安全要求。
ShutdownWatchdog::ShutdownWatchdog(TimeoutHandler timeoutHandler)
    : m_timeoutHandler(std::move(timeoutHandler))
{
}

/// @brief 停止仍在运行的倒计时，并等待监控线程退出。
/// @warning 析构位于低频退出路径，可能短暂等待已收到停止请求的线程。
ShutdownWatchdog::~ShutdownWatchdog()
{
    complete();
}

/// @brief 取消旧倒计时并启动一轮新的退出监控。
/// @param timeout 正常退出流程可占用的最长时间。
/// @warning 每次调用都会先回收旧监控线程，再创建独立的 jthread。
void ShutdownWatchdog::arm(std::chrono::milliseconds timeout)
{
    // 先完成旧一轮，保证 m_thread 不会被仍可运行的 jthread 覆盖。
    complete();
    {
        // 完成标志与条件变量谓词共用同一把锁，避免丢失完成通知。
        std::lock_guard lock(m_mutex);
        m_completed = false;
    }
    // 看门狗不得使用其监控的应用线程池，否则线程池卡死时无法兜底。
    m_thread = std::jthread([this, timeout](std::stop_token stopToken) {
        waitForTimeout(stopToken, timeout);
    });
}

/// @brief 宣告正常退出完成，并同步回收当前监控线程。
/// @warning join 只发生在退出或重新布防路径，不得从监控线程自身调用。
void ShutdownWatchdog::complete()
{
    {
        // 即使当前没有线程也写入完成态，使下一次等待前的状态确定。
        std::lock_guard lock(m_mutex);
        m_completed = true;
    }
    // 第一次通知让条件等待立即观察正常完成，而不必耗尽超时时间。
    m_condition.notify_all();
    if ( m_thread.joinable() ) {
        // 停止令牌覆盖非正常完成路径，第二次通知唤醒 stop-aware wait。
        m_thread.request_stop();
        m_condition.notify_all();
        // 回收后才能安全复用对象中的互斥量、条件变量和处理器。
        m_thread.join();
    }
}

/// @brief 等待正常完成、停止请求或退出时限三者之一发生。
/// @param stopToken jthread 在回收或重新布防时发出的停止令牌。
/// @param timeout 本轮正常清理允许占用的最长时间。
/// @warning 超时处理器在解锁后执行，避免处理器终止或回调时持有内部锁。
void ShutdownWatchdog::waitForTimeout(std::stop_token           stopToken,
                                      std::chrono::milliseconds timeout)
{
    // wait_for 原子地释放并重新获取锁，通知不会与谓词检查形成竞态窗口。
    std::unique_lock lock(m_mutex);
    const bool       completed = m_condition.wait_for(
        lock, stopToken, timeout, [this]() { return m_completed; });
    // 正常完成和显式停止都不属于超时，二者均禁止触发终止处理器。
    if ( completed || stopToken.stop_requested() ) return;

    // 处理器可能直接结束进程，也可能在测试中访问其他同步原语。
    lock.unlock();
    if ( m_timeoutHandler ) m_timeoutHandler();
}

}  // namespace MMM::Runtime
