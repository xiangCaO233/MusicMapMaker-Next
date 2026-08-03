#include "logic/RenderSyncRegistry.h"
#include "logic/BeatmapSyncBuffer.h"
#include <mutex>

namespace MMM::Logic
{

namespace
{
/// @brief 判断两个图集 UV 映射是否完全一致。
/// @param lhs 左侧图集 UV 映射。
/// @param rhs 右侧图集 UV 映射。
/// @return 两个映射的键和值均一致时返回 true。
/// @warning 渲染/UI 高频路径：setAtlasUVMap 可能每帧调用；只做小型
/// unordered_map 比较，避免无变化时发布新快照和递增修订号。
bool atlasUVMapsEqual(const std::unordered_map<uint32_t, glm::vec4>& lhs,
                      const std::unordered_map<uint32_t, glm::vec4>& rhs)
{
    if ( lhs.size() != rhs.size() ) {
        return false;
    }

    for ( const auto& [textureId, uv] : lhs ) {
        auto it = rhs.find(textureId);
        if ( it == rhs.end() ) {
            return false;
        }
        const auto& other = it->second;
        if ( uv.x != other.x || uv.y != other.y || uv.z != other.z ||
             uv.w != other.w ) {
            return false;
        }
    }

    return true;
}

/// @brief 判断两套 ASCII 字体度量是否完全一致。
/// @param lhs 左侧字体度量。
/// @param rhs 右侧字体度量。
/// @return 全部标量与字形度量一致时返回 true。
bool asciiFontMetricsEqual(const Common::AsciiFontMetrics& lhs,
                           const Common::AsciiFontMetrics& rhs)
{
    if ( lhs.valid != rhs.valid || lhs.ascender != rhs.ascender ||
         lhs.lineHeight != rhs.lineHeight ) {
        return false;
    }
    for ( std::size_t i = 0; i < lhs.glyphs.size(); ++i ) {
        const auto& left  = lhs.glyphs[i];
        const auto& right = rhs.glyphs[i];
        if ( left.available != right.available ||
             left.hasBitmap != right.hasBitmap || left.width != right.width ||
             left.height != right.height || left.bearingX != right.bearingX ||
             left.bearingY != right.bearingY ||
             left.advanceX != right.advanceX ) {
            return false;
        }
    }
    return true;
}

/// @brief 判断两套多档 ASCII 字体度量是否完全一致。
/// @param lhs 左侧多档字体度量。
/// @param rhs 右侧多档字体度量。
/// @return 全部字号档位均一致时返回 true。
bool asciiFontAtlasMetricsEqual(const Common::AsciiFontAtlasMetrics& lhs,
                                const Common::AsciiFontAtlasMetrics& rhs)
{
    if ( lhs.valid != rhs.valid || lhs.rasterScale != rhs.rasterScale ) {
        return false;
    }
    for ( std::size_t tierIndex = 0U; tierIndex < lhs.tiers.size();
          ++tierIndex ) {
        if ( !asciiFontMetricsEqual(lhs.tiers[tierIndex],
                                    rhs.tiers[tierIndex]) ) {
            return false;
        }
    }
    return true;
}

/// @brief 判断两套按需 Unicode 字体度量是否完全一致。
/// @param lhs 左侧 Unicode 字体度量。
/// @param rhs 右侧 Unicode 字体度量。
/// @return 字体标量与全部码点字形度量一致时返回 true。
bool unicodeFontMetricsEqual(const Common::UnicodeFontMetrics& lhs,
                             const Common::UnicodeFontMetrics& rhs)
{
    if ( lhs.valid != rhs.valid || lhs.ascender != rhs.ascender ||
         lhs.lineHeight != rhs.lineHeight ||
         lhs.glyphs.size() != rhs.glyphs.size() ) {
        return false;
    }
    for ( std::size_t index = 0U; index < lhs.glyphs.size(); ++index ) {
        const auto& left        = lhs.glyphs[index];
        const auto& right       = rhs.glyphs[index];
        const auto& leftMetric  = left.metrics;
        const auto& rightMetric = right.metrics;
        if ( left.codepoint != right.codepoint ||
             leftMetric.available != rightMetric.available ||
             leftMetric.hasBitmap != rightMetric.hasBitmap ||
             leftMetric.width != rightMetric.width ||
             leftMetric.height != rightMetric.height ||
             leftMetric.bearingX != rightMetric.bearingX ||
             leftMetric.bearingY != rightMetric.bearingY ||
             leftMetric.advanceX != rightMetric.advanceX ) {
            return false;
        }
    }
    return true;
}
}  // namespace

/// @brief 构造空渲染同步注册表并发布初始空图集快照。
RenderSyncRegistry::RenderSyncRegistry()
{
    /// @brief 初始化图集发布快照的短临界区。
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    publishAtlasUVSnapshotUnsafe();
}

/// @brief 获取或创建指定画布的同步缓冲区。
/// @warning 逻辑热路径/共享指针：shared_ptr 拷贝用于跨 UI
/// 关闭路径延长缓冲区生命周期，避免锁外发布快照时悬垂。
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
    const std::unordered_map<uint32_t, glm::vec4>& uvMap,
    const Common::AsciiFontAtlasMetrics&           asciiFontAtlasMetrics,
    const Common::UnicodeFontMetrics&              unicodeFontMetrics)
{
    /// @brief 保护本次图集 UV 映射写入的临界区。
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto&                               state = m_cameraUVMaps[cameraId];
    if ( atlasUVMapsEqual(state.uvMap, uvMap) &&
         asciiFontAtlasMetricsEqual(state.asciiFontAtlasMetrics,
                                    asciiFontAtlasMetrics) &&
         unicodeFontMetricsEqual(state.unicodeFontMetrics,
                                 unicodeFontMetrics) ) {
        return;
    }

    state.uvMap                 = uvMap;
    state.asciiFontAtlasMetrics = asciiFontAtlasMetrics;
    state.unicodeFontMetrics    = unicodeFontMetrics;
    state.revision              = m_nextAtlasUvRevision++;
    if ( state.revision == 0 ) {
        state.revision = m_nextAtlasUvRevision++;
    }
    if ( m_nextAtlasUvRevision == 0 ) {
        m_nextAtlasUvRevision = 1;
    }
    publishAtlasUVSnapshotUnsafe();
}

/// @brief 获取指定画布的图集 UV 映射，缺失时回退到 Basic2DCanvas。
std::shared_ptr<const std::unordered_map<uint32_t, glm::vec4>>
RenderSyncRegistry::getAtlasUVMap(const std::string& cameraId) const
{
    auto snapshot = std::atomic_load_explicit(&m_publishedAtlasUVSnapshot,
                                              std::memory_order_acquire);
    if ( snapshot ) {
        if ( const auto* state =
                 findAtlasUVMapStateInSnapshot(*snapshot, cameraId) ) {
            return { snapshot, &state->uvMap };
        }
    }

    /// @brief 空 UV 映射回退值，用于没有任何可用图集时返回稳定引用。
    static const auto emptyMap =
        std::make_shared<const std::unordered_map<uint32_t, glm::vec4>>();
    return emptyMap;
}

/// @brief 按修订号将指定画布的图集 UV 映射同步到快照。
/// @warning 逻辑/渲染热路径：每个快照生成时调用；普通路径不复制 UV 表。
void RenderSyncRegistry::updateSnapshotAtlasUVMap(
    const std::string&                       cameraId,
    std::unordered_map<uint32_t, glm::vec4>& target,
    std::uint64_t&                           targetRevision,
    Common::AsciiFontAtlasMetrics&           targetAsciiFontAtlasMetrics,
    Common::UnicodeFontMetrics&              targetUnicodeFontMetrics) const
{
    const auto snapshot = std::atomic_load_explicit(&m_publishedAtlasUVSnapshot,
                                                    std::memory_order_acquire);
    const auto* state =
        snapshot ? findAtlasUVMapStateInSnapshot(*snapshot, cameraId) : nullptr;
    if ( !state ) {
        if ( targetRevision != 0 || !target.empty() ||
             targetAsciiFontAtlasMetrics.valid ||
             targetUnicodeFontMetrics.valid ) {
            target.clear();
            targetRevision              = 0;
            targetAsciiFontAtlasMetrics = {};
            targetUnicodeFontMetrics    = {};
        }
        return;
    }

    if ( targetRevision == state->revision ) {
        return;
    }

    target                      = state->uvMap;
    targetRevision              = state->revision;
    targetAsciiFontAtlasMetrics = state->asciiFontAtlasMetrics;
    targetUnicodeFontMetrics    = state->unicodeFontMetrics;
}

/// @brief 缓存指定画布的最后已知视口尺寸。
void RenderSyncRegistry::cacheViewportSize(const std::string& cameraId,
                                           glm::vec2          size)
{
    /// @brief 保护本次视口尺寸缓存写入的临界区。
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_lastViewportSizes[cameraId] = size;
}

/// @brief 获取指定画布的最后已知视口尺寸。
std::optional<glm::vec2> RenderSyncRegistry::getViewportSize(
    const std::string& cameraId) const
{
    /// @brief 保护本次视口尺寸缓存读取的临界区。
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    /// @brief 指定画布视口尺寸缓存的迭代器。
    auto it = m_lastViewportSizes.find(cameraId);
    if ( it == m_lastViewportSizes.end() ) {
        return std::nullopt;
    }
    return it->second;
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
    publishAtlasUVSnapshotUnsafe();
}

/// @brief 在发布快照中查找画布图集，缺失时回退到 Basic2DCanvas。
const RenderSyncRegistry::AtlasUVMapState*
RenderSyncRegistry::findAtlasUVMapStateInSnapshot(
    const PublishedAtlasUVSnapshot& snapshot, const std::string& cameraId) const
{
    auto it = snapshot.cameraUVMaps.find(cameraId);
    if ( it != snapshot.cameraUVMaps.end() ) {
        return &it->second;
    }

    if ( cameraId != "Basic2DCanvas" ) {
        auto itMain = snapshot.cameraUVMaps.find("Basic2DCanvas");
        if ( itMain != snapshot.cameraUVMaps.end() ) {
            return &itMain->second;
        }
    }

    return nullptr;
}

/// @brief 将当前图集 UV 映射发布为新的逻辑线程只读快照。
void RenderSyncRegistry::publishAtlasUVSnapshotUnsafe()
{
    auto snapshot          = std::make_shared<PublishedAtlasUVSnapshot>();
    snapshot->cameraUVMaps = m_cameraUVMaps;

    std::atomic_store_explicit(
        &m_publishedAtlasUVSnapshot,
        std::shared_ptr<const PublishedAtlasUVSnapshot>(std::move(snapshot)),
        std::memory_order_release);
}

/// @brief 判断画布是否为需要同步给新 Session 的共享视口。
bool RenderSyncRegistry::isSharedViewport(const std::string& cameraId) const
{
    return cameraId == "Preview" || cameraId == "Timeline";
}

}  // namespace MMM::Logic
