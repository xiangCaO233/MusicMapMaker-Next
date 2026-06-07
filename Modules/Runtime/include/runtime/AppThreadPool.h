#pragma once

#include <cstdint>
#include <memory>

namespace ice
{
class ThreadPool;
}

namespace MMM::Runtime
{

/// @brief 应用级共享线程池。
/// 统一承载音频解码、后台计算、渲染命令录制和逻辑任务。
class AppThreadPool final
{
public:
    /// @brief 获取应用级线程池管理器单例。
    /// @return 应用级线程池管理器。
    static AppThreadPool& instance();

    /// @brief 按当前硬件逻辑核心数初始化共享线程池。
    /// @warning 生命周期路径：由 GameLoop 启动阶段调用；禁止放入每帧热路径。
    void init();

    /// @brief 关闭共享线程池并等待已提交任务完成。
    /// @warning 不可中断操作：由 GameLoop
    /// 退出阶段调用，可能阻塞等待后台任务收尾。
    void shutdown();

    /// @brief 获取共享线程池。
    /// @return 已初始化时返回线程池指针，否则返回 nullptr。
    ice::ThreadPool* get() const;

    /// @brief 获取创建线程池时请求的工作线程数量。
    /// @return 请求的工作线程数量；未初始化时返回 0。
    int32_t requestedWorkerCount() const;

private:
    AppThreadPool() = default;
    ~AppThreadPool();

    AppThreadPool(AppThreadPool&&)                 = delete;
    AppThreadPool(const AppThreadPool&)            = delete;
    AppThreadPool& operator=(AppThreadPool&&)      = delete;
    AppThreadPool& operator=(const AppThreadPool&) = delete;

    /// @brief IonCachyEngine 线程池实例。
    std::unique_ptr<ice::ThreadPool> m_threadPool;

    /// @brief 创建线程池时请求的工作线程数量。
    int32_t m_requestedWorkerCount{ 0 };
};

}  // namespace MMM::Runtime
