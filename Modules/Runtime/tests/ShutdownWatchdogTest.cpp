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
/// @warning 仅用于独立测试进程；固定轮询间隔不得复制到应用热路径。
bool waitUntilTrue(const std::atomic<bool>&  value,
                   std::chrono::milliseconds timeout)
{
    // 使用 steady_clock 避免系统时间校准改变测试截止点。
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while ( std::chrono::steady_clock::now() < deadline ) {
        // acquire 与回调的 release 配对，使观察结果具有明确的跨线程顺序。
        if ( value.load(std::memory_order_acquire) ) return true;
        // 短暂休眠让出 CPU；总体截止时间远大于看门狗的预期触发时间。
        std::this_thread::sleep_for(2ms);
    }
    // 截止边界再读取一次，避免回调恰好在最后一次循环之后完成而误判。
    return value.load(std::memory_order_acquire);
}

/// @brief 验证超时会触发终止回调。
bool testTimeoutFires()
{
    // 测试处理器只写原子标志，避免真正终止测试进程。
    std::atomic<bool>              fired{ false };
    MMM::Runtime::ShutdownWatchdog watchdog(
        [&fired]() { fired.store(true, std::memory_order_release); });
    // 较短的看门狗时限缩短测试，外层等待则为繁忙 CI 留出调度余量。
    watchdog.arm(20ms);
    const bool observed = waitUntilTrue(fired, 500ms);
    // 无论断言结果如何都回收线程，避免失败返回时泄漏后台执行。
    watchdog.complete();
    return observed;
}

/// @brief 验证正常完成会取消终止回调。
bool testCompletionSuppressesTimeout()
{
    // 与超时用例使用相同内存序，保证两个分支的观察方式一致。
    std::atomic<bool>              fired{ false };
    MMM::Runtime::ShutdownWatchdog watchdog(
        [&fired]() { fired.store(true, std::memory_order_release); });
    watchdog.arm(100ms);
    // 立即完成模拟业务清理先于时限结束，处理器必须保持未调用。
    watchdog.complete();
    // 等过原始时限，证明取消不是单纯延迟了回调执行。
    std::this_thread::sleep_for(150ms);
    return !fired.load(std::memory_order_acquire);
}
}  // namespace

/// @brief 运行退出看门狗回归测试。
/// @return 0 表示全部通过；1 和 2 分别定位超时及取消场景。
int main()
{
    // 独立返回码让 CTest 失败日志无需额外输出即可定位场景。
    if ( !testTimeoutFires() ) return 1;
    if ( !testCompletionSuppressesTimeout() ) return 2;
    return 0;
}
