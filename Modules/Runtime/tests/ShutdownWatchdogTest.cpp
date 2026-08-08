#include "runtime/ShutdownWatchdog.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
using namespace std::chrono_literals;

/// @brief 在给定时限内轮询原子完成标志。
/// @param value 由看门狗回调写入的标志。
/// @param timeout 最长等待时间。
/// @return 时限内观察到 true 时返回 true。
bool waitUntilTrue(const std::atomic<bool>&  value,
                   std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while ( std::chrono::steady_clock::now() < deadline ) {
        if ( value.load(std::memory_order_acquire) ) return true;
        std::this_thread::sleep_for(2ms);
    }
    return value.load(std::memory_order_acquire);
}

/// @brief 验证超时会触发终止回调。
bool testTimeoutFires()
{
    std::atomic<bool>              fired{ false };
    MMM::Runtime::ShutdownWatchdog watchdog(
        [&fired]() { fired.store(true, std::memory_order_release); });
    watchdog.arm(20ms);
    const bool observed = waitUntilTrue(fired, 500ms);
    watchdog.complete();
    return observed;
}

/// @brief 验证正常完成会取消终止回调。
bool testCompletionSuppressesTimeout()
{
    std::atomic<bool>              fired{ false };
    MMM::Runtime::ShutdownWatchdog watchdog(
        [&fired]() { fired.store(true, std::memory_order_release); });
    watchdog.arm(100ms);
    watchdog.complete();
    std::this_thread::sleep_for(150ms);
    return !fired.load(std::memory_order_acquire);
}
}  // namespace

/// @brief 运行退出看门狗回归测试。
int main()
{
    if ( !testTimeoutFires() ) return 1;
    if ( !testCompletionSuppressesTimeout() ) return 2;
    return 0;
}
