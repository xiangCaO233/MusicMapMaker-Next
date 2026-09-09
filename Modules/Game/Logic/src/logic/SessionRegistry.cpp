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
    // 必须消费整个后缀，拒绝 Canvas_1_extra 等仅数字前缀可解析的名称。
    auto result = std::from_chars(first, last, parsed);
    if ( result.ec != std::errc{} || result.ptr != last || parsed < 0 ) {
        return false;
    }

    // 失败时不修改输出，调用方不会拿到部分解析的编号。
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

/// @brief 暴露递归锁供多步注册表操作共用一个临界区。
/// @return 注册表拥有的锁，调用方不得在注册表析构后继续持有。
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
    // 已关闭画布的编号不回收，避免新窗口复用旧窗口的停靠身份。
    return "Canvas_" + std::to_string(canvasId);
}

/// @brief 保留指定画布 ID，避免后续自动分配重复编号。
void SessionRegistry::reserveCameraId(const std::string& cameraId)
{
    /// @brief 保护本次画布编号保留的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    int32_t canvasId = 0;
    if ( !parseCanvasCameraId(cameraId, canvasId) ) {
        // 自定义名称不参与 Canvas_N 编号空间，保留计数器现值。
        return;
    }
    // 工作区可能乱序恢复，计数器只前进，不因恢复较小编号而倒退。
    m_nextCanvasId = std::max(m_nextCanvasId, canvasId + 1);
}

/// @brief 获取当前活跃 Session 索引。
/// @warning 逻辑/UI 热路径原子：只读取活跃索引脏状态，使用 relaxed。
int32_t SessionRegistry::activeIndex() const
{
    // 索引不是会话内容的发布屏障，解引用仍需锁或快照提供生命周期。
    return m_activeIndex.load(std::memory_order_relaxed);
}

/// @brief 设置当前活跃 Session 索引。
/// @warning 逻辑/UI 热路径原子：只写入活跃索引脏状态，使用 relaxed。
void SessionRegistry::setActiveIndex(int32_t index)
{
    // 聚焦请求只更新选中状态，不复制或发布整个会话列表。
    // 有效范围由消费方按所使用的列表检查，允许 -1 表示未选中。
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
    // 内部锁仅保护本次查找；若要继续借用指针，外层必须保持递归锁。
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
    // const 不阻止另一线程调整 vector，调用方仍需维持借用期间的结构稳定。
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
    // 返回副本可在锁外使用，字符串及共享引用的复制成本留在此入口。
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
    // 在锁内取得拥有句柄，退出临界区后即使关闭标签，会话仍存活。
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
        // 占位项有画布身份但没有已打开的谱面，不能用于业务观察者绑定。
        return nullptr;
    }
    return m_entries[index].session;
}

/// @brief 获取当前活跃画布的 cameraId。
std::string SessionRegistry::activeCameraId() const
{
    // 返回字符串副本，避免外层窗口代码借用可被删除的条目字段。
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
    // 仅收集非空会话，结果中的下标不再与注册表下标一一对应。
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
    // 与无索引版本不同，此结构可在过滤空项后仍定位原注册表位置。
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
    // 复用外层 vector 容量；条目中的字符串与共享引用仍按快照重新复制。
    sessions.reserve(m_entries.size());
    for ( int32_t index = 0; index < static_cast<int32_t>(m_entries.size());
          ++index ) {
        const auto& entry = m_entries[static_cast<size_t>(index)];
        if ( entry.session ) {
            sessions.push_back({ index,
                                 // 保存原索引，不能用过滤后的输出长度替代。
                                 entry.session,
                                 entry.isCanvasVisible,
                                 entry.audioTimelineFingerprint,
                                 entry.mainAudioSyncFingerprint,
                                 entry.isLogoPlaceholder });
        }
    }
}

/// @brief 获取当前发布给逻辑线程的不可变 Session 快照。
/// @return 拥有型读取句柄，调用方应持有到本轮所有会话访问结束。
/// @warning 逻辑热路径原子：只做 acquire shared_ptr 读取，不获取注册表锁。
std::shared_ptr<const PublishedSessionSnapshot>
SessionRegistry::publishedSnapshot() const
{
    // acquire 对应写侧 release，使同一个快照的会话和元数据完整可见。
    // 读取句柄持有旧快照，写侧替换不会让本轮访问中的会话悬空。
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
    // 多项都被标成占位时按注册顺序返回首项，不改变当前选中状态。
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
    // 这里查询条目角色，不能代替对具体 session 指针的有效性检查。
    /// @brief 保护本次非占位 Session 查询的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return std::any_of(
        m_entries.begin(), m_entries.end(), [](const SessionEntry& entry) {
            return !entry.isLogoPlaceholder;
        });
}

/// @brief 添加 Session 条目并将其设为活跃项。
/// @param entry 会话与画布元数据，函数取得此值对象的内容。
/// @return 插入后的注册表索引，后续删除其他项可能使该位置变化。
int32_t SessionRegistry::append(SessionEntry entry)
{
    // 结构修改、活跃索引更新和快照生成共用临界区，发布的是完整新列表。
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
/// @param index 当前注册表中的位置，不能直接沿用旧快照中的过期索引。
/// @return 已删除条目的画布 ID，非法索引返回空串且不发布快照。
std::string SessionRegistry::erase(int32_t index)
{
    /// @brief 保护本次 Session 移除的临界区。
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if ( !isValidIndexUnsafe(index) ) {
        return "";
    }

    /// @brief 被移除 Session 的 cameraId 快照。
    std::string cameraId = m_entries[index].cameraId;
    // cameraId 已复制，可在条目销毁后交给窗口层清理对应画布。
    m_entries.erase(m_entries.begin() + index);
    // 删除使后续索引左移，必须先修正选中位置再生成新的索引快照。
    normalizeActiveIndexAfterErase(index);
    publishSnapshotUnsafe();
    return cameraId;
}

/// @brief 获取可变 SessionEntry 列表，调用者必须已持有 mutex()。
std::vector<SessionEntry>& SessionRegistry::entriesUnsafe()
{
    // 用于调用方已持锁的批量修改；不隐式发布，便于合并多项变更。
    return m_entries;
}

/// @brief 获取只读 SessionEntry 列表，调用者必须已持有 mutex()。
const std::vector<SessionEntry>& SessionRegistry::entriesUnsafe() const
{
    return m_entries;
}

/// @brief 将当前 SessionEntry 列表发布为新的逻辑线程只读快照。
/// @warning 低频结构或可见性变更路径，调用方必须持锁；此处会分配和复制。
void SessionRegistry::publishSnapshotUnsafe()
{
    // 先完整构造候选，再原子替换，读者不会看到填充到一半的 vector。
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

    // 快照自身只读，但所持会话仍按各自线程协议更新，不是深拷贝谱面。
    // 旧快照由并发读句柄保活，最后一个拥有者释放后才销毁。
    m_publishedSnapshot.store(
        std::shared_ptr<const PublishedSessionSnapshot>(std::move(snapshot)),
        std::memory_order_release);
}

/// @brief 在调用者已持锁时判断索引是否有效。
bool SessionRegistry::isValidIndexUnsafe(int32_t index) const
{
    // 先排除负索引，-1 的未选中状态不能转成无符号下标访问容器。
    return index >= 0 && index < static_cast<int32_t>(m_entries.size());
}

/// @brief 在移除 Session 后修正当前活跃索引。
/// @param erasedIndex 删除前的位置；调用方已持有锁并完成容器删除。
void SessionRegistry::normalizeActiveIndexAfterErase(int32_t erasedIndex)
{
    // 此时列表已完成删除，currentActive 仍是删除前的选中位置。
    /// @brief 移除前记录的当前活跃索引。
    const int32_t currentActive = activeIndex();
    if ( m_entries.empty() ) {
        // 最后一个条目关闭后不保留指向零号位置的虚假选中状态。
        setActiveIndex(-1);
    } else if ( currentActive >= static_cast<int32_t>(m_entries.size()) ) {
        // 先处理原末项选中越界，直接落到删除后仍存在的最后一项。
        setActiveIndex(static_cast<int32_t>(m_entries.size()) - 1);
    } else if ( currentActive == erasedIndex ) {
        // 关闭当前项时优先向左选邻居，关闭首项则仍选零号位置。
        /// @brief 关闭活跃 Session 后应切换到的候选索引。
        int32_t newActive = std::max(0, erasedIndex - 1);
        if ( newActive >= static_cast<int32_t>(m_entries.size()) ) {
            newActive = static_cast<int32_t>(m_entries.size()) - 1;
        }
        setActiveIndex(newActive);
    } else if ( currentActive > erasedIndex ) {
        // 关闭选中项之前的条目时只平移索引，保持同一个会话被选中。
        setActiveIndex(currentActive - 1);
    }
}

}  // namespace MMM::Logic
