#include "runtime/AppThreadPool.h"
#include "log/colorful-log.h"
#include <algorithm>
#include <cmath>
#include <ice/thread/ThreadPool.hpp>
#include <thread>

namespace MMM::Runtime
{

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
}

/// @brief 按当前硬件逻辑核心数初始化共享线程池。
/// @warning 生命周期路径：由 GameLoop 启动阶段调用；禁止放入每帧热路径。
void AppThreadPool::init()
{
    if ( m_threadPool ) {
        return;
    }

    const unsigned int logicalCoreCount =
        std::max(1u, std::thread::hardware_concurrency());
    m_requestedWorkerCount =
        std::max<int32_t>(1,
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
