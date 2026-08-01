#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>

namespace MMM::Logic
{

/// @brief 监听项目目录中的文件系统变更，并向逻辑线程发布待处理变更标记。
class ProjectDirectoryWatcher
{
public:
    /// @brief 创建未启动的项目目录监听器。
    ProjectDirectoryWatcher() = default;

    /// @brief 停止监听线程并释放平台相关监听句柄。
    ~ProjectDirectoryWatcher();

    /// @brief 禁止复制项目目录监听器，避免重复持有同一线程和系统句柄。
    ProjectDirectoryWatcher(const ProjectDirectoryWatcher&) = delete;

    /// @brief 禁止移动项目目录监听器，避免监听线程持有失效的 this 指针。
    ProjectDirectoryWatcher(ProjectDirectoryWatcher&&) = delete;

    /// @brief 禁止复制赋值项目目录监听器，避免重复持有同一线程和系统句柄。
    ProjectDirectoryWatcher& operator=(const ProjectDirectoryWatcher&) = delete;

    /// @brief 禁止移动赋值项目目录监听器，避免监听线程持有失效的 this 指针。
    ProjectDirectoryWatcher& operator=(ProjectDirectoryWatcher&&) = delete;

    /// @brief 启动文件夹监听器。
    /// @param path 需要递归监听的项目目录路径。
    void start(const std::filesystem::path& path);

    /// @brief 停止文件夹监听器。
    void stop();

    /// @brief 读取并清空当前是否存在待处理的文件系统变更。
    /// @return 存在待处理变更时返回 true。
    /// @warning 逻辑热路径原子：loop 每次迭代读取并清空监听线程写入的变更标记；
    /// 不可避免，用于将 watcher 线程的文件系统事件去抖后转入低频扫描。
    bool consumeChangePending();

    /// @brief 判断项目内相对路径变化是否需要触发资源重扫。
    /// @param relativePath 相对于项目根目录的变化路径。
    /// @return 资源文件变化返回 true；项目描述文件自身变化返回 false。
    static bool isRelevantProjectPathChange(
        const std::filesystem::path& relativePath);

private:
    /// @brief 文件夹监听线程的主循环。
    /// @param watchPath 需要递归监听的项目目录路径。
    void watcherThreadLoop(std::filesystem::path watchPath);

    /// @brief 文件夹监听线程。
    std::thread m_thread;

    /// @brief 监听线程运行标志。
    /// @warning 文件监听线程原子：启动/停止线程时写入，监听循环轮询读取；
    /// 不属于渲染热路径，但用于跨线程退出信号。
    std::atomic<bool> m_running{ false };

    /// @brief 是否有未处理的文件系统变更挂起。
    /// @warning 逻辑热路径原子：监听线程写入，逻辑 loop 每次迭代 exchange；
    /// 不可避免，用于把文件系统事件去抖后转入低频扫描。
    std::atomic<bool> m_changePending{ false };

#ifdef _WIN32
    /// @brief Win32 目录句柄，用于取消阻塞的 ReadDirectoryChangesW。
    void* m_directoryHandle{ nullptr };

    /// @brief Win32 退出事件句柄，用于安全退出监听线程。
    void* m_exitEvent{ nullptr };
#endif

    /// @brief 保护平台目录句柄和退出事件句柄的独立锁。
    mutable std::mutex m_mutex;
};

}  // namespace MMM::Logic
