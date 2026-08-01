#include "logic/ProjectDirectoryWatcher.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <chrono>
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
using DirectoryPollingSnapshot =
    std::unordered_map<std::filesystem::path, std::filesystem::file_time_type>;

/// @brief 采集目录当前状态，所有文件系统错误都以跳过条目处理。
/// @param root 需要递归采样的根目录。
/// @param snapshot 输出快照。
/// @return 根目录可访问时返回 true。
bool collectDirectoryPollingSnapshot(const std::filesystem::path& root,
                                     DirectoryPollingSnapshot&    snapshot)
{
    snapshot.clear();

    std::error_code rootError;
    if ( !std::filesystem::exists(root, rootError) || rootError ) {
        return false;
    }

    constexpr auto iteratorOptions =
        std::filesystem::directory_options::skip_permission_denied;
    std::error_code                               iteratorError;
    std::filesystem::recursive_directory_iterator iterator(
        root, iteratorOptions, iteratorError);
    const std::filesystem::recursive_directory_iterator end;
    while ( !iteratorError && iterator != end ) {
        const auto currentPath = iterator->path().lexically_normal();
        const auto relativePath =
            currentPath.lexically_relative(root.lexically_normal());
        if ( !ProjectDirectoryWatcher::isRelevantProjectPathChange(
                 relativePath) ) {
            std::error_code directoryError;
            if ( relativePath == std::filesystem::path(".mmm") &&
                 iterator->is_directory(directoryError) && !directoryError ) {
                iterator.disable_recursion_pending();
            }
            iterator.increment(iteratorError);
            continue;
        }

        std::error_code timeError;
        const auto      writeTime =
            std::filesystem::last_write_time(currentPath, timeError);
        if ( !timeError ) {
            snapshot[currentPath] = writeTime;
        }

        iterator.increment(iteratorError);
    }

    return true;
}

/// @brief 判断两个目录轮询快照是否存在差异。
bool directoryPollingSnapshotChanged(const DirectoryPollingSnapshot& previous,
                                     const DirectoryPollingSnapshot& current)
{
    if ( previous.size() != current.size() ) return true;
    for ( const auto& [path, writeTime] : current ) {
        auto it = previous.find(path);
        if ( it == previous.end() || it->second != writeTime ) return true;
    }
    return false;
}
}  // namespace
#endif

/// @brief 停止监听线程并释放平台相关监听句柄。
ProjectDirectoryWatcher::~ProjectDirectoryWatcher()
{
    stop();
}

/// @brief 启动文件夹监听器。
/// @param path 需要递归监听的项目目录路径。
void ProjectDirectoryWatcher::start(const std::filesystem::path& path)
{
    stop();

    {
        /// @brief 保护监听状态切换和平台退出事件创建的互斥锁。
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running.store(true, std::memory_order_release);
        m_changePending.store(false, std::memory_order_release);
#ifdef _WIN32
        m_exitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if ( !m_exitEvent ) {
            m_running.store(false, std::memory_order_release);
            XERROR("Directory Watcher: Failed to create exit event.");
            return;
        }
#endif
    }

    m_thread =
        std::thread(&ProjectDirectoryWatcher::watcherThreadLoop, this, path);
    XINFO("Directory Watcher: Started monitoring directory: {}",
          Config::pathToUtf8(path));
}

/// @brief 停止文件夹监听器。
void ProjectDirectoryWatcher::stop()
{
    /// @brief 停止前监听器是否处于运行状态。
    bool wasRunning = false;
    /// @brief 停止前监听线程是否仍可 join。
    bool hadThread = false;

    {
        /// @brief 保护监听状态切换和平台退出事件触发的互斥锁。
        std::lock_guard<std::mutex> lock(m_mutex);
        wasRunning = m_running.exchange(false, std::memory_order_acq_rel);
        hadThread  = m_thread.joinable();
#ifdef _WIN32
        if ( wasRunning && m_exitEvent &&
             m_exitEvent != INVALID_HANDLE_VALUE ) {
            // 触发退出事件，使 ReadDirectoryChangesW 阻塞立刻解除并退出
            SetEvent(static_cast<HANDLE>(m_exitEvent));
        }
#endif
    }

    if ( m_thread.joinable() ) {
        m_thread.join();
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

    if ( wasRunning || hadThread ) {
        XINFO("Directory Watcher: Stopped monitoring.");
    }
}

/// @brief 读取并清空当前是否存在待处理的文件系统变更。
/// @return 存在待处理变更时返回 true。
bool ProjectDirectoryWatcher::consumeChangePending()
{
    return m_changePending.exchange(false, std::memory_order_acq_rel);
}

/// @brief 判断项目内相对路径变化是否需要触发资源重扫。
/// @param relativePath 相对于项目根目录的变化路径。
/// @return 资源文件变化返回 true；项目描述文件自身变化返回 false。
bool ProjectDirectoryWatcher::isRelevantProjectPathChange(
    const std::filesystem::path& relativePath)
{
    const auto normalized = relativePath.lexically_normal();
    if ( normalized == std::filesystem::path("mmm_project.json") ) {
        return false;
    }
    const auto firstComponent = normalized.begin();
    return firstComponent == normalized.end() ||
           *firstComponent != std::filesystem::path(".mmm");
}

/// @brief 文件夹监听线程的主循环。
/// @param watchPath 需要递归监听的项目目录路径。
void ProjectDirectoryWatcher::watcherThreadLoop(std::filesystem::path watchPath)
{
#ifdef _WIN32
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
        XERROR("Directory Watcher: Failed to open directory for monitoring: {}",
               Config::pathToUtf8(watchPath));
        m_running.store(false, std::memory_order_release);
        return;
    }

    {
        /// @brief 保护目录监听句柄发布的互斥锁。
        std::lock_guard<std::mutex> lock(m_mutex);
        m_directoryHandle = directoryHandle;
    }

    // 创建重叠 I/O 事件
    /// @brief 目录变更完成事件，用于等待 ReadDirectoryChangesW 完成。
    HANDLE changeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if ( !changeEvent ) {
        m_running.store(false, std::memory_order_release);
        return;
    }

    /// @brief ReadDirectoryChangesW 写入文件变更通知的缓冲区。
    alignas(DWORD) BYTE buffer[4096];

    /// @brief 重叠 I/O 状态，绑定目录变更完成事件。
    OVERLAPPED overlapped = {};
    overlapped.hEvent     = changeEvent;

    /// @brief 停止监听和目录变更两个事件的等待数组。
    HANDLE waitHandles[2] = { static_cast<HANDLE>(m_exitEvent), changeEvent };

    while ( m_running.load(std::memory_order_acquire) ) {
        ResetEvent(changeEvent);

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

        if ( !success && GetLastError() != ERROR_IO_PENDING ) {
            break;
        }

        // 等待退出事件或变更事件
        /// @brief 当前完成等待的事件下标或错误状态。
        DWORD waitResult =
            WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

        if ( waitResult == WAIT_OBJECT_0 ) {
            // 收到退出事件，主动取消挂起的 I/O 并退出循环
            CancelIoEx(directoryHandle, &overlapped);
            break;
        } else if ( waitResult == WAIT_OBJECT_0 + 1 ) {
            /// @brief 本次重叠目录读取实际返回的通知字节数。
            DWORD transferredBytes = 0;
            if ( !GetOverlappedResult(
                     directoryHandle, &overlapped, &transferredBytes, FALSE) ) {
                if ( GetLastError() == ERROR_OPERATION_ABORTED ) {
                    break;
                }
                continue;
            }

            /// @brief 当前通知缓冲区中是否包含需要重扫的资源路径。
            bool  hasRelevantChange  = false;
            DWORD notificationOffset = 0;
            while ( notificationOffset < transferredBytes ) {
                const auto* notification =
                    reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                        buffer + notificationOffset);
                const std::wstring relativeName(
                    notification->FileName,
                    notification->FileNameLength / sizeof(wchar_t));
                if ( isRelevantProjectPathChange(
                         std::filesystem::path(relativeName)) ) {
                    hasRelevantChange = true;
                    break;
                }
                if ( notification->NextEntryOffset == 0 ) {
                    break;
                }
                notificationOffset += notification->NextEntryOffset;
            }
            if ( hasRelevantChange ) {
                m_changePending.store(true, std::memory_order_release);
            }
        } else {
            // 发生错误
            break;
        }
    }

    CloseHandle(changeEvent);
#else
    // 非 Windows 平台备用轮询，避免逻辑线程在每帧路径中做文件系统操作。
    DirectoryPollingSnapshot previousSnapshot;
    (void)collectDirectoryPollingSnapshot(watchPath, previousSnapshot);

    while ( m_running.load(std::memory_order_acquire) ) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        DirectoryPollingSnapshot currentSnapshot;
        if ( !collectDirectoryPollingSnapshot(watchPath, currentSnapshot) ) {
            continue;
        }

        if ( directoryPollingSnapshotChanged(previousSnapshot,
                                             currentSnapshot) ) {
            previousSnapshot = std::move(currentSnapshot);
            m_changePending.store(true, std::memory_order_release);
        }
    }
#endif
}

}  // namespace MMM::Logic
