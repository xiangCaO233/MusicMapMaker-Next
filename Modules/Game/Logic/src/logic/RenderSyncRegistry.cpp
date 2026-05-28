#include "logic/RenderSyncRegistry.h"
#include <mutex>

namespace MMM::Logic
{

/// @brief 获取或创建指定画布的同步缓冲区。
std::shared_ptr<BeatmapSyncBuffer> RenderSyncRegistry::getSyncBuffer(
    const std::string& cameraId)
{
    {
        /// @brief 保护本次同步缓冲区只读查找的临界区。
        std::shared_lock<std::shared_mutex> lock(m_mutex);

        /// @brief 已存在同步缓冲区的迭代器。
        auto it = m_syncBuffers.find(cameraId);
        if ( it != m_syncBuffers.end() ) {
            return it->second;
        }
    }

    /// @brief 保护本次同步缓冲区创建的临界区。
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if ( m_syncBuffers.find(cameraId) == m_syncBuffers.end() ) {
        m_syncBuffers[cameraId] = std::make_shared<BeatmapSyncBuffer>();
    }
    return m_syncBuffers[cameraId];
}

/// @brief 设置指定画布的图集 UV 映射。
void RenderSyncRegistry::setAtlasUVMap(
    const std::string&                             cameraId,
    const std::unordered_map<uint32_t, glm::vec4>& uvMap)
{
    /// @brief 保护本次图集 UV 映射写入的临界区。
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_cameraUVMaps[cameraId] = uvMap;
}

/// @brief 获取指定画布的图集 UV 映射，缺失时回退到 Basic2DCanvas。
const std::unordered_map<uint32_t, glm::vec4>&
RenderSyncRegistry::getAtlasUVMap(const std::string& cameraId) const
{
    /// @brief 保护本次图集 UV 映射读取的临界区。
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    /// @brief 指定画布图集 UV 映射的迭代器。
    auto it = m_cameraUVMaps.find(cameraId);
    if ( it != m_cameraUVMaps.end() ) {
        return it->second;
    }

    // 回退到默认图集 (Basic2DCanvas)
    if ( cameraId != "Basic2DCanvas" ) {
        /// @brief 默认 Basic2DCanvas 图集 UV 映射的迭代器。
        auto itMain = m_cameraUVMaps.find("Basic2DCanvas");
        if ( itMain != m_cameraUVMaps.end() ) {
            return itMain->second;
        }
    }

    /// @brief 空 UV 映射回退值，用于没有任何可用图集时返回稳定引用。
    static const std::unordered_map<uint32_t, glm::vec4> emptyMap;
    return emptyMap;
}

/// @brief 缓存指定画布的最后已知视口尺寸。
void RenderSyncRegistry::cacheViewportSize(const std::string& cameraId,
                                           glm::vec2          size)
{
    /// @brief 保护本次视口尺寸缓存写入的临界区。
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_lastViewportSizes[cameraId] = size;
}

/// @brief 获取 Preview 和 Timeline 等共享视口尺寸快照。
std::vector<std::pair<std::string, glm::vec2>>
RenderSyncRegistry::getSharedViewportSizes() const
{
    /// @brief 保护本次共享视口尺寸快照读取的临界区。
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    /// @brief 当前共享视口尺寸快照。
    std::vector<std::pair<std::string, glm::vec2>> viewportSizes;
    viewportSizes.reserve(m_lastViewportSizes.size());
    for ( const auto& [cameraId, size] : m_lastViewportSizes ) {
        if ( isSharedViewport(cameraId) ) {
            viewportSizes.emplace_back(cameraId, size);
        }
    }
    return viewportSizes;
}

/// @brief 移除指定画布的同步缓存、图集映射和视口尺寸。
void RenderSyncRegistry::eraseCamera(const std::string& cameraId)
{
    /// @brief 保护本次画布渲染同步状态移除的临界区。
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_syncBuffers.erase(cameraId);
    m_cameraUVMaps.erase(cameraId);
    m_lastViewportSizes.erase(cameraId);
}

/// @brief 判断画布是否为需要同步给新 Session 的共享视口。
bool RenderSyncRegistry::isSharedViewport(const std::string& cameraId) const
{
    return cameraId == "Preview" || cameraId == "Timeline";
}

}  // namespace MMM::Logic
