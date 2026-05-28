#pragma once

#include "logic/BeatmapSyncBuffer.h"
#include <cstdint>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MMM::Logic
{

/// @brief 渲染同步注册表，封装画布同步缓冲区、图集 UV 映射和视口尺寸缓存。
class RenderSyncRegistry
{
public:
    /// @brief 构造空渲染同步注册表。
    RenderSyncRegistry() = default;

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
    void setAtlasUVMap(const std::string&                             cameraId,
                       const std::unordered_map<uint32_t, glm::vec4>& uvMap);

    /// @brief 获取指定画布的图集 UV 映射，缺失时回退到 Basic2DCanvas。
    /// @param cameraId 目标画布 cameraId。
    /// @return 当前可用的图集 UV 映射引用。
    const std::unordered_map<uint32_t, glm::vec4>& getAtlasUVMap(
        const std::string& cameraId) const;

    /// @brief 缓存指定画布的最后已知视口尺寸。
    /// @param cameraId 目标画布 cameraId。
    /// @param size 视口尺寸。
    void cacheViewportSize(const std::string& cameraId, glm::vec2 size);

    /// @brief 获取 Preview 和 Timeline 等共享视口尺寸快照。
    /// @return 共享视口 cameraId 与尺寸列表。
    std::vector<std::pair<std::string, glm::vec2>>
    getSharedViewportSizes() const;

    /// @brief 移除指定画布的同步缓存、图集映射和视口尺寸。
    /// @param cameraId 待移除的画布 cameraId。
    void eraseCamera(const std::string& cameraId);

private:
    /// @brief 判断画布是否为需要同步给新 Session 的共享视口。
    /// @param cameraId 待检查的画布 cameraId。
    /// @return 是否为共享视口。
    bool isSharedViewport(const std::string& cameraId) const;

    /// @brief 所有的同步缓冲区，键为 CameraID。
    std::unordered_map<std::string, std::shared_ptr<BeatmapSyncBuffer>>
        m_syncBuffers;

    /// @brief 各摄像机独立的图集 UV 映射表。
    std::unordered_map<std::string, std::unordered_map<uint32_t, glm::vec4>>
        m_cameraUVMaps;

    /// @brief 缓存各摄像机的最后已知视口尺寸。
    std::unordered_map<std::string, glm::vec2> m_lastViewportSizes;

    /// @brief 保护同步缓冲区、图集 UV 映射和视口尺寸缓存的共享锁。
    mutable std::shared_mutex m_mutex;
};

}  // namespace MMM::Logic
