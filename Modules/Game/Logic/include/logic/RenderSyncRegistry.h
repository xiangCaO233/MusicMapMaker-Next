#pragma once

#include "common/AsciiFontData.h"
#include "common/UnicodeFontData.h"
#include <atomic>
#include <cstdint>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace MMM::Logic
{

class BeatmapSyncBuffer;

/// @brief 渲染同步注册表，封装画布同步缓冲区、图集 UV 映射和视口尺寸缓存。
class RenderSyncRegistry
{
public:
    /// @brief 构造空渲染同步注册表。
    RenderSyncRegistry();

    /// @brief 析构渲染同步注册表。
    ~RenderSyncRegistry() = default;

    /// @brief 禁止拷贝构造，避免复制同步缓冲区和共享锁。
    RenderSyncRegistry(const RenderSyncRegistry&) = delete;

    /// @brief 禁止拷贝赋值，避免复制同步缓冲区和共享锁。
    RenderSyncRegistry& operator=(const RenderSyncRegistry&) = delete;

    /// @brief 禁止移动构造，保持注册表地址稳定。
    RenderSyncRegistry(RenderSyncRegistry&&) = delete;

    /// @brief 禁止移动赋值，保持注册表地址稳定。
    RenderSyncRegistry& operator=(RenderSyncRegistry&&) = delete;

    /// @brief 获取或创建指定画布的同步缓冲区。
    /// @param cameraId 目标画布 cameraId。
    /// @return 指定画布对应的 BeatmapSyncBuffer。
    /// @warning 逻辑热路径/共享指针：逻辑线程发布快照时会复制
    /// shared_ptr，以保证 UI
    /// 关闭画布并擦除注册表时缓冲区不会在本次发布中析构；若要改为引用，必须先实现延迟销毁。
    std::shared_ptr<BeatmapSyncBuffer> getSyncBuffer(
        const std::string& cameraId);

    /// @brief 设置指定画布的图集 UV 映射。
    /// @param cameraId 目标画布 cameraId。
    /// @param uvMap 图集纹理 ID 到 UV 矩形的映射表。
    /// @param asciiFontAtlasMetrics 当前画布多档 ASCII 字体度量。
    /// @param unicodeFontMetrics 当前画布按需 Unicode 字体度量。
    void setAtlasUVMap(
        const std::string&                             cameraId,
        const std::unordered_map<uint32_t, glm::vec4>& uvMap,
        const Common::AsciiFontAtlasMetrics& asciiFontAtlasMetrics = {},
        const Common::UnicodeFontMetrics&    unicodeFontMetrics    = {});

    /// @brief 获取指定画布的图集 UV 映射，缺失时回退到 Basic2DCanvas。
    /// @param cameraId 目标画布 cameraId。
    /// @return 当前可用的图集 UV 映射共享读取句柄。
    /// @warning 逻辑交互路径使用；acquire 读取会产生一次 shared_ptr
    /// 引用计数变更，用于保证 UI 线程替换图集快照时返回的 UV 表不悬空。
    std::shared_ptr<const std::unordered_map<uint32_t, glm::vec4>>
    getAtlasUVMap(const std::string& cameraId) const;

    /// @brief 按修订号将指定画布的图集 UV 映射同步到快照。
    /// @param cameraId 目标画布 cameraId。
    /// @param target 目标快照中的 UV 映射表。
    /// @param targetRevision 目标快照当前持有的 UV 修订号。
    /// @param targetAsciiFontAtlasMetrics 目标快照中的多档 ASCII 字体度量。
    /// @param targetUnicodeFontMetrics 目标快照中的按需 Unicode 字体度量。
    /// @warning
    /// 逻辑/渲染热路径：每个快照生成时调用；普通路径只做原子快照读取和
    /// 修订号比较，只有图集修订号变化时才复制 unordered_map。
    void updateSnapshotAtlasUVMap(
        const std::string&                       cameraId,
        std::unordered_map<uint32_t, glm::vec4>& target,
        std::uint64_t&                           targetRevision,
        Common::AsciiFontAtlasMetrics&           targetAsciiFontAtlasMetrics,
        Common::UnicodeFontMetrics& targetUnicodeFontMetrics) const;

    /// @brief 缓存指定画布的最后已知视口尺寸。
    /// @param cameraId 目标画布 cameraId。
    /// @param size 视口尺寸。
    void cacheViewportSize(const std::string& cameraId, glm::vec2 size);

    /// @brief 获取指定画布的最后已知视口尺寸。
    /// @param cameraId 目标画布 cameraId。
    /// @return 已缓存的视口尺寸；未缓存时返回空值。
    std::optional<glm::vec2> getViewportSize(const std::string& cameraId) const;

    /// @brief 获取 Preview 和 Timeline 等共享视口尺寸快照。
    /// @return 共享视口 cameraId 与尺寸列表。
    std::vector<std::pair<std::string, glm::vec2>>
    getSharedViewportSizes() const;

    /// @brief 移除指定画布的同步缓存、图集映射和视口尺寸。
    /// @param cameraId 待移除的画布 cameraId。
    void eraseCamera(const std::string& cameraId);

private:
    /// @brief 单个画布图集 UV 映射及其修订号。
    struct AtlasUVMapState {
        std::unordered_map<uint32_t, glm::vec4> uvMap;  ///< 图集 UV 映射。
        /// @brief 当前图集包含的多档 ASCII 字体度量。
        Common::AsciiFontAtlasMetrics asciiFontAtlasMetrics;
        /// @brief 当前图集包含的按需 Unicode 字体度量。
        Common::UnicodeFontMetrics unicodeFontMetrics;
        std::uint64_t              revision{ 0 };  ///< 当前图集修订号。
    };

    /// @brief 发布给逻辑热路径的不可变图集 UV 快照。
    struct PublishedAtlasUVSnapshot {
        /// @brief 各画布图集 UV 映射及其修订号。
        std::unordered_map<std::string, AtlasUVMapState> cameraUVMaps;
    };

    /// @brief 在发布快照中查找画布图集，缺失时回退到 Basic2DCanvas。
    /// @param snapshot 已发布图集快照。
    /// @param cameraId 目标画布 cameraId。
    /// @return 找到的图集状态；没有可用图集时返回 nullptr。
    const AtlasUVMapState* findAtlasUVMapStateInSnapshot(
        const PublishedAtlasUVSnapshot& snapshot,
        const std::string&              cameraId) const;

    /// @brief 将当前图集 UV 映射发布为新的逻辑线程只读快照。
    /// @warning 调用者必须持有
    /// m_mutex；低频图集变更路径使用。旧快照仅保留到最后一个并发读句柄释放。
    void publishAtlasUVSnapshotUnsafe();

    /// @brief 判断画布是否为需要同步给新 Session 的共享视口。
    /// @param cameraId 待检查的画布 cameraId。
    /// @return 是否为共享视口。
    bool isSharedViewport(const std::string& cameraId) const;

    /// @brief 所有的同步缓冲区，键为 CameraID。
    std::unordered_map<std::string, std::shared_ptr<BeatmapSyncBuffer>>
        m_syncBuffers;

    /// @brief 各摄像机独立的图集 UV 映射表。
    std::unordered_map<std::string, AtlasUVMapState> m_cameraUVMaps;

    /// @brief 全局图集 UV 修订号计数器，确保不同 camera 的修订号也不会撞号。
    std::uint64_t m_nextAtlasUvRevision{ 1 };

    /// @brief 缓存各摄像机的最后已知视口尺寸。
    std::unordered_map<std::string, glm::vec2> m_lastViewportSizes;

    /// @brief 保护同步缓冲区、图集 UV 映射和视口尺寸缓存的共享锁。
    mutable std::shared_mutex m_mutex;

    /// @brief 逻辑线程当前可读取的不可变图集 UV 快照。
    /// @warning 逻辑热路径原子：每个快照生成时 acquire 读取；写侧在持有
    /// m_mutex 后 release 发布新快照。shared_ptr
    /// 所有权用于解决读写并发时的快照生命周期。
    std::shared_ptr<const PublishedAtlasUVSnapshot> m_publishedAtlasUVSnapshot;
};

}  // namespace MMM::Logic
