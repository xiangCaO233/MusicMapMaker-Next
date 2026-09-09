#include "logic/ProjectDirectoryWatcher.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"

#include <chrono>
#include <ice/thread/ThreadPool.hpp>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#endif

namespace MMM::Logic
{

#ifndef _WIN32
namespace
{
/// @brief 非 Windows 备用轮询快照，记录路径和最后修改时间。
/// 快照只用于判定是否需要重扫，不承载资源内容或业务对象。
using DirectoryPollingSnapshot =
    std::unordered_map<std::filesystem::path, std::filesystem::file_time_type>;

/// @brief 采集目录当前状态，所有文件系统错误都以跳过条目处理。
/// @param root 需要递归采样的根目录。
/// @param snapshot 输出快照。
/// 调用前清空输出；单项查询失败时省略该项，不继承上一轮的记录。
/// @return 根目录可访问时返回 true。
/// 采样比较的是写入时间而非文件内容，时间戳不变的内容变化不在此处检测。
bool collectDirectoryPollingSnapshot(const std::filesystem::path& root,
                                     DirectoryPollingSnapshot&    snapshot)
{
    snapshot.clear();
    // 每次重建目录状态，删除文件会通过键消失反映在快照差异中。

    std::error_code rootError;
    if ( !std::filesystem::exists(root, rootError) || rootError ) {
        // 根丢失不提交空基准，避免把临时访问失败解释成所有资源已删除。
        return false;
    }

    constexpr auto iteratorOptions =
        std::filesystem::directory_options::skip_permission_denied;
    // 轮询不跟随目录符号链接，避免从项目树向外继续递归。
    std::error_code                               iteratorError;
    std::filesystem::recursive_directory_iterator iterator(
        root, iteratorOptions, iteratorError);
    const std::filesystem::recursive_directory_iterator end;
    while ( !iteratorError && iterator != end ) {
        const auto currentPath = iterator->path().lexically_normal();
        const auto relativePath =
            currentPath.lexically_relative(root.lexically_normal());
        // 过滤基于项目内相对路径，两边都做词法规范化，不额外解析符号链接。
        if ( !ProjectDirectoryWatcher::isRelevantProjectPathChange(
                 relativePath) ) {
            std::error_code directoryError;
            if ( relativePath == std::filesystem::path(".mmm") &&
                 iterator->is_directory(directoryError) && !directoryError ) {
                // 根下内部配置目录整树跳过，保存工作区不应反过来触发资源重扫。
                iterator.disable_recursion_pending();
            }
            iterator.increment(iteratorError);
            continue;
        }

        std::error_code timeError;
        // 文件和目录都记修改时间，新增或删除子项也能通过目录状态变化发现。
        const auto writeTime =
            std::filesystem::last_write_time(currentPath, timeError);
        if ( !timeError ) {
            // 采样期间条目可能已经消失，查询失败不为它制造默认时间戳。
            snapshot[currentPath] = writeTime;
        }

        iterator.increment(iteratorError);
    }

    // 根存在后采用尽力采样；部分遍历错误不会抛弃已经采到的快照。
    return true;
}

/// @brief 判断两个目录轮询快照是否存在差异。
/// @param previous 上一次接受的目录状态。
/// @param current 本次采样状态，不要求无序表迭代次序相同。
/// @return 路径集合或任一路径时间戳变化时为 true。
/// 检测到首个差异即可结束，上层无须知道具体变化文件数量。
bool directoryPollingSnapshotChanged(const DirectoryPollingSnapshot& previous,
                                     const DirectoryPollingSnapshot& current)
{
    if ( previous.size() != current.size() ) return true;
    // 数量相同仍可能是一删一增，必须按键检查，而不能只比较条目总数。
    for ( const auto& [path, writeTime] : current ) {
        auto it = previous.find(path);
        if ( it == previous.end() || it->second != writeTime ) return true;
    }
    return false;
}
}  // namespace
#endif

/// @brief 停止监听线程并释放平台相关监听句柄。
/// @warning 析构等待捕获 this 的工作任务结束，不能在该工作任务自身执行析构。
ProjectDirectoryWatcher::~ProjectDirectoryWatcher()
{
    stop();
}

/// @brief 启动文件夹监听器。
/// @param path 需要递归监听的项目目录路径。
/// @warning 项目切换的低频入口，先停止旧任务，可能等待上一轮后台采样结束。
void ProjectDirectoryWatcher::start(const std::filesystem::path& path)
{
    // start 负责替换监听对象，不负责校验项目配置或加载项目资源。
    stop();
    // 不让两个项目的监听任务共用此实例，新任务只能在旧任务退出后建立。

    auto* appThreadPool = Runtime::AppThreadPool::instance().get();
    // 共享线程池由应用生命周期管理，此处只借用任务提交入口。
    if ( !appThreadPool ) {
        // 未初始化 Runtime 时保持未启动，不另建独立线程绕开应用退出管理。
        XERROR("Directory Watcher: Runtime thread pool is not initialized.");
        return;
    }

    {
        /// @brief 保护监听状态切换和平台退出事件创建的互斥锁。
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running.store(true, std::memory_order_release);
        m_changePending.store(false, std::memory_order_release);
        // 新项目不继承旧项目的待扫描信号。
#ifdef _WIN32
        m_exitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        // 手动复位退出事件由本轮 start 创建，stop
        // 唤醒后直到任务退出都保持触发。
        if ( !m_exitEvent ) {
            m_running.store(false, std::memory_order_release);
            XERROR("Directory Watcher: Failed to create exit event.");
            return;
        }
#endif
    }

    m_stopSource = std::stop_source{};
    // stop_source 不能沿用已请求停止的旧状态，每次监听使用新令牌。
    const auto token = m_stopSource.get_token();
    m_workerFuture   = appThreadPool->enqueue(
        // 路径按值捕获，调用者离开启动函数后不依赖其参数对象继续存活。
        [this, path, token]() { watcherThreadLoop(path, token); });
    // 日志表示任务已提交，平台目录打开可能稍后在工作线程内失败。
    XINFO("Directory Watcher: Started monitoring directory: {}",
          Config::pathToUtf8(path));
}

/// @brief 停止文件夹监听器。
/// @warning 控制侧等待后台任务退出；不能从监听任务内部等待自己的 future。
void ProjectDirectoryWatcher::stop()
{
    // 清理不消费 m_changePending，新一次 start 会清零旧项目的挂起状态。
    /// @brief 停止前监听器是否处于运行状态。
    bool wasRunning = false;
    /// @brief 停止前监听任务是否仍有效。
    bool hadWorker = false;

    {
        /// @brief 保护监听状态切换和平台退出事件触发的互斥锁。
        std::lock_guard<std::mutex> lock(m_mutex);
        wasRunning = m_running.exchange(false, std::memory_order_acq_rel);
        // 运行位负责循环退出，future 是否有效决定是否仍需等待任务收尾。
        hadWorker = m_workerFuture.valid();
#ifdef _WIN32
        if ( wasRunning && m_exitEvent &&
             m_exitEvent != INVALID_HANDLE_VALUE ) {
            // 触发退出事件，使 ReadDirectoryChangesW 阻塞立刻解除并退出
            SetEvent(static_cast<HANDLE>(m_exitEvent));
        }
#endif
    }

    m_stopSource.request_stop();
    // 令牌与运行标志同时用于循环退出；Win32 还依靠退出事件唤醒系统等待。
    // 等待放在互斥锁之外，后台任务仍可能需要该锁发布平台句柄。
    if ( m_workerFuture.valid() ) {
        m_workerFuture.wait();
        m_workerFuture = std::future<void>{};
        // 消费掉本轮任务句柄，使重复 stop 不再次等待已结束的任务。
    }

    // 在线程完全退出并 Join 之后，再安全地在主线程清理句柄，防止重叠 I/O
    // 并发冲突
    {
        /// @brief 保护平台句柄释放的互斥锁。
        std::lock_guard<std::mutex> lock(m_mutex);
#ifdef _WIN32
        if ( m_directoryHandle && m_directoryHandle != INVALID_HANDLE_VALUE ) {
            CloseHandle(static_cast<HANDLE>(m_directoryHandle));
            m_directoryHandle = INVALID_HANDLE_VALUE;
        }
        if ( m_exitEvent && m_exitEvent != INVALID_HANDLE_VALUE ) {
            CloseHandle(static_cast<HANDLE>(m_exitEvent));
            m_exitEvent = nullptr;
        }
#endif
    }

    if ( wasRunning || hadWorker ) {
        // 空闲实例重复停止不重复输出日志。
        XINFO("Directory Watcher: Stopped monitoring.");
    }
}

/// @brief 读取并清空当前是否存在待处理的文件系统变更。
/// @return 存在待处理变更时返回 true。
/// @warning 逻辑每轮调用；只做 acq_rel 原子交换，不在此处扫描或等待文件系统。
bool ProjectDirectoryWatcher::consumeChangePending()
{
    // exchange 原子消费；消费期间新到达的通知可留下下一轮待处理标志。
    // 多个事件合并为一次重扫请求，不承诺逐事件计数。
    return m_changePending.exchange(false, std::memory_order_acq_rel);
}

/// @brief 判断项目内相对路径变化是否需要触发资源重扫。
/// @param relativePath 相对于项目根目录的变化路径。
/// @return 资源文件变化返回 true；项目描述文件自身变化返回 false。
/// 这里只排除内部配置，不按媒体扩展名筛选，未知路径也可影响后续资源发现。
bool ProjectDirectoryWatcher::isRelevantProjectPathChange(
    const std::filesystem::path& relativePath)
{
    const auto normalized = relativePath.lexically_normal();
    // 词法处理消除 ./ 等写法差异，不为每条通知增加文件系统查询。
    if ( normalized == std::filesystem::path("mmm_project.json") ) {
        return false;
    }
    const auto firstComponent = normalized.begin();
    // 只检查第一段，嵌套资源目录中的同名文件不当作项目根配置。
    // 空路径仍保守地视为相关变化，让上层重扫而不是错过不带名称的通知。
    return firstComponent == normalized.end() ||
           *firstComponent != std::filesystem::path(".mmm");
}

/// @brief 文件夹监听线程的主循环。
/// @param watchPath 需要递归监听的项目目录路径。
/// @param stopToken 共享线程池任务的停止令牌。
/// @warning 仅在线程池后台任务运行；系统事件等待及轮询休眠不阻塞逻辑更新。
void ProjectDirectoryWatcher::watcherThreadLoop(std::filesystem::path watchPath,
                                                std::stop_token       stopToken)
{
#ifdef _WIN32
    // 平台句柄只属于本轮任务，控制侧 stop 在任务结束后统一释放目录句柄。
    /// @brief Win32 API 需要使用的宽字符目录路径。
    std::wstring widePath = watchPath.wstring();
    /// @brief 项目目录监听句柄，使用重叠 I/O 开启非阻塞模式。
    HANDLE directoryHandle =
        CreateFileW(widePath.c_str(),
                    FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS |
                        FILE_FLAG_OVERLAPPED,  // 使用重叠 I/O 开启非阻塞模式
                    NULL);

    if ( directoryHandle == INVALID_HANDLE_VALUE ) {
        // 失败只终止监听；项目加载由控制层独立完成，不在此线程重试打开项目。
        XERROR("Directory Watcher: Failed to open directory for monitoring: {}",
               Config::pathToUtf8(watchPath));
        m_running.store(false, std::memory_order_release);
        return;
    }

    {
        /// @brief 保护目录监听句柄发布的互斥锁。
        std::lock_guard<std::mutex> lock(m_mutex);
        m_directoryHandle = directoryHandle;
        // 向 stop 公开已打开句柄，退出清理在 future 完成后才执行。
    }

    // 创建重叠 I/O 事件
    /// @brief 目录变更完成事件，用于等待 ReadDirectoryChangesW 完成。
    HANDLE changeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if ( !changeEvent ) {
        // 目录句柄已交给控制侧收尾，当前任务不重复关闭它。
        m_running.store(false, std::memory_order_release);
        return;
    }

    /// @brief ReadDirectoryChangesW 写入文件变更通知的缓冲区。
    alignas(DWORD) BYTE buffer[4096];

    /// @brief 重叠 I/O 状态，绑定目录变更完成事件。
    OVERLAPPED overlapped = {};
    // 零初始化其余状态，只指定本轮完成事件，不把上一次任务的状态带进来。
    overlapped.hEvent = changeEvent;

    /// @brief 停止监听和目录变更两个事件的等待数组。
    HANDLE waitHandles[2] = { static_cast<HANDLE>(m_exitEvent), changeEvent };
    // 退出事件置于首位；停机与变更同时就绪时优先走退出分支。

    while ( m_running.load(std::memory_order_acquire) &&
            !stopToken.stop_requested() ) {
        ResetEvent(changeEvent);
        // 每轮复用同一缓冲及 OVERLAPPED，完成上一轮等待后才提交下一次请求。

        /// @brief 本次目录变更读取返回的字节数。
        DWORD bytesReturned = 0;
        /// @brief 是否成功投递目录变更读取请求。
        BOOL success = ReadDirectoryChangesW(directoryHandle,
                                             buffer,
                                             sizeof(buffer),
                                             TRUE,  // 递归监听子目录
                                             FILE_NOTIFY_CHANGE_FILE_NAME |
                                                 FILE_NOTIFY_CHANGE_DIR_NAME |
                                                 FILE_NOTIFY_CHANGE_LAST_WRITE,
                                             &bytesReturned,
                                             &overlapped,
                                             NULL);
        // 这里只监控名称与写入变化，不把访问时间变化当作资源内容更新。

        if ( !success && GetLastError() != ERROR_IO_PENDING ) {
            // 重叠请求尚在进行属于正常状态，其余投递失败结束本次监听。
            break;
        }

        // 等待退出事件或变更事件
        /// @brief 当前完成等待的事件下标或错误状态。
        DWORD waitResult =
            WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        // 无限等待发生在后台线程，控制侧通过专用退出事件请求结束。

        if ( waitResult == WAIT_OBJECT_0 ) {
            // 收到退出事件，主动取消挂起的 I/O 并退出循环
            CancelIoEx(directoryHandle, &overlapped);
            break;
        } else if ( waitResult == WAIT_OBJECT_0 + 1 ) {
            /// @brief 本次重叠目录读取实际返回的通知字节数。
            DWORD transferredBytes = 0;
            // 使用实际完成字节数解析，未写入的固定缓冲尾部不能视为通知。
            if ( !GetOverlappedResult(
                     directoryHandle, &overlapped, &transferredBytes, FALSE) ) {
                // 退出取消不发布变更；其他失败留给下一轮重新投递监听请求。
                if ( GetLastError() == ERROR_OPERATION_ABORTED ) {
                    break;
                }
                continue;
            }

            /// @brief 当前通知缓冲区中是否包含需要重扫的资源路径。
            bool hasRelevantChange = false;
            // 同一批内部配置变化会被跳过，遇到资源路径才设置本批通知状态。
            DWORD notificationOffset = 0;
            while ( notificationOffset < transferredBytes ) {
                // 系统缓冲以偏移串联变更记录，文件名长度按字节而不是结尾零字符给出。
                const auto* notification =
                    reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                        buffer + notificationOffset);
                const std::wstring relativeName(
                    notification->FileName,
                    notification->FileNameLength / sizeof(wchar_t));
                // 保留系统返回的相对路径层级，由统一过滤函数判断配置目录。
                if ( isRelevantProjectPathChange(
                         std::filesystem::path(relativeName)) ) {
                    // 任意一条资源变化即足够请求重扫，无需把整批路径排入业务队列。
                    hasRelevantChange = true;
                    break;
                }
                if ( notification->NextEntryOffset == 0 ) {
                    // 零偏移表示批次末尾，不继续解释缓冲中的剩余空间。
                    break;
                }
                notificationOffset += notification->NextEntryOffset;
            }
            if ( hasRelevantChange ) {
                // release 与逻辑侧 exchange
                // 配对，通知只承载脏状态而非资源快照。
                m_changePending.store(true, std::memory_order_release);
            }
        } else {
            // 非预期等待结果结束任务，避免以失效事件继续占用后台线程。
            break;
        }
    }

    // 完成事件只由工作线程管理，目录与退出事件留给 stop 在锁内关闭。
    CloseHandle(changeEvent);
#else
    // 非 Windows 平台备用轮询，避免逻辑线程在每帧路径中做文件系统操作。
    DirectoryPollingSnapshot previousSnapshot;
    // 快照完全局限于工作线程，不向逻辑线程传递文件时间或容器所有权。
    (void)collectDirectoryPollingSnapshot(watchPath, previousSnapshot);
    // 首次采样只建立基准，避免启动监听就把全部已有文件当作新增事件。

    while ( m_running.load(std::memory_order_acquire) &&
            !stopToken.stop_requested() ) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        // 此节拍限制后台文件系统扫描频率，不作为逻辑交互的同步等待窗口。

        DirectoryPollingSnapshot currentSnapshot;
        // 当前轮使用独立候选，采样失败不会破坏已接受的基准内容。
        if ( !collectDirectoryPollingSnapshot(watchPath, currentSnapshot) ) {
            // 根暂时不可访问时保留旧基准，恢复后再与最近有效状态比较。
            continue;
        }

        if ( directoryPollingSnapshotChanged(previousSnapshot,
                                             currentSnapshot) ) {
            previousSnapshot = std::move(currentSnapshot);
            // 仅变化时替换基准，随后通知逻辑层做真正的资源同步。
            m_changePending.store(true, std::memory_order_release);
        }
    }
#endif
    // 控制侧通过 future 确认函数已结束，之后才可以销毁捕获的 this。
}

}  // namespace MMM::Logic
