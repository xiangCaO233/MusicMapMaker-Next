#include "logic/SessionRegistry.h"
#include <algorithm>
#include <utility>

namespace MMM::Logic
{

/// @brief 获取保护会话列表的递归锁。
std::recursive_mutex& SessionRegistry::mutex() const
{
    return m_mutex;
}

/// @brief 生成下一个唯一画布 cameraId。
std::string SessionRegistry::createNextCameraId()
{
    /// @brief 保护本次画布编号分配的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    /// @brief 本次分配使用的递增画布编号。
    const int32_t canvasId = m_nextCanvasId++;
    return "Canvas_" + std::to_string(canvasId);
}

/// @brief 获取当前活跃 Session 索引。
int32_t SessionRegistry::activeIndex() const
{
    return m_activeIndex.load(std::memory_order_relaxed);
}

/// @brief 设置当前活跃 Session 索引。
void SessionRegistry::setActiveIndex(int32_t index)
{
    m_activeIndex.store(index, std::memory_order_relaxed);
}

/// @brief 获取 Session 总数。
int32_t SessionRegistry::count() const
{
    /// @brief 保护本次 Session 数量读取的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return static_cast<int32_t>(m_entries.size());
}

/// @brief 判断索引是否指向有效 Session。
bool SessionRegistry::isValidIndex(int32_t index) const
{
    /// @brief 保护本次索引有效性检查的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return isValidIndexUnsafe(index);
}

/// @brief 获取指定索引的 SessionEntry。
SessionEntry* SessionRegistry::entry(int32_t index)
{
    /// @brief 保护本次 SessionEntry 指针读取的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if ( !isValidIndexUnsafe(index) ) {
        return nullptr;
    }
    return &m_entries[index];
}

/// @brief 获取指定索引的只读 SessionEntry。
const SessionEntry* SessionRegistry::entry(int32_t index) const
{
    /// @brief 保护本次只读 SessionEntry 指针读取的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if ( !isValidIndexUnsafe(index) ) {
        return nullptr;
    }
    return &m_entries[index];
}

/// @brief 获取所有 Session 条目的只读快照。
std::vector<SessionEntry> SessionRegistry::entries() const
{
    /// @brief 保护本次 SessionEntry 快照读取的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_entries;
}

/// @brief 获取当前活跃谱面会话。
std::shared_ptr<BeatmapSession> SessionRegistry::activeSession() const
{
    /// @brief 保护本次活跃 Session 读取的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    /// @brief 当前活跃 Session 索引快照。
    const int32_t index = activeIndex();
    if ( !isValidIndexUnsafe(index) ) {
        return nullptr;
    }
    return m_entries[index].session;
}

/// @brief 获取当前活跃画布的 cameraId。
std::string SessionRegistry::activeCameraId() const
{
    /// @brief 保护本次活跃 cameraId 读取的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    /// @brief 当前活跃 Session 索引快照。
    const int32_t index = activeIndex();
    if ( !isValidIndexUnsafe(index) ) {
        return "";
    }
    return m_entries[index].cameraId;
}

/// @brief 获取当前所有有效 Session 指针快照。
std::vector<std::shared_ptr<BeatmapSession>>
SessionRegistry::sessionSnapshot() const
{
    /// @brief 保护本次 Session 指针快照读取的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    /// @brief 当前非空 Session 指针快照。
    std::vector<std::shared_ptr<BeatmapSession>> sessions;
    sessions.reserve(m_entries.size());
    for ( const auto& entry : m_entries ) {
        if ( entry.session ) {
            sessions.push_back(entry.session);
        }
    }
    return sessions;
}

/// @brief 查找第一个 Logo 占位 Session。
int32_t SessionRegistry::findLogoPlaceholder() const
{
    /// @brief 保护本次 Logo 占位 Session 查找的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    for ( int32_t index = 0; index < static_cast<int32_t>(m_entries.size());
          ++index ) {
        if ( m_entries[index].isLogoPlaceholder ) {
            return index;
        }
    }
    return -1;
}

/// @brief 判断是否存在已打开谱面的非 Logo 占位 Session。
bool SessionRegistry::hasNonLogoSession() const
{
    /// @brief 保护本次非占位 Session 查询的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return std::any_of(
        m_entries.begin(), m_entries.end(), [](const SessionEntry& entry) {
            return !entry.isLogoPlaceholder;
        });
}

/// @brief 添加 Session 条目并将其设为活跃项。
int32_t SessionRegistry::append(SessionEntry entry)
{
    /// @brief 保护本次 Session 添加的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_entries.push_back(std::move(entry));

    /// @brief 新添加 Session 的列表索引。
    const int32_t newIndex = static_cast<int32_t>(m_entries.size()) - 1;
    setActiveIndex(newIndex);
    return newIndex;
}

/// @brief 移除指定索引的 Session 并修正活跃索引。
std::string SessionRegistry::erase(int32_t index)
{
    /// @brief 保护本次 Session 移除的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if ( !isValidIndexUnsafe(index) ) {
        return "";
    }

    /// @brief 被移除 Session 的 cameraId 快照。
    std::string cameraId = m_entries[index].cameraId;
    m_entries.erase(m_entries.begin() + index);
    normalizeActiveIndexAfterErase(index);
    return cameraId;
}

/// @brief 获取可变 SessionEntry 列表，调用者必须已持有 mutex()。
std::vector<SessionEntry>& SessionRegistry::entriesUnsafe()
{
    return m_entries;
}

/// @brief 获取只读 SessionEntry 列表，调用者必须已持有 mutex()。
const std::vector<SessionEntry>& SessionRegistry::entriesUnsafe() const
{
    return m_entries;
}

/// @brief 在调用者已持锁时判断索引是否有效。
bool SessionRegistry::isValidIndexUnsafe(int32_t index) const
{
    return index >= 0 && index < static_cast<int32_t>(m_entries.size());
}

/// @brief 在移除 Session 后修正当前活跃索引。
void SessionRegistry::normalizeActiveIndexAfterErase(int32_t erasedIndex)
{
    /// @brief 移除前记录的当前活跃索引。
    const int32_t currentActive = activeIndex();
    if ( m_entries.empty() ) {
        setActiveIndex(-1);
    } else if ( currentActive >= static_cast<int32_t>(m_entries.size()) ) {
        setActiveIndex(static_cast<int32_t>(m_entries.size()) - 1);
    } else if ( currentActive == erasedIndex ) {
        /// @brief 关闭活跃 Session 后应切换到的候选索引。
        int32_t newActive = std::max(0, erasedIndex - 1);
        if ( newActive >= static_cast<int32_t>(m_entries.size()) ) {
            newActive = static_cast<int32_t>(m_entries.size()) - 1;
        }
        setActiveIndex(newActive);
    } else if ( currentActive > erasedIndex ) {
        setActiveIndex(currentActive - 1);
    }
}

}  // namespace MMM::Logic
