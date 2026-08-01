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
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace MMM::Logic::System
{

/// @brief UI 侧亚帧补偿允许的最大滞后秒数，与 CanvasSnapshotPrepare 保持一致。
constexpr double MAX_UI_INTERPOLATION_SECONDS = 0.1;

/// @brief 音符可见性 AbsY 索引桶尺寸。
constexpr double NOTE_ABSY_BUCKET_SIZE = 2048.0;

/// @brief 单个音符在 AbsY 空间覆盖的保守区间。
struct NoteAbsYRangeEntry {
    /// @brief 音符实体。
    entt::entity entity{ entt::null };
    /// @brief 区间下界。
    double minAbsY{ 0.0 };
    /// @brief 区间上界。
    double maxAbsY{ 0.0 };
};

/// @brief 音符可见性 AbsY 分桶索引。
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
static double getCarrierEndAnchorTime(const NoteComponent& note,
                                      const ScrollCache*   cache)
{
    (void)cache;
    if ( note.m_type == ::MMM::NoteType::HOLD ) {
        return note.m_timestamp;
    }
    return note.m_timestamp + note.m_duration;
}

/// @brief 在玩家轨道物件锚点上方绘制绑定音效标签。
/// @warning
/// 主画布热路径：只处理调用方已剔除的物件锚点，不得访问文件系统或分配堆内存。
static void renderBoundSampleLabelAt(Batcher& batcher, const ScrollCache* cache,
                                     double currentAbsY, float noteH,
                                     const ::MMM::AudioSampleBinding& binding,
                                     double timestamp, int32_t trackIndex,
                                     int32_t trackCount, float judgmentLineY,
                                     float leftX, float topY, float bottomY,
                                     float singleTrackW, float renderScaleY,
                                     float noteScaleY, glm::vec4 color)
{
    if ( binding.m_audioResourceId.empty() || trackIndex < 0 ||
         trackIndex >= trackCount ) {
        return;
    }

    const float screenY =
        judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                            timestamp, currentAbsY, timestamp)) *
                            renderScaleY;
    if ( screenY + noteH * 0.5F < topY || screenY - noteH * 0.5F > bottomY ) {
        return;
    }

    renderAudioObjectLabel(
        batcher,
        binding.m_audioResourceId,
        binding.m_volume,
        leftX + static_cast<float>(trackIndex) * singleTrackW,
        screenY - noteH * 0.5F,
        singleTrackW,
        noteScaleY,
        color,
        batcher.snapshot->snapshotSysTime);
}

void NoteRenderSystem::renderNotes(
    entt::registry& registry, RenderSnapshot* snapshot,
    const std::string& cameraId, double currentTime, float judgmentLineY,
    int32_t trackCount, const Config::EditorConfig& config, Batcher& batcher,
    float leftX, float rightX, float topY, float bottomY, float singleTrackW,
    float renderScaleY)
{
    // 1. 准备上下文与颜色
    NoteRenderSystem::NoteRenderContext ctx =
        NoteRenderSystem::prepareNoteRenderContext(
            registry, snapshot, currentTime, singleTrackW, config);
    if ( !ctx.cache ) return;

    auto& noteEntities = snapshot->noteQueryScratch;
    auto& noteSeen     = snapshot->noteQuerySeenScratch;
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

    // 布局模式即使正在播放也需要逐物件边界，供 UI 直接调整渲染比例。
    const bool shouldGenerateHitboxes =
        (!snapshot->isPlaying || snapshot->currentTool == EditTool::Layout) &&
        snapshot->acceptsInteraction &&
        SessionUtils::isMainCanvasCameraId(cameraId);

    // 2. 生成碰撞盒并获取可见实体
    if ( shouldGenerateHitboxes ) {
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
                                               config);
    }

    // 3. 基础层渲染
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
            SessionUtils::isMainCanvasCameraId(cameraId));

    // 4. 发光层渲染
    NoteRenderSystem::renderNoteGlowLayer(registry,
                                          snapshot,
                                          ctx,
                                          config,
                                          noteEntities,
                                          (float)currentTime,
                                          judgmentLineY,
                                          leftX,
                                          rightX,
                                          topY,
                                          bottomY,
                                          singleTrackW,
                                          renderScaleY);

    // 5. 笔刷预览渲染
    if ( snapshot->brush.isActive && !snapshot->brush.createsAudioSample ) {
        NoteRenderSystem::renderBrushPreview(snapshot,
                                             ctx,
                                             config,
                                             batcher,
                                             judgmentLineY,
                                             leftX,
                                             singleTrackW,
                                             renderScaleY);
    }

    // 6. 顶层重叠遮罩
    if ( !snapshot->isPlaying ) {
        NoteRenderSystem::renderOverlapMasks(registry,
                                             snapshot,
                                             ctx,
                                             config,
                                             noteEntities,
                                             judgmentLineY,
                                             leftX,
                                             rightX,
                                             topY,
                                             bottomY,
                                             singleTrackW,
                                             renderScaleY);
    }
}

NoteRenderSystem::NoteRenderContext NoteRenderSystem::prepareNoteRenderContext(
    entt::registry& registry, RenderSnapshot* snapshot, double currentTime,
    float singleTrackW, const Config::EditorConfig& config)
{
    NoteRenderSystem::NoteRenderContext ctx{};

    const auto** cachePtr = registry.ctx().find<const ScrollCache*>();
    if ( !cachePtr || !(*cachePtr) ) return ctx;
    ctx.cache       = *cachePtr;
    ctx.currentAbsY = ctx.cache->getVisualAnchorAbsY(currentTime);
    ctx.currentTime = currentTime;

    auto itBase = snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Note));
    if ( itBase == snapshot->uvMap.end() ) return ctx;
    ctx.baseAspect = itBase->second.z / itBase->second.w;

    ctx.noteW = singleTrackW * config.visual.noteScaleX;
    ctx.noteH = (singleTrackW / ctx.baseAspect) * config.visual.noteScaleY;

    auto& skin      = Config::SkinManager::instance();
    auto  color_tap = skin.getColor("note_tap");
    ctx.colorTap    = { color_tap.r, color_tap.g, color_tap.b, color_tap.a };

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

    return ctx;
}

static std::pair<double, double> getNoteTimeRange(const NoteComponent& note)
{
    double minTime = note.m_timestamp;
    double maxTime = note.m_timestamp + std::max(0.0, note.m_duration);
    if ( minTime > maxTime ) {
        std::swap(minTime, maxTime);
    }

    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
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
static void forEachNoteVisibilitySampleTime(const NoteComponent& note,
                                            const ScrollCache*   cache,
                                            Callback&&           callback)
{
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

    if ( !cache ) {
        return;
    }

    const auto [minTime, maxTime] = getNoteTimeRange(note);
    if ( !std::isfinite(minTime) || !std::isfinite(maxTime) ||
         minTime > maxTime ) {
        return;
    }

    const auto& segments = cache->getSegments();
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

static double calculateInterpolationPaddingAbsY(const ScrollCache* cache,
                                                double             currentTime,
                                                double interpolationSeconds)
{
    if ( !cache || interpolationSeconds <= 0.0 ||
         !std::isfinite(interpolationSeconds) ) {
        return 0.0;
    }

    const auto& segments = cache->getSegments();
    if ( segments.empty() ) {
        return std::abs(cache->getSpeedAt(currentTime)) * interpolationSeconds;
    }

    const double endTime = currentTime + interpolationSeconds;
    auto it = std::upper_bound(segments.begin(),
                               segments.end(),
                               currentTime,
                               [](double value, const ScrollSegment& seg) {
                                   return value < seg.time;
                               });

    if ( it != segments.begin() ) {
        --it;
    }

    double cursor      = currentTime;
    double paddingAbsY = 0.0;
    while ( cursor < endTime ) {
        const double speed = std::isfinite(it->speed)
                                 ? it->speed * cache->getAnimatedZoomScale()
                                 : 0.0;
        auto         next  = std::next(it);
        const double sliceEnd =
            next != segments.end() ? std::min(endTime, next->time) : endTime;

        if ( sliceEnd > cursor ) {
            paddingAbsY += std::abs(speed) * (sliceEnd - cursor);
            cursor = sliceEnd;
        }

        if ( next == segments.end() ) {
            break;
        }
        it = next;
    }

    return paddingAbsY;
}

static NoteAbsYBucketIndex& getOrBuildNoteAbsYBucketIndex(
    entt::registry& registry, const ScrollCache* cache,
    const std::vector<entt::entity>& entities, std::uint64_t noteRevision)
{
    auto* index = registry.ctx().find<NoteAbsYBucketIndex>();
    if ( !index ) {
        index = &registry.ctx().emplace<NoteAbsYBucketIndex>();
    }

    const std::uint64_t scrollRevision = cache ? cache->getRevision() : 0;
    if ( index->cache == cache && index->sourceEntities == &entities &&
         index->sourceCount == entities.size() &&
         index->scrollRevision == scrollRevision &&
         index->noteRevision == noteRevision ) {
        return *index;
    }

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

    if ( !cache || entities.empty() ) {
        index->minHs = 1.0;
        index->maxHs = 1.0;
        return *index;
    }

    index->entries.reserve(entities.size());
    double globalMinAbsY = std::numeric_limits<double>::infinity();
    double globalMaxAbsY = -std::numeric_limits<double>::infinity();

    for ( auto entity : entities ) {
        if ( !registry.valid(entity) ||
             !registry.all_of<NoteComponent>(entity) ) {
            continue;
        }

        const auto& note = registry.get<const NoteComponent>(entity);
        if ( note.m_isSubNote ) continue;

        double minAbsY = std::numeric_limits<double>::infinity();
        double maxAbsY = -std::numeric_limits<double>::infinity();

        auto includeSampleTime = [&](double time) {
            if ( !std::isfinite(time) ) return;

            const double absY = cache->toUnscaledAbsY(cache->getAbsY(time));
            if ( !std::isfinite(absY) ) return;

            const double hs = cache->getHsAt(time);
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

        if ( !std::isfinite(minAbsY) || !std::isfinite(maxAbsY) ) {
            continue;
        }

        globalMinAbsY = std::min(globalMinAbsY, minAbsY);
        globalMaxAbsY = std::max(globalMaxAbsY, maxAbsY);
        index->entries.push_back({ entity, minAbsY, maxAbsY });
    }

    if ( !std::isfinite(index->minHs) || index->maxHs <= 1e-6 ) {
        index->minHs                 = 1.0;
        index->maxHs                 = 1.0;
        index->requiresFullExactScan = true;
    }

    index->seenSerials.assign(index->entries.size(), 0);
    if ( index->entries.empty() || !std::isfinite(globalMinAbsY) ||
         !std::isfinite(globalMaxAbsY) ||
         globalMaxAbsY < globalMinAbsY - 1e-6 ) {
        return *index;
    }

    index->bucketOrigin = globalMinAbsY;
    const double bucketSpan =
        std::max(0.0, globalMaxAbsY - globalMinAbsY) / NOTE_ABSY_BUCKET_SIZE;
    const auto bucketCount =
        static_cast<std::size_t>(std::floor(bucketSpan)) + 1;
    if ( bucketCount == 0 || bucketCount > 500000 ) {
        index->requiresFullExactScan = true;
        return *index;
    }

    index->buckets.resize(bucketCount);
    auto bucketForAbsY = [&](double absY) {
        const double relative =
            (absY - index->bucketOrigin) / NOTE_ABSY_BUCKET_SIZE;
        if ( relative <= 0.0 ) return std::size_t{ 0 };
        auto bucket = static_cast<std::size_t>(std::floor(relative));
        return std::min(bucket, index->buckets.size() - 1);
    };

    for ( std::size_t i = 0; i < index->entries.size(); ++i ) {
        const auto& entry       = index->entries[i];
        const auto  startBucket = bucketForAbsY(entry.minAbsY);
        const auto  endBucket   = bucketForAbsY(entry.maxAbsY);
        for ( std::size_t bucket = startBucket; bucket <= endBucket;
              ++bucket ) {
            index->buckets[bucket].push_back(static_cast<std::uint32_t>(i));
            if ( bucket == index->buckets.size() - 1 ) break;
        }
    }

    return *index;
}

static void collectNotesInRange(
    entt::registry& registry, const ScrollCache* cache, double currentTime,
    double currentAbsY, float judgmentLineY, float topY, float bottomY,
    float renderScaleY, float visualPaddingPixels, double interpolationSeconds,
    std::vector<entt::entity>& result, std::unordered_set<entt::entity>& seen)
{
    result.clear();
    seen.clear();
    const auto** sortedEntitiesPtr =
        registry.ctx().find<const std::vector<entt::entity>*>();
    if ( !sortedEntitiesPtr || !(*sortedEntitiesPtr) ) return;

    const auto& entities = **sortedEntitiesPtr;
    size_t      count    = entities.size();
    if ( count == 0 || !cache || std::abs(renderScaleY) < 1e-6f ) {
        return;
    }

    double maxDelta =
        (judgmentLineY - topY) / static_cast<double>(renderScaleY);
    double minDelta =
        (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
    double padDelta = std::max(0.0, static_cast<double>(visualPaddingPixels)) /
                          static_cast<double>(std::abs(renderScaleY)) +
                      calculateInterpolationPaddingAbsY(
                          cache, currentTime, interpolationSeconds);

    auto isDisplayVisible = [&](const NoteComponent& note) {
        double minDisplayDelta = std::numeric_limits<double>::infinity();
        double maxDisplayDelta = -std::numeric_limits<double>::infinity();

        auto includeTime = [&](double time) {
            double displayDelta =
                cache->getDisplayDelta(time, currentAbsY, time);
            if ( !std::isfinite(displayDelta) ) return;
            minDisplayDelta = std::min(minDisplayDelta, displayDelta);
            maxDisplayDelta = std::max(maxDisplayDelta, displayDelta);
        };

        forEachNoteVisibilitySampleTime(note, cache, includeTime);

        if ( !std::isfinite(minDisplayDelta) ||
             !std::isfinite(maxDisplayDelta) ) {
            return false;
        }

        return maxDisplayDelta >= minDelta - padDelta &&
               minDisplayDelta <= maxDelta + padDelta;
    };

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

    auto appendPinnedDragEntities = [&]() {
        const auto* pinnedEntities =
            registry.ctx().find<DragRenderPinnedEntities>();
        if ( !pinnedEntities || !pinnedEntities->entities ||
             pinnedEntities->entities->empty() ) {
            return;
        }

        seen.clear();
        seen.reserve(result.size() + pinnedEntities->entities->size());
        for ( auto entity : result ) {
            seen.insert(entity);
        }

        for ( auto entity : *pinnedEntities->entities ) {
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

    const auto** noteRevisionPtr = registry.ctx().find<const std::uint64_t*>();
    if ( !noteRevisionPtr || !(*noteRevisionPtr) ) {
        runFullExactScan();
        appendPinnedDragEntities();
        return;
    }

    auto& index = getOrBuildNoteAbsYBucketIndex(
        registry, cache, entities, **noteRevisionPtr);
    if ( index.requiresFullExactScan ) {
        runFullExactScan();
        appendPinnedDragEntities();
        return;
    }
    if ( index.entries.empty() || index.buckets.empty() ) {
        appendPinnedDragEntities();
        return;
    }

    const double displayMin     = std::min(minDelta, maxDelta) - padDelta;
    const double displayMax     = std::max(minDelta, maxDelta) + padDelta;
    double       queryMinAbsY   = std::numeric_limits<double>::infinity();
    double       queryMaxAbsY   = -std::numeric_limits<double>::infinity();
    auto         includeHsBound = [&](double hs) {
        if ( !std::isfinite(hs) || hs <= 1e-6 ) return;
        const double a = currentAbsY + displayMin / hs;
        const double b = currentAbsY + displayMax / hs;
        queryMinAbsY   = std::min(queryMinAbsY, std::min(a, b));
        queryMaxAbsY   = std::max(queryMaxAbsY, std::max(a, b));
    };
    includeHsBound(index.minHs);
    includeHsBound(index.maxHs);
    if ( !std::isfinite(queryMinAbsY) || !std::isfinite(queryMaxAbsY) ) {
        runFullExactScan();
        appendPinnedDragEntities();
        return;
    }
    queryMinAbsY = cache->toUnscaledAbsY(queryMinAbsY);
    queryMaxAbsY = cache->toUnscaledAbsY(queryMaxAbsY);
    if ( queryMinAbsY > queryMaxAbsY ) {
        std::swap(queryMinAbsY, queryMaxAbsY);
    }

    auto bucketForAbsY = [&](double absY) {
        const double relative =
            (absY - index.bucketOrigin) / NOTE_ABSY_BUCKET_SIZE;
        if ( relative <= 0.0 ) return std::size_t{ 0 };
        auto bucket = static_cast<std::size_t>(std::floor(relative));
        return std::min(bucket, index.buckets.size() - 1);
    };

    const auto startBucket = bucketForAbsY(queryMinAbsY);
    const auto endBucket   = bucketForAbsY(queryMaxAbsY);
    ++index.querySerial;
    if ( index.querySerial == 0 ) {
        std::fill(index.seenSerials.begin(), index.seenSerials.end(), 0);
        index.querySerial = 1;
    }

    result.reserve(256);
    for ( std::size_t bucket = startBucket; bucket <= endBucket; ++bucket ) {
        for ( std::uint32_t entryIndex : index.buckets[bucket] ) {
            if ( entryIndex >= index.entries.size() ) continue;
            if ( index.seenSerials[entryIndex] == index.querySerial ) {
                continue;
            }
            index.seenSerials[entryIndex] = index.querySerial;

            const auto& entry = index.entries[entryIndex];
            if ( entry.maxAbsY < queryMinAbsY ||
                 entry.minAbsY > queryMaxAbsY ) {
                continue;
            }
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
        if ( bucket == endBucket || bucket == index.buckets.size() - 1 ) {
            break;
        }
    }
    appendPinnedDragEntities();
}

void NoteRenderSystem::generateNoteHitboxes(
    entt::registry& registry, RenderSnapshot* snapshot,
    const NoteRenderSystem::NoteRenderContext& ctx,
    const std::vector<entt::entity>& noteEntities, float judgmentLineY,
    float leftX, float topY, float bottomY, float singleTrackW,
    float renderScaleY, const Config::EditorConfig& config)
{
    // 第一遍：连接体，优先级较低。
    for ( auto entity : noteEntities ) {
        const auto& transform = registry.get<const TransformComponent>(entity);
        const auto& note      = registry.get<const NoteComponent>(entity);
        if ( !SessionUtils::isNoteEditable(note, config.settings) ) continue;

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

        float visualH =
            static_cast<float>(displayDeltaStart - displayDeltaEnd) *
            renderScaleY;

        if ( !note.m_isSubNote ) {
            if ( note.m_type == ::MMM::NoteType::FLICK && note.m_dtrack != 0 ) {
                float startTrack =
                    std::min((float)note.m_trackIndex,
                             (float)note.m_trackIndex + note.m_dtrack);
                float drawW = std::abs(note.m_dtrack) * singleTrackW;
                float bodyX =
                    leftX + startTrack * singleTrackW + singleTrackW * 0.5f;

                float drawH   = ctx.noteH;
                auto  itBodyH = snapshot->uvMap.find(
                    static_cast<uint32_t>(TextureID::HoldBodyHorizontal));
                if ( itBodyH != snapshot->uvMap.end() ) {
                    drawH = ctx.noteH *
                            (itBodyH->second.w /
                             snapshot->uvMap.at(uint32_t(TextureID::Note)).w);
                }

                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::HoldBody,
                                               -1,
                                               bodyX,
                                               screenY - drawH * 0.5f,
                                               drawW,
                                               drawH });
            } else if ( note.m_type == ::MMM::NoteType::HOLD &&
                        std::abs(visualH) > ctx.noteH * 0.1f ) {
                float bodyW  = ctx.noteW;
                auto  itBody = snapshot->uvMap.find(
                    static_cast<uint32_t>(TextureID::HoldBodyVertical));
                if ( itBody != snapshot->uvMap.end() ) {
                    float baseWRatio =
                        snapshot->uvMap.at(uint32_t(TextureID::Note)).z;
                    bodyW = ctx.noteW * (itBody->second.z / baseWRatio);
                }

                float bodyX = leftX + note.m_trackIndex * singleTrackW +
                              (singleTrackW - bodyW) * 0.5f;
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

        float minY = std::min(screenY, endY) - ctx.noteH;
        float maxY = std::max(screenY, endY) + ctx.noteH;
        if ( minY > bottomY || maxY < topY ) continue;

        float headX = leftX + note.m_trackIndex * singleTrackW +
                      (singleTrackW - ctx.noteW) * 0.5f;

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
                TextureID arrowId = (note.m_dtrack < 0)
                                        ? TextureID::FlickArrowLeft
                                        : TextureID::FlickArrowRight;
                auto  it = snapshot->uvMap.find(static_cast<uint32_t>(arrowId));
                float arrowW = ctx.noteW;
                float arrowH = ctx.noteH;
                if ( it != snapshot->uvMap.end() ) {
                    float baseWRatio =
                        snapshot->uvMap.at(uint32_t(TextureID::Note)).z;
                    float baseHRatio =
                        snapshot->uvMap.at(uint32_t(TextureID::Note)).w;
                    float wRatio = it->second.z / baseWRatio;
                    float hRatio = it->second.w / baseHRatio;
                    arrowW       = ctx.noteW * wRatio;
                    arrowH       = ctx.noteH * hRatio;
                }

                float arrowX =
                    leftX + (note.m_trackIndex + note.m_dtrack) * singleTrackW +
                    (singleTrackW - arrowW) * 0.5f;

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

void NoteRenderSystem::renderNoteBaseLayer(
    entt::registry& registry, RenderSnapshot* snapshot,
    const NoteRenderSystem::NoteRenderContext& ctx,
    const Config::EditorConfig&                config,
    const std::vector<entt::entity>& noteEntities, Batcher& batcher,
    float currentTime, float judgmentLineY, float leftX, float rightX,
    float topY, float bottomY, float singleTrackW, float renderScaleY,
    int32_t trackCount, bool generateHitboxes, bool showBoundSampleLabels)
{
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

    /// @brief visibleEntities 保持自 SessionContext
    /// 预排序缓存继承来的时间升序。
    /// 基础层按反向顺序绘制，避免在热路径内再次完整排序。
    for ( auto it = visibleEntities.rbegin(); it != visibleEntities.rend();
          ++it ) {
        /// @brief 当前反向遍历到的可见音符实体。
        entt::entity entity    = *it;
        const auto&  transform = registry.get<const TransformComponent>(entity);
        const auto&  note      = registry.get<const NoteComponent>(entity);

        // 处理拖拽/剪切时的视觉反馈；选中反馈由发光层按当前颜色绘制。
        float alphaMul = 1.0f;
        if ( auto* ic = registry.try_get<InteractionComponent>(entity) ) {
            if ( ic->isDragging || ic->isCut ) {
                alphaMul = 0.5f;
            }
        }

        float screenY =
            judgmentLineY -
            static_cast<float>(ctx.cache->getDisplayDelta(
                note.m_timestamp, ctx.currentAbsY, note.m_timestamp)) *
                renderScaleY;
        float visualH = static_cast<float>(ctx.cache->getDisplayDelta(
                            note.m_timestamp + note.m_duration,
                            ctx.cache->getAbsY(note.m_timestamp),
                            note.m_timestamp)) *
                        renderScaleY;
        float trackX  = leftX + note.m_trackIndex * singleTrackW;

        // 应用自定义颜色与 Alpha。
        glm::vec4 curColorNote =
            resolveNoteColor(note, NoteColorSlot::Tap, ctx.colorTap);
        glm::vec4 curColorHead =
            resolveNoteColor(note, NoteColorSlot::Head, ctx.colorHead);
        glm::vec4 curColorHoldBody =
            resolveNoteColor(note, NoteColorSlot::Hold, ctx.colorHold);
        glm::vec4 curColorHoldEnd =
            resolveNoteColor(note, NoteColorSlot::End, ctx.colorEnd);
        glm::vec4 curColorNode =
            resolveNoteColor(note, NoteColorSlot::Node, ctx.colorNode);
        glm::vec4 curColorArrow =
            resolveNoteColor(note, NoteColorSlot::FlickArrow, ctx.colorArrow);

        bool isFullErasing =
            snapshot->erasingObjectKind == ChartObjectKind::PlayerNote &&
            snapshot->erasingEntities.count(entity) &&
            snapshot->erasingSubIndex == -1;
        if ( isFullErasing ) {
            curColorNote     = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorHead     = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorHoldBody = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorHoldEnd  = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorNode     = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorArrow    = { 1.0f, 0.2f, 0.2f, 1.0f };
            alphaMul *= 0.5f;
        }

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
                renderScaleY);
        else if ( note.m_type == ::MMM::NoteType::FLICK )
            NoteRenderSystem::renderFlick(
                batcher,
                note,
                config,
                snapshot,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                screenY,
                ctx.noteW,
                ctx.noteH,
                singleTrackW,
                curColorHead,
                curColorHoldBody,
                curColorArrow);
        else if ( note.m_type == ::MMM::NoteType::POLYLINE )
            NoteRenderSystem::renderPolyline(ctx.cache,
                                             batcher,
                                             note,
                                             config,
                                             snapshot,
                                             ctx.currentAbsY,
                                             ctx.currentTime,
                                             judgmentLineY,
                                             leftX,
                                             rightX,
                                             topY,
                                             bottomY,
                                             singleTrackW,
                                             renderScaleY,
                                             curColorHead,
                                             curColorHoldBody,
                                             curColorHoldEnd,
                                             curColorNode,
                                             curColorArrow,
                                             entity,
                                             generateHitboxes);
    }

    if ( showBoundSampleLabels ) {
        const auto labelColor = audioObjectLabelColor();
        for ( auto it = visibleEntities.rbegin(); it != visibleEntities.rend();
              ++it ) {
            const auto& note = registry.get<const NoteComponent>(
                static_cast<entt::entity>(*it));
            if ( note.m_type != ::MMM::NoteType::POLYLINE ) {
                if ( note.m_sampleBinding ) {
                    renderBoundSampleLabelAt(batcher,
                                             ctx.cache,
                                             ctx.currentAbsY,
                                             ctx.noteH,
                                             *note.m_sampleBinding,
                                             note.m_timestamp,
                                             note.m_trackIndex,
                                             trackCount,
                                             judgmentLineY,
                                             leftX,
                                             topY,
                                             bottomY,
                                             singleTrackW,
                                             renderScaleY,
                                             config.visual.noteScaleY,
                                             labelColor);
                }
                continue;
            }

            for ( std::size_t subIndex = 0; subIndex < note.m_subNotes.size();
                  ++subIndex ) {
                const auto& subNote = note.m_subNotes[subIndex];
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
                                         trackCount,
                                         judgmentLineY,
                                         leftX,
                                         topY,
                                         bottomY,
                                         singleTrackW,
                                         renderScaleY,
                                         config.visual.noteScaleY,
                                         labelColor);
            }
        }
    }
    batcher.flush();
}

/// @brief 绘制悬浮/选中音符的发光层，并使用轨道框限制可见区域。
/// @warning
/// 热路径：悬浮/选中音符每帧绘制；只允许使用已缓存的实体列表和纹理信息。
void NoteRenderSystem::renderNoteGlowLayer(
    entt::registry& registry, RenderSnapshot* snapshot,
    const NoteRenderSystem::NoteRenderContext& ctx,
    const Config::EditorConfig&                config,
    const std::vector<entt::entity>& noteEntities, float currentTime,
    float judgmentLineY, float leftX, float rightX, float topY, float bottomY,
    float singleTrackW, float renderScaleY)
{
    std::vector<entt::entity> glowEntities;
    glowEntities.reserve(noteEntities.size());
    for ( auto entity : noteEntities ) {
        /// @brief 当前可见实体的交互状态；不存在时跳过发光层。
        const auto* ic = registry.try_get<const InteractionComponent>(entity);
        if ( !ic ) continue;
        if ( ic->isHovered || ic->isSelected ) glowEntities.push_back(entity);
    }

    if ( glowEntities.empty() ) return;

    Batcher glowBatcher(snapshot, &snapshot->glowCmds);
    glowBatcher.setScissor(leftX, topY, rightX - leftX, bottomY - topY);
    /// @brief 发光实体继承可见实体时间升序。
    /// 继承可见实体时间升序，反向绘制即可获得原先的后到前覆盖顺序。
    for ( auto it = glowEntities.rbegin(); it != glowEntities.rend(); ++it ) {
        /// @brief 当前反向遍历到的发光音符实体。
        entt::entity entity    = *it;
        const auto&  transform = registry.get<const TransformComponent>(entity);
        const auto&  note      = registry.get<const NoteComponent>(entity);
        const auto&  ic = registry.get<const InteractionComponent>(entity);
        float        screenY =
            judgmentLineY -
            static_cast<float>(ctx.cache->getDisplayDelta(
                note.m_timestamp, ctx.currentAbsY, note.m_timestamp)) *
                renderScaleY;
        float     visualH  = static_cast<float>(ctx.cache->getDisplayDelta(
                                 note.m_timestamp + note.m_duration,
                                 ctx.cache->getAbsY(note.m_timestamp),
                                 note.m_timestamp)) *
                             renderScaleY;
        float     trackX   = leftX + note.m_trackIndex * singleTrackW;
        HoverPart glowPart = static_cast<HoverPart>(ic.hoveredPart);
        int       glowIdx  = ic.hoveredSubIndex;
        if ( ic.isSelected ) {
            glowPart = HoverPart::None;
            glowIdx  = -1;
        }
        glm::vec4 glowNote =
            resolveNoteColor(note, NoteColorSlot::Tap, ctx.colorTap);
        glm::vec4 glowHead =
            resolveNoteColor(note, NoteColorSlot::Head, ctx.colorHead);
        glm::vec4 glowBody =
            resolveNoteColor(note, NoteColorSlot::Hold, ctx.colorHold);
        glm::vec4 glowEnd =
            resolveNoteColor(note, NoteColorSlot::End, ctx.colorEnd);
        glm::vec4 glowNode =
            resolveNoteColor(note, NoteColorSlot::Node, ctx.colorNode);
        glm::vec4 glowArrow =
            resolveNoteColor(note, NoteColorSlot::FlickArrow, ctx.colorArrow);

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
                glowPart);
        else if ( note.m_type == ::MMM::NoteType::FLICK )
            NoteRenderSystem::renderFlick(
                glowBatcher,
                note,
                config,
                snapshot,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                screenY,
                ctx.noteW,
                ctx.noteH,
                singleTrackW,
                glowHead,
                glowBody,
                glowArrow,
                glowPart);
        else if ( note.m_type == ::MMM::NoteType::POLYLINE )
            NoteRenderSystem::renderPolyline(ctx.cache,
                                             glowBatcher,
                                             note,
                                             config,
                                             snapshot,
                                             ctx.currentAbsY,
                                             ctx.currentTime,
                                             judgmentLineY,
                                             leftX,
                                             rightX,
                                             topY,
                                             bottomY,
                                             singleTrackW,
                                             renderScaleY,
                                             glowHead,
                                             glowBody,
                                             glowEnd,
                                             glowNode,
                                             glowArrow,
                                             entity,
                                             false,
                                             glowPart,
                                             glowIdx);
    }
    glowBatcher.flush();
}

void NoteRenderSystem::renderOverlapMasks(
    entt::registry& registry, RenderSnapshot* snapshot,
    const NoteRenderSystem::NoteRenderContext& ctx,
    const Config::EditorConfig&                config,
    const std::vector<entt::entity>& noteEntities, float judgmentLineY,
    float leftX, float rightX, float topY, float bottomY, float singleTrackW,
    float renderScaleY)
{
    if ( !snapshot || !ctx.cache || noteEntities.size() < 2 ) return;

    struct OverlapItem {
        ::MMM::NoteType type{ ::MMM::NoteType::NOTE };
        double          startTime{ 0.0 };
        double          endTime{ 0.0 };
        int             track{ 0 };
        int             dtrack{ 0 };
        entt::entity    owner{ entt::null };
        int             subIndex{ -1 };
    };

    struct OverlapPoint {
        double       time{ 0.0 };
        int          track{ 0 };
        entt::entity owner{ entt::null };
        float        scale{ 1.0f };
        bool         testsFlickBody{ false };
    };

    auto makeEndTime =
        [](double startTime, double duration, ::MMM::NoteType type) {
            if ( type != ::MMM::NoteType::HOLD ) return startTime;
            return startTime + std::max(0.0, duration);
        };

    std::vector<OverlapItem> items;
    items.reserve(noteEntities.size());
    for ( auto entity : noteEntities ) {
        const auto& note = registry.get<const NoteComponent>(entity);
        if ( note.m_isSubNote ) continue;

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

    if ( items.empty() ) return;

    const double windowSeconds =
        static_cast<double>(
            std::max(0.0f, config.settings.overlapTimeWindowMs)) *
        0.001;
    constexpr double timeEpsilon = 1e-7;

    auto textureSize = [snapshot](TextureID id, float baseW, float baseH) {
        auto itBase =
            snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Note));
        if ( itBase == snapshot->uvMap.end() ) return glm::vec2(baseW, baseH);

        auto it = snapshot->uvMap.find(static_cast<uint32_t>(id));
        if ( it == snapshot->uvMap.end() ) return glm::vec2(baseW, baseH);

        return glm::vec2(baseW * (it->second.z / itBase->second.z),
                         baseH * (it->second.w / itBase->second.w));
    };

    const glm::vec2 verticalBodySize =
        textureSize(TextureID::HoldBodyVertical, ctx.noteW, ctx.noteH);
    const glm::vec2 horizontalBodySize =
        textureSize(TextureID::HoldBodyHorizontal, ctx.noteW, ctx.noteH);

    auto timeToY = [&](double time) {
        return judgmentLineY - static_cast<float>(ctx.cache->getDisplayDelta(
                                   time, ctx.currentAbsY, time)) *
                                   renderScaleY;
    };

    auto sameOwner = [](const OverlapItem& a, const OverlapItem& b) {
        return a.owner != entt::null && a.owner == b.owner;
    };

    auto countUniqueOwners = [](const std::vector<const OverlapItem*>& group,
                                size_t                                 begin,
                                size_t                                 end) {
        std::unordered_set<entt::entity> owners;
        owners.reserve(end - begin);
        for ( size_t i = begin; i < end; ++i ) {
            const auto* item = group[i];
            owners.insert(item->owner);
        }
        return static_cast<int>(owners.size());
    };

    auto appendMask = [&](float x, float y, float w, float h, int count) {
        if ( w <= 0.0f || h <= 0.0f || count < 2 ) return;
        if ( x > rightX || x + w < leftX || y > bottomY || y + h < topY )
            return;

        // 这里不能预先合并成外接矩形，否则相邻遮罩之间的空区域也会被误涂红。
        // 后面的扫描线拆分会负责去掉重复覆盖并保证同一像素最多只画一层。
        snapshot->overlapMasks.push_back({ x, y, w, h, count });
    };

    auto appendPointMask = [&](double time, int track, float scale, int count) {
        float w = ctx.noteW * scale;
        float h = ctx.noteH * scale;
        float x = leftX + static_cast<float>(track) * singleTrackW +
                  (singleTrackW - w) * 0.5f;
        float y = timeToY(time) - h * 0.5f;
        appendMask(x, y, w, h, count);
    };

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

    auto includeBucketTrack = [&](int track) {
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
            if ( item.endTime > item.startTime + timeEpsilon ) {
                holds.push_back(&item);
                pointMarkers.push_back(
                    { item.endTime, item.track, item.owner, 1.0f, true });
            }
            pointMarkers.push_back(
                { item.startTime, item.track, item.owner, 1.0f, true });
            includeBucketTrack(item.track);
        } else if ( item.type == ::MMM::NoteType::FLICK ) {
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

    if ( !hasBucketTrack ) return;

    auto bucketIndex = [&](int track) {
        return static_cast<size_t>(track - minBucketTrack);
    };

    size_t bucketCount =
        static_cast<size_t>(maxBucketTrack - minBucketTrack + 1);
    std::vector<std::vector<const OverlapItem*>> notesByTrack(bucketCount);
    std::vector<std::vector<const OverlapItem*>> holdsByTrack(bucketCount);
    std::vector<std::vector<OverlapPoint>> pointMarkersByTrack(bucketCount);

    for ( const auto* note : notes ) {
        notesByTrack[bucketIndex(note->track)].push_back(note);
    }
    for ( const auto* hold : holds ) {
        holdsByTrack[bucketIndex(hold->track)].push_back(hold);
    }
    for ( const auto& point : pointMarkers ) {
        pointMarkersByTrack[bucketIndex(point.track)].push_back(point);
    }

    auto itemStartLess = [](const OverlapItem* a, const OverlapItem* b) {
        if ( a->startTime != b->startTime ) {
            return a->startTime < b->startTime;
        }
        return a->endTime < b->endTime;
    };
    auto ensureItemTimeOrder = [&](std::vector<const OverlapItem*>& bucket) {
        if ( bucket.size() < 2 ) return;
        if ( std::is_sorted(bucket.begin(), bucket.end(), itemStartLess) )
            return;
        std::sort(bucket.begin(), bucket.end(), itemStartLess);
    };
    auto ensurePointTimeOrder = [](std::vector<OverlapPoint>& bucket) {
        if ( bucket.size() < 2 ) return;
        auto pointTimeLess = [](const OverlapPoint& a, const OverlapPoint& b) {
            return a.time < b.time;
        };
        if ( std::is_sorted(bucket.begin(), bucket.end(), pointTimeLess) )
            return;
        std::sort(bucket.begin(), bucket.end(), pointTimeLess);
    };

    for ( auto& trackNotes : notesByTrack ) {
        if ( trackNotes.size() < 2 ) continue;
        ensureItemTimeOrder(trackNotes);

        for ( size_t i = 0; i < trackNotes.size(); ) {
            size_t j = i + 1;
            while ( j < trackNotes.size() &&
                    trackNotes[j]->startTime - trackNotes[j - 1]->startTime <
                        windowSeconds ) {
                ++j;
            }

            if ( j - i >= 2 ) {
                double minTime = trackNotes[i]->startTime;
                double maxTime = trackNotes[i]->startTime;
                for ( size_t k = i; k < j; ++k ) {
                    minTime = std::min(minTime, trackNotes[k]->startTime);
                    maxTime = std::max(maxTime, trackNotes[k]->startTime);
                }

                int uniqueCount = countUniqueOwners(trackNotes, i, j);
                if ( uniqueCount >= 2 ) {
                    float y0 = timeToY(minTime);
                    float y1 = timeToY(maxTime);
                    float x  = leftX + trackNotes[i]->track * singleTrackW +
                               (singleTrackW - ctx.noteW) * 0.5f;
                    float y  = std::min(y0, y1) - ctx.noteH * 0.5f;
                    float h  = std::abs(y0 - y1) + ctx.noteH;
                    appendMask(x, y, ctx.noteW, h, uniqueCount);
                }
            }

            i = j;
        }
    }

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
                std::unordered_set<entt::entity> owners;
                double                           minTime = trackPoints[i].time;
                double                           maxTime = trackPoints[i].time;
                float maxScale                           = trackPoints[i].scale;
                for ( size_t k = i; k < j; ++k ) {
                    owners.insert(trackPoints[k].owner);
                    minTime  = std::min(minTime, trackPoints[k].time);
                    maxTime  = std::max(maxTime, trackPoints[k].time);
                    maxScale = std::max(maxScale, trackPoints[k].scale);
                }

                if ( owners.size() >= 2 ) {
                    float w  = ctx.noteW * maxScale;
                    float h0 = ctx.noteH * maxScale;
                    float y0 = timeToY(minTime);
                    float y1 = timeToY(maxTime);
                    float x  = leftX + trackPoints[i].track * singleTrackW +
                               (singleTrackW - w) * 0.5f;
                    float y  = std::min(y0, y1) - h0 * 0.5f;
                    float h  = std::abs(y0 - y1) + h0;
                    appendMask(x, y, w, h, static_cast<int>(owners.size()));
                }
            }

            i = j;
        }
    }

    for ( auto& trackHolds : holdsByTrack ) {
        if ( trackHolds.size() < 2 ) continue;
        ensureItemTimeOrder(trackHolds);

        int                 track = trackHolds.front()->track;
        std::vector<double> bounds;
        bounds.reserve(trackHolds.size() * 2);

        for ( const auto* hold : trackHolds ) {
            bounds.push_back(hold->startTime);
            bounds.push_back(hold->endTime);
        }
        std::sort(bounds.begin(), bounds.end());
        bounds.erase(std::unique(bounds.begin(),
                                 bounds.end(),
                                 [](double a, double b) {
                                     return std::abs(a - b) < 1e-7;
                                 }),
                     bounds.end());

        bool   hasOpenMask = false;
        double openStart   = 0.0;
        double openEnd     = 0.0;
        int    openCount   = 0;

        auto flushOpenMask = [&]() {
            if ( !hasOpenMask ) return;
            float y0 = timeToY(openStart);
            float y1 = timeToY(openEnd);
            float x  = leftX + track * singleTrackW +
                       (singleTrackW - verticalBodySize.x) * 0.5f;
            appendMask(x,
                       std::min(y0, y1),
                       verticalBodySize.x,
                       std::abs(y0 - y1),
                       openCount);
            hasOpenMask = false;
            openCount   = 0;
        };

        for ( size_t k = 0; k + 1 < bounds.size(); ++k ) {
            double segStart = bounds[k];
            double segEnd   = bounds[k + 1];
            if ( segEnd <= segStart + timeEpsilon ) continue;

            std::unordered_set<entt::entity> activeOwners;
            for ( const auto* hold : trackHolds ) {
                if ( hold->startTime >= segEnd - timeEpsilon ) break;
                if ( hold->endTime > segStart + timeEpsilon ) {
                    activeOwners.insert(hold->owner);
                }
            }

            int activeCount = static_cast<int>(activeOwners.size());
            if ( activeCount >= 2 ) {
                if ( !hasOpenMask ) {
                    hasOpenMask = true;
                    openStart   = segStart;
                    openEnd     = segEnd;
                    openCount   = activeCount;
                } else {
                    openEnd   = segEnd;
                    openCount = std::max(openCount, activeCount);
                }
            } else {
                flushOpenMask();
            }
        }
        flushOpenMask();
    }

    ensureItemTimeOrder(flicks);

    auto flickBodyMinTrack = [](const OverlapItem& item) {
        return std::min(item.track, item.track + item.dtrack);
    };
    auto flickBodyMaxTrack = [](const OverlapItem& item) {
        return std::max(item.track, item.track + item.dtrack);
    };

    for ( size_t i = 0; i < flicks.size(); ++i ) {
        const auto& a = *flicks[i];
        if ( a.dtrack == 0 ) continue;

        for ( size_t j = i + 1; j < flicks.size(); ++j ) {
            const auto& b = *flicks[j];
            if ( b.startTime - a.startTime > windowSeconds + timeEpsilon )
                break;
            if ( b.dtrack == 0 || sameOwner(a, b) ) continue;

            int overlapMin =
                std::max(flickBodyMinTrack(a), flickBodyMinTrack(b));
            int overlapMax =
                std::min(flickBodyMaxTrack(a), flickBodyMaxTrack(b));
            if ( overlapMax <= overlapMin ) continue;

            float y0 = timeToY(a.startTime);
            float y1 = timeToY(b.startTime);
            float x  = leftX + static_cast<float>(overlapMin) * singleTrackW +
                       singleTrackW * 0.5f;
            float w =
                static_cast<float>(overlapMax - overlapMin) * singleTrackW;
            float y = std::min(y0, y1) - horizontalBodySize.y * 0.5f;
            float h = std::abs(y0 - y1) + horizontalBodySize.y;
            appendMask(x, y, w, h, 2);
        }
    }

    const std::vector<const OverlapItem*> emptyHoldBucket;
    auto                                  getHoldBucket =
        [&](int track) -> const std::vector<const OverlapItem*>& {
        if ( track < minBucketTrack || track > maxBucketTrack ) {
            return emptyHoldBucket;
        }
        return holdsByTrack[bucketIndex(track)];
    };

    for ( const auto& point : pointMarkers ) {
        std::unordered_set<entt::entity> owners;
        owners.insert(point.owner);

        for ( const auto* hold : getHoldBucket(point.track) ) {
            if ( point.time <= hold->startTime + timeEpsilon ) break;
            if ( hold->owner == point.owner ) continue;
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
        if ( !point.testsFlickBody ) continue;

        std::unordered_set<entt::entity> owners;
        owners.insert(point.owner);

        auto firstFlick =
            std::lower_bound(flicks.begin(),
                             flicks.end(),
                             point.time - windowSeconds - timeEpsilon,
                             [](const OverlapItem* item, double time) {
                                 return item->startTime < time;
                             });

        for ( auto it = firstFlick; it != flicks.end(); ++it ) {
            const auto* flick = *it;
            if ( flick->startTime > point.time + windowSeconds + timeEpsilon )
                break;
            if ( flick->owner == point.owner || flick->dtrack == 0 ) continue;

            int minTrack = flickBodyMinTrack(*flick);
            int maxTrack = flickBodyMaxTrack(*flick);
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

        std::unordered_map<int, std::unordered_set<entt::entity>> ownersByTrack;
        int bodyMinTrack    = flickBodyMinTrack(*flick);
        int bodyMaxTrack    = flickBodyMaxTrack(*flick);
        int checkedMinTrack = std::max(bodyMinTrack, minBucketTrack);
        int checkedMaxTrack = std::min(bodyMaxTrack, maxBucketTrack);

        for ( int track = checkedMinTrack; track <= checkedMaxTrack; ++track ) {
            for ( const auto* hold : getHoldBucket(track) ) {
                if ( flick->startTime <= hold->startTime + timeEpsilon ) break;
                if ( sameOwner(*flick, *hold) ) continue;
                if ( flick->startTime >= hold->endTime - timeEpsilon ) continue;

                auto& owners = ownersByTrack[hold->track];
                owners.insert(flick->owner);
                owners.insert(hold->owner);
            }
        }

        for ( const auto& [track, owners] : ownersByTrack ) {
            appendPointMask(
                flick->startTime, track, 0.7f, static_cast<int>(owners.size()));
        }
    }

    if ( snapshot->overlapMasks.empty() ) return;

    auto flattenOverlapMasks = [&]() {
        if ( snapshot->overlapMasks.size() < 2 ) return;

        std::vector<float> xs;
        std::vector<float> ys;
        xs.reserve(snapshot->overlapMasks.size() * 2);
        ys.reserve(snapshot->overlapMasks.size() * 2);

        for ( const auto& mask : snapshot->overlapMasks ) {
            if ( mask.w <= 0.0f || mask.h <= 0.0f ) continue;
            xs.push_back(mask.x);
            xs.push_back(mask.x + mask.w);
            ys.push_back(mask.y);
            ys.push_back(mask.y + mask.h);
        }

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
        if ( xs.size() < 2 || ys.size() < 2 ) return;

        std::vector<RenderSnapshot::OverlapMask> flattened;
        flattened.reserve(snapshot->overlapMasks.size());

        // 将重叠矩形拆成互不相交的扫描线小矩形，避免同一像素被红色滤镜重复覆盖。
        for ( size_t yIndex = 0; yIndex + 1 < ys.size(); ++yIndex ) {
            const float y0 = ys[yIndex];
            const float y1 = ys[yIndex + 1];
            if ( y1 <= y0 + 0.01f ) continue;

            bool  hasRun   = false;
            float runStart = 0.0f;
            int   runCount = 0;
            auto  flushRun = [&](float runEnd) {
                if ( !hasRun || runEnd <= runStart + 0.01f ) return;
                flattened.push_back(
                    { runStart, y0, runEnd - runStart, y1 - y0, runCount });
                hasRun   = false;
                runCount = 0;
            };

            for ( size_t xIndex = 0; xIndex + 1 < xs.size(); ++xIndex ) {
                const float x0 = xs[xIndex];
                const float x1 = xs[xIndex + 1];
                if ( x1 <= x0 + 0.01f ) continue;

                const float sampleX = (x0 + x1) * 0.5f;
                const float sampleY = (y0 + y1) * 0.5f;
                int         count   = 0;
                for ( const auto& mask : snapshot->overlapMasks ) {
                    if ( sampleX < mask.x || sampleX > mask.x + mask.w ||
                         sampleY < mask.y || sampleY > mask.y + mask.h ) {
                        continue;
                    }
                    count = std::max(count, mask.objectCount);
                }

                if ( count >= 2 ) {
                    if ( !hasRun ) {
                        hasRun   = true;
                        runStart = x0;
                        runCount = count;
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

            flushRun(xs.back());
        }

        snapshot->overlapMasks = std::move(flattened);
    };

    flattenOverlapMasks();
    if ( snapshot->overlapMasks.empty() ) return;

    Batcher overlayBatcher(snapshot, &snapshot->overlayCmds);
    overlayBatcher.setScissor(leftX, topY, rightX - leftX, bottomY - topY);
    overlayBatcher.setTexture(TextureID::None);

    const glm::vec4 overlayColor{ 1.0f, 0.0f, 0.0f, 0.35f };
    for ( const auto& mask : snapshot->overlapMasks ) {
        overlayBatcher.pushQuad(
            mask.x, mask.y + mask.h, mask.w, mask.h, overlayColor);
    }
    overlayBatcher.flush();
}

void NoteRenderSystem::renderBrushPreview(
    RenderSnapshot* snapshot, const NoteRenderSystem::NoteRenderContext& ctx,
    const Config::EditorConfig& config, Batcher& batcher, float judgmentLineY,
    float leftX, float singleTrackW, float renderScaleY)
{
    const auto& brush = snapshot->brush;
    if ( !brush.isActive ) return;

    double noteAbsY = ctx.cache->getAbsY(brush.time);
    float  screenY =
        judgmentLineY - static_cast<float>(ctx.cache->getDisplayDelta(
                            brush.time, ctx.currentAbsY, brush.time)) *
                            renderScaleY;

    float trackX = leftX + brush.track * singleTrackW;

    NoteComponent tempNote;
    tempNote.m_type         = brush.type;
    tempNote.m_timestamp    = brush.time;
    tempNote.m_duration     = brush.duration;
    tempNote.m_trackIndex   = brush.track;
    tempNote.m_dtrack       = brush.dtrack;
    tempNote.m_customColors = brush.customColors;

    glm::vec4 previewNote =
        resolveNoteColor(tempNote, NoteColorSlot::Tap, ctx.colorTap);
    glm::vec4 previewHead =
        resolveNoteColor(tempNote, NoteColorSlot::Head, ctx.colorHead);
    glm::vec4 previewBody =
        resolveNoteColor(tempNote, NoteColorSlot::Hold, ctx.colorHold);
    glm::vec4 previewEnd =
        resolveNoteColor(tempNote, NoteColorSlot::End, ctx.colorEnd);
    glm::vec4 previewNode =
        resolveNoteColor(tempNote, NoteColorSlot::Node, ctx.colorNode);
    glm::vec4 previewArrow =
        resolveNoteColor(tempNote, NoteColorSlot::FlickArrow, ctx.colorArrow);

    previewNote.a *= 0.5f;
    previewHead.a *= 0.5f;
    previewBody.a *= 0.5f;
    previewEnd.a *= 0.5f;
    previewNode.a *= 0.5f;
    previewArrow.a *= 0.5f;

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
                                     renderScaleY);
    } else if ( brush.type == ::MMM::NoteType::FLICK ) {
        NoteRenderSystem::renderFlick(
            batcher,
            tempNote,
            config,
            snapshot,
            trackX + (singleTrackW - ctx.noteW) * 0.5f,
            screenY,
            ctx.noteW,
            ctx.noteH,
            singleTrackW,
            previewHead,
            previewBody,
            previewArrow);
    } else if ( brush.type == ::MMM::NoteType::POLYLINE ) {
        tempNote.m_subNotes = brush.polylineSegments;
        if ( !tempNote.m_subNotes.empty() ) {
            tempNote.m_timestamp  = tempNote.m_subNotes.front().timestamp;
            tempNote.m_trackIndex = tempNote.m_subNotes.front().trackIndex;
        }

        float rightX  = leftX + snapshot->trackCount * singleTrackW;
        float topY    = 0.0f;
        float bottomY = judgmentLineY * 2.0f;  // 简单包围盒

        NoteRenderSystem::renderPolyline(ctx.cache,
                                         batcher,
                                         tempNote,
                                         config,
                                         snapshot,
                                         ctx.currentAbsY,
                                         ctx.currentTime,
                                         judgmentLineY,
                                         leftX,
                                         rightX,
                                         topY,
                                         bottomY,
                                         singleTrackW,
                                         renderScaleY,
                                         previewHead,
                                         previewBody,
                                         previewEnd,
                                         previewNode,
                                         previewArrow);
    } else {
        // 兜底调试绘制。
        float x = trackX + (singleTrackW - ctx.noteW) * 0.5f;
        batcher.setTexture(TextureID::None);
        batcher.pushQuad(x,
                         screenY + ctx.noteH * 0.5f,
                         ctx.noteW,
                         ctx.noteH,
                         { 1.0f, 0.0f, 0.0f, 0.5f });
    }
    batcher.flush();
}

}  // namespace MMM::Logic::System
