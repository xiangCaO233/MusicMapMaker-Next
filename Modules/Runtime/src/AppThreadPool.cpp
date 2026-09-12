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
    // 强制退出无法依赖普通日志刷新，先向调试器保留一条诊断信息。
    OutputDebugStringA(
        "MusicMapMaker-Next shutdown timed out; terminating process.\n");
    // Windows 原生终止覆盖 CRT 清理也已卡住的情况，不再等待静态析构。
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(EXIT_FAILURE));
#endif
    // _Exit 是跨平台兜底；它刻意跳过可能再次阻塞的缓冲刷新和析构链。
    std::_Exit(EXIT_FAILURE);
}
}  // namespace

/// @brief 获取应用级线程池管理器单例。
/// @return 应用级线程池管理器。
AppThreadPool& AppThreadPool::instance()
{
    // 函数内静态对象由 C++ 运行时保证只初始化一次，并在进程退出时析构。
    static AppThreadPool pool;
    return pool;
}

/// @brief 在静态单例析构时兜底回收线程池和退出看门狗。
/// @warning 该路径可能等待后台任务；正常流程应在 GameLoop 中提前完成关闭。
AppThreadPool::~AppThreadPool()
{
    // 先回收业务工作线程，避免任务在看门狗撤销后继续阻塞正常退出。
    shutdown();
    // 正常静态析构不应留下仍可能调用强制退出处理器的监控线程。
    completeApplicationShutdownWatchdog();
}

/// @brief 按当前硬件逻辑核心数初始化共享线程池。
/// @warning 生命周期路径：由 main 启动阶段调用；禁止放入每帧热路径。
void AppThreadPool::init()
{
    // 初始化保持幂等，多个上层子系统共享同一个所有权入口。
    if ( m_threadPool ) {
        return;
    }

    // 标准库允许无法查询并返回零，因此至少按一个逻辑核心计算。
    const unsigned int logicalCoreCount =
        std::max(1u, std::thread::hardware_concurrency());
    // 保留最低并发度，并只申请约八成逻辑核心，给主循环与驱动线程留余量。
    m_requestedWorkerCount =
        std::max<int32_t>(MINIMUM_APP_WORKER_COUNT,
                          static_cast<int32_t>(std::ceil(
                              static_cast<float>(logicalCoreCount) * 0.8f)));
    // 计数先于构造保存，便于日志和使用方采用同一份容量基准。
    m_threadPool = std::make_unique<ice::ThreadPool>(m_requestedWorkerCount);
    XINFO("AppThreadPool initialized: logical cores={}, requested workers={}",
          logicalCoreCount,
          m_requestedWorkerCount);
}

/// @brief 关闭共享线程池并等待已提交任务完成。
/// @warning 不可中断操作：由 GameLoop 退出阶段调用，可能阻塞等待后台任务收尾。
void AppThreadPool::shutdown()
{
    // 未初始化或已经关闭时直接返回，保证退出清理可重复执行。
    if ( !m_threadPool ) {
        return;
    }

    // ThreadPool 析构负责停止接收任务并等待其内部工作线程收尾。
    m_threadPool.reset();
    // 对外可见的容量与 get() 的空状态同步复位，避免报告过期容量。
    m_requestedWorkerCount = 0;
    XINFO("AppThreadPool shutdown.");
}

/// @brief 创建看门狗并开始本轮应用退出倒计时。
/// @param timeout 正常关闭线程池及其他业务资源允许占用的时间。
/// @warning 退出低频路径：重新调用会同步停止上一轮监控线程。
void AppThreadPool::armApplicationShutdownWatchdog(
    std::chrono::milliseconds timeout)
{
    // 延迟创建可避免正常运行期间常驻一个从未使用的同步对象。
    if ( !m_shutdownWatchdog ) {
        // 回调不捕获单例状态，超时后可直接进入不可恢复的进程终止路径。
        m_shutdownWatchdog =
            std::make_unique<ShutdownWatchdog>(forceTerminateApplication);
    }
    // 独立线程开始计时后，主退出流程仍可继续关闭共享线程池。
    m_shutdownWatchdog->arm(timeout);
    XINFO("Application shutdown watchdog armed: {} ms", timeout.count());
}

/// @brief 通知看门狗退出成功，并释放其独立监控线程。
/// @warning 应在共享线程池和业务资源均完成回收之后调用。
void AppThreadPool::completeApplicationShutdownWatchdog()
{
    // 未布防时保持幂等，兼容启动失败后的早期退出路径。
    if ( !m_shutdownWatchdog ) return;
    // complete 先阻止超时回调，再由 reset 销毁已停止的同步状态。
    m_shutdownWatchdog->complete();
    m_shutdownWatchdog.reset();
}

/// @brief 获取共享线程池。
/// @return 已初始化时返回线程池指针，否则返回 nullptr。
ice::ThreadPool* AppThreadPool::get() const
{
    // 返回值仅为观察指针，其生命周期仍完全受本单例的 init/shutdown 控制。
    return m_threadPool.get();
}

/// @brief 获取创建线程池时请求的工作线程数量。
/// @return 请求的工作线程数量；未初始化时返回 0。
int32_t AppThreadPool::requestedWorkerCount() const
{
    // 该快照只描述创建时容量；关闭路径会与线程池指针一起复位。
    return m_requestedWorkerCount;
}

}  // namespace MMM::Runtime
