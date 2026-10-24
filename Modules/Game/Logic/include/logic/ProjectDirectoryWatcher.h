#pragma once

#include <atomic>
#include <filesystem>
#include <memory>

namespace MMM::Logic
{

/// @brief 监听项目目录中的文件系统变更，并向逻辑线程发布待处理变更标记。
/// 通知只表示“目录可能变化”，不保留事件计数或路径队列，资源重扫由上层执行。
/// start/stop/析构由控制侧串行调用，工作任务只发布变更标志。
class ProjectDirectoryWatcher
{
public:
    /// @brief 创建未启动的项目目录监听器。
    ProjectDirectoryWatcher();

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
    /// @warning 低频生命周期路径，等待线程池任务退出；非 Windows
    /// 轮询可能尚在休眠。
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
    /// @brief 仅由实现文件定义的线程、取消令牌与平台句柄状态。
    struct State;
    /// @brief 生命周期内地址稳定；析构先等待任务退出，再释放状态。
    std::unique_ptr<State> m_state;

    /// @brief 是否有未处理的文件系统变更挂起。
    /// @warning 逻辑热路径原子：监听线程写入，逻辑 loop 每次迭代 exchange；
    /// 不可避免，用于把文件系统事件去抖后转入低频扫描。
    std::atomic<bool> m_changePending{ false };
};

}  // namespace MMM::Logic
