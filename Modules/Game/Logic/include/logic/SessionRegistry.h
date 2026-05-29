#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace MMM::Logic
{

class BeatmapSession;

/// @brief 多画布 Session 条目，绑定 Session 与其对应画布的 cameraId。
struct SessionEntry {
    /// @brief 逻辑会话。
    std::shared_ptr<BeatmapSession> session;

    /// @brief 该画布对应的唯一 cameraId，例如 "Canvas_0"。
    std::string cameraId;

    /// @brief 显示名称，例如谱面名或默认标签。
    std::string displayName;

    /// @brief 是否为初始 Logo 占位画布，尚未加载谱面时为 true。
    bool isLogoPlaceholder{ false };

    /// @brief 是否应使用项目工作区 ini 中保存的停靠状态。
    bool restoreDockFromWorkspace{ false };
};

/// @brief 编辑器多画布会话注册表，封装 Session 列表、活跃索引和 cameraId 分配。
class SessionRegistry
{
public:
    /// @brief 构造空会话注册表。
    SessionRegistry() = default;

    /// @brief 析构会话注册表。
    ~SessionRegistry() = default;

    /// @brief 禁止拷贝构造，避免复制会话容器和递归锁。
    SessionRegistry(const SessionRegistry&) = delete;

    /// @brief 禁止拷贝赋值，避免复制会话容器和递归锁。
    SessionRegistry& operator=(const SessionRegistry&) = delete;

    /// @brief 禁止移动构造，保持会话注册表地址稳定。
    SessionRegistry(SessionRegistry&&) = delete;

    /// @brief 禁止移动赋值，保持会话注册表地址稳定。
    SessionRegistry& operator=(SessionRegistry&&) = delete;

    /// @brief 获取保护会话列表的递归锁。
    /// @return 会话注册表递归锁。
    std::recursive_mutex& mutex() const;

    /// @brief 生成下一个唯一画布 cameraId。
    /// @return 新生成的 cameraId。
    std::string createNextCameraId();

    /// @brief 保留指定画布 ID，避免后续自动分配重复编号。
    /// @param cameraId 已恢复或外部分配的画布 ID。
    void reserveCameraId(const std::string& cameraId);

    /// @brief 获取当前活跃 Session 索引。
    /// @return 当前活跃 Session 索引，-1 表示没有活跃 Session。
    /// @warning 逻辑/UI 热路径原子：只读取活跃索引脏状态，使用 relaxed。
    int32_t activeIndex() const;

    /// @brief 设置当前活跃 Session 索引。
    /// @param index 目标活跃 Session 索引。
    /// @warning 逻辑/UI 热路径原子：只写入活跃索引脏状态，使用 relaxed。
    void setActiveIndex(int32_t index);

    /// @brief 获取 Session 总数。
    /// @return 当前注册的 Session 数量。
    int32_t count() const;

    /// @brief 判断索引是否指向有效 Session。
    /// @param index 待检查的 Session 索引。
    /// @return 索引是否在当前 Session 列表范围内。
    bool isValidIndex(int32_t index) const;

    /// @brief 获取指定索引的 SessionEntry。
    /// @param index 目标 Session 索引。
    /// @return 指定索引的 SessionEntry；索引无效时返回 nullptr。
    SessionEntry* entry(int32_t index);

    /// @brief 获取指定索引的只读 SessionEntry。
    /// @param index 目标 Session 索引。
    /// @return 指定索引的只读 SessionEntry；索引无效时返回 nullptr。
    const SessionEntry* entry(int32_t index) const;

    /// @brief 获取所有 Session 条目的只读快照。
    /// @return 当前 SessionEntry 列表副本。
    std::vector<SessionEntry> entries() const;

    /// @brief 获取当前活跃谱面会话。
    /// @return 当前活跃 BeatmapSession；没有活跃会话时返回 nullptr。
    std::shared_ptr<BeatmapSession> activeSession() const;

    /// @brief 获取当前活跃画布的 cameraId。
    /// @return 当前活跃画布 cameraId；没有活跃画布时返回空字符串。
    std::string activeCameraId() const;

    /// @brief 获取当前所有有效 Session 指针快照。
    /// @return 当前注册的非空 BeatmapSession 指针列表。
    /// @warning 逻辑热路径/共享指针：逻辑循环每次 update 前调用；shared_ptr
    /// 拷贝用于保证会话在锁外更新期间不被 UI
    /// 关闭销毁，不能替换为裸引用，除非先引入延迟销毁队列。
    std::vector<std::shared_ptr<BeatmapSession>> sessionSnapshot() const;

    /// @brief 查找第一个 Logo 占位 Session。
    /// @return Logo 占位 Session 索引；不存在时返回 -1。
    int32_t findLogoPlaceholder() const;

    /// @brief 判断是否存在已打开谱面的非 Logo 占位 Session。
    /// @return 是否存在非 Logo 占位 Session。
    bool hasNonLogoSession() const;

    /// @brief 添加 Session 条目并将其设为活跃项。
    /// @param entry 待添加的 Session 条目。
    /// @return 新 Session 在列表中的索引。
    int32_t append(SessionEntry entry);

    /// @brief 移除指定索引的 Session 并修正活跃索引。
    /// @param index 待移除的 Session 索引。
    /// @return 被移除 Session 的 cameraId；索引无效时返回空字符串。
    std::string erase(int32_t index);

    /// @brief 获取可变 SessionEntry 列表，调用者必须已持有 mutex()。
    /// @return 内部 SessionEntry 列表引用。
    std::vector<SessionEntry>& entriesUnsafe();

    /// @brief 获取只读 SessionEntry 列表，调用者必须已持有 mutex()。
    /// @return 内部 SessionEntry 列表只读引用。
    const std::vector<SessionEntry>& entriesUnsafe() const;

private:
    /// @brief 在调用者已持锁时判断索引是否有效。
    /// @param index 待检查的 Session 索引。
    /// @return 索引是否在内部 Session 列表范围内。
    bool isValidIndexUnsafe(int32_t index) const;

    /// @brief 在移除 Session 后修正当前活跃索引。
    /// @param erasedIndex 刚刚被移除的 Session 索引。
    void normalizeActiveIndexAfterErase(int32_t erasedIndex);

    /// @brief 所有打开的画布 Session 列表。
    std::vector<SessionEntry> m_entries;

    /// @brief 当前活跃画布 Session 索引，-1 表示没有活跃 Session。
    /// @warning 逻辑/UI 热路径原子：UI
    /// 聚焦和逻辑路由都会读取；只保存索引脏状态，使用 relaxed 访问。
    std::atomic<int32_t> m_activeIndex{ -1 };

    /// @brief 全局递增的画布 ID 计数器，用于生成唯一 cameraId。
    int32_t m_nextCanvasId{ 0 };

    /// @brief 保护会话列表和活跃索引相关复合操作的递归锁。
    mutable std::recursive_mutex m_mutex;
};

}  // namespace MMM::Logic
