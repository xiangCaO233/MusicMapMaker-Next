#include "config/skin/SkinConfig.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/AudioObjectLabelRenderer.h"
#include "logic/ecs/system/render/Batcher.h"
#include "logic/ecs/system/render/NoteLaneGeometry.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace MMM::Logic::System
{

/// @brief UI 侧亚帧补偿允许的最大滞后秒数，与 CanvasSnapshotPrepare 保持一致。
constexpr double MAX_UI_INTERPOLATION_SECONDS = 0.1;

/// @brief 音符可见性 AbsY 索引桶尺寸。
constexpr double NOTE_ABSY_BUCKET_SIZE = 2048.0;

/// @brief 草稿音符颜色键在热路径复用，避免逐帧构造长字符串。
const std::string DRAFT_NOTE_TAP_COLOR_KEY{ "draft_notes.note_tap" };
/// @brief 草稿 Hold 头部颜色键。
const std::string DRAFT_NOTE_HEAD_COLOR_KEY{ "draft_notes.note_head" };
/// @brief 草稿 Hold 主体颜色键。
const std::string DRAFT_NOTE_HOLD_COLOR_KEY{ "draft_notes.note_hold" };
/// @brief 草稿 Hold 尾部颜色键。
const std::string DRAFT_NOTE_END_COLOR_KEY{ "draft_notes.note_end" };
/// @brief 草稿 Polyline 节点颜色键。
const std::string DRAFT_NOTE_NODE_COLOR_KEY{ "draft_notes.note_node" };
/// @brief 草稿 Flick 箭头颜色键。
const std::string DRAFT_NOTE_ARROW_COLOR_KEY{ "draft_notes.note_flick_arrow" };

/// @brief 解析音符主体当前应使用的轨道左边界。
/// @param note 待解析音符。
/// @param laneProjection 主画布统一轨道投影；非主画布时为空。
/// @param fallbackLeftX 兼容玩家轨道区左边界。
/// @param singleTrackWidth 兼容玩家单轨宽度。
/// @return 音符所属草稿、玩家或 BGM 轨道的真实左边界。
/// @warning 渲染热路径：基础层、发光层与命中盒均会调用，只允许常量坐标换算。
/// @note 只返回横向左界，调用者仍需用同一领域的真实宽度计算居中偏移。
float resolveNoteTrackLeftX(const NoteComponent&        note,
                            const CanvasLaneProjection* laneProjection,
                            float fallbackLeftX, float singleTrackWidth)
{
    return resolveNoteLaneGeometry(note.m_trackIndex,
                                   laneProjection,
                                   fallbackLeftX,
                                   singleTrackWidth)
        .leftX;
}

/// @brief 单个音符在 AbsY 空间覆盖的保守区间。
/// @note 区间存储未缩放的 AbsY，查询端必须解除动画缩放后才能比较。
/// @note 一个长物件可覆盖多个桶，entity 用于最后读取当前组件精查。
/// @note 这是空间包络，不能由区间端点直接推导唯一的最早或最晚时间。
struct NoteAbsYRangeEntry {
    /// @brief 音符实体。
    entt::entity entity{ entt::null };
    /// @brief 区间下界。
    double minAbsY{ 0.0 };
    /// @brief 区间上界。
    double maxAbsY{ 0.0 };
};

/// @brief 音符可见性 AbsY 分桶索引。
/// @note 此索引属于 Registry 上下文，不能在不同会话间按裸指针复用。
/// @note cache 与 sourceEntities 都是观察地址，不延长其所属对象生命周期。
/// @note 版本号检测内容变化，地址及数量检测来源替换，两者共同决定是否重建。
/// @note 桶中保存 entries 下标而非实体号，seenSerials 与 entries 长度保持一致。
/// @warning 逻辑线程串行查询和重建；querySerial 与去重数组不提供并发访问保证。
/// @note 对同一来源原地编辑时数量可能不变，调用方必须发布新的 noteRevision。
/// @note querySerial 回绕清零仅影响去重，不改变几何条目及其来源版本。
/// @note 桶数保护只限制空间跨度的桶分配，每条目的跨桶引用仍与覆盖长度有关。
struct NoteAbsYBucketIndex {
    /// @brief 建立索引时使用的 ScrollCache。
    const ScrollCache* cache{ nullptr };
    /// @brief 建立索引时使用的排序音符列表。
    const std::vector<entt::entity>* sourceEntities{ nullptr };
    /// @brief 建立索引时的排序音符数量。
    std::size_t sourceCount{ 0 };
    /// @brief 建立索引时的 ScrollCache 版本。
    std::uint64_t scrollRevision{ 0 };
    /// @brief 建立索引时的音符版本。
    std::uint64_t noteRevision{ 0 };
    /// @brief 桶起始 AbsY。
    double bucketOrigin{ 0.0 };
    /// @brief 可用的最小正 HS。
    double minHs{ 1.0 };
    /// @brief 可用的最大正 HS。
    double maxHs{ 1.0 };
    /// @brief 是否存在无法安全索引的 HS 数据。
    bool requiresFullExactScan{ false };
    /// @brief 音符 AbsY 区间条目。
    std::vector<NoteAbsYRangeEntry> entries;
    /// @brief AbsY 桶到 entries 下标的映射。
    std::vector<std::vector<std::uint32_t>> buckets;
    /// @brief 查询去重标记。
    std::vector<std::uint32_t> seenSerials;
    /// @brief 当前查询序号。
    std::uint32_t querySerial{ 0 };
};

/// @brief 计算单个音符在时间维度上的保守覆盖范围。
/// @param note 音符组件。
/// @return 音符及其子段可能覆盖的最小/最大时间。
/// @warning 索引重建路径：只读取 NoteComponent，不访问 ECS 或分配内存。
/// @note 不假定子段按时间排列，包络同时覆盖中途提前或延后的子项。
/// @note 返回值可能含非有限输入，调用者在分段查找前负责验证。
static std::pair<double, double> getNoteTimeRange(const NoteComponent& note);

/// @brief 枚举单个音符用于可见性判断的采样时间。
/// @param note 音符组件。
/// @param cache 当前 ScrollCache。
/// @param callback 接收采样时间的回调。
/// @warning 热路径候选精查：只遍历该音符时间范围内的 ScrollSegment
/// 边界，避免完整扫描所有 Note 或所有 Timing。
template<typename Callback>
static void forEachNoteVisibilitySampleTime(const NoteComponent& note,
                                            const ScrollCache*   cache,
                                            Callback&&           callback);

/// @brief 获取当前可视窗口附近的音符实体。
/// @param currentTime 当前快照的动画时间。
/// @param currentAbsY 当前快照动画时间对应的绝对 Y。
/// @param visualPaddingPixels 当前皮肤与缩放下的候选视觉余量。
/// @param interpolationSeconds UI 亚帧补偿需要覆盖的播放时间。
/// @warning 热路径：每次音符快照生成时执行；只能查询已构建的 AbsY
/// 分桶索引，不得完整遍历全量 Note，除非索引失效进入保守兜底。
static void collectNotesInRange(
    entt::registry& registry, const ScrollCache* cache, double currentTime,
    double currentAbsY, float judgmentLineY, float topY, float bottomY,
    float renderScaleY, float visualPaddingPixels, double interpolationSeconds,
    std::vector<entt::entity>& result, std::unordered_set<entt::entity>& seen);

/// @brief 估算 UI 亚帧补偿期间 ScrollCache 可能产生的最大 AbsY 位移。
/// @warning 热路径：每次音符候选反查前执行；只允许访问当前时间附近的
/// ScrollSegment，禁止完整遍历全部流速段。
/// @param cache 当前显示缓存，空指针返回零余量。
/// @param currentTime 补间窗口起点，调用方应提供有限时间。
/// @param interpolationSeconds 非负预测时长，异常值返回零。
/// @return 显示空间绝对路程，包含当前动画缩放。
static double calculateInterpolationPaddingAbsY(const ScrollCache* cache,
                                                double             currentTime,
                                                double interpolationSeconds);

/// @brief 获取或重建音符 AbsY 分桶索引。
/// @warning 逻辑热路径低频分支：仅在音符版本或 ScrollCache
/// 版本变化时完整扫描音符；生成快照热路径只查询桶。
static NoteAbsYBucketIndex& getOrBuildNoteAbsYBucketIndex(
    entt::registry& registry, const ScrollCache* cache,
    const std::vector<entt::entity>& entities, std::uint64_t noteRevision);

/// @brief 获取普通物件主体末端的 HS 锚点时间。
/// @warning 热路径：音符可见性和命中盒计算中调用；保持纯计算，不得分配。
/// @param note 当前主体物件，提供起点、类型和持续值。
/// @param cache 保留的缓存参数；当前锚点规则不读取该指针。
/// @return Hold 使用起点 HS，其余物件使用结束时间的 HS。
/// @note 返回的是显示倍率锚点，不是主体几何的结束时间。
static double getCarrierEndAnchorTime(const NoteComponent& note,
                                      const ScrollCache*   cache)
{
    (void)cache;
    // Hold 主体两端共用起点 HS，避免跨 HS 变化时长度与起点分离。
    // 终点实际时间仍由调用方传给 getDisplayDelta 的第一个参数。
    if ( note.m_type == ::MMM::NoteType::HOLD ) {
        return note.m_timestamp;
    }
    return note.m_timestamp + note.m_duration;
}

/// @brief 在草稿或玩家轨道物件锚点上方绘制绑定音效标签。
/// @warning
/// 主画布热路径：只处理调用方已剔除的物件锚点，不得访问文件系统或分配堆内存。
/// @param batcher 当前快照的标签批处理入口，调用期间有效。
/// @param cache 当前滚动缓存，必须非空。
/// @param currentAbsY 快照时间对应的显示锚点。
/// @param noteH 玩家域基准音符高度，随后按实际领域宽度调整。
/// @param binding 音效引用与实例音量，不触发资源解码。
/// @param timestamp 标签附着音符的秒时间。
/// @param trackIndex 统一轨号，允许负草稿编号。
/// @param judgmentLineY 当前视口判定线像素坐标。
/// @param leftX 兼容玩家域的左边界。
/// @param topY 可绘制区上边界。
/// @param bottomY 可绘制区下边界。
/// @param singleTrackW 兼容玩家域单轨宽度，必须非零。
/// @param renderScaleY 主视图或预览纵向缩放。
/// @param noteScaleY 标签布局所用音符纵向缩放。
/// @param color 标签颜色及透明度。
/// @param laneProjection 可选真实分区投影；为空时使用连续玩家域。
static void renderBoundSampleLabelAt(
    Batcher& batcher, const ScrollCache* cache, double currentAbsY, float noteH,
    const ::MMM::AudioSampleBinding& binding, double timestamp,
    int32_t trackIndex, float judgmentLineY, float leftX, float topY,
    float bottomY, float singleTrackW, float renderScaleY, float noteScaleY,
    glm::vec4 color, const CanvasLaneProjection* laneProjection)
{
    // 空引用没有可显示的资源身份，跳过标签排版而非生成空文本命令。
    // 实际资源名称解析由标签渲染入口处理，此处只传递绑定值。
    if ( binding.m_audioResourceId.empty() ) return;
    const auto lane = resolveNoteLaneGeometry(
        trackIndex, laneProjection, leftX, singleTrackW);
    // 基准高度由玩家单轨宽度推导，草稿或 BGM 的独立轨宽需同比缩放。
    // 这样标签与所在物件上边缘对齐，不借用另一领域的几何尺寸。
    const float laneScale   = lane.width / singleTrackW;
    const float scaledNoteH = noteH * laneScale;

    // 标签使用音符自身时间作为 HS 锚点，与主体点击中心的显示位置一致。
    // 不能只用未乘 HS 的绝对 Y 差代替显示差。
    const float screenY =
        judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                            timestamp, currentAbsY, timestamp)) *
                            renderScaleY;
    // 纵向判断包含物件半高，中心在视口外但边缘仍可见时保留标签。
    // 这里只做纵向粗裁剪，文本排版与水平约束由标签入口负责。
    if ( screenY + scaledNoteH * 0.5F < topY ||
         screenY - scaledNoteH * 0.5F > bottomY ) {
        return;
    }

    // 标签锚点取物件上边缘，宽度取实际轨宽。
    // 系统时间来自同一快照，避免同帧不同标签各自读取时钟产生滚动差异。
    renderAudioObjectLabel(batcher,
                           binding.m_audioResourceId,
                           binding.m_volume,
                           lane.leftX,
                           screenY - scaledNoteH * 0.5F,
                           lane.width,
                           noteScaleY,
                           color,
                           batcher.snapshot->snapshotSysTime);
}

/// @brief 为当前画布生成音符候选、命中盒、基础层、发光层和顶层遮罩。
/// @param registry 提供音符组件、滚动缓存与可见性索引的会话注册表。
/// @param snapshot 本次写入的渲染快照，临时查询容器由其持有。
/// @param cameraId 决定交互及标签是否属于主画布的相机标识。
/// @param currentTime 本次快照动画时间。
/// @param judgmentLineY 判定线像素 Y。
/// @param trackCount 玩家轨道数。
/// @param config 本次一致读取的编辑器配置。
/// @param batcher 当前快照的主体批处理器。
/// @param leftX 兼容玩家域左边界。
/// @param clipLeftX 遮罩及发光层裁剪左界。
/// @param clipRightX 顶层遮罩裁剪右界。
/// @param rightX 兼容玩家域右边界。
/// @param topY 可视区域上界。
/// @param bottomY 可视区域下界。
/// @param singleTrackW 玩家单轨基准宽度。
/// @param renderScaleY 当前相机纵向压缩比例。
/// @param laneProjection 主画布真实分区几何；辅助视图可为空。
/// @warning
/// 每次音符快照生成调用；复用快照容器，禁止文件操作、阻塞同步或全量排序。
/// @note 可见性索引失效及保守兜底的扫描成本由候选查询入口单独约束。
/// @pre 输入几何已经配置为有效可视区域，后续命中和绘制按正纵向倍率解释边界。
/// @note 关闭交互只抑制命中盒生成，基础层和播放中的遮罩仍会更新。
/// @note 主画布标签开关在调用基础层前确定，辅助相机不会重复输出音效名称。
void NoteRenderSystem::renderNotes(
    entt::registry& registry, RenderSnapshot* snapshot,
    const std::string& cameraId, double currentTime, float judgmentLineY,
    int32_t trackCount, const Config::EditorConfig& config, Batcher& batcher,
    float leftX, float clipLeftX, float clipRightX, float rightX, float topY,
    float bottomY, float singleTrackW, float renderScaleY,
    const CanvasLaneProjection* laneProjection)
{
    // 1. 准备上下文与颜色
    NoteRenderSystem::NoteRenderContext ctx =
        NoteRenderSystem::prepareNoteRenderContext(
            registry, snapshot, currentTime, singleTrackW, config);
    if ( !ctx.cache ) return;

    // 复用当前写入快照的暂存容器，候选列表不拥有 ECS 组件。
    // 同一候选集合供后续各层使用，避免每层重复进行空间反查。
    auto& noteEntities = snapshot->noteQueryScratch;
    auto& noteSeen     = snapshot->noteQuerySeenScratch;
    // 视觉余量至少为一个像素，包含音符高度以及播放期间 UI 亚帧位移。
    // 播放倍率取绝对值，反向播放也需要非负的候选保护范围。
    collectNotesInRange(
        registry,
        ctx.cache,
        ctx.currentTime,
        ctx.currentAbsY,
        judgmentLineY,
        topY,
        bottomY,
        renderScaleY,
        std::max(ctx.noteH, 1.0f),
        snapshot->isPlaying
            ? std::abs(snapshot->playbackSpeed) * MAX_UI_INTERPOLATION_SECONDS
            : 0.0,
        noteEntities,
        noteSeen);

    // 非专业模式统一过滤草稿，所有后续主体、发光和命中层使用相同可见集合。
    // 不要只在绘制层隐藏草稿而留下可拾取的命中区域。
    if ( !config.settings.professionalMode ) {
        std::erase_if(noteEntities, [&registry](entt::entity entity) {
            const auto* note = registry.try_get<const NoteComponent>(entity);
            return note && note->m_isDraft;
        });
    }

    // 布局模式即使正在播放也需要逐物件边界，供 UI 直接调整渲染比例。
    const bool shouldGenerateHitboxes =
        (!snapshot->isPlaying || snapshot->currentTool == EditTool::Layout) &&
        snapshot->acceptsInteraction &&
        SessionUtils::isMainCanvasCameraId(cameraId);

    // 2. 生成碰撞盒并获取可见实体
    if ( shouldGenerateHitboxes ) {
        // 记录本次追加起点，只重标当前音符命中盒，不修改快照之前已有的其他领域条目。
        // 草稿与玩家共享组件 Registry，但命中种类必须保留正确交互路由。
        const std::size_t hitboxStart = snapshot->hitboxes.size();
        NoteRenderSystem::generateNoteHitboxes(registry,
                                               snapshot,
                                               ctx,
                                               noteEntities,
                                               judgmentLineY,
                                               leftX,
                                               topY,
                                               bottomY,
                                               singleTrackW,
                                               renderScaleY,
                                               config,
                                               laneProjection);
        for ( std::size_t index = hitboxStart;
              index < snapshot->hitboxes.size();
              ++index ) {
            auto&       hitbox = snapshot->hitboxes[index];
            const auto* note =
                registry.try_get<const NoteComponent>(hitbox.entity);
            if ( note && note->m_isDraft ) {
                hitbox.kind = ChartObjectKind::DraftNote;
            }
        }
    }

    // 3. 基础层渲染
    // 普通物件命中与折线局部编辑命中分开控制。
    // 关闭折线编辑时不生成可编辑局部部件，主画布基础交互仍按前面的门禁决定。
    const bool generatePolylineHitboxes =
        shouldGenerateHitboxes && config.settings.enablePolylineEditing;
    NoteRenderSystem::renderNoteBaseLayer(
        registry,
        snapshot,
        ctx,
        config,
        noteEntities,
        batcher,
        (float)currentTime,
        judgmentLineY,
        leftX,
        rightX,
        topY,
        bottomY,
        singleTrackW,
        renderScaleY,
        trackCount,
        generatePolylineHitboxes,
        config.visual.showBoundSampleLabels &&
            SessionUtils::isMainCanvasCameraId(cameraId),
        laneProjection);

    // 4. 发光层渲染
    NoteRenderSystem::renderNoteGlowLayer(registry,
                                          snapshot,
                                          ctx,
                                          config,
                                          noteEntities,
                                          (float)currentTime,
                                          judgmentLineY,
                                          leftX,
                                          clipLeftX,
                                          rightX,
                                          topY,
                                          bottomY,
                                          singleTrackW,
                                          renderScaleY,
                                          laneProjection);

    // 5. 笔刷预览渲染
    // 采样画笔由独立采样渲染路径负责，不能在这里重复绘制成 Note。
    // 普通画笔可没有对应 ECS 身份，使用快照保存的临时几何。
    if ( snapshot->brush.isActive && !snapshot->brush.createsAudioSample ) {
        NoteRenderSystem::renderBrushPreview(snapshot,
                                             ctx,
                                             config,
                                             batcher,
                                             judgmentLineY,
                                             leftX,
                                             singleTrackW,
                                             renderScaleY,
                                             laneProjection);
    }

    // 6. 顶层重叠遮罩；播放时同样生成，并随动态内容使用统一的 UI 补间偏移。
    NoteRenderSystem::renderOverlapMasks(registry,
                                         snapshot,
                                         ctx,
                                         config,
                                         noteEntities,
                                         judgmentLineY,
                                         leftX,
                                         clipLeftX,
                                         clipRightX,
                                         topY,
                                         bottomY,
                                         singleTrackW,
                                         renderScaleY,
                                         laneProjection);
}

/// @brief 读取当前快照的滚动锚点、基准尺寸和玩家及草稿颜色。
/// @param registry 提供借用滚动缓存的上下文。
/// @param snapshot 提供当前皮肤纹理 UV 的快照。
/// @param currentTime 几何和可见性共同使用的动画时间。
/// @param singleTrackW 玩家域基准宽度。
/// @param config 提供宽高缩放的当前配置。
/// @return 按值返回上下文；缺少缓存时 cache 为空，缺少纹理时提前返回部分状态。
/// @warning 快照热路径：只借用缓存及皮肤表，不复制共享所有权或加载资源。
/// @note 调用方必须保证 Note 纹理 UV 宽高有效，才能建立纵横比。
/// @note 颜色为本次调用的值副本，后续局部尺寸换算不会修改皮肤配置。
/// @note 缺少专用头尾键的回退规则与草稿逐槽回退分别执行。
NoteRenderSystem::NoteRenderContext NoteRenderSystem::prepareNoteRenderContext(
    entt::registry& registry, RenderSnapshot* snapshot, double currentTime,
    float singleTrackW, const Config::EditorConfig& config)
{
    NoteRenderSystem::NoteRenderContext ctx{};

    // 上下文存放缓存观察指针，先确认槽位及指针本身均存在。
    // 返回值不持有缓存所有权，须在会话缓存生命周期内消费。
    const auto** cachePtr = registry.ctx().find<const ScrollCache*>();
    if ( !cachePtr || !(*cachePtr) ) return ctx;
    ctx.cache       = *cachePtr;
    ctx.currentAbsY = ctx.cache->getVisualAnchorAbsY(currentTime);
    ctx.currentTime = currentTime;

    auto itBase = snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Note));
    if ( itBase == snapshot->uvMap.end() ) return ctx;
    // UV 范围宽高比决定基准音符高度，宽高缩放分别从配置读取。
    // 后续分区几何会在此基准上按各领域单轨宽度调整。
    ctx.baseAspect = itBase->second.z / itBase->second.w;

    ctx.noteW = singleTrackW * config.visual.noteScaleX;
    ctx.noteH = (singleTrackW / ctx.baseAspect) * config.visual.noteScaleY;

    auto& skin      = Config::SkinManager::instance();
    auto  color_tap = skin.getColor("note_tap");
    ctx.colorTap    = { color_tap.r, color_tap.g, color_tap.b, color_tap.a };

    // 头部与尾部支持独立颜色，旧皮肤缺少相应键时沿用 Hold 颜色。
    // 显式存在的透明色仍有效，不按颜色数值判断是否需要回退。
    auto color_head = skin.getData().colors.contains("note_head")
                          ? skin.getColor("note_head")
                          : skin.getColor("note_hold");
    ctx.colorHead = { color_head.r, color_head.g, color_head.b, color_head.a };

    auto color_hold = skin.getColor("note_hold");
    ctx.colorHold = { color_hold.r, color_hold.g, color_hold.b, color_hold.a };

    auto color_end = skin.getData().colors.contains("note_end")
                         ? skin.getColor("note_end")
                         : skin.getColor("note_hold");
    ctx.colorEnd   = { color_end.r, color_end.g, color_end.b, color_end.a };

    auto color_node = skin.getColor("note_node");
    ctx.colorNode = { color_node.r, color_node.g, color_node.b, color_node.a };

    auto color_arrow = skin.getColor("note_flick_arrow");
    ctx.colorArrow   = {
        color_arrow.r, color_arrow.g, color_arrow.b, color_arrow.a
    };

    // 草稿专用键缺失时逐部件回退对应玩家颜色，而非统一使用单一草稿色。
    // 字符串常量由文件作用域复用，不在每次查询构造长键。
    /// @brief 从已加载皮肤表读取草稿覆盖，缺键时使用对应玩家颜色。
    /// @param key 文件作用域复用的草稿颜色键。
    /// @param fallback 当前部件玩家颜色。
    /// @return 草稿覆盖或按值返回的回退色。
    /// @warning 快照热路径只查内存表，不读取皮肤文件。
    const auto draftColor = [&skin](const std::string& key,
                                    glm::vec4          fallback) {
        const auto& colors = skin.getData().colors;
        const auto  it     = colors.find(key);
        if ( it == colors.end() ) return fallback;
        return glm::vec4{
            it->second.r, it->second.g, it->second.b, it->second.a
        };
    };
    ctx.colorDraftTap  = draftColor(DRAFT_NOTE_TAP_COLOR_KEY, ctx.colorTap);
    ctx.colorDraftHead = draftColor(DRAFT_NOTE_HEAD_COLOR_KEY, ctx.colorHead);
    ctx.colorDraftHold = draftColor(DRAFT_NOTE_HOLD_COLOR_KEY, ctx.colorHold);
    ctx.colorDraftEnd  = draftColor(DRAFT_NOTE_END_COLOR_KEY, ctx.colorEnd);
    ctx.colorDraftNode = draftColor(DRAFT_NOTE_NODE_COLOR_KEY, ctx.colorNode);
    ctx.colorDraftArrow =
        draftColor(DRAFT_NOTE_ARROW_COLOR_KEY, ctx.colorArrow);

    return ctx;
}

/// @brief 汇总根及内嵌子项的时间包络，为流速边界采样限定范围。
/// @warning 重建与精查共用的局部计算，禁止扩展为全谱扫描。
static std::pair<double, double> getNoteTimeRange(const NoteComponent& note)
{
    // 负持续值按零处理，避免错误反向跨度扩展索引范围。
    // 折线随后以全部局部子项扩大包络，不假定最后子项时间最大。
    double minTime = note.m_timestamp;
    double maxTime = note.m_timestamp + std::max(0.0, note.m_duration);
    if ( minTime > maxTime ) {
        std::swap(minTime, maxTime);
    }

    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        // 子项可能未按时间排序，用最小和最大聚合保持保守覆盖。
        // 每个子项同时考虑起点和非负持续末端，普通点击自然形成零长度区间。
        for ( const auto& sub : note.m_subNotes ) {
            const double subStart = sub.timestamp;
            const double subEnd   = sub.timestamp + std::max(0.0, sub.duration);
            minTime = std::min(minTime, std::min(subStart, subEnd));
            maxTime = std::max(maxTime, std::max(subStart, subEnd));
        }
    }

    return { minTime, maxTime };
}

template<typename Callback>
/// @brief 对音符关键端点及覆盖时间内的流速分段边界执行采样回调。
/// @warning 候选精查热路径只遍历当前音符的局部时间区间。
/// @note 回调同步执行，可重复接收同一时间，不保存回调或时间列表。
static void forEachNoteVisibilitySampleTime(const NoteComponent& note,
                                            const ScrollCache*   cache,
                                            Callback&&           callback)
{
    // 非有限时间不能交给滚动计算或区间比较，统一在回调入口过滤。
    // 端点重复不影响调用者的极值聚合，无需分配额外去重集合。
    /// @brief 将有限秒时间同步交给调用方回调。
    /// @param time 组件端点或流速分段边界。
    /// @warning 可能在每次候选精查重复调用，不应在回调中增加分配或同步。
    auto emitFinite = [&](double time) {
        if ( std::isfinite(time) ) {
            callback(time);
        }
    };

    emitFinite(note.m_timestamp);
    emitFinite(note.m_timestamp + std::max(0.0, note.m_duration));
    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        for ( const auto& sub : note.m_subNotes ) {
            emitFinite(sub.timestamp);
            emitFinite(sub.timestamp + std::max(0.0, sub.duration));
        }
    }

    // 缺少缓存仍已采样组件端点，只省略无法获得的流速边界。
    // 调用者可据自己收到的有效样本决定是否保留结果。
    if ( !cache ) {
        return;
    }

    const auto [minTime, maxTime] = getNoteTimeRange(note);
    if ( !std::isfinite(minTime) || !std::isfinite(maxTime) ||
         minTime > maxTime ) {
        return;
    }

    const auto& segments = cache->getSegments();
    // 分段按时间排序，从物件最早时间二分定位，避免扫描之前所有流速事件。
    // 包络内的边界也要采样，负流速或变化点可能让极值出现在物件中途。
    auto it = std::lower_bound(segments.begin(),
                               segments.end(),
                               minTime,
                               [](const ScrollSegment& segment, double value) {
                                   return segment.time < value;
                               });
    for ( ; it != segments.end() && it->time <= maxTime; ++it ) {
        emitFinite(it->time);
    }
}

/// @brief 按未来补间时间窗口积分绝对滚动速度，得到保守位移余量。
/// @warning 每次候选反查调用，只推进补间窗口内的分段，不执行等待。
/// @note 累加路程而非净位移，窗口内速度反转也不会抵消需要保留的范围。
static double calculateInterpolationPaddingAbsY(const ScrollCache* cache,
                                                double             currentTime,
                                                double interpolationSeconds)
{
    // 无缓存、非正或非有限补间窗口不额外扩张候选范围。
    // 补间时长是几何预测范围，不表示阻塞逻辑线程等待这么久。
    if ( !cache || interpolationSeconds <= 0.0 ||
         !std::isfinite(interpolationSeconds) ) {
        return 0.0;
    }

    const auto& segments = cache->getSegments();
    if ( segments.empty() ) {
        return std::abs(cache->getSpeedAt(currentTime)) * interpolationSeconds;
    }

    const double endTime = currentTime + interpolationSeconds;
    // 定位当前时刻之后第一个边界，再回退到当前有效速度段。
    // 恰好位于边界时使用该边界开始的速度，避免整段采用上一流速。
    auto it = std::upper_bound(segments.begin(),
                               segments.end(),
                               currentTime,
                               [](double value, const ScrollSegment& seg) {
                                   return value < seg.time;
                               });

    // 当前时间早于首边界时使用首段进行兼容外推，不把迭代器减到 begin 之前。
    // 通常时间落在已有分段内，回退一步即可得到覆盖当前时刻的速度。
    if ( it != segments.begin() ) {
        --it;
    }

    double cursor      = currentTime;
    double paddingAbsY = 0.0;
    while ( cursor < endTime ) {
        // 缓存速度乘当前动画缩放后才与显示空间余量匹配。
        // 非有限速度按零处理，避免一段异常值污染整段积分结果。
        const double speed = std::isfinite(it->speed)
                                 ? it->speed * cache->getAnimatedZoomScale()
                                 : 0.0;
        auto         next  = std::next(it);
        // 每一片最多积分到下个边界或补间终点，以更早者为准。
        // 最后一段没有后续事件时可直接覆盖窗口余下时间。
        const double sliceEnd =
            next != segments.end() ? std::min(endTime, next->time) : endTime;

        // 只对正长度片段累计路程，跳过重复时间的边界。
        // 游标先推进，再移动分段迭代器，保持时间窗口覆盖连续。
        if ( sliceEnd > cursor ) {
            paddingAbsY += std::abs(speed) * (sliceEnd - cursor);
            cursor = sliceEnd;
        }

        // 最后片段已覆盖窗口终点，退出而不递增尾迭代器。
        // 即使中途存在无长度片段，循环仍沿分段向前推进，不等待外部事件。
        if ( next == segments.end() ) {
            break;
        }
        it = next;
    }

    return paddingAbsY;
}

/// @brief 按来源身份、数量和内容版本复用索引，失效时重新建立绝对位置桶。
/// @param registry 索引所属会话及组件来源。
/// @param cache 有效滚动缓存观察指针，可为空以建立空索引。
/// @param entities 会话已维护的排序实体列表，调用期间保持结构稳定。
/// @param noteRevision 物件内容版本，位置或子结构改变后应同步递增。
/// @return Registry 上下文拥有的索引引用，不能跨会话保存。
/// @warning 失效低频分支完整扫描来源列表并分配桶；版本不变时常量返回缓存。
static NoteAbsYBucketIndex& getOrBuildNoteAbsYBucketIndex(
    entt::registry& registry, const ScrollCache* cache,
    const std::vector<entt::entity>& entities, std::uint64_t noteRevision)
{
    // 索引随 Registry 生命周期保存，首次查询才创建上下文槽位。
    // 该引用不能在清空 Registry 上下文后继续复用。
    auto* index = registry.ctx().find<NoteAbsYBucketIndex>();
    if ( !index ) {
        index = &registry.ctx().emplace<NoteAbsYBucketIndex>();
    }

    const std::uint64_t scrollRevision = cache ? cache->getRevision() : 0;
    // 同一地址和相同数量无法证明内容未变，必须同时检查音符及滚动版本。
    // 相反，版本相同但容器实例更换也应重建，避免复用另一份实体列表。
    if ( index->cache == cache && index->sourceEntities == &entities &&
         index->sourceCount == entities.size() &&
         index->scrollRevision == scrollRevision &&
         index->noteRevision == noteRevision ) {
        return *index;
    }

    // 重建前先记录新的来源签名并清空旧条目，不能把两套坐标桶混在一起。
    // HS 极值从哨兵重新聚合，异常标志也只描述当前这一版数据。
    index->cache                 = cache;
    index->sourceEntities        = &entities;
    index->sourceCount           = entities.size();
    index->scrollRevision        = scrollRevision;
    index->noteRevision          = noteRevision;
    index->bucketOrigin          = 0.0;
    index->minHs                 = std::numeric_limits<double>::infinity();
    index->maxHs                 = 0.0;
    index->requiresFullExactScan = false;
    index->entries.clear();
    index->buckets.clear();
    index->seenSerials.clear();

    // 空来源保留可复用的空索引，并把 HS 极值设为中性值。
    // 无需为没有任何条目的索引分配桶或执行全表精查。
    if ( !cache || entities.empty() ) {
        index->minHs = 1.0;
        index->maxHs = 1.0;
        return *index;
    }

    // 容量以来源数量预留，子实体和无效条目会被过滤，实际条目数可以更少。
    // 全局包络只由有效根物件的采样范围贡献。
    index->entries.reserve(entities.size());
    double globalMinAbsY = std::numeric_limits<double>::infinity();
    double globalMaxAbsY = -std::numeric_limits<double>::infinity();

    for ( auto entity : entities ) {
        // 来源列表可能仍包含失效身份，重建阶段先检查代际和组件存在性。
        // 同一实体号的新代际不能当作旧列表成员直接读取。
        if ( !registry.valid(entity) ||
             !registry.all_of<NoteComponent>(entity) ) {
            continue;
        }

        const auto& note = registry.get<const NoteComponent>(entity);
        // 根折线已经包含完整局部子项，独立子实体不重复占用候选条目。
        // 后续基础层与命中盒从父结构生成子几何。
        if ( note.m_isSubNote ) continue;

        double minAbsY = std::numeric_limits<double>::infinity();
        double maxAbsY = -std::numeric_limits<double>::infinity();

        // 单根条目聚合全部有限采样的未缩放绝对位置。
        // 整个索引同时聚合正 HS 极值，供显示窗口反推保守查询区间。
        /// @brief 累计当前根物件绝对位置包络和索引级正 HS 极值。
        /// @param time 要采样的有限时间。
        /// @note 不安全 HS 设置整个索引的精查标志，不能仅排除当前采样点。
        /// @warning 索引重建的局部采样，不新增事件扫描或资源访问。
        auto includeSampleTime = [&](double time) {
            if ( !std::isfinite(time) ) return;

            // 桶坐标解除动画缩放，交互缩放过程中不必仅因缩放值改变就重建分桶。
            // 查询端必须执行相同转换，否则桶边界与视野不在同一空间。
            const double absY = cache->toUnscaledAbsY(cache->getAbsY(time));
            if ( !std::isfinite(absY) ) return;

            const double hs = cache->getHsAt(time);
            // 反推窗口需要除以正 HS；零、负和非有限值不满足此约束。
            // 一旦出现异常便启用完整精查，不能把该样本忽略后宣称索引仍保守。
            if ( !std::isfinite(hs) || hs <= 1e-6 ) {
                index->requiresFullExactScan = true;
                return;
            }

            minAbsY      = std::min(minAbsY, absY);
            maxAbsY      = std::max(maxAbsY, absY);
            index->minHs = std::min(index->minHs, hs);
            index->maxHs = std::max(index->maxHs, hs);
        };

        forEachNoteVisibilitySampleTime(note, cache, includeSampleTime);

        // 没有有效坐标样本的物件不生成无穷区间条目。
        // 此前检测到的不安全 HS 标志仍保留，查询时可走完整显示精查。
        if ( !std::isfinite(minAbsY) || !std::isfinite(maxAbsY) ) {
            continue;
        }

        // 只用成功形成有限包络的根更新全局范围。
        // 被过滤的子实体或异常根不会制造不可表示的桶原点。
        globalMinAbsY = std::min(globalMinAbsY, minAbsY);
        globalMaxAbsY = std::max(globalMaxAbsY, maxAbsY);
        index->entries.push_back({ entity, minAbsY, maxAbsY });
    }

    // 缺少可用正 HS 时不能按无穷或零值做窗口反算。
    // 中性极值只用于保持数值有效，requiresFullExactScan 仍阻止桶查询。
    if ( !std::isfinite(index->minHs) || index->maxHs <= 1e-6 ) {
        index->minHs                 = 1.0;
        index->maxHs                 = 1.0;
        index->requiresFullExactScan = true;
    }

    // 去重数组与新 entries 一一对应，旧查询印记不跨重建沿用。
    // 条目为空或总包络非法时不计算桶数量。
    index->seenSerials.assign(index->entries.size(), 0);
    if ( index->entries.empty() || !std::isfinite(globalMinAbsY) ||
         !std::isfinite(globalMaxAbsY) ||
         globalMaxAbsY < globalMinAbsY - 1e-6 ) {
        return *index;
    }

    // 以全局最小位置为原点，允许谱面整体包含负 AbsY。
    // 桶宽是位置单位而非秒数，流速变化不会改变单桶的空间意义。
    index->bucketOrigin = globalMinAbsY;
    const double bucketSpan =
        std::max(0.0, globalMaxAbsY - globalMinAbsY) / NOTE_ABSY_BUCKET_SIZE;
    // 末点恰好落在桶边界时仍需一个包含该端点的桶，因此取 floor 后加一。
    // 数量上限防止极大位置跨度造成过量小向量分配，超限改用精查。
    const auto bucketCount =
        static_cast<std::size_t>(std::floor(bucketSpan)) + 1;
    // 超过桶数上限只切换查询策略，仍保留条目和来源签名用于本版数据。
    // 下次相同版本查询可复用不安全标志，避免每帧重试同一昂贵重建。
    if ( bucketCount == 0 || bucketCount > 500000 ) {
        index->requiresFullExactScan = true;
        return *index;
    }

    index->buckets.resize(bucketCount);
    // 端点与原点的浮点误差可能产生微小负值，统一落入首桶。
    // 上界钳到末桶，确保闭区间端点不会访问 buckets.size()。
    /// @brief 将有限位置换算为已分配桶中的闭区间下标。
    /// @param absY 未缩放绝对位置。
    /// @return 钳制到有效桶范围的索引。
    /// @pre buckets 已分配且非空，bucketOrigin 属于当前重建版本。
    auto bucketForAbsY = [&](double absY) {
        const double relative =
            (absY - index->bucketOrigin) / NOTE_ABSY_BUCKET_SIZE;
        if ( relative <= 0.0 ) return std::size_t{ 0 };
        auto bucket = static_cast<std::size_t>(std::floor(relative));
        return std::min(bucket, index->buckets.size() - 1);
    };

    // 每个物件加入覆盖范围内所有桶，不能只索引头部时间或根位置。
    // 长 Hold 与跨流速折线即使根在视野外，也能被中途覆盖桶找回。
    for ( std::size_t i = 0; i < index->entries.size(); ++i ) {
        const auto& entry       = index->entries[i];
        const auto  startBucket = bucketForAbsY(entry.minAbsY);
        const auto  endBucket   = bucketForAbsY(entry.maxAbsY);
        for ( std::size_t bucket = startBucket; bucket <= endBucket;
              ++bucket ) {
            // 桶保存紧凑条目下标，查询后再通过条目读取实体与精确包络。
            // 同一条目可在多个桶重复出现，去重成本留给查询序号数组。
            index->buckets[bucket].push_back(static_cast<std::uint32_t>(i));
            // 闭区间填桶包含末桶，达到尾端立即退出。
            // 显式结束防止索引递增越过有效容器范围。
            if ( bucket == index->buckets.size() - 1 ) break;
        }
    }

    return *index;
}

/// @brief 从绝对位置桶粗筛根物件，再按当前显示坐标精查并补回拖动参与者。
/// @param registry 提供排序列表、版本、索引和拖动保留身份。
/// @param cache 当前显示坐标换算缓存。
/// @param currentTime 快照动画时间，用于估算未来补间路程。
/// @param currentAbsY 当前显示锚点，需与实际绘制一致。
/// @param judgmentLineY 判定线像素位置。
/// @param topY 视野上界。
/// @param bottomY 视野下界。
/// @param renderScaleY 相机纵向缩放，接近零时不查询。
/// @param visualPaddingPixels 物件可见边缘所需像素余量。
/// @param interpolationSeconds UI 补间需要保护的未来秒数。
/// @param result 输出候选身份，入口清空并复用容量。
/// @param seen 拖动身份合并所用暂存集合，不能与其他查询并发共享。
/// @warning 每次快照调用；优先查桶，缺失版本或异常 HS 时存在完整精查兜底。
/// @note 输出只包含根对象，结果顺序不保证与时间排序列表相同。
static void collectNotesInRange(
    entt::registry& registry, const ScrollCache* cache, double currentTime,
    double currentAbsY, float judgmentLineY, float topY, float bottomY,
    float renderScaleY, float visualPaddingPixels, double interpolationSeconds,
    std::vector<entt::entity>& result, std::unordered_set<entt::entity>& seen)
{
    // 输出容器由调用方复用，每次查询必须替换而非累加上一相机的结果。
    // 索引内部的序号去重与外部拖动合并集合各自负责不同阶段。
    result.clear();
    seen.clear();
    // 会话发布排序列表观察指针，缺少来源时无法建立候选基础。
    // 不能在快照热路径临时完整枚举并排序 ECS 来代替该契约。
    const auto** sortedEntitiesPtr =
        registry.ctx().find<const std::vector<entt::entity>*>();
    if ( !sortedEntitiesPtr || !(*sortedEntitiesPtr) ) return;

    const auto& entities = **sortedEntitiesPtr;
    size_t      count    = entities.size();
    // 有效来源、缓存和非退化纵向缩放是坐标反查的前提。
    // 这些条件不满足时保留空结果，不用默认坐标去扩大查询。
    if ( count == 0 || !cache || std::abs(renderScaleY) < 1e-6f ) {
        return;
    }

    // 由视口边缘反推相对判定线的显示差，像素方向与显示差方向相反。
    // 视觉高度余量按缩放绝对值还原，再加播放补间预计路程。
    double maxDelta =
        (judgmentLineY - topY) / static_cast<double>(renderScaleY);
    double minDelta =
        (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
    double padDelta = std::max(0.0, static_cast<double>(visualPaddingPixels)) /
                          static_cast<double>(std::abs(renderScaleY)) +
                      calculateInterpolationPaddingAbsY(
                          cache, currentTime, interpolationSeconds);

    // 粗筛后仍按组件当前值精查，不能仅依赖可能较宽的索引包络。
    // 局部流速边界也参与极值，保留中途反向进入视野的物件。
    /// @brief 按组件当前显示包络判断是否与扩张视野相交。
    /// @param note 根物件及其内嵌子列表。
    /// @return 至少存在有效样本且包络相交时为 true。
    /// @warning 候选精查热路径，成本仅来自当前物件覆盖的局部流速分段。
    auto isDisplayVisible = [&](const NoteComponent& note) {
        double minDisplayDelta = std::numeric_limits<double>::infinity();
        double maxDisplayDelta = -std::numeric_limits<double>::infinity();

        // 显示差以采样时间自身的 HS 解释，与候选可见性规则保持一致。
        // 非有限结果不进入极值，避免一个异常样本破坏所有范围比较。
        /// @brief 用单个有限显示差扩张当前候选的可见性包络。
        /// @param time 物件关键端点或内部流速边界时间。
        /// @note 无法得到有限显示差时不修改已有极值。
        auto includeTime = [&](double time) {
            double displayDelta =
                cache->getDisplayDelta(time, currentAbsY, time);
            if ( !std::isfinite(displayDelta) ) return;
            minDisplayDelta = std::min(minDisplayDelta, displayDelta);
            maxDisplayDelta = std::max(maxDisplayDelta, displayDelta);
        };

        forEachNoteVisibilitySampleTime(note, cache, includeTime);

        // 所有样本都无效时无法证明可见，直接排除该候选。
        // 至少存在一个有效样本才进行与扩张视野的闭区间相交判断。
        if ( !std::isfinite(minDisplayDelta) ||
             !std::isfinite(maxDisplayDelta) ) {
            return false;
        }

        return maxDisplayDelta >= minDelta - padDelta &&
               minDisplayDelta <= maxDelta + padDelta;
    };

    // 兜底依赖会话排序列表中的身份和 Note 组件仍有效。
    // 它避免不安全的 HS 反推，但仍是全量工作，不能在正常桶路径重复调用。
    /// @brief 对来源列表所有根对象使用当前组件显示精查。
    /// @warning
    /// 缺少版本契约或索引不安全时的既有全量兜底，禁止正常桶路径重复调用。
    /// @pre 排序列表中身份仍持有 NoteComponent，调用期间不可并发删除。
    /// @note 本助手不补入来源列表之外的拖动身份，由调用分支单独处理。
    auto runFullExactScan = [&]() {
        result.reserve(count);
        for ( auto entity : entities ) {
            const auto& note = registry.get<const NoteComponent>(entity);
            if ( note.m_isSubNote ) continue;

            if ( isDisplayVisible(note) ) {
                result.push_back(entity);
            }
        }
    };

    // 拖动预览直接改当前组件，旧索引位置可能尚未随每个鼠标事件更新。
    // 保留身份绕过桶粗筛，再按最新显示值精查，避免移动物件突然消失。
    /// @brief 合并手势显式保留身份，弥补旧空间索引未反映预览位移的情况。
    /// @warning 仅遍历本次拖动参与集合和已得到的候选，不重新扫描全谱。
    /// @note result 已有身份保持顺序，新增可见根追加到末尾。
    /// @note 子实体不会单独返回，绘制仍由父折线负责。
    auto appendPinnedDragEntities = [&]() {
        const auto* pinnedEntities =
            registry.ctx().find<DragRenderPinnedEntities>();
        if ( !pinnedEntities || !pinnedEntities->entities ||
             pinnedEntities->entities->empty() ) {
            return;
        }

        seen.clear();
        // 只在存在拖动保留列表时构建候选身份集合。
        // 普通静止帧使用索引序号去重，不额外把全部候选复制进哈希集合。
        seen.reserve(result.size() + pinnedEntities->entities->size());
        for ( auto entity : result ) {
            seen.insert(entity);
        }

        for ( auto entity : *pinnedEntities->entities ) {
            // 已在正常候选里的拖动对象无需重复绘制。
            // 先去重再验证身份，重复的无效保留项也不会反复执行组件查询。
            if ( !seen.insert(entity).second ) {
                continue;
            }
            if ( !registry.valid(entity) ||
                 !registry.all_of<NoteComponent>(entity) ) {
                continue;
            }

            const auto& note = registry.get<const NoteComponent>(entity);
            if ( note.m_isSubNote ) continue;
            if ( isDisplayVisible(note) ) {
                result.push_back(entity);
            }
        }
    };

    // 缺少物件版本契约时不能可靠判断位置索引是否过期。
    // 保守扫描使用当前组件，再补充不在来源候选中的有效拖动参与者。
    const auto** noteRevisionPtr = registry.ctx().find<const std::uint64_t*>();
    if ( !noteRevisionPtr || !(*noteRevisionPtr) ) {
        runFullExactScan();
        appendPinnedDragEntities();
        return;
    }

    auto& index = getOrBuildNoteAbsYBucketIndex(
        registry, cache, entities, **noteRevisionPtr);
    // 不安全索引完全绕开 HS 反推与桶访问，避免部分桶结果造成漏绘。
    // 当前组件精查完成后仍补回拖动身份，两个可见性保障同时生效。
    if ( index.requiresFullExactScan ) {
        runFullExactScan();
        appendPinnedDragEntities();
        return;
    }
    // 空桶不代表拖动中的物件也不可见，仍检查显式保留身份。
    // 该分支无需分配查询序号或访问桶下标。
    if ( index.entries.empty() || index.buckets.empty() ) {
        appendPinnedDragEntities();
        return;
    }

    const double displayMin   = std::min(minDelta, maxDelta) - padDelta;
    const double displayMax   = std::max(minDelta, maxDelta) + padDelta;
    double       queryMinAbsY = std::numeric_limits<double>::infinity();
    double       queryMaxAbsY = -std::numeric_limits<double>::infinity();
    // 正 HS 范围的两个端点给出视野反推位置的保守包络。
    // 同时考虑显示区间两侧，避免视野跨过判定线时只取单侧极值。
    /// @brief 用一个正 HS 边界反推显示窗口对应的绝对位置范围。
    /// @param hs 索引记录的最小或最大正倍率。
    /// @note 无效倍率不修改累计包络，最终有效性检查决定是否退回精查。
    auto includeHsBound = [&](double hs) {
        if ( !std::isfinite(hs) || hs <= 1e-6 ) return;
        const double a = currentAbsY + displayMin / hs;
        const double b = currentAbsY + displayMax / hs;
        queryMinAbsY   = std::min(queryMinAbsY, std::min(a, b));
        queryMaxAbsY   = std::max(queryMaxAbsY, std::max(a, b));
    };
    includeHsBound(index.minHs);
    includeHsBound(index.maxHs);
    // 反推失败时丢弃未定义查询区间，不把无穷强转为桶下标。
    // 使用完整精查保证异常输入不会直接表现为整批候选消失。
    if ( !std::isfinite(queryMinAbsY) || !std::isfinite(queryMaxAbsY) ) {
        runFullExactScan();
        appendPinnedDragEntities();
        return;
    }
    // 反推出的是当前显示空间绝对位置，查桶前解除动画缩放。
    // 转换后重新规范上下界，以免缩放方向改变导致区间颠倒。
    queryMinAbsY = cache->toUnscaledAbsY(queryMinAbsY);
    queryMaxAbsY = cache->toUnscaledAbsY(queryMaxAbsY);
    if ( queryMinAbsY > queryMaxAbsY ) {
        std::swap(queryMinAbsY, queryMaxAbsY);
    }

    /// @brief 查询侧按索引原点和固定桶宽解析桶编号。
    /// @param absY 已解除当前动画缩放的绝对位置。
    /// @return 范围内桶编号；外侧位置钳到边缘桶后再做区间精查。
    /// @pre 索引桶非空，坐标与 entries 使用相同未缩放空间。
    auto bucketForAbsY = [&](double absY) {
        const double relative =
            (absY - index.bucketOrigin) / NOTE_ABSY_BUCKET_SIZE;
        if ( relative <= 0.0 ) return std::size_t{ 0 };
        auto bucket = static_cast<std::size_t>(std::floor(relative));
        return std::min(bucket, index.buckets.size() - 1);
    };

    const auto startBucket = bucketForAbsY(queryMinAbsY);
    const auto endBucket   = bucketForAbsY(queryMaxAbsY);
    // 单次查询共用序号，跨多个桶碰到同一条目只精查一次。
    // 无符号序号回绕到零时清空旧标记，避免历史序号碰撞漏掉候选。
    ++index.querySerial;
    // 零值是新建或重置去重数组的初始标记，不能用于有效查询。
    // 回绕后从一开始，使当前查询与所有已清空条目保持可区分。
    if ( index.querySerial == 0 ) {
        std::fill(index.seenSerials.begin(), index.seenSerials.end(), 0);
        index.querySerial = 1;
    }

    // 预留常见可见规模但不限制结果数量，密集谱面仍可按需增长。
    // 容量随快照复用，不能把这个数值当作可见物件上限。
    result.reserve(256);
    for ( std::size_t bucket = startBucket; bucket <= endBucket; ++bucket ) {
        for ( std::uint32_t entryIndex : index.buckets[bucket] ) {
            if ( entryIndex >= index.entries.size() ) continue;
            if ( index.seenSerials[entryIndex] == index.querySerial ) {
                continue;
            }
            // 进入区间精查前即标记，跨桶重复出现的不可见条目也只判断一次。
            // 标记按 entries 下标，不依赖实体编号在不同代际之间是否复用。
            index.seenSerials[entryIndex] = index.querySerial;

            const auto& entry = index.entries[entryIndex];
            // 桶边界是粗粒度范围，条目自身可能与视野完全不交。
            // 先用索引包络排除，再读取 ECS 与计算当前显示差，减少精查开销。
            if ( entry.maxAbsY < queryMinAbsY ||
                 entry.minAbsY > queryMaxAbsY ) {
                continue;
            }
            // 空间索引身份可能尚未随删除更新，读取当前组件前再次检验代际。
            // 位置仍匹配也不能据此认为该实体仍有 Note 组件。
            if ( !registry.valid(entry.entity) ||
                 !registry.all_of<NoteComponent>(entry.entity) ) {
                continue;
            }

            const auto& note = registry.get<const NoteComponent>(entry.entity);
            if ( note.m_isSubNote ) continue;
            if ( isDisplayVisible(note) ) {
                result.push_back(entry.entity);
            }
        }
        // 查询使用闭合桶区间，到目标末桶或容器末桶即停止。
        // 范围外查询已钳到边缘桶，精确包络检查负责排除不相交条目。
        if ( bucket == endBucket || bucket == index.buckets.size() - 1 ) {
            break;
        }
    }
    appendPinnedDragEntities();
}

/// @brief 分两遍生成连接主体和可操作端点的命中区域。
/// @param registry 已通过候选过滤的音符组件来源。
/// @param snapshot 命中盒追加目标，不清除其他系统生成的条目。
/// @param ctx 当前滚动锚点、纹理比例及玩家基准尺寸。
/// @param noteEntities 待检查的根音符身份列表。
/// @param judgmentLineY 当前视口判定线坐标。
/// @param leftX 兼容玩家域左边界。
/// @param topY 可视区域上界。
/// @param bottomY 可视区域下界。
/// @param singleTrackW 玩家域单轨基准宽度。
/// @param renderScaleY 当前相机纵向缩放。
/// @param config 可编辑性门禁及视觉配置。
/// @param laneProjection 可选分区投影，用于解析每个端点独立轨宽。
/// @warning 快照热路径：仅遍历候选两次，不得加入全谱查询、排序或阻塞同步。
/// @pre 候选身份同时持有 NoteComponent 与 TransformComponent，缓存和基准 UV
/// 有效。
/// @note 先追加连接体再追加头尾，使拾取层可保留端点更高的覆盖优先级。
/// @note UV 尺寸查询默认 Note 基准纹理存在且宽高非零，专用部件纹理可缺失。
/// @note 普通部件子索引为负，折线局部子索引由专用渲染入口负责。
/// @note Flick 连接体包含两端中心之间的真实间隙，箭头则只覆盖终点图像范围。
/// @note Hold 主体高度过小时不创建身体命中，头尾仍由第二遍处理。
void NoteRenderSystem::generateNoteHitboxes(
    entt::registry& registry, RenderSnapshot* snapshot,
    const NoteRenderSystem::NoteRenderContext& ctx,
    const std::vector<entt::entity>& noteEntities, float judgmentLineY,
    float leftX, float topY, float bottomY, float singleTrackW,
    float renderScaleY, const Config::EditorConfig& config,
    const CanvasLaneProjection* laneProjection)
{
    // 第一遍：连接体，优先级较低。
    for ( auto entity : noteEntities ) {
        const auto& transform = registry.get<const TransformComponent>(entity);
        const auto& note      = registry.get<const NoteComponent>(entity);
        if ( !SessionUtils::isNoteEditable(note, config.settings) ) continue;
        // 命中盒按根节点与终点各自的真实轨道宽度和中心解析。
        // 先保存玩家域基准，再建立当前音符所在域的局部上下文。
        // 终点可能位于另一种宽度的区域，解析它时不能把根域尺寸当成玩家基准。
        const float fallbackLeftX  = leftX;
        const float fallbackTrackW = singleTrackW;
        const float fallbackNoteW  = ctx.noteW;
        const float fallbackNoteH  = ctx.noteH;
        const auto  laneGeometry   = resolveNoteLaneGeometry(note.m_trackIndex,
                                                             laneProjection,
                                                             fallbackLeftX,
                                                             fallbackTrackW,
                                                             fallbackNoteW,
                                                             fallbackNoteH);
        // 局部值副本只覆盖宽高，滚动锚点与颜色仍沿用本次快照。
        // 不修改调用方上下文，后一个音符可以属于不同领域。
        auto laneContext         = ctx;
        laneContext.noteW        = laneGeometry.noteW;
        laneContext.noteH        = laneGeometry.noteH;
        const auto& ctx          = laneContext;
        const float singleTrackW = laneGeometry.width;
        // 构造兼容线性公式的局部原点，让 leftX+track×width 恢复实际轨道左界。
        // 真实分区可能存在间隙，该原点不等于整个画布的左边界。
        const float leftX =
            laneGeometry.leftX -
            static_cast<float>(note.m_trackIndex) * singleTrackW;

        // 主体起终点分别换算显示差，Hold 末端继续使用根时间 HS。
        // 命中范围必须与主体绘制使用相同倍率锚点，不能只按 duration
        // 乘全局速度。
        double displayDeltaStart = ctx.cache->getDisplayDelta(
            note.m_timestamp, ctx.currentAbsY, note.m_timestamp);
        const double endAnchorTime   = getCarrierEndAnchorTime(note, ctx.cache);
        double       displayDeltaEnd = ctx.cache->getDisplayDelta(
            note.m_timestamp + note.m_duration, ctx.currentAbsY, endAnchorTime);

        double maxDelta =
            (judgmentLineY - topY) / static_cast<double>(renderScaleY);
        double minDelta =
            (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
        double padDelta = ctx.noteH / static_cast<double>(renderScaleY);

        if ( !NoteRenderSystem::isCarrierVisible(
                 note.m_timestamp,
                 note.m_timestamp + note.m_duration,
                 ctx.currentTime,
                 displayDeltaStart,
                 displayDeltaEnd,
                 maxDelta + padDelta,
                 minDelta - padDelta) ) {
            continue;
        }

        float screenY = judgmentLineY -
                        static_cast<float>(displayDeltaStart) * renderScaleY;

        // 有符号高度保留滚动方向，后面构造矩形再取最小 Y 和绝对跨度。
        // 这样反向显示的长条仍拥有非负命中矩形。
        float visualH =
            static_cast<float>(displayDeltaStart - displayDeltaEnd) *
            renderScaleY;

        // 子节点由根折线的局部绘制路径生成命中，不在普通主体层重复追加。
        // 这里只对独立 Flick 与具有可见长度的 Hold 建立连接体命中。
        if ( !note.m_isSubNote ) {
            if ( note.m_type == ::MMM::NoteType::FLICK && note.m_dtrack != 0 ) {
                const auto flickEndpoint =
                    resolveNoteLaneGeometry(note.m_trackIndex + note.m_dtrack,
                                            laneProjection,
                                            fallbackLeftX,
                                            fallbackTrackW,
                                            fallbackNoteW,
                                            fallbackNoteH);
                // 连接体命中范围覆盖根节点与终点的真实中心及中间间隙。
                const float drawW =
                    std::abs(flickEndpoint.centerX() - laneGeometry.centerX());
                const float bodyX =
                    std::min(laneGeometry.centerX(), flickEndpoint.centerX());

                // 横向连接体高度按专用纹理相对 Note 纹理的高度比例计算。
                // 专用纹理缺失时使用音符基准高度，保持可操作区域存在。
                float drawH   = ctx.noteH;
                auto  itBodyH = snapshot->uvMap.find(
                    static_cast<uint32_t>(TextureID::HoldBodyHorizontal));
                if ( itBodyH != snapshot->uvMap.end() ) {
                    drawH = ctx.noteH *
                            (itBodyH->second.w /
                             snapshot->uvMap.at(uint32_t(TextureID::Note)).w);
                }

                // Flick
                // 横向连接体沿两个真实轨道中心覆盖整个区间，包括分区间隙。
                // 子索引为负表示独立物件部件，不对应折线内嵌列表。
                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::HoldBody,
                                               -1,
                                               bodyX,
                                               screenY - drawH * 0.5f,
                                               drawW,
                                               drawH });
            } else if ( note.m_type == ::MMM::NoteType::HOLD &&
                        std::abs(visualH) > ctx.noteH * 0.1f ) {
                // Hold 身体宽度跟随竖向主体纹理的相对宽度，不直接使用整轨宽度。
                // 极短主体由前面的高度阈值排除，避免连接体命中吞掉头尾手柄。
                float bodyW  = ctx.noteW;
                auto  itBody = snapshot->uvMap.find(
                    static_cast<uint32_t>(TextureID::HoldBodyVertical));
                if ( itBody != snapshot->uvMap.end() ) {
                    // 部件比例以图集中 Note 的 UV
                    // 宽度为参考，缩放后仍按所在轨道居中。 这里依赖同一快照 UV
                    // 表，不能混用另一个皮肤版本的基准。
                    float baseWRatio =
                        snapshot->uvMap.at(uint32_t(TextureID::Note)).z;
                    bodyW = ctx.noteW * (itBody->second.z / baseWRatio);
                }

                float bodyX = leftX + note.m_trackIndex * singleTrackW +
                              (singleTrackW - bodyW) * 0.5f;
                // 用两端较小 Y 作为矩形上边缘，保持逆向滚动下的正高度约定。
                // 主体区域只连接中心线，头尾图像额外半高由第二遍独立覆盖。
                float bodyY = std::min(screenY, screenY + visualH);
                float bodyH = std::abs(visualH);

                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::HoldBody,
                                               -1,
                                               bodyX,
                                               bodyY,
                                               bodyW,
                                               bodyH });
            }
        }
    }

    // 第二遍：头部、尾部和箭头，优先级较高。
    for ( auto entity : noteEntities ) {
        const auto& transform = registry.get<const TransformComponent>(entity);
        const auto& note      = registry.get<const NoteComponent>(entity);
        if ( !SessionUtils::isNoteEditable(note, config.settings) ) continue;
        // 命中盒按根节点与终点各自的真实轨道宽度和中心解析。
        const float fallbackLeftX  = leftX;
        const float fallbackTrackW = singleTrackW;
        const float fallbackNoteW  = ctx.noteW;
        const float fallbackNoteH  = ctx.noteH;
        const auto  laneGeometry   = resolveNoteLaneGeometry(note.m_trackIndex,
                                                             laneProjection,
                                                             fallbackLeftX,
                                                             fallbackTrackW,
                                                             fallbackNoteW,
                                                             fallbackNoteH);
        auto        laneContext    = ctx;
        laneContext.noteW          = laneGeometry.noteW;
        laneContext.noteH          = laneGeometry.noteH;
        const auto& ctx            = laneContext;
        const float singleTrackW   = laneGeometry.width;
        const float leftX =
            laneGeometry.leftX -
            static_cast<float>(note.m_trackIndex) * singleTrackW;
        float screenY =
            judgmentLineY -
            static_cast<float>(ctx.cache->getDisplayDelta(
                note.m_timestamp, ctx.currentAbsY, note.m_timestamp)) *
                renderScaleY;
        float endY =
            judgmentLineY - static_cast<float>(ctx.cache->getDisplayDelta(
                                note.m_timestamp + note.m_duration,
                                ctx.currentAbsY,
                                getCarrierEndAnchorTime(note, ctx.cache))) *
                                renderScaleY;

        // 端点阶段先以整个起终包络加一个音符高度做粗裁剪。
        // 中心刚越出视口但纹理仍覆盖边缘时不应丢失命中。
        float minY = std::min(screenY, endY) - ctx.noteH;
        float maxY = std::max(screenY, endY) + ctx.noteH;
        if ( minY > bottomY || maxY < topY ) continue;

        // 根头部按所在轨宽居中，不能从旧 Transform 的连续轨位置推导。
        // 实际分区几何是当前帧主画布横向位置的来源。
        const float headX =
            resolveNoteTrackLeftX(note, laneProjection, leftX, singleTrackW) +
            (singleTrackW - ctx.noteW) * 0.5f;

        // 折线头尾部件由 Polyline 专用路径决定，此处仅生成独立物件。
        // 防止同一折线头同时出现普通 Head 与 PolylineNode 两种命中身份。
        if ( !note.m_isSubNote && note.m_type != ::MMM::NoteType::POLYLINE ) {
            // 所有非 Polyline 音符的 Head
            snapshot->hitboxes.push_back({ entity,
                                           HoverPart::Head,
                                           -1,
                                           headX,
                                           screenY - ctx.noteH * 0.5f,
                                           ctx.noteW,
                                           ctx.noteH });

            if ( note.m_type == ::MMM::NoteType::FLICK && note.m_dtrack != 0 ) {
                const auto flickEndpoint =
                    resolveNoteLaneGeometry(note.m_trackIndex + note.m_dtrack,
                                            laneProjection,
                                            fallbackLeftX,
                                            fallbackTrackW,
                                            fallbackNoteW,
                                            fallbackNoteH);
                // 箭头方向取轨差符号，尺寸基准取终点所在域。
                // 根和终点跨域宽度不同时，箭头命中必须跟随终点图像而非根头大小。
                TextureID arrowId = (note.m_dtrack < 0)
                                        ? TextureID::FlickArrowLeft
                                        : TextureID::FlickArrowRight;
                auto it = snapshot->uvMap.find(static_cast<uint32_t>(arrowId));
                // 缺少方向纹理时使用终点基准宽高。
                // 存在纹理则分别按 UV
                // 宽和高缩放，保持非等比箭头皮肤的实际命中范围。
                float arrowW = flickEndpoint.noteW;
                float arrowH = flickEndpoint.noteH;
                if ( it != snapshot->uvMap.end() ) {
                    float baseWRatio =
                        snapshot->uvMap.at(uint32_t(TextureID::Note)).z;
                    float baseHRatio =
                        snapshot->uvMap.at(uint32_t(TextureID::Note)).w;
                    float wRatio = it->second.z / baseWRatio;
                    float hRatio = it->second.w / baseHRatio;
                    arrowW       = flickEndpoint.noteW * wRatio;
                    arrowH       = flickEndpoint.noteH * hRatio;
                }

                // 箭头水平中心固定在目标轨道中心，左右方向不改变中心定位规则。
                // 图像本身的宽度已经包含纹理比例，无需再加轨差。
                const float arrowX = flickEndpoint.centerX() - arrowW * 0.5F;

                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::FlickArrow,
                                               -1,
                                               arrowX,
                                               screenY - arrowH * 0.5f,
                                               arrowW,
                                               arrowH });
            } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
                auto it = snapshot->uvMap.find(
                    static_cast<uint32_t>(TextureID::HoldEnd));
                // Hold 尾部独立纹理可以比头部更宽或更高。
                // 命中盒按尾纹理比例调整，不能直接复用头部矩形。
                float endW = ctx.noteW;
                float endH = ctx.noteH;
                if ( it != snapshot->uvMap.end() ) {
                    float baseWRatio =
                        snapshot->uvMap.at(uint32_t(TextureID::Note)).z;
                    float baseHRatio =
                        snapshot->uvMap.at(uint32_t(TextureID::Note)).w;
                    endW = ctx.noteW * (it->second.z / baseWRatio);
                    endH = ctx.noteH * (it->second.w / baseHRatio);
                }

                snapshot->hitboxes.push_back(
                    { entity,
                      HoverPart::HoldEnd,
                      -1,
                      leftX + note.m_trackIndex * singleTrackW +
                          (singleTrackW - endW) * 0.5f,
                      endY - endH * 0.5f,
                      endW,
                      endH });
            }
        }
    }
}

/// @brief 精筛普通主体并按候选反序绘制基础几何，最后追加音效标签。
/// @param registry 当前音符与交互组件来源。
/// @param snapshot 当前纹理与交互反馈快照。
/// @param ctx 玩家基准尺寸、颜色及滚动锚点。
/// @param config 当前绘制配置。
/// @param noteEntities 空间查询提供的根对象候选。
/// @param batcher 主体与标签共用的批处理器。
/// @param currentTime 调用方动画时间，实际几何读取 ctx 中的一致锚点。
/// @param judgmentLineY 当前判定线坐标。
/// @param leftX 玩家域兼容左界。
/// @param rightX 调用方提供的区域右界。
/// @param topY 主体可视上界。
/// @param bottomY 主体可视下界。
/// @param singleTrackW 玩家轨道基准宽度。
/// @param renderScaleY 相机纵向缩放。
/// @param trackCount 当前玩家轨道数。
/// @param generateHitboxes 是否由折线渲染追加局部命中盒。
/// @param showBoundSampleLabels 是否绘制资源绑定标签。
/// @param laneProjection 真实领域投影，辅助视图可不提供。
/// @warning 每次快照调用；当前使用局部可见列表，禁止再添加全谱扫描或逐帧排序。
/// @note 本层只输出基础颜色及拖动/擦除反馈，选中和悬浮由发光层叠加。
/// @pre 空间候选中的组件在整个基础层与标签遍历期间保持有效。
/// @note 此入口结束时刷新 batcher，后续图层可以独立切换纹理或输出命令列表。
void NoteRenderSystem::renderNoteBaseLayer(
    entt::registry& registry, RenderSnapshot* snapshot,
    const NoteRenderSystem::NoteRenderContext& ctx,
    const Config::EditorConfig&                config,
    const std::vector<entt::entity>& noteEntities, Batcher& batcher,
    float currentTime, float judgmentLineY, float leftX, float rightX,
    float topY, float bottomY, float singleTrackW, float renderScaleY,
    int32_t trackCount, bool generateHitboxes, bool showBoundSampleLabels,
    const CanvasLaneProjection* laneProjection)
{
    // 候选粗筛后还需普通主体的显示包络检查。
    // 当前实现使用局部向量保存结果，仍存在每次调用的容量分配成本。
    std::vector<entt::entity> visibleEntities;
    for ( auto entity : noteEntities ) {
        const auto& note = registry.get<const NoteComponent>(entity);
        if ( note.m_isSubNote ) continue;

        double displayDeltaStart = ctx.cache->getDisplayDelta(
            note.m_timestamp, ctx.currentAbsY, note.m_timestamp);
        const double endAnchorTime   = getCarrierEndAnchorTime(note, ctx.cache);
        double       displayDeltaEnd = ctx.cache->getDisplayDelta(
            note.m_timestamp + note.m_duration, ctx.currentAbsY, endAnchorTime);

        double maxDelta =
            (judgmentLineY - topY) / static_cast<double>(renderScaleY);
        double minDelta =
            (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
        double padDelta = ctx.noteH / static_cast<double>(renderScaleY);

        // 折线可能只有中间子段进入视口，不能用根基础 duration
        // 再次排除整个对象。 其局部可见性由折线绘制路径按段处理。
        if ( note.m_type != ::MMM::NoteType::POLYLINE ) {
            if ( !NoteRenderSystem::isCarrierVisible(
                     note.m_timestamp,
                     note.m_timestamp + note.m_duration,
                     ctx.currentTime,
                     displayDeltaStart,
                     displayDeltaEnd,
                     maxDelta + padDelta,
                     minDelta - padDelta) ) {
                continue;
            }
        }

        visibleEntities.push_back(entity);
    }

    // 保留候选收集顺序并反向绘制，不在热路径重新排序。
    // 分桶和拖动补入路径不保证时间升序，不能把此处反序等同于按时间降序。
    for ( auto it = visibleEntities.rbegin(); it != visibleEntities.rend();
          ++it ) {
        /// @brief 当前反向遍历到的可见音符实体。
        entt::entity entity    = *it;
        const auto&  transform = registry.get<const TransformComponent>(entity);
        const auto&  note      = registry.get<const NoteComponent>(entity);
        // 根节点与跨区域端点都保留玩家连续投影作为 Preview 回退参数。
        const float fallbackLeftX  = leftX;
        const float fallbackTrackW = singleTrackW;
        const float fallbackNoteW  = ctx.noteW;
        const float fallbackNoteH  = ctx.noteH;
        const auto  laneGeometry   = resolveNoteLaneGeometry(note.m_trackIndex,
                                                             laneProjection,
                                                             fallbackLeftX,
                                                             fallbackTrackW,
                                                             fallbackNoteW,
                                                             fallbackNoteH);
        auto        laneContext    = ctx;
        laneContext.noteW          = laneGeometry.noteW;
        laneContext.noteH          = laneGeometry.noteH;
        const auto& ctx            = laneContext;
        const float singleTrackW   = laneGeometry.width;
        const float leftX =
            laneGeometry.leftX -
            static_cast<float>(note.m_trackIndex) * singleTrackW;

        // 处理拖拽/剪切时的视觉反馈；选中反馈由发光层按当前颜色绘制。
        // 交互组件可缺失，此时采用完整基础透明度。
        // 拖动与剪切只调整 alpha，不改对象本身的颜色覆盖数据。
        float       alphaMul = 1.0f;
        const auto* interaction =
            registry.try_get<const InteractionComponent>(entity);
        if ( interaction && (interaction->isDragging || interaction->isCut) ) {
            alphaMul = 0.5f;
        }

        float screenY =
            judgmentLineY -
            static_cast<float>(ctx.cache->getDisplayDelta(
                note.m_timestamp, ctx.currentAbsY, note.m_timestamp)) *
                renderScaleY;
        float       visualH = static_cast<float>(ctx.cache->getDisplayDelta(
                                  note.m_timestamp + note.m_duration,
                                  ctx.cache->getAbsY(note.m_timestamp),
                                  note.m_timestamp)) *
                              renderScaleY;
        const float trackX =
            resolveNoteTrackLeftX(note, laneProjection, leftX, singleTrackW);

        // 草稿物件始终使用皮肤草稿色，避免玩家调色盘覆盖区域辨识度。
        glm::vec4 curColorNote =
            note.m_isDraft
                ? ctx.colorDraftTap
                : resolveNoteColor(note, NoteColorSlot::Tap, ctx.colorTap);
        glm::vec4 curColorHead =
            note.m_isDraft
                ? ctx.colorDraftHead
                : resolveNoteColor(note, NoteColorSlot::Head, ctx.colorHead);
        glm::vec4 curColorHoldBody =
            note.m_isDraft
                ? ctx.colorDraftHold
                : resolveNoteColor(note, NoteColorSlot::Hold, ctx.colorHold);
        glm::vec4 curColorHoldEnd =
            note.m_isDraft
                ? ctx.colorDraftEnd
                : resolveNoteColor(note, NoteColorSlot::End, ctx.colorEnd);
        glm::vec4 curColorNode =
            note.m_isDraft
                ? ctx.colorDraftNode
                : resolveNoteColor(note, NoteColorSlot::Node, ctx.colorNode);
        glm::vec4 curColorArrow =
            note.m_isDraft
                ? ctx.colorDraftArrow
                : resolveNoteColor(
                      note, NoteColorSlot::FlickArrow, ctx.colorArrow);

        // 草稿和玩家在同一 Registry 内仍有不同交互种类。
        // 擦除高亮先匹配种类，保证当前工具目标与可见区域语义一致。
        const auto objectKind = note.m_isDraft ? ChartObjectKind::DraftNote
                                               : ChartObjectKind::PlayerNote;
        // 擦除反馈同时匹配领域和身份，避免不同 Registry 同号实体串色。
        // 只有负子索引表示整对象擦除，局部折线反馈由专用部件处理。
        bool isFullErasing = snapshot->erasingObjectKind == objectKind &&
                             snapshot->erasingEntities.count(entity) &&
                             snapshot->erasingSubIndex == -1;
        // 整对象擦除使用统一红色覆盖各部件，随后再乘整体透明度。
        // 若对象同时处于拖动或剪切状态，两个反馈的透明度乘数继续叠加。
        if ( isFullErasing ) {
            curColorNote     = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorHead     = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorHoldBody = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorHoldEnd  = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorNode     = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorArrow    = { 1.0f, 0.2f, 0.2f, 1.0f };
            alphaMul *= 0.5f;
        }

        // 每个部件保留独立基础 alpha，再应用同一交互乘数。
        // 不直接覆盖为固定透明度，避免丢失皮肤自身透明设置。
        curColorNote.a *= alphaMul;
        curColorHead.a *= alphaMul;
        curColorHoldBody.a *= alphaMul;
        curColorHoldEnd.a *= alphaMul;
        curColorNode.a *= alphaMul;
        curColorArrow.a *= alphaMul;

        if ( note.m_type == ::MMM::NoteType::NOTE )
            NoteRenderSystem::renderTap(
                batcher,
                note,
                config,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                screenY,
                ctx.noteW,
                ctx.noteH,
                ctx.baseAspect,
                curColorNote);
        else if ( note.m_type == ::MMM::NoteType::HOLD )
            NoteRenderSystem::renderHold(
                batcher,
                note,
                config,
                snapshot,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                ctx.noteW,
                ctx.noteH,
                singleTrackW,
                curColorHead,
                curColorHoldBody,
                curColorHoldEnd,
                ctx.cache,
                ctx.currentAbsY,
                judgmentLineY,
                renderScaleY,
                topY,
                bottomY);
        else if ( note.m_type == ::MMM::NoteType::FLICK ) {
            // Flick 终点重新使用玩家兼容基准解析，避免根域缩放被再乘一次。
            // 连接体和箭头分别消费终点中心及终点尺寸。
            const auto flickEndpoint =
                resolveNoteLaneGeometry(note.m_trackIndex + note.m_dtrack,
                                        laneProjection,
                                        fallbackLeftX,
                                        fallbackTrackW,
                                        fallbackNoteW,
                                        fallbackNoteH);
            NoteRenderSystem::renderFlick(
                batcher,
                note,
                config,
                snapshot,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                screenY,
                ctx.noteW,
                ctx.noteH,
                flickEndpoint.centerX(),
                flickEndpoint.noteW,
                flickEndpoint.noteH,
                curColorHead,
                curColorHoldBody,
                curColorArrow);
        } else if ( note.m_type == ::MMM::NoteType::POLYLINE )
            NoteRenderSystem::renderPolyline(ctx.cache,
                                             batcher,
                                             note,
                                             config,
                                             snapshot,
                                             ctx.currentAbsY,
                                             ctx.currentTime,
                                             judgmentLineY,
                                             fallbackLeftX,
                                             topY,
                                             bottomY,
                                             fallbackTrackW,
                                             renderScaleY,
                                             curColorHead,
                                             curColorHoldBody,
                                             curColorHoldEnd,
                                             curColorNode,
                                             curColorArrow,
                                             entity,
                                             generateHitboxes,
                                             HoverPart::None,
                                             -1,
                                             laneProjection);
    }

    // 所有主体先写入批次再追加标签，避免后续音符主体把先画出的标签盖住。
    // 标签沿用主体可见列表，不再查询全谱音频绑定。
    if ( showBoundSampleLabels ) {
        const auto labelColor = audioObjectLabelColor();
        for ( auto it = visibleEntities.rbegin(); it != visibleEntities.rend();
              ++it ) {
            const auto& note = registry.get<const NoteComponent>(
                static_cast<entt::entity>(*it));
            if ( note.m_type != ::MMM::NoteType::POLYLINE ) {
                // 普通物件只有显式绑定才尝试标签渲染，空资源引用在下层被过滤。
                // 标签显示不修改绑定或触发音效播放。
                if ( note.m_sampleBinding ) {
                    renderBoundSampleLabelAt(batcher,
                                             ctx.cache,
                                             ctx.currentAbsY,
                                             ctx.noteH,
                                             *note.m_sampleBinding,
                                             note.m_timestamp,
                                             note.m_trackIndex,
                                             judgmentLineY,
                                             leftX,
                                             topY,
                                             bottomY,
                                             singleTrackW,
                                             renderScaleY,
                                             config.visual.noteScaleY,
                                             labelColor,
                                             laneProjection);
                }
                continue;
            }

            for ( std::size_t subIndex = 0; subIndex < note.m_subNotes.size();
                  ++subIndex ) {
                const auto& subNote = note.m_subNotes[subIndex];
                // 子项绑定优先，只有首子项无绑定时回退父级音效。
                // 不能让每个无绑定子段都复制父音效标签，造成整条折线重复标注。
                const auto* binding = subNote.sampleBinding
                                          ? &*subNote.sampleBinding
                                      : subIndex == 0 && note.m_sampleBinding
                                          ? &*note.m_sampleBinding
                                          : nullptr;
                if ( !binding ) continue;
                renderBoundSampleLabelAt(batcher,
                                         ctx.cache,
                                         ctx.currentAbsY,
                                         ctx.noteH,
                                         *binding,
                                         subNote.timestamp,
                                         subNote.trackIndex,
                                         judgmentLineY,
                                         leftX,
                                         topY,
                                         bottomY,
                                         singleTrackW,
                                         renderScaleY,
                                         config.visual.noteScaleY,
                                         labelColor,
                                         laneProjection);
            }
        }
    }
    // 提交最后一个尚未切纹理的批段，让后续发光与遮罩使用完整基础命令。
    // flush 只处理当前批处理状态，不等待 GPU 或其他线程。
    batcher.flush();
}

/// @brief 绘制悬浮/选中音符的发光层，并使用轨道框限制可见区域。
/// @warning
/// 热路径：悬浮/选中音符每帧绘制；只允许使用已缓存的实体列表和纹理信息。
/// @param registry 候选交互状态及 Note 组件来源。
/// @param snapshot 独立发光命令列表的输出快照。
/// @param ctx 与基础层相同的滚动锚点及玩家几何基准。
/// @param config 当前物件绘制设置。
/// @param noteEntities 已粗筛的根身份，不代表全部都需要发光。
/// @param currentTime 调用方动画时间参数，主体计算使用 ctx 锚点。
/// @param judgmentLineY 判定线像素 Y。
/// @param leftX 兼容玩家域左界。
/// @param clipLeftX 发光层实际裁剪左界。
/// @param rightX 发光层裁剪右界。
/// @param topY 发光层裁剪上界。
/// @param bottomY 发光层裁剪下界。
/// @param singleTrackW 玩家轨宽基准。
/// @param renderScaleY 当前纵向缩放。
/// @param laneProjection 可选的真实分区投影。
/// @note 悬浮可限制到部件，选中则强制覆盖整个对象。
/// @pre 选中筛选和绘制串行执行，交互组件不能在两次访问之间被其他线程移除。
/// @note 本层使用自身裁剪和命令列表，不能据普通批处理器状态推断发光裁剪。
/// @note 普通点击全体发光，Hold、Flick 与折线可按悬浮部件限制输出。
/// @note 草稿专用颜色仍优先于物件自定义槽，以保持与基础层一致的区域辨识。
void NoteRenderSystem::renderNoteGlowLayer(
    entt::registry& registry, RenderSnapshot* snapshot,
    const NoteRenderSystem::NoteRenderContext& ctx,
    const Config::EditorConfig&                config,
    const std::vector<entt::entity>& noteEntities, float currentTime,
    float judgmentLineY, float leftX, float clipLeftX, float rightX, float topY,
    float bottomY, float singleTrackW, float renderScaleY,
    const CanvasLaneProjection* laneProjection)
{
    // 先筛出真正有悬浮或选中状态的身份，无目标时不创建发光批处理器。
    // 当前列表为局部分配，容量按候选数预留，不能声称该层完全无分配。
    std::vector<entt::entity> glowEntities;
    glowEntities.reserve(noteEntities.size());
    for ( auto entity : noteEntities ) {
        /// @brief 当前可见实体的交互状态；不存在时跳过发光层。
        const auto* ic = registry.try_get<const InteractionComponent>(entity);
        if ( !ic ) continue;
        if ( ic->isHovered || ic->isSelected ) glowEntities.push_back(entity);
    }

    // 没有交互目标时不输出空发光命令，也不改变基础层批处理状态。
    // 局部筛选结束后才设置发光专属 scissor。
    if ( glowEntities.empty() ) return;

    // 发光写入独立命令列表，后续合成可应用不同效果。
    // 裁剪覆盖实际轨道区，不能让描边越过可编辑视野边界。
    Batcher glowBatcher(snapshot, &snapshot->glowCmds);
    glowBatcher.setScissor(clipLeftX, topY, rightX - clipLeftX, bottomY - topY);
    // 发光列表保留输入候选相对顺序，反向绘制与基础层保持一致。
    // 候选可能来自分桶和拖动补入，不能假定它严格按时间排列。
    for ( auto it = glowEntities.rbegin(); it != glowEntities.rend(); ++it ) {
        /// @brief 当前反向遍历到的发光音符实体。
        entt::entity entity    = *it;
        const auto&  transform = registry.get<const TransformComponent>(entity);
        const auto&  note      = registry.get<const NoteComponent>(entity);
        const auto&  ic = registry.get<const InteractionComponent>(entity);
        // 发光层逐端点复用基础层投影，避免描边回到根节点的连续轨宽。
        const float fallbackLeftX  = leftX;
        const float fallbackTrackW = singleTrackW;
        const float fallbackNoteW  = ctx.noteW;
        const float fallbackNoteH  = ctx.noteH;
        const auto  laneGeometry   = resolveNoteLaneGeometry(note.m_trackIndex,
                                                             laneProjection,
                                                             fallbackLeftX,
                                                             fallbackTrackW,
                                                             fallbackNoteW,
                                                             fallbackNoteH);
        auto        laneContext    = ctx;
        laneContext.noteW          = laneGeometry.noteW;
        laneContext.noteH          = laneGeometry.noteH;
        const auto& ctx            = laneContext;
        const float singleTrackW   = laneGeometry.width;
        const float leftX =
            laneGeometry.leftX -
            static_cast<float>(note.m_trackIndex) * singleTrackW;
        float screenY =
            judgmentLineY -
            static_cast<float>(ctx.cache->getDisplayDelta(
                note.m_timestamp, ctx.currentAbsY, note.m_timestamp)) *
                renderScaleY;
        float       visualH = static_cast<float>(ctx.cache->getDisplayDelta(
                                  note.m_timestamp + note.m_duration,
                                  ctx.cache->getAbsY(note.m_timestamp),
                                  note.m_timestamp)) *
                              renderScaleY;
        const float trackX =
            resolveNoteTrackLeftX(note, laneProjection, leftX, singleTrackW);
        HoverPart glowPart = static_cast<HoverPart>(ic.hoveredPart);
        int       glowIdx  = ic.hoveredSubIndex;
        // 选中优先于局部悬浮，将部件和子索引都恢复为全对象范围。
        // 否则被选中的折线可能只亮起当前鼠标所在的一小段。
        if ( ic.isSelected ) {
            glowPart = HoverPart::None;
            glowIdx  = -1;
        }
        // 发光颜色与基础物件的自定义槽一致，草稿继续使用专用皮肤色。
        // 不把选中统一改成固定颜色，以便保留不同物件颜色的辨识。
        glm::vec4 glowNote =
            note.m_isDraft
                ? ctx.colorDraftTap
                : resolveNoteColor(note, NoteColorSlot::Tap, ctx.colorTap);
        glm::vec4 glowHead =
            note.m_isDraft
                ? ctx.colorDraftHead
                : resolveNoteColor(note, NoteColorSlot::Head, ctx.colorHead);
        glm::vec4 glowBody =
            note.m_isDraft
                ? ctx.colorDraftHold
                : resolveNoteColor(note, NoteColorSlot::Hold, ctx.colorHold);
        glm::vec4 glowEnd =
            note.m_isDraft
                ? ctx.colorDraftEnd
                : resolveNoteColor(note, NoteColorSlot::End, ctx.colorEnd);
        glm::vec4 glowNode =
            note.m_isDraft
                ? ctx.colorDraftNode
                : resolveNoteColor(note, NoteColorSlot::Node, ctx.colorNode);
        glm::vec4 glowArrow =
            note.m_isDraft
                ? ctx.colorDraftArrow
                : resolveNoteColor(
                      note, NoteColorSlot::FlickArrow, ctx.colorArrow);

        if ( note.m_type == ::MMM::NoteType::NOTE )
            NoteRenderSystem::renderTap(
                glowBatcher,
                note,
                config,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                screenY,
                ctx.noteW,
                ctx.noteH,
                ctx.baseAspect,
                glowNote);
        else if ( note.m_type == ::MMM::NoteType::HOLD )
            NoteRenderSystem::renderHold(
                glowBatcher,
                note,
                config,
                snapshot,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                ctx.noteW,
                ctx.noteH,
                singleTrackW,
                glowHead,
                glowBody,
                glowEnd,
                ctx.cache,
                ctx.currentAbsY,
                judgmentLineY,
                renderScaleY,
                topY,
                bottomY,
                glowPart);
        else if ( note.m_type == ::MMM::NoteType::FLICK ) {
            const auto flickEndpoint =
                resolveNoteLaneGeometry(note.m_trackIndex + note.m_dtrack,
                                        laneProjection,
                                        fallbackLeftX,
                                        fallbackTrackW,
                                        fallbackNoteW,
                                        fallbackNoteH);
            NoteRenderSystem::renderFlick(
                glowBatcher,
                note,
                config,
                snapshot,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                screenY,
                ctx.noteW,
                ctx.noteH,
                flickEndpoint.centerX(),
                flickEndpoint.noteW,
                flickEndpoint.noteH,
                glowHead,
                glowBody,
                glowArrow,
                glowPart);
        } else if ( note.m_type == ::MMM::NoteType::POLYLINE )
            NoteRenderSystem::renderPolyline(ctx.cache,
                                             glowBatcher,
                                             note,
                                             config,
                                             snapshot,
                                             ctx.currentAbsY,
                                             ctx.currentTime,
                                             judgmentLineY,
                                             fallbackLeftX,
                                             topY,
                                             bottomY,
                                             fallbackTrackW,
                                             renderScaleY,
                                             glowHead,
                                             glowBody,
                                             glowEnd,
                                             glowNode,
                                             glowArrow,
                                             entity,
                                             false,
                                             glowPart,
                                             glowIdx,
                                             laneProjection);
    }
    // 在独立发光批处理器离开作用域前提交最后段命令。
    // 折线发光传入关闭命中生成，不能在此重复追加交互条目。
    glowBatcher.flush();
}

/// @brief 计算不同根对象的点和主体重叠，并生成不重复着色的顶层矩形。
/// @param registry 候选根对象及其内嵌子段来源。
/// @param snapshot 重叠矩形和顶层绘制命令的输出快照。
/// @param ctx 当前显示锚点、颜色和基准音符尺寸。
/// @param config 提供重叠判断时间容差的设置。
/// @param noteEntities 已收集的根对象候选，不要求时间有序。
/// @param judgmentLineY 判定线像素 Y。
/// @param leftX 玩家域兼容左界。
/// @param clipLeftX 统一可见区裁剪左界，包含独立草稿区域。
/// @param clipRightX 统一可见区裁剪右界。
/// @param topY 视野上界。
/// @param bottomY 视野下界。
/// @param singleTrackW 玩家基准轨宽，分区宽度以此换算尺寸。
/// @param renderScaleY 当前相机纵向缩放。
/// @param laneProjection 可选真实分区几何。
/// @warning 快照热路径存在既有局部容器分配、分组排序及遮罩网格扫描；
/// 禁止叠加全谱扫描、文件操作或阻塞同步，不能将此实现视为常量开销。
/// @note 同一折线多个子段拥有相同 owner，不作为多个独立对象计数。
/// @note 几何时间容差只用于识别重叠，不会延迟渲染或更改实际物件时间。
/// @pre 轨号跨度及 Flick 终轨计算在整数可表示范围内，基准单轨宽度非零。
/// @note 临时条目仅借用有效根身份，所有指针分组都在函数返回前销毁。
/// @note 遮罩拆分采用像素容差，谱面时间聚类采用秒容差，两者不能互换。
/// @note 同轨相近点按相邻间隔归组，不能解释为全组任意两点都在配置窗口内。
/// @note 两个横移体仅端点相触不产生主体矩形，端点关系由点检测阶段表达。
void NoteRenderSystem::renderOverlapMasks(
    entt::registry& registry, RenderSnapshot* snapshot,
    const NoteRenderSystem::NoteRenderContext& ctx,
    const Config::EditorConfig&                config,
    const std::vector<entt::entity>& noteEntities, float judgmentLineY,
    float leftX, float clipLeftX, float clipRightX, float topY, float bottomY,
    float singleTrackW, float renderScaleY,
    const CanvasLaneProjection* laneProjection)
{
    // 少于两个候选根时不存在不同 owner 的重叠，不展开单条折线自交。
    // 空快照或缓存无法输出一致屏幕坐标，直接返回。
    if ( !snapshot || !ctx.cache || noteEntities.size() < 2 ) return;

    /// @brief 将独立物件与折线子段统一为时间区间及轨道跨度。
    /// @note 所有条目构造结束后才建立指针分组，之后不得扩容 items。
    struct OverlapItem {
        /// @brief 子段几何类型，决定区间、点和横移体的检测方式。
        ::MMM::NoteType type{ ::MMM::NoteType::NOTE };
        /// @brief 物件或子段的起始秒时间。
        double startTime{ 0.0 };
        /// @brief Hold 非负持续末端，其他类型等于起点。
        double endTime{ 0.0 };
        /// @brief 统一起轨，草稿使用负编号。
        int track{ 0 };
        /// @brief Flick 的有符号终轨偏移。
        int dtrack{ 0 };
        /// @brief 所属根实体身份，用于排除折线内部自重叠。
        entt::entity owner{ entt::null };
        /// @brief 原父列表索引，独立物件为负值。
        int subIndex{ -1 };
    };

    /// @brief 把头尾及箭头端点表示为可参与主体相交的点标记。
    struct OverlapPoint {
        /// @brief 端点秒时间，按当前滚动缓存转为 Y。
        double time{ 0.0 };
        /// @brief 端点统一轨号，可能是 Flick 终轨。
        int track{ 0 };
        /// @brief 端点所属根身份，不拥有组件。
        entt::entity owner{ entt::null };
        /// @brief 相对基准音符的点遮罩缩放。
        float scale{ 1.0f };
        /// @brief 该端点是否参与与其他 Flick 横向主体的相交检测。
        bool testsFlickBody{ false };
    };

    /// @brief 按类型生成非负的逻辑时间区间末端。
    /// @param startTime 起始秒时间。
    /// @param duration 源持续值，负值按零处理。
    /// @param type 仅 Hold 使用持续跨度。
    /// @return Hold 的非负持续末端，其他类型返回起始时间。
    /// @warning 每次展开局部候选调用，保持纯数值计算。
    auto makeEndTime =
        [](double startTime, double duration, ::MMM::NoteType type) {
            if ( type != ::MMM::NoteType::HOLD ) return startTime;
            return startTime + std::max(0.0, duration);
        };

    // 先按值展开几何，后续分组只借用 items 元素地址。
    // 子实体不单独加入，避免同一个子段同时从父数组和 ECS 重复展开。
    std::vector<OverlapItem> items;
    items.reserve(noteEntities.size());
    for ( auto entity : noteEntities ) {
        const auto& note = registry.get<const NoteComponent>(entity);
        if ( note.m_isSubNote ) continue;

        // 折线每个局部段独立参与几何检测，但统一保留父 owner。
        // 这样能够与外部物件相交，同时排除自身相邻连接点的假阳性。
        if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
            for ( size_t i = 0; i < note.m_subNotes.size(); ++i ) {
                const auto& sub = note.m_subNotes[i];
                items.push_back(
                    { sub.type,
                      sub.timestamp,
                      makeEndTime(sub.timestamp, sub.duration, sub.type),
                      sub.trackIndex,
                      sub.dtrack,
                      entity,
                      static_cast<int>(i) });
            }
            continue;
        }

        items.push_back(
            { note.m_type,
              note.m_timestamp,
              makeEndTime(note.m_timestamp, note.m_duration, note.m_type),
              note.m_trackIndex,
              note.m_dtrack,
              entity,
              -1 });
    }

    // 两个根候选也可能只展开出空折线，数量门禁不能代替展开结果检查。
    // 无几何条目时不再分配分类列表、轨桶或遮罩网格。
    if ( items.empty() ) return;

    // 用户配置以毫秒表示，转为秒后和谱面时间直接比较。
    // 负配置钳制到零，小浮点误差由各检测分支自己的 timeEpsilon 处理。
    const double windowSeconds =
        static_cast<double>(
            std::max(0.0f, config.settings.overlapTimeWindowMs)) *
        0.001;
    constexpr double timeEpsilon = 1e-7;

    /// @brief 将专用纹理的 UV 宽高换算为相对 Note 基准的像素尺寸。
    /// @param id 主体纹理身份。
    /// @param baseW 基准音符宽度。
    /// @param baseH 基准音符高度。
    /// @return 缺少纹理时退回基准尺寸；存在时按宽高分别缩放。
    auto textureSize = [snapshot](TextureID id, float baseW, float baseH) {
        auto itBase =
            snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Note));
        // 基准纹理缺失时无法建立相对比例，保持传入尺寸作为兼容回退。
        // 专用纹理存在也不能绕过基准宽高来源。
        if ( itBase == snapshot->uvMap.end() ) return glm::vec2(baseW, baseH);

        auto it = snapshot->uvMap.find(static_cast<uint32_t>(id));
        if ( it == snapshot->uvMap.end() ) return glm::vec2(baseW, baseH);

        return glm::vec2(baseW * (it->second.z / itBase->second.z),
                         baseH * (it->second.w / itBase->second.w));
    };

    // 两个方向的主体各取自己的纹理比例，不把竖向宽度套到横向高度。
    // 这里先求玩家基准，绘制每段遮罩时再乘对应领域轨宽比例。
    const glm::vec2 verticalBodySize =
        textureSize(TextureID::HoldBodyVertical, ctx.noteW, ctx.noteH);
    const glm::vec2 horizontalBodySize =
        textureSize(TextureID::HoldBodyHorizontal, ctx.noteW, ctx.noteH);

    /// @brief 用当前显示锚点和采样时间自身 HS 把逻辑时间投影到屏幕。
    /// @param time 需要投影的秒时间。
    /// @warning 本地几何热路径，禁止读取系统时钟或重建滚动缓存。
    auto timeToY = [&](double time) {
        return judgmentLineY - static_cast<float>(ctx.cache->getDisplayDelta(
                                   time, ctx.currentAbsY, time)) *
                                   renderScaleY;
    };
    // 遮罩必须复用音符所在区域的真实单轨几何，而不是玩家轨道宽度。
    /// @brief 解析统一轨号对应的实际分区几何。
    /// @param track 允许负草稿编号的轨道。
    /// @return 实际轨道左界与宽度，缺少分区投影时使用玩家兼容公式。
    const auto laneForTrack = [&](std::int32_t track) {
        return resolveNoteLaneGeometry(
            track, laneProjection, leftX, singleTrackW);
    };
    /// @brief 返回当前轨宽相对玩家基准的尺寸比例。
    /// @param track 要换算的统一轨号。
    const auto laneScaleForTrack = [&](std::int32_t track) {
        return laneForTrack(track).width / singleTrackW;
    };

    /// @brief 判断两个几何条目是否来自同一个有效根对象。
    /// @note 空身份不被视为共同根；正常条目由候选实体展开。
    /// @param a 第一个独立物件或子段条目。
    /// @param b 第二个条目。
    /// @return 两者共享非空根身份时为 true。
    auto sameOwner = [](const OverlapItem& a, const OverlapItem& b) {
        return a.owner != entt::null && a.owner == b.owner;
    };

    /// @brief 对指定半开分组范围按根身份计数。
    /// @param group 持有 items 元素观察指针的分组。
    /// @param begin 分组起始索引。
    /// @param end 分组结束后的索引。
    /// @warning 当前每组构造局部哈希集合，开销与组内段数相关。
    /// @pre begin 与 end 是 group 的有效半开区间，指针来自稳定的 items 存储。
    /// @return 不同根数量，多个子段属于同根时只计一次。
    auto countUniqueOwners = [](const std::vector<const OverlapItem*>& group,
                                size_t                                 begin,
                                size_t                                 end) {
        std::unordered_set<entt::entity> owners;
        // 预留组内条目数是根数量的上界，同根子段合并后实际集合更小。
        // 集合只为本组计数服务，返回后不保留对局部指针的引用。
        owners.reserve(end - begin);
        for ( size_t i = begin; i < end; ++i ) {
            const auto* item = group[i];
            owners.insert(item->owner);
        }
        return static_cast<int>(owners.size());
    };

    /// @brief 追加可能与视野相交的有效重叠矩形。
    /// @param x 矩形左上角 X。
    /// @param y 矩形左上角 Y。
    /// @param w 正宽度。
    /// @param h 正高度。
    /// @param count 至少为二的不同对象数。
    /// @note 只追加当前快照列表，调用前既有遮罩也会参与后续拆分。
    /// @warning 本地集合追加可扩容，不得在此加载纹理或操作 Registry。
    auto appendMask = [&](float x, float y, float w, float h, int count) {
        if ( w <= 0.0f || h <= 0.0f || count < 2 ) return;
        // 这里只剔除完全位于裁剪区之外的矩形，不提前裁小相交矩形。
        // 实际裁剪由顶层批处理器的 scissor 保证，网格拆分仍读取完整矩形边界。
        if ( x > clipRightX || x + w < clipLeftX || y > bottomY ||
             y + h < topY )
            return;

        // 这里不能预先合并成外接矩形，否则相邻遮罩之间的空区域也会被误涂红。
        // 后面的扫描线拆分会负责去掉重复覆盖并保证同一像素最多只画一层。
        snapshot->overlapMasks.push_back({ x, y, w, h, count });
    };

    /// @brief 以物件点为中心生成按领域缩放的矩形遮罩。
    /// @param time 点的逻辑时间。
    /// @param track 点所在轨道。
    /// @param scale 额外点尺寸倍率。
    /// @param count 检测到的不同根数量。
    /// @note 宽高同时乘领域比例和点比例，Y 中心来自同一显示时间转换。
    /// @warning 每个命中点即时生成几何，不引入交互等待。
    auto appendPointMask = [&](double time, int track, float scale, int count) {
        const auto  lane      = laneForTrack(track);
        const float laneScale = lane.width / singleTrackW;
        const float w         = ctx.noteW * laneScale * scale;
        const float h         = ctx.noteH * laneScale * scale;
        const float x         = lane.leftX + (lane.width - w) * 0.5F;
        const float y         = timeToY(time) - h * 0.5F;
        appendMask(x, y, w, h, count);
    };

    // 分类列表借用已完成构造的 items 存储，不能在分类后再向 items 追加。
    // 点标记按值保存，后续分桶和排序不会改变原几何条目地址。
    std::vector<const OverlapItem*> notes;
    std::vector<const OverlapItem*> holds;
    std::vector<const OverlapItem*> flicks;
    std::vector<OverlapPoint>       pointMarkers;
    bool                            hasBucketTrack = false;
    int                             minBucketTrack = 0;
    int                             maxBucketTrack = 0;
    notes.reserve(items.size());
    holds.reserve(items.size());
    flicks.reserve(items.size());
    pointMarkers.reserve(items.size() * 2);

    /// @brief 扩张本次候选端点覆盖的统一轨道范围。
    /// @param track 起点或 Flick 终点轨号。
    /// @note 首个轨道建立范围，不能以零初始化后强行包含不相关玩家轨。
    /// @warning 仅更新整数极值，不做轨道分配或全谱查询。
    auto includeBucketTrack = [&](int track) {
        // 第一次收集使用实际轨号初始化上下界，纯负草稿轨不被零值扩大范围。
        // hasBucketTrack 同时表示后续桶数量计算已有合法起点。
        if ( !hasBucketTrack ) {
            minBucketTrack = track;
            maxBucketTrack = track;
            hasBucketTrack = true;
            return;
        }

        minBucketTrack = std::min(minBucketTrack, track);
        maxBucketTrack = std::max(maxBucketTrack, track);
    };

    for ( const auto& item : items ) {
        if ( item.type == ::MMM::NoteType::NOTE ) {
            notes.push_back(&item);
            pointMarkers.push_back(
                { item.startTime, item.track, item.owner, 1.0f, true });
            includeBucketTrack(item.track);
        } else if ( item.type == ::MMM::NoteType::HOLD ) {
            // 只有正长度 Hold 进入主体相交列表，退化长条仍保留头部点。
            // 正长度 Hold 另加尾点，避免只检查持续内部而漏掉端点叠放。
            if ( item.endTime > item.startTime + timeEpsilon ) {
                holds.push_back(&item);
                pointMarkers.push_back(
                    { item.endTime, item.track, item.owner, 1.0f, true });
            }
            pointMarkers.push_back(
                { item.startTime, item.track, item.owner, 1.0f, true });
            includeBucketTrack(item.track);
        } else if ( item.type == ::MMM::NoteType::FLICK ) {
            // Flick 起点不参与其他 Flick 主体的点检测，终点则参与。
            // 两个端点都进入点分组，可继续检测同轨同时间的头尾重叠。
            flicks.push_back(&item);
            pointMarkers.push_back(
                { item.startTime, item.track, item.owner, 1.0f, false });
            pointMarkers.push_back({ item.startTime,
                                     item.track + item.dtrack,
                                     item.owner,
                                     1.0f,
                                     true });
            includeBucketTrack(item.track);
            includeBucketTrack(item.track + item.dtrack);
        }
    }

    // 不支持的条目类型不会贡献桶轨号，全部不支持时直接结束。
    // 避免以默认零上下界构造一个没有真实候选的玩家轨桶。
    if ( !hasBucketTrack ) return;

    /// @brief 把允许为负的统一轨号平移为连续数组索引。
    /// @pre track 位于已收集的最小与最大轨道闭区间。
    /// @return 相对 minBucketTrack 的数组下标。
    auto bucketIndex = [&](int track) {
        return static_cast<size_t>(track - minBucketTrack);
    };

    // 按实际候选跨度分配轨桶，因此可同时容纳草稿负轨与玩家正轨。
    // 这里只分配有界连续轨号空间，使用者必须维持合理的轨道范围。
    size_t bucketCount =
        static_cast<size_t>(maxBucketTrack - minBucketTrack + 1);
    std::vector<std::vector<const OverlapItem*>> notesByTrack(bucketCount);
    std::vector<std::vector<const OverlapItem*>> holdsByTrack(bucketCount);
    std::vector<std::vector<OverlapPoint>> pointMarkersByTrack(bucketCount);

    // 每个普通点击只进入起轨桶，时间重叠判断在桶内执行。
    // Flick 的跨轨主体保留独立列表，不按每条经过轨复制整个几何条目。
    for ( const auto* note : notes ) {
        notesByTrack[bucketIndex(note->track)].push_back(note);
    }
    for ( const auto* hold : holds ) {
        holdsByTrack[bucketIndex(hold->track)].push_back(hold);
    }
    // 先检测任意端点是否落在另一根 Hold 的严格内部。
    // 长条头尾相接由点组覆盖，此处排除边界以免重复计算主体相交。
    for ( const auto& point : pointMarkers ) {
        pointMarkersByTrack[bucketIndex(point.track)].push_back(point);
    }

    /// @brief 先按起始时间、再按结束时间稳定定义几何排序关系。
    /// @note 同时间的不同 owner 无需额外排序，计数按身份集合消重。
    /// @param a 时间比较左项，必须非空。
    /// @param b 时间比较右项，必须非空。
    /// @return 起点优先、终点次优先的严格小于关系。
    auto itemStartLess = [](const OverlapItem* a, const OverlapItem* b) {
        if ( a->startTime != b->startTime ) {
            return a->startTime < b->startTime;
        }
        return a->endTime < b->endTime;
    };
    /// @brief 仅在当前局部分组无序时执行时间排序。
    /// @warning
    /// 每次快照检查局部分组；既有无序分支会排序，不保证完全无排序开销。
    /// @param bucket 同轨物件或当前候选 Flick 的观察指针列表。
    /// @note 只排序指针，不移动 items 中的原始几何条目。
    auto ensureItemTimeOrder = [&](std::vector<const OverlapItem*>& bucket) {
        // 零或单条目天然有序，无需比较器和排序调用。
        // 对较长列表先检查顺序，保留来源已经有序时的线性快路径。
        if ( bucket.size() < 2 ) return;
        if ( std::is_sorted(bucket.begin(), bucket.end(), itemStartLess) )
            return;
        std::sort(bucket.begin(), bucket.end(), itemStartLess);
    };
    /// @brief 为点标记建立时间升序以支持相邻聚类。
    /// @note 点值可移动，排序不影响 items 观察指针的有效性。
    /// @param bucket 同轨端点列表，按时间原地排列。
    /// @warning 非有序输入会触发局部排序，禁止改为全谱排序。
    auto ensurePointTimeOrder = [](std::vector<OverlapPoint>& bucket) {
        if ( bucket.size() < 2 ) return;
        /// @brief 仅按点时间比较，等时间端点无需按根身份排序。
        /// @return 左点早于右点时为 true。
        auto pointTimeLess = [](const OverlapPoint& a, const OverlapPoint& b) {
            return a.time < b.time;
        };
        if ( std::is_sorted(bucket.begin(), bucket.end(), pointTimeLess) )
            return;
        std::sort(bucket.begin(), bucket.end(), pointTimeLess);
    };

    // 先检测同轨普通点击，轨道分桶避免比较不可能重叠的横向位置。
    // 聚类按相邻时间差连接，组总跨度可以大于单次容差窗口。
    for ( auto& trackNotes : notesByTrack ) {
        if ( trackNotes.size() < 2 ) continue;
        ensureItemTimeOrder(trackNotes);

        for ( size_t i = 0; i < trackNotes.size(); ) {
            // 聚类边界采用半开区间，j 指向当前组之后的首个条目。
            // 组处理后直接跳到 j，避免同一点击从多个起点重复归组。
            size_t j = i + 1;
            // 普通点击分支使用严格小于窗口，边界等于配置窗口时不合并本组。
            // 后面的统一点分组另带 epsilon，两种分支的边界规则并不完全相同。
            while ( j < trackNotes.size() &&
                    trackNotes[j]->startTime - trackNotes[j - 1]->startTime <
                        windowSeconds ) {
                ++j;
            }

            if ( j - i >= 2 ) {
                // 分别聚合组内时间极值，矩形覆盖相邻接近形成的整段链。
                // 显示位置可能逆向，因此转屏幕后仍取两个 Y 的最小值与绝对差。
                double minTime = trackNotes[i]->startTime;
                double maxTime = trackNotes[i]->startTime;
                for ( size_t k = i; k < j; ++k ) {
                    minTime = std::min(minTime, trackNotes[k]->startTime);
                    maxTime = std::max(maxTime, trackNotes[k]->startTime);
                }

                // 同一折线的多个 NOTE 子段可能属于同组，但只算一个根对象。
                // 至少两个独立根才产生红色遮罩。
                int uniqueCount = countUniqueOwners(trackNotes, i, j);
                if ( uniqueCount >= 2 ) {
                    const auto  lane       = laneForTrack(trackNotes[i]->track);
                    const float laneScale  = lane.width / singleTrackW;
                    const float maskWidth  = ctx.noteW * laneScale;
                    const float maskHeight = ctx.noteH * laneScale;
                    const float y0         = timeToY(minTime);
                    const float y1         = timeToY(maxTime);
                    const float x =
                        lane.leftX + (lane.width - maskWidth) * 0.5F;
                    const float y = std::min(y0, y1) - maskHeight * 0.5F;
                    // 时间跨度之外再加一颗音符高度，保留首尾纹理的外半边。
                    // 只使用中心之间距离会把两端重叠提示裁短。
                    const float h = std::abs(y0 - y1) + maskHeight;
                    appendMask(x, y, maskWidth, h, uniqueCount);
                }
            }

            i = j;
        }
    }

    // 统一点组涵盖点击、长条头尾和箭头端点。
    // 遮罩保留组内最大点尺寸，并用不同 owner 数而不是点数计数。
    for ( auto& trackPoints : pointMarkersByTrack ) {
        if ( trackPoints.size() < 2 ) continue;
        ensurePointTimeOrder(trackPoints);

        for ( size_t i = 0; i < trackPoints.size(); ) {
            size_t j = i + 1;
            while ( j < trackPoints.size() &&
                    trackPoints[j].time - trackPoints[j - 1].time <=
                        windowSeconds + timeEpsilon ) {
                ++j;
            }

            if ( j - i >= 2 ) {
                // 点组可能同时包含同根头尾或相邻折线端点，以身份集合消除重复。
                // 数量门禁使用去重结果，不以 j-i 判断独立对象数。
                std::unordered_set<entt::entity> owners;
                double                           minTime = trackPoints[i].time;
                double                           maxTime = trackPoints[i].time;
                // 相邻时间聚类可能链式扩张，矩形包住组的最早与最晚端点。
                // 最大缩放保证大端点边缘不会被较小端点的尺寸裁掉。
                float maxScale = trackPoints[i].scale;
                for ( size_t k = i; k < j; ++k ) {
                    owners.insert(trackPoints[k].owner);
                    minTime  = std::min(minTime, trackPoints[k].time);
                    maxTime  = std::max(maxTime, trackPoints[k].time);
                    maxScale = std::max(maxScale, trackPoints[k].scale);
                }

                if ( owners.size() >= 2 ) {
                    const auto  lane      = laneForTrack(trackPoints[i].track);
                    const float laneScale = lane.width / singleTrackW;
                    const float w         = ctx.noteW * laneScale * maxScale;
                    const float h0        = ctx.noteH * laneScale * maxScale;
                    const float y0        = timeToY(minTime);
                    const float y1        = timeToY(maxTime);
                    const float x = lane.leftX + (lane.width - w) * 0.5F;
                    const float y = std::min(y0, y1) - h0 * 0.5F;
                    const float h = std::abs(y0 - y1) + h0;
                    appendMask(x, y, w, h, static_cast<int>(owners.size()));
                }
            }

            i = j;
        }
    }

    // 同轨 Hold 主体重叠以时间区间交集为准，不使用点击的近邻窗口。
    // 先收集所有起终边界，把活跃集合变化限制在这些分割点上。
    for ( auto& trackHolds : holdsByTrack ) {
        if ( trackHolds.size() < 2 ) continue;
        ensureItemTimeOrder(trackHolds);

        int                 track = trackHolds.front()->track;
        std::vector<double> bounds;
        // 每个有效长条贡献一个起点和一个终点，预留两倍条目数。
        // 边界去重只影响切片数量，不删除任何 Hold 候选。
        bounds.reserve(trackHolds.size() * 2);

        for ( const auto* hold : trackHolds ) {
            bounds.push_back(hold->startTime);
            bounds.push_back(hold->endTime);
        }
        // 排序并消除极近边界，避免产生浮点噪声导致的极窄时间片。
        // 这份边界列表属于单轨候选，不能扩展成全部谱面的时间重排。
        std::sort(bounds.begin(), bounds.end());
        bounds.erase(std::unique(bounds.begin(),
                                 bounds.end(),
                                 [](double a, double b) {
                                     return std::abs(a - b) < 1e-7;
                                 }),
                     bounds.end());

        // 连续有重叠的时间片暂时合为一条开放遮罩。
        // openCount
        // 保存该连续范围遇到的最大根数量，不代表每个像素都具有相同密度。
        bool   hasOpenMask = false;
        double openStart   = 0.0;
        double openEnd     = 0.0;
        int    openCount   = 0;

        /// @brief 将当前连续 Hold 重叠区间转为屏幕矩形并关闭。
        /// @note 正反向滚动均取较小 Y 和绝对高度，不生成负尺寸矩形。
        /// @warning 同轨区间扫描期间调用，只把当前开放片段变为矩形。
        auto flushOpenMask = [&]() {
            if ( !hasOpenMask ) return;
            const auto  lane      = laneForTrack(track);
            const float laneScale = lane.width / singleTrackW;
            const float bodyWidth = verticalBodySize.x * laneScale;
            const float y0        = timeToY(openStart);
            const float y1        = timeToY(openEnd);
            const float x = lane.leftX + (lane.width - bodyWidth) * 0.5F;
            appendMask(
                x, std::min(y0, y1), bodyWidth, std::abs(y0 - y1), openCount);
            // 封闭后清除活动标志与计数，下一段不能继承之前的重叠强度。
            // 时间端点无需清零，因为只有活动标志为真时才读取它们。
            hasOpenMask = false;
            openCount   = 0;
        };

        for ( size_t k = 0; k + 1 < bounds.size(); ++k ) {
            double segStart = bounds[k];
            double segEnd   = bounds[k + 1];
            // 零长度或极短片段不具备主体面积，留给端点重叠处理。
            // 主体判断排除仅在同一时间相接的两个长条。
            if ( segEnd <= segStart + timeEpsilon ) continue;

            // 同一根折线的多段可能覆盖同一时间片，必须按 owner 合并活跃集合。
            // 分组已按起点排序，起点达到片段末端后可提前停止扫描。
            std::unordered_set<entt::entity> activeOwners;
            for ( const auto* hold : trackHolds ) {
                // 起点已不早于当前片段末端时，后续有序条目也不可能覆盖片段内部。
                // 使用严格内部重叠，单纯首尾接触不会增加主体活跃数。
                if ( hold->startTime >= segEnd - timeEpsilon ) break;
                if ( hold->endTime > segStart + timeEpsilon ) {
                    activeOwners.insert(hold->owner);
                }
            }

            int activeCount = static_cast<int>(activeOwners.size());
            if ( activeCount >= 2 ) {
                // 第一个至少双根覆盖片段开启遮罩，起点取当前切片左边界。
                // 直到覆盖数降到二以下才封闭，避免相邻交集之间出现像素接缝。
                if ( !hasOpenMask ) {
                    hasOpenMask = true;
                    openStart   = segStart;
                    openEnd     = segEnd;
                    openCount   = activeCount;
                } else {
                    openEnd = segEnd;
                    // 连续片段即使重叠数量变化也保持同一开放几何范围。
                    // 后续遮罩去重使用最大计数，不能把这里当作精确密度分段输出。
                    openCount = std::max(openCount, activeCount);
                }
            } else {
                flushOpenMask();
            }
        }
        flushOpenMask();
    }

    ensureItemTimeOrder(flicks);

    /// @brief 取得横移体两端中较小轨号，兼容负轨差。
    /// @param item 具有有符号轨差的横移条目。
    /// @return 起轨和终轨中的最小值。
    auto flickBodyMinTrack = [](const OverlapItem& item) {
        return std::min(item.track, item.track + item.dtrack);
    };
    /// @brief 取得横移体两端中较大轨号，供闭区间比较。
    /// @param item 具有有符号轨差的横移条目。
    /// @return 起轨和终轨中的最大值。
    auto flickBodyMaxTrack = [](const OverlapItem& item) {
        return std::max(item.track, item.track + item.dtrack);
    };

    // 横向主体只比较时间窗口内的后续 Flick，每对最多检查一次。
    // 零轨差无连接体，同根子段的连接不构成物件间重叠。
    for ( size_t i = 0; i < flicks.size(); ++i ) {
        const auto& a = *flicks[i];
        if ( a.dtrack == 0 ) continue;

        for ( size_t j = i + 1; j < flicks.size(); ++j ) {
            const auto& b = *flicks[j];
            // Flick 列表已按起点排序，越过时间上界便可结束当前配对循环。
            // 窗口内仍需检查不同 owner 和正长度横向交集。
            if ( b.startTime - a.startTime > windowSeconds + timeEpsilon )
                break;
            if ( b.dtrack == 0 || sameOwner(a, b) ) continue;

            int overlapMin =
                std::max(flickBodyMinTrack(a), flickBodyMinTrack(b));
            int overlapMax =
                std::min(flickBodyMaxTrack(a), flickBodyMaxTrack(b));
            // 轨道交集只有一个端点时不生成横向主体面积。
            // 端点相遇由点标记检测负责，不用零宽矩形重复表达。
            if ( overlapMax <= overlapMin ) continue;

            // 相交区两端按真实轨道中心构造宽度，独立分区间隙也计入横移体。
            // 高度采用相交起轨所在域的横向主体纹理比例。
            const auto  startLane   = laneForTrack(overlapMin);
            const auto  endLane     = laneForTrack(overlapMax);
            const float startCenter = startLane.leftX + startLane.width * 0.5F;
            const float endCenter   = endLane.leftX + endLane.width * 0.5F;
            const float bodyHeight =
                horizontalBodySize.y * laneScaleForTrack(overlapMin);
            const float y0 = timeToY(a.startTime);
            const float y1 = timeToY(b.startTime);
            const float x  = std::min(startCenter, endCenter);
            const float w  = std::abs(endCenter - startCenter);
            const float y  = std::min(y0, y1) - bodyHeight * 0.5F;
            const float h  = std::abs(y0 - y1) + bodyHeight;
            // 当前记录只描述一对独立横移根，因此写入计数二。
            // 多组配对的空间重合随后取最大提示值，不按配对次数累加 alpha。
            appendMask(x, y, w, h, 2);
        }
    }

    // 越界轨道借用同一个空桶，避免为每次查询创建临时向量。
    // 返回引用仅在当前函数内使用，不能保存到快照中。
    const std::vector<const OverlapItem*> emptyHoldBucket;
    /// @brief 获取指定轨道的 Hold 列表，范围外返回共享局部空桶。
    /// @param track 查询的统一轨号。
    /// @return 当前轨道的有序观察指针列表。
    auto getHoldBucket =
        [&](int track) -> const std::vector<const OverlapItem*>& {
        // 跨轨检测可能查询候选范围外位置，先判界再计算无符号下标。
        // 不能把负的相对轨差直接转换为 size_t 后索引。
        if ( track < minBucketTrack || track > maxBucketTrack ) {
            return emptyHoldBucket;
        }
        return holdsByTrack[bucketIndex(track)];
    };

    for ( const auto& point : pointMarkers ) {
        std::unordered_set<entt::entity> owners;
        // 点所属物件也参与重叠数量，后续加入另一根后才达到提示阈值。
        // 同根长条被跳过，避免一个物件自己的端点落在自身主体上被计作二重。
        owners.insert(point.owner);

        for ( const auto* hold : getHoldBucket(point.track) ) {
            if ( point.time <= hold->startTime + timeEpsilon ) break;
            if ( hold->owner == point.owner ) continue;
            // 仅长条严格内部与点重叠时加入 owner，末端接触留给点组。
            // 不同长条子段共享根时集合仍只增加一个身份。
            if ( point.time < hold->endTime - timeEpsilon ) {
                owners.insert(hold->owner);
            }
        }

        if ( owners.size() >= 2 ) {
            appendPointMask(point.time,
                            point.track,
                            point.scale,
                            static_cast<int>(owners.size()));
        }
    }

    for ( const auto& point : pointMarkers ) {
        // 只有允许测试横移体的标记进入这一阶段。
        // 起点和终点标记的不同许可在展开时决定，不能在此统一放开。
        if ( !point.testsFlickBody ) continue;

        std::unordered_set<entt::entity> owners;
        owners.insert(point.owner);

        // 按点时间减窗口二分定位首个可能相交的 Flick。
        // 扫描在点时间加窗口之后结束，避免每个点遍历全部横移体。
        auto firstFlick =
            std::lower_bound(flicks.begin(),
                             flicks.end(),
                             point.time - windowSeconds - timeEpsilon,
                             [](const OverlapItem* item, double time) {
                                 return item->startTime < time;
                             });

        for ( auto it = firstFlick; it != flicks.end(); ++it ) {
            const auto* flick = *it;
            // 从 lower_bound 定位下界后按升序扫描，越过上界立即停止。
            // 边界两侧使用同一个窗口和 epsilon，避免搜索与过滤条件不一致。
            if ( flick->startTime > point.time + windowSeconds + timeEpsilon )
                break;
            if ( flick->owner == point.owner || flick->dtrack == 0 ) continue;

            int minTrack = flickBodyMinTrack(*flick);
            int maxTrack = flickBodyMaxTrack(*flick);
            // 点与 Flick 主体检测使用闭合轨道区间，端点也可能算相遇。
            // 同根过滤先于此判断，避免折线自身箭头或节点形成假遮罩。
            if ( point.track < minTrack || point.track > maxTrack ) continue;

            owners.insert(flick->owner);
        }

        if ( owners.size() >= 2 ) {
            appendPointMask(point.time,
                            point.track,
                            point.scale,
                            static_cast<int>(owners.size()));
        }
    }

    for ( const auto* flick : flicks ) {
        if ( flick->dtrack == 0 ) continue;

        // 最后检测横移体穿过其他 Hold 内部，按交叉轨道分别积累根身份。
        // 一条长 Flick 可跨多轨，不把所有相交位置合成一个宽矩形。
        std::unordered_map<int, std::unordered_set<entt::entity>> ownersByTrack;
        int bodyMinTrack = flickBodyMinTrack(*flick);
        int bodyMaxTrack = flickBodyMaxTrack(*flick);
        // 检测范围取横移轨跨度与已有轨桶的交集。
        // 不存在 Hold 的外部轨道无需逐轨查询，但保留跨过中间实际轨道的检测。
        int checkedMinTrack = std::max(bodyMinTrack, minBucketTrack);
        int checkedMaxTrack = std::min(bodyMaxTrack, maxBucketTrack);

        for ( int track = checkedMinTrack; track <= checkedMaxTrack; ++track ) {
            for ( const auto* hold : getHoldBucket(track) ) {
                if ( flick->startTime <= hold->startTime + timeEpsilon ) break;
                if ( sameOwner(*flick, *hold) ) continue;
                // 横移时间必须落在长条严格内部，刚好结束时不生成主体交叉点。
                // 起点处已由前一分支排除，头尾相接由端点检测覆盖。
                if ( flick->startTime >= hold->endTime - timeEpsilon ) continue;

                // 只在确实命中长条时建立该轨集合，避免为每条经过轨创建空集合。
                // 当前 Flick owner 与所有相交 Hold owner 共同组成该轨提示数量。
                auto& owners = ownersByTrack[hold->track];
                owners.insert(flick->owner);
                owners.insert(hold->owner);
            }
        }

        // 交叉点使用缩小的点遮罩，突出主体交叉而不过度覆盖整颗音符。
        // 每轨计数保留横移根及所有相交长条根，重复子段不增加数量。
        for ( const auto& [track, owners] : ownersByTrack ) {
            appendPointMask(
                flick->startTime, track, 0.7f, static_cast<int>(owners.size()));
        }
    }

    // 没有任何有效重叠几何时跳过坐标网格和顶层批处理。
    // 条件发生在各类检测结束后，点和主体提示共用同一出口。
    if ( snapshot->overlapMasks.empty() ) return;

    /// @brief 按所有矩形边界拆成网格，输出没有重复覆盖的水平条带。
    /// @note 每个网格采用输入遮罩最大计数，不叠加多个检测路径的相同对象。
    /// @warning 当前实现排序局部坐标并逐格扫描遮罩，复杂度随候选交点增加。
    /// @note 仅合并同一条带内的水平连续单元，不把不规则形状转成外接矩形。
    auto flattenOverlapMasks = [&]() {
        // 单个矩形没有重复覆盖，无需建立二维边界网格。
        // 多个检测阶段可能描述同一片区域，不能直接逐个透明叠加。
        if ( snapshot->overlapMasks.size() < 2 ) return;

        // 水平与垂直切分边界由所有有效矩形的两端组成。
        // 网格单元内部不会再跨越输入矩形边缘，可用一个内部采样点判断覆盖。
        std::vector<float> xs;
        std::vector<float> ys;
        xs.reserve(snapshot->overlapMasks.size() * 2);
        ys.reserve(snapshot->overlapMasks.size() * 2);

        for ( const auto& mask : snapshot->overlapMasks ) {
            // 退化矩形不贡献切分边界，避免零面积输入膨胀扫描网格。
            // 有效矩形使用完整范围，裁剪仍由后续批次统一处理。
            if ( mask.w <= 0.0f || mask.h <= 0.0f ) continue;
            xs.push_back(mask.x);
            xs.push_back(mask.x + mask.w);
            ys.push_back(mask.y);
            ys.push_back(mask.y + mask.h);
        }

        /// @brief 排序边界坐标并去掉小于百分之一像素的相邻差值。
        /// @param coords 当前轴的切分边界。
        /// @note 容差用于抑制浮点裂缝，不作为逻辑轨道或时间吸附规则。
        /// @warning 当前遮罩集合的局部坐标排序，不允许扩展到所有物件坐标。
        auto uniqueCoords = [](std::vector<float>& coords) {
            std::sort(coords.begin(), coords.end());
            coords.erase(std::unique(coords.begin(),
                                     coords.end(),
                                     [](float a, float b) {
                                         return std::abs(a - b) < 0.01f;
                                     }),
                         coords.end());
        };

        uniqueCoords(xs);
        uniqueCoords(ys);
        // 任一轴不足两个边界时无法组成正面积网格，保持原遮罩结果。
        // 正常有效矩形至少提供每轴两端，此分支处理退化输入。
        if ( xs.size() < 2 || ys.size() < 2 ) return;

        // 先写独立结果，不在扫描原遮罩期间改变其数量或地址。
        // 完成后整体替换快照列表，渲染端只看最终互不重叠的几何。
        std::vector<RenderSnapshot::OverlapMask> flattened;
        flattened.reserve(snapshot->overlapMasks.size());

        // 将重叠矩形拆成互不相交的扫描线小矩形，避免同一像素被红色滤镜重复覆盖。
        for ( size_t yIndex = 0; yIndex + 1 < ys.size(); ++yIndex ) {
            const float y0 = ys[yIndex];
            const float y1 = ys[yIndex + 1];
            // 极薄 Y 条带不进入内部 X 扫描，避免数值噪声放大成大量小矩形。
            // 有效条带边界固定后，整行网格共用同一高度。
            if ( y1 <= y0 + 0.01f ) continue;

            // 同一 Y 条带内把相邻且计数相同的网格合并成水平连续段。
            // 不同 Y 条带不在这里纵向合并，避免增加形状合并的状态复杂度。
            bool  hasRun   = false;
            float runStart = 0.0f;
            int   runCount = 0;
            /// @brief 在指定右边界封闭当前水平连续遮罩。
            /// @param runEnd 当前段右边界，不包含之后的网格。
            /// @note 写入后清除开放状态，后续可从不同计数重新起段。
            /// @warning 网格行内调用，仅追加当前水平段，不访问实体或纹理。
            auto flushRun = [&](float runEnd) {
                // 空状态或极短水平跨度不追加矩形，避免亚像素碎片。
                // 输出高度由当前 Y 条带定义，始终与水平连续段共享同一上下界。
                if ( !hasRun || runEnd <= runStart + 0.01f ) return;
                flattened.push_back(
                    { runStart, y0, runEnd - runStart, y1 - y0, runCount });
                hasRun   = false;
                runCount = 0;
            };

            for ( size_t xIndex = 0; xIndex + 1 < xs.size(); ++xIndex ) {
                const float x0 = xs[xIndex];
                const float x1 = xs[xIndex + 1];
                // 极窄 X 单元被忽略，沿用坐标消重相同的像素阈值。
                // 采样点因此始终位于一个具有实际面积的单元内部。
                if ( x1 <= x0 + 0.01f ) continue;

                // 取网格中点而非边缘，避免边界同时属于相邻矩形时产生归属歧义。
                // 所有输入边界已进入网格，中点足以代表当前单元的覆盖集合。
                const float sampleX = (x0 + x1) * 0.5f;
                const float sampleY = (y0 + y1) * 0.5f;
                int         count   = 0;
                for ( const auto& mask : snapshot->overlapMasks ) {
                    if ( sampleX < mask.x || sampleX > mask.x + mask.w ||
                         sampleY < mask.y || sampleY > mask.y + mask.h ) {
                        continue;
                    }
                    // 不同检测类别可能重复指出相同重叠区域，计数不能直接相加。
                    // 最大值保留最强的已有重叠提示，并不重新求出该格所有 owner
                    // 的精确并集。
                    count = std::max(count, mask.objectCount);
                }

                // 至少两个对象的原始提示覆盖当前单元时才打开或延伸水平段。
                // 空白单元触发前段封闭，使相邻遮罩间的空隙保持透明。
                if ( count >= 2 ) {
                    if ( !hasRun ) {
                        hasRun   = true;
                        runStart = x0;
                        runCount = count;
                        // 相邻网格计数变化时立即结束前段，再以新计数起段。
                        // 否则一个水平遮罩会把密集区域的计数错误传播到较稀疏部分。
                    } else if ( runCount != count ) {
                        flushRun(x0);
                        hasRun   = true;
                        runStart = x0;
                        runCount = count;
                    }
                } else {
                    flushRun(x0);
                }
            }

            // 最后一列之后没有下一个空格触发封闭，必须显式提交尾段。
            // 每一行条带独立清理开放状态，不能延续到下一行。
            flushRun(xs.back());
        }

        // 扫描完成后转移结果存储，旧重复矩形不再参与实际绘制。
        // 结果为空表示没有保留下来的有效面积，调用方随后会直接返回。
        snapshot->overlapMasks = std::move(flattened);
    };

    flattenOverlapMasks();
    if ( snapshot->overlapMasks.empty() ) return;

    // 顶层遮罩使用独立命令列表，避免和基础纹理批次混合状态。
    // 统一裁剪范围必须包含主画布草稿、玩家等全部可见分区。
    Batcher overlayBatcher(snapshot, &snapshot->overlayCmds);
    // 草稿区可能独立位于玩家区左侧，遮罩裁剪必须覆盖统一可见范围。
    overlayBatcher.setScissor(
        clipLeftX, topY, clipRightX - clipLeftX, bottomY - topY);
    // 遮罩使用纯色四边形，不继承主体或标签纹理的采样状态。
    // 顶层红色透明度由固定颜色给出，图集 alpha 不应参与。
    overlayBatcher.setTexture(TextureID::None);

    // 遮罩使用固定半透明红色，objectCount 不直接放大透明度。
    // 前面的互斥拆分确保同一像素最多施加一次该颜色。
    const glm::vec4 overlayColor{ 1.0f, 0.0f, 0.0f, 0.35f };
    for ( const auto& mask : snapshot->overlapMasks ) {
        // 遮罩存储为左上角矩形，批处理四边形入口使用另一条水平边作为 Y 基准。
        // 传入 y+h 与正高度保持和其他实体四边形相同的坐标约定。
        overlayBatcher.pushQuad(
            mask.x, mask.y + mask.h, mask.w, mask.h, overlayColor);
    }
    overlayBatcher.flush();
}

/// @brief 把活动音符画笔临时几何转换为半透明的普通物件绘制调用。
/// @param snapshot 提供当前画笔和纹理的快照，不在这里改写逻辑 Registry。
/// @param baseContext 玩家域基准尺寸及当前滚动锚点。
/// @param config 当前绘制配置。
/// @param batcher 与当前画布一致的输出批处理器。
/// @param judgmentLineY 判定线像素 Y。
/// @param fallbackLeftX 兼容玩家域的原点。
/// @param fallbackTrackWidth 玩家单轨基准宽度。
/// @param renderScaleY 相机纵向缩放。
/// @param laneProjection 可选真实分区投影，用于草稿和跨域端点。
/// @warning
/// 画笔活动期间每次快照调用；折线当前复制临时子列表，禁止增加全谱扫描或等待。
/// @pre snapshot 与滚动缓存有效，调用者已排除自动采样画笔。
/// @note 临时组件没有 ECS 身份，不生成普通对象选择或折线命中盒。
/// @note 预览透明度作用于所有部件颜色，最终提交不把这份临时 alpha 写回物件。
/// @note 当前预览不输出绑定标签，标签仅随正式可见物件在基础层生成。
void NoteRenderSystem::renderBrushPreview(
    RenderSnapshot*                            snapshot,
    const NoteRenderSystem::NoteRenderContext& baseContext,
    const Config::EditorConfig& config, Batcher& batcher, float judgmentLineY,
    float fallbackLeftX, float fallbackTrackWidth, float renderScaleY,
    const CanvasLaneProjection* laneProjection)
{
    // 借用快照中的画笔状态，绘制期间不回读逻辑线程的活动工具。
    // 避免同一帧根几何与子列表来自不同次鼠标更新。
    const auto& brush = snapshot->brush;
    // 快照可以保留上一次画笔几何，但非活动状态禁止继续输出预览。
    // 不通过时间或默认类型推测手势是否仍存在。
    if ( !brush.isActive ) return;

    // 笔刷预览按目标草稿/玩家区域物化局部尺寸，不依赖主轨道宽度。
    const auto laneGeometry = resolveNoteLaneGeometry(brush.track,
                                                      laneProjection,
                                                      fallbackLeftX,
                                                      fallbackTrackWidth,
                                                      baseContext.noteW,
                                                      baseContext.noteH);
    // 局部覆盖当前根轨的宽高，基准上下文仍保留玩家域尺寸。
    // Flick 终点与 Polyline 每个子项必须继续用原基准解析，避免重复领域缩放。
    auto ctx                 = baseContext;
    ctx.noteW                = laneGeometry.noteW;
    ctx.noteH                = laneGeometry.noteH;
    const float singleTrackW = laneGeometry.width;
    const float leftX =
        laneGeometry.leftX - static_cast<float>(brush.track) * singleTrackW;

    double noteAbsY = ctx.cache->getAbsY(brush.time);
    // 预览以画笔时间自身 HS 计算显示位置，与正式物件绘制一致。
    // 动画锚点取快照上下文，不能临时读取另一个播放进度。
    float screenY =
        judgmentLineY - static_cast<float>(ctx.cache->getDisplayDelta(
                            brush.time, ctx.currentAbsY, brush.time)) *
                            renderScaleY;

    float trackX = leftX + brush.track * singleTrackW;

    // 用栈上值复用正式物件的颜色和几何入口，不创建临时 Registry 实体。
    // 这里只设置绘制所需字段，不把一次预览记入撤销历史或触发音频事件。
    NoteComponent tempNote;
    tempNote.m_type       = brush.type;
    tempNote.m_timestamp  = brush.time;
    tempNote.m_duration   = brush.duration;
    tempNote.m_trackIndex = brush.track;
    tempNote.m_dtrack     = brush.dtrack;
    // 临时物件按当前根轨决定区域色，和正式音符创建时的域标志一致。
    // 绘制不会在这里修改草稿轨数量或创建新轨道。
    tempNote.m_isDraft      = brush.track < 0;
    tempNote.m_customColors = brush.customColors;
    // 草稿预览沿用专用区域色，玩家预览使用画笔自定义颜色覆盖。
    // 各部件独立解析，不能只把 Tap 颜色广播给整个 Hold 或 Flick。
    glm::vec4 previewNote =
        tempNote.m_isDraft
            ? ctx.colorDraftTap
            : resolveNoteColor(tempNote, NoteColorSlot::Tap, ctx.colorTap);
    glm::vec4 previewHead =
        tempNote.m_isDraft
            ? ctx.colorDraftHead
            : resolveNoteColor(tempNote, NoteColorSlot::Head, ctx.colorHead);
    glm::vec4 previewBody =
        tempNote.m_isDraft
            ? ctx.colorDraftHold
            : resolveNoteColor(tempNote, NoteColorSlot::Hold, ctx.colorHold);
    glm::vec4 previewEnd =
        tempNote.m_isDraft
            ? ctx.colorDraftEnd
            : resolveNoteColor(tempNote, NoteColorSlot::End, ctx.colorEnd);
    glm::vec4 previewNode =
        tempNote.m_isDraft
            ? ctx.colorDraftNode
            : resolveNoteColor(tempNote, NoteColorSlot::Node, ctx.colorNode);
    glm::vec4 previewArrow =
        tempNote.m_isDraft
            ? ctx.colorDraftArrow
            : resolveNoteColor(
                  tempNote, NoteColorSlot::FlickArrow, ctx.colorArrow);

    // 半透明预览乘在原部件 alpha 上，保留皮肤和用户配置的透明程度。
    // 所有部件采用同一预览乘数，避免头部与主体的状态反馈不一致。
    previewNote.a *= 0.5f;
    previewHead.a *= 0.5f;
    previewBody.a *= 0.5f;
    previewEnd.a *= 0.5f;
    previewNode.a *= 0.5f;
    previewArrow.a *= 0.5f;

    // 普通点击按目标轨宽居中，预览不使用旧实体 Transform。
    // 后续各类型复用正式绘制 helper，保持纹理比例与完成后物件一致。
    if ( brush.type == ::MMM::NoteType::NOTE ) {
        NoteRenderSystem::renderTap(batcher,
                                    tempNote,
                                    config,
                                    trackX + (singleTrackW - ctx.noteW) * 0.5f,
                                    screenY,
                                    ctx.noteW,
                                    ctx.noteH,
                                    ctx.baseAspect,
                                    previewNote);
    } else if ( brush.type == ::MMM::NoteType::HOLD ) {
        NoteRenderSystem::renderHold(batcher,
                                     tempNote,
                                     config,
                                     snapshot,
                                     trackX + (singleTrackW - ctx.noteW) * 0.5f,
                                     ctx.noteW,
                                     ctx.noteH,
                                     singleTrackW,
                                     previewHead,
                                     previewBody,
                                     previewEnd,
                                     ctx.cache,
                                     ctx.currentAbsY,
                                     judgmentLineY,
                                     renderScaleY,
                                     0.0f,
                                     judgmentLineY * 2.0f);
    } else if ( brush.type == ::MMM::NoteType::FLICK ) {
        // 终点可落在与根不同的分区宽度，使用原玩家尺寸重新解析。
        // 连接体由根与终点真实中心连接，箭头尺寸取终点领域。
        const auto flickEndpoint =
            resolveNoteLaneGeometry(brush.track + brush.dtrack,
                                    laneProjection,
                                    fallbackLeftX,
                                    fallbackTrackWidth,
                                    baseContext.noteW,
                                    baseContext.noteH);
        NoteRenderSystem::renderFlick(
            batcher,
            tempNote,
            config,
            snapshot,
            trackX + (singleTrackW - ctx.noteW) * 0.5f,
            screenY,
            ctx.noteW,
            ctx.noteH,
            flickEndpoint.centerX(),
            flickEndpoint.noteW,
            flickEndpoint.noteH,
            previewHead,
            previewBody,
            previewArrow);
    } else if ( brush.type == ::MMM::NoteType::POLYLINE ) {
        // 现有入口按值复制临时子段以适配 NoteComponent 接口。
        // 这是活动预览的局部成本，不能将该路径标为完全零分配。
        tempNote.m_subNotes = brush.polylineSegments;
        // 折线根时间和轨道以首子项恢复，画笔的游标位置可能已移动到末端。
        // 空列表不访问 front，保留已有临时根字段给绘制 helper 处理。
        if ( !tempNote.m_subNotes.empty() ) {
            // 折线预览根锚点从复制后的首段读取，避免把末端游标时间当作父起点。
            // 子项顺序保留绘制工具状态，不在渲染时进行结构清理或排序。
            tempNote.m_timestamp  = tempNote.m_subNotes.front().timestamp;
            tempNote.m_trackIndex = tempNote.m_subNotes.front().trackIndex;
        }

        // 预览目前采用判定线上下对称的兼容包围范围，不等于实际视口全部高度。
        // 真实画布裁剪由批处理上层状态继续约束。
        float topY    = 0.0f;
        float bottomY = judgmentLineY * 2.0f;  // 简单包围盒

        // 预览传入空实体身份且关闭命中生成，不能成为可选择的正式折线。
        // 局部高亮部件也保持 None，预览颜色已经在本函数统一处理。
        NoteRenderSystem::renderPolyline(ctx.cache,
                                         batcher,
                                         tempNote,
                                         config,
                                         snapshot,
                                         ctx.currentAbsY,
                                         ctx.currentTime,
                                         judgmentLineY,
                                         fallbackLeftX,
                                         topY,
                                         bottomY,
                                         fallbackTrackWidth,
                                         renderScaleY,
                                         previewHead,
                                         previewBody,
                                         previewEnd,
                                         previewNode,
                                         previewArrow,
                                         entt::null,
                                         false,
                                         HoverPart::None,
                                         -1,
                                         laneProjection);
    } else {
        // 兜底调试绘制。
        // 未知类型输出调试矩形，以便发现无效画笔状态而不是访问不存在的纹理。
        // 显式设置无纹理模式，避免沿用上一物件的图集采样。
        float x = trackX + (singleTrackW - ctx.noteW) * 0.5f;
        batcher.setTexture(TextureID::None);
        batcher.pushQuad(x,
                         screenY + ctx.noteH * 0.5f,
                         ctx.noteW,
                         ctx.noteH,
                         { 1.0f, 0.0f, 0.0f, 0.5f });
    }
    // 提交预览最后一个批段，使后续顶层内容不会继承未完成的纹理状态。
    // 此处不结束画笔逻辑手势，只完成当前快照的命令生成。
    batcher.flush();
}

}  // namespace MMM::Logic::System
