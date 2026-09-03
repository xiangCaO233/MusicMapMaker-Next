#include "logic/SessionRegistry.h"
#include <algorithm>
#include <charconv>
#include <string_view>
#include <utility>

namespace MMM::Logic
{

namespace
{
/// @brief 解析 Canvas_N 形式的画布 ID。
/// @param cameraId 待解析的画布 ID。
/// @param canvasId 解析成功时写入的数字部分。
/// @return 解析是否成功。
bool parseCanvasCameraId(const std::string& cameraId, int32_t& canvasId)
{
    static constexpr std::string_view PREFIX = "Canvas_";
    if ( cameraId.rfind(PREFIX.data(), 0) != 0 ) {
        return false;
    }

    const char* first = cameraId.data() + PREFIX.size();
    const char* last  = cameraId.data() + cameraId.size();
    if ( first == last ) {
        return false;
    }

    int32_t parsed = 0;
    auto    result = std::from_chars(first, last, parsed);
    if ( result.ec != std::errc{} || result.ptr != last || parsed < 0 ) {
        return false;
    }

    canvasId = parsed;
    return true;
}
}  // namespace

/// @brief 构造空会话注册表并发布初始空快照。
SessionRegistry::SessionRegistry()
{
    /// @brief 初始化发布快照的短临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    publishSnapshotUnsafe();
}

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

/// @brief 保留指定画布 ID，避免后续自动分配重复编号。
void SessionRegistry::reserveCameraId(const std::string& cameraId)
{
    /// @brief 保护本次画布编号保留的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    int32_t canvasId = 0;
    if ( !parseCanvasCameraId(cameraId, canvasId) ) {
        return;
    }
    m_nextCanvasId = std::max(m_nextCanvasId, canvasId + 1);
}

/// @brief 获取当前活跃 Session 索引。
/// @warning 逻辑/UI 热路径原子：只读取活跃索引脏状态，使用 relaxed。
int32_t SessionRegistry::activeIndex() const
{
    return m_activeIndex.load(std::memory_order_relaxed);
}

/// @brief 设置当前活跃 Session 索引。
/// @warning 逻辑/UI 热路径原子：只写入活跃索引脏状态，使用 relaxed。
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

/// @brief 获取当前活跃的非 Logo 谱面会话。
/// @warning UI 低频绑定路径：复制一个 shared_ptr 以保证锁外调用期间生命周期。
std::shared_ptr<BeatmapSession> SessionRegistry::activeNonLogoSession() const
{
    /// @brief 保护本次非 Logo 活跃 Session 读取的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    /// @brief 当前活跃 Session 索引快照。
    const int32_t index = activeIndex();
    if ( !isValidIndexUnsafe(index) || m_entries[index].isLogoPlaceholder ) {
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
/// @warning 逻辑热路径/共享指针：shared_ptr 拷贝用于延长会话生命周期，避免锁外
/// update 时被 UI 线程关闭释放。
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

/// @brief 获取当前所有有效 Session 指针快照，并保留注册表索引。
/// @warning 逻辑热路径/共享指针：shared_ptr 拷贝用于延长会话生命周期，index
/// 用于锁外匹配当前活跃 Session。
std::vector<SessionSnapshotEntry>
SessionRegistry::indexedSessionSnapshot() const
{
    /// @brief 当前非空 Session 指针与索引快照。
    std::vector<SessionSnapshotEntry> sessions;
    fillIndexedSessionSnapshot(sessions);
    return sessions;
}

/// @brief 填充当前所有有效 Session 指针快照，并保留注册表索引。
/// @warning 逻辑热路径/共享指针：复用调用方 vector 容量，shared_ptr
/// 拷贝用于延长会话生命周期。
void SessionRegistry::fillIndexedSessionSnapshot(
    std::vector<SessionSnapshotEntry>& sessions) const
{
    /// @brief 保护本次带索引 Session 指针快照读取的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    sessions.clear();
    sessions.reserve(m_entries.size());
    for ( int32_t index = 0; index < static_cast<int32_t>(m_entries.size());
          ++index ) {
        const auto& entry = m_entries[static_cast<size_t>(index)];
        if ( entry.session ) {
            sessions.push_back({ index,
                                 entry.session,
                                 entry.isCanvasVisible,
                                 entry.audioTimelineFingerprint,
                                 entry.mainAudioSyncFingerprint,
                                 entry.isLogoPlaceholder });
        }
    }
}

/// @brief 获取当前发布给逻辑线程的不可变 Session 快照。
/// @warning 逻辑热路径原子：只做 acquire shared_ptr 读取，不获取注册表锁。
std::shared_ptr<const PublishedSessionSnapshot>
SessionRegistry::publishedSnapshot() const
{
    auto snapshot = m_publishedSnapshot.load(std::memory_order_acquire);
    if ( snapshot ) {
        return snapshot;
    }

    /// @brief 极早期访问时使用的空快照兜底。
    static const auto emptySnapshot =
        std::make_shared<const PublishedSessionSnapshot>();
    return emptySnapshot;
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
    publishSnapshotUnsafe();
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
    publishSnapshotUnsafe();
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

/// @brief 将当前 SessionEntry 列表发布为新的逻辑线程只读快照。
void SessionRegistry::publishSnapshotUnsafe()
{
    auto snapshot = std::make_shared<PublishedSessionSnapshot>();
    snapshot->sessions.reserve(m_entries.size());
    for ( int32_t index = 0; index < static_cast<int32_t>(m_entries.size());
          ++index ) {
        const auto& entry = m_entries[static_cast<size_t>(index)];
        if ( entry.session ) {
            snapshot->sessions.push_back({ index,
                                           entry.session,
                                           entry.isCanvasVisible,
                                           entry.audioTimelineFingerprint,
                                           entry.mainAudioSyncFingerprint,
                                           entry.isLogoPlaceholder });
        }
    }

    m_publishedSnapshot.store(
        std::shared_ptr<const PublishedSessionSnapshot>(std::move(snapshot)),
        std::memory_order_release);
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
