#include "logic/RenderSyncRegistry.h"
#include "logic/BeatmapSyncBuffer.h"
#include <mutex>

namespace MMM::Logic
{

namespace
{
/// @brief 判断两个图集 UV 映射是否完全一致。
/// 相等判定采用精确值比较，重复提交同一图集无需浮点容差。
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
        // 无序表的桶顺序没有含义，按纹理 ID 查询而不是逐迭代器比较。
        auto it = rhs.find(textureId);
        if ( it == rhs.end() ) {
            return false;
        }
        const auto& other = it->second;
        // 比较完整矩形；只改变 UV 起点或宽高都要求重新发布图集。
        if ( uv.x != other.x || uv.y != other.y || uv.z != other.z ||
             uv.w != other.w ) {
            return false;
        }
    }

    return true;
}

/// @brief 判断两套 ASCII 字体度量是否完全一致。
/// 有效标记参与比较，尚未准备的字体不能沿用已有排版缓存。
/// @param lhs 左侧字体度量。
/// @param rhs 右侧字体度量。
/// @return 全部标量与字形度量一致时返回 true。
/// @warning 图集提交路径调用，按固定字形表逐项比较，不分配临时容器。
bool asciiFontMetricsEqual(const Common::AsciiFontMetrics& lhs,
                           const Common::AsciiFontMetrics& rhs)
{
    if ( lhs.valid != rhs.valid || lhs.ascender != rhs.ascender ||
         lhs.lineHeight != rhs.lineHeight ) {
        return false;
    }
    for ( std::size_t i = 0; i < lhs.glyphs.size(); ++i ) {
        // ASCII 数组以字符槽定位；空字形也比较可用性和前进量。
        // 没有位图的空格仍影响排版，不能仅比较可绘制字形的矩形。
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
/// rasterScale 的变化会影响字号档位选择，即使字形表相同也需要更新。
/// @warning 图集提交路径调用，只比较已有值，不在这里重建字体。
bool asciiFontAtlasMetricsEqual(const Common::AsciiFontAtlasMetrics& lhs,
                                const Common::AsciiFontAtlasMetrics& rhs)
{
    if ( lhs.valid != rhs.valid || lhs.rasterScale != rhs.rasterScale ) {
        return false;
    }
    for ( std::size_t tierIndex = 0U; tierIndex < lhs.tiers.size();
          ++tierIndex ) {
        // 按档位成对比较，防止某一档缺失被其他可用字号掩盖。
        if ( !asciiFontMetricsEqual(lhs.tiers[tierIndex],
                                    rhs.tiers[tierIndex]) ) {
            return false;
        }
    }
    return true;
}

/// @brief 判断两套按需 Unicode 字体度量是否完全一致。
/// 字形表来自同一次字体准备流程，此处不合并码点或补建缺失字形。
/// @param lhs 左侧 Unicode 字体度量。
/// @param rhs 右侧 Unicode 字体度量。
/// @return 字体标量与全部码点字形度量一致时返回 true。
/// @warning 图集提交路径调用，顺序遍历按需字形列表，不做额外排序。
bool unicodeFontMetricsEqual(const Common::UnicodeFontMetrics& lhs,
                             const Common::UnicodeFontMetrics& rhs)
{
    if ( lhs.valid != rhs.valid || lhs.ascender != rhs.ascender ||
         lhs.lineHeight != rhs.lineHeight ||
         lhs.glyphs.size() != rhs.glyphs.size() ) {
        return false;
    }
    for ( std::size_t index = 0U; index < lhs.glyphs.size(); ++index ) {
        // 列表顺序也是快照内容的一部分，码点重排同样触发新修订。
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
    // 在对象可供外部读取前发布空表，正常读取无需等待首次图集提交。
    /// @brief 初始化图集发布快照的短临界区。
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    publishAtlasUVSnapshotUnsafe();
}

/// @brief 获取或创建指定画布的同步缓冲区。
/// @param cameraId 稳定画布身份，同一键复用同一个缓冲对象。
/// @return 拥有型句柄，调用方可在释放注册表锁后继续发布数据。
/// @warning 逻辑热路径/共享指针：shared_ptr 拷贝用于跨 UI
/// 关闭路径延长缓冲区生命周期，避免锁外发布快照时悬垂。
std::shared_ptr<BeatmapSyncBuffer> RenderSyncRegistry::getSyncBuffer(
    const std::string& cameraId)
{
    {
        // 已有缓冲只走共享锁；锁内复制拥有句柄后再交给锁外调用者。
        // 不持锁执行缓冲内部的发布与读取，注册表仅负责按名称取得对象。
        /// @brief 保护本次同步缓冲区只读查找的临界区。
        std::shared_lock<std::shared_mutex> lock(m_mutex);

        /// @brief 已存在同步缓冲区的迭代器。
        auto it = m_syncBuffers.find(cameraId);
        if ( it != m_syncBuffers.end() ) {
            return it->second;
        }
    }

    /// @brief 保护本次同步缓冲区创建的临界区。
    // 共享锁到独占锁之间存在空隙，必须复查以免覆盖另一调用者刚创建的缓冲。
    // 必须先离开共享临界区再申请独占锁，不能把 shared_mutex 当作递归锁升级。
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if ( m_syncBuffers.find(cameraId) == m_syncBuffers.end() ) {
        m_syncBuffers[cameraId] = std::make_shared<BeatmapSyncBuffer>();
    }
    return m_syncBuffers[cameraId];
}

/// @brief 设置指定画布的图集 UV 映射。
/// @param cameraId 图集所属画布，其他画布的记录保持不变。
/// @param uvMap 当前纹理矩形表，提交后不借用调用方容器。
/// @param asciiFontAtlasMetrics 与此 UV 表配套的 ASCII 字号档位。
/// @param unicodeFontMetrics 与此 UV 表配套的按需 Unicode 字形度量。
/// @warning 图集提交可能逐帧发生；内容不变时不生成快照，变化时复制数据。
void RenderSyncRegistry::setAtlasUVMap(
    const std::string&                             cameraId,
    const std::unordered_map<uint32_t, glm::vec4>& uvMap,
    const Common::AsciiFontAtlasMetrics&           asciiFontAtlasMetrics,
    const Common::UnicodeFontMetrics&              unicodeFontMetrics)
{
    /// @brief 保护本次图集 UV 映射写入的临界区。
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto&                               state = m_cameraUVMaps[cameraId];
    // 三部分共同决定排版：UV 未变不代表字形间距或字号缩放未变。
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
    // 同一全局计数器服务所有画布，回退来源切换不需要额外记录来源名称。
    // 零号专门表示无图集；计数回绕时跳过零，不与清空状态混淆。
    if ( state.revision == 0 ) {
        state.revision = m_nextAtlasUvRevision++;
    }
    if ( m_nextAtlasUvRevision == 0 ) {
        m_nextAtlasUvRevision = 1;
    }
    publishAtlasUVSnapshotUnsafe();
}

/// @brief 获取指定画布的图集 UV 映射，缺失时回退到 Basic2DCanvas。
/// @param cameraId 优先查询的画布，只有条目缺失才尝试公共主画布。
/// @return 保活完整快照的只读 UV 表句柄，未找到时返回共享空表。
/// @warning 逻辑查询路径进行 acquire 与共享引用复制，不新增图集复制或文件访问。
std::shared_ptr<const std::unordered_map<uint32_t, glm::vec4>>
RenderSyncRegistry::getAtlasUVMap(const std::string& cameraId) const
{
    auto snapshot = m_publishedAtlasUVSnapshot.load(std::memory_order_acquire);
    if ( snapshot ) {
        if ( const auto* state =
                 findAtlasUVMapStateInSnapshot(*snapshot, cameraId) ) {
            // 别名 shared_ptr 指向子表，但持有父快照，替换图集不会悬空。
            return { snapshot, &state->uvMap };
        }
    }

    /// @brief 空 UV 映射回退值，用于没有任何可用图集时返回稳定引用。
    static const auto emptyMap =
        std::make_shared<const std::unordered_map<uint32_t, glm::vec4>>();
    return emptyMap;
}

/// @brief 按修订号将指定画布的图集 UV 映射同步到快照。
/// @param cameraId 本次画布，回退表也使用其实际修订号。
/// @param target 接收 UV 表的既有缓存。
/// @param targetRevision 与目标数据匹配的修订号，零表示空状态。
/// @param targetAsciiFontAtlasMetrics 接收同一次发布的 ASCII 度量。
/// @param targetUnicodeFontMetrics 接收同一次发布的 Unicode 度量。
/// @warning 逻辑/渲染热路径：每个快照生成时调用；普通路径不复制 UV 表。
void RenderSyncRegistry::updateSnapshotAtlasUVMap(
    const std::string&                       cameraId,
    std::unordered_map<uint32_t, glm::vec4>& target,
    std::uint64_t&                           targetRevision,
    Common::AsciiFontAtlasMetrics&           targetAsciiFontAtlasMetrics,
    Common::UnicodeFontMetrics&              targetUnicodeFontMetrics) const
{
    const auto snapshot =
        m_publishedAtlasUVSnapshot.load(std::memory_order_acquire);
    const auto* state =
        snapshot ? findAtlasUVMapStateInSnapshot(*snapshot, cameraId) : nullptr;
    if ( !state ) {
        // 图集被删除且没有主画布回退时，清理旧 UV 和度量，避免继续画旧字形。
        // 清空也会归零修订号，下次图集到达时必然进入数据复制分支。
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
        // 修订号全局分配，切换到另一画布或回退图集也能识别缓存变化。
        return;
    }

    // 同一拥有句柄贯穿全部复制，UV 与字体度量不会混入下一次发布的数据。
    target                      = state->uvMap;
    targetRevision              = state->revision;
    targetAsciiFontAtlasMetrics = state->asciiFontAtlasMetrics;
    targetUnicodeFontMetrics    = state->unicodeFontMetrics;
}

/// @brief 缓存指定画布的最后已知视口尺寸。
/// 更新尺寸不递增图集修订号，两类缓存有独立的变化来源。
/// @param cameraId 视口身份，不要求对应会话已经创建。
/// @param size 调用方提供的尺寸，隐藏画布仍可沿用最后值。
void RenderSyncRegistry::cacheViewportSize(const std::string& cameraId,
                                           glm::vec2          size)
{
    /// @brief 保护本次视口尺寸缓存写入的临界区。
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_lastViewportSizes[cameraId] = size;
}

/// @brief 获取指定画布的最后已知视口尺寸。
/// 只读查询不插入缺失键，避免未显示画布产生伪造的缓存记录。
/// @return 值副本；未上报与已上报零尺寸通过 optional 区分。
std::optional<glm::vec2> RenderSyncRegistry::getViewportSize(
    const std::string& cameraId) const
{
    /// @brief 保护本次视口尺寸缓存读取的临界区。
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    /// @brief 指定画布视口尺寸缓存的迭代器。
    auto it = m_lastViewportSizes.find(cameraId);
    if ( it == m_lastViewportSizes.end() ) {
        // 不虚构默认尺寸，新会话由上层决定首次视野的初始化方式。
        return std::nullopt;
    }
    return it->second;
}

/// @brief 获取 Preview 和 Timeline 等共享视口尺寸快照。
/// @return 独立值列表，遍历次序不表示窗口展示顺序。
std::vector<std::pair<std::string, glm::vec2>>
RenderSyncRegistry::getSharedViewportSizes() const
{
    /// @brief 保护本次共享视口尺寸快照读取的临界区。
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    /// @brief 当前共享视口尺寸快照。
    std::vector<std::pair<std::string, glm::vec2>> viewportSizes;
    viewportSizes.reserve(m_lastViewportSizes.size());
    for ( const auto& [cameraId, size] : m_lastViewportSizes ) {
        // 新会话只继承共享面板尺寸，主画布的独立尺寸不跨会话传播。
        if ( isSharedViewport(cameraId) ) {
            viewportSizes.emplace_back(cameraId, size);
        }
    }
    return viewportSizes;
}

/// @brief 移除指定画布的同步缓存、图集映射和视口尺寸。
/// 此处只撤销注册表持有的引用，不取消已经开始的缓冲发布操作。
/// @param cameraId 要从后续查询中移除的画布身份，允许重复清理。
void RenderSyncRegistry::eraseCamera(const std::string& cameraId)
{
    /// @brief 保护本次画布渲染同步状态移除的临界区。
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_syncBuffers.erase(cameraId);
    m_cameraUVMaps.erase(cameraId);
    m_lastViewportSizes.erase(cameraId);
    // 发布剔除后的图集；在途读者保留旧句柄，后续查询改用新快照或回退。
    publishAtlasUVSnapshotUnsafe();
}

/// @brief 在发布快照中查找画布图集，缺失时回退到 Basic2DCanvas。
/// 不访问可变图集表，查找与回退始终在同一发布版本中完成。
/// @return 快照内借用地址，仅在调用方持有该快照期间有效。
const RenderSyncRegistry::AtlasUVMapState*
RenderSyncRegistry::findAtlasUVMapStateInSnapshot(
    const PublishedAtlasUVSnapshot& snapshot, const std::string& cameraId) const
{
    auto it = snapshot.cameraUVMaps.find(cameraId);
    if ( it != snapshot.cameraUVMaps.end() ) {
        // 显式存在的空表也是画布自己的状态，不因其为空而换成其他图集。
        return &it->second;
    }

    if ( cameraId != "Basic2DCanvas" ) {
        // 回退只指向固定主画布，不从无序表任意挑选另一份图集。
        auto itMain = snapshot.cameraUVMaps.find("Basic2DCanvas");
        if ( itMain != snapshot.cameraUVMaps.end() ) {
            return &itMain->second;
        }
    }

    return nullptr;
}

/// @brief 将当前图集 UV 映射发布为新的逻辑线程只读快照。
/// 原子句柄只同步快照可见性；可变源表的并发修改仍由独占锁保护。
/// @warning 调用方持独占锁；此路径复制各画布图集，不在普通读取时执行。
void RenderSyncRegistry::publishAtlasUVSnapshotUnsafe()
{
    auto snapshot          = std::make_shared<PublishedAtlasUVSnapshot>();
    snapshot->cameraUVMaps = m_cameraUVMaps;

    // 构造完成后 release 发布，逻辑侧 acquire 读取可见完整的配套度量。
    // 不维护退休列表，最后一个读取句柄释放后自然回收旧快照。
    m_publishedAtlasUVSnapshot.store(
        std::shared_ptr<const PublishedAtlasUVSnapshot>(std::move(snapshot)),
        std::memory_order_release);
}

/// @brief 判断画布是否为需要同步给新 Session 的共享视口。
/// @return 只有固定 Preview 与 Timeline 身份参与共享尺寸继承。
bool RenderSyncRegistry::isSharedViewport(const std::string& cameraId) const
{
    return cameraId == "Preview" || cameraId == "Timeline";
}

}  // namespace MMM::Logic
