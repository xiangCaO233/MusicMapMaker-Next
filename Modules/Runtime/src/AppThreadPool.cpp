#include "runtime/AppThreadPool.h"
#include "log/colorful-log.h"
#include "runtime/ShutdownWatchdog.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ice/thread/ThreadPool.hpp>
#include <thread>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

namespace MMM::Runtime
{

namespace
{
/// @brief 共享池保留的最低工作线程数，避免逻辑循环和监听任务占满小型机器。
constexpr int32_t MINIMUM_APP_WORKER_COUNT = 8;

/// @brief 在正常退出超时后立即终止整个进程。
/// @warning 仅由独立退出看门狗调用；不得执行可能等待其他线程的清理或日志刷新。
[[noreturn]] void forceTerminateApplication() noexcept
{
#ifdef _WIN32
    OutputDebugStringA(
        "MusicMapMaker-Next shutdown timed out; terminating process.\n");
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(EXIT_FAILURE));
#endif
    std::_Exit(EXIT_FAILURE);
}
}  // namespace

/// @brief 获取应用级线程池管理器单例。
/// @return 应用级线程池管理器。
AppThreadPool& AppThreadPool::instance()
{
    static AppThreadPool pool;
    return pool;
}

AppThreadPool::~AppThreadPool()
{
    shutdown();
    completeApplicationShutdownWatchdog();
}

/// @brief 按当前硬件逻辑核心数初始化共享线程池。
/// @warning 生命周期路径：由 main 启动阶段调用；禁止放入每帧热路径。
void AppThreadPool::init()
{
    if ( m_threadPool ) {
        return;
    }

    const unsigned int logicalCoreCount =
        std::max(1u, std::thread::hardware_concurrency());
    m_requestedWorkerCount =
        std::max<int32_t>(MINIMUM_APP_WORKER_COUNT,
                          static_cast<int32_t>(std::ceil(
                              static_cast<float>(logicalCoreCount) * 0.8f)));
    m_threadPool = std::make_unique<ice::ThreadPool>(m_requestedWorkerCount);
    XINFO("AppThreadPool initialized: logical cores={}, requested workers={}",
          logicalCoreCount,
          m_requestedWorkerCount);
}

/// @brief 关闭共享线程池并等待已提交任务完成。
/// @warning 不可中断操作：由 GameLoop 退出阶段调用，可能阻塞等待后台任务收尾。
void AppThreadPool::shutdown()
{
    if ( !m_threadPool ) {
        return;
    }

    m_threadPool.reset();
    m_requestedWorkerCount = 0;
    XINFO("AppThreadPool shutdown.");
}

void AppThreadPool::armApplicationShutdownWatchdog(
    std::chrono::milliseconds timeout)
{
    if ( !m_shutdownWatchdog ) {
        m_shutdownWatchdog =
            std::make_unique<ShutdownWatchdog>(forceTerminateApplication);
    }
    m_shutdownWatchdog->arm(timeout);
    XINFO("Application shutdown watchdog armed: {} ms", timeout.count());
}

void AppThreadPool::completeApplicationShutdownWatchdog()
{
    if ( !m_shutdownWatchdog ) return;
    m_shutdownWatchdog->complete();
    m_shutdownWatchdog.reset();
}

/// @brief 获取共享线程池。
/// @return 已初始化时返回线程池指针，否则返回 nullptr。
ice::ThreadPool* AppThreadPool::get() const
{
    return m_threadPool.get();
}

/// @brief 获取创建线程池时请求的工作线程数量。
/// @return 请求的工作线程数量；未初始化时返回 0。
int32_t AppThreadPool::requestedWorkerCount() const
{
    return m_requestedWorkerCount;
}

}  // namespace MMM::Runtime
