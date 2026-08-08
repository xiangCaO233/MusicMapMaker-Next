#include "runtime/ShutdownWatchdog.h"

#include <utility>

namespace MMM::Runtime
{

ShutdownWatchdog::ShutdownWatchdog(TimeoutHandler timeoutHandler)
    : m_timeoutHandler(std::move(timeoutHandler))
{
}

ShutdownWatchdog::~ShutdownWatchdog()
{
    complete();
}

void ShutdownWatchdog::arm(std::chrono::milliseconds timeout)
{
    complete();
    {
        std::lock_guard lock(m_mutex);
        m_completed = false;
    }
    m_thread = std::jthread([this, timeout](std::stop_token stopToken) {
        waitForTimeout(stopToken, timeout);
    });
}

void ShutdownWatchdog::complete()
{
    {
        std::lock_guard lock(m_mutex);
        m_completed = true;
    }
    m_condition.notify_all();
    if ( m_thread.joinable() ) {
        m_thread.request_stop();
        m_condition.notify_all();
        m_thread.join();
    }
}

void ShutdownWatchdog::waitForTimeout(std::stop_token           stopToken,
                                      std::chrono::milliseconds timeout)
{
    std::unique_lock lock(m_mutex);
    const bool       completed = m_condition.wait_for(
        lock, stopToken, timeout, [this]() { return m_completed; });
    if ( completed || stopToken.stop_requested() ) return;

    lock.unlock();
    if ( m_timeoutHandler ) m_timeoutHandler();
}

}  // namespace MMM::Runtime
