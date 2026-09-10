#include "logic/BeatmapSession.h"

#include "audio/AudioManager.h"
#include "config/Utf8Path.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/PreviewDensity.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/NoteTransformSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/InteractionController.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "mmm/timing/BpmNormalization.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace MMM::Logic
{

namespace
{
/// @brief 在批注或目标物件变化后重建时间戳分组缓存。
/// @param ctx 当前会话上下文。
/// @warning 逻辑热路径中的脏分支：仅在编辑或载入后完整扫描一次物件 Registry，
/// 禁止改为每个 update 无条件执行。
/// @note 协作 ID 用于关联稳定对象，实体 ID 仅供本会话交互定位。
/// @note 目标缺失时保留批注自身时间及缺失标志，不删除用户批注。
/// @note 缓存按时间分组，视口生成阶段只截取可见分组。
/// @pre 脏标记必须随批注内容、目标位置和对象删除一起维护。
/// @post 完成重建或空谱面早退后，缓存版本均已推进。
/// @note 目标索引只覆盖当前会话；不能用另一会话实体号填充本缓存。
void rebuildAnnotationRenderCacheIfNeeded(SessionContext& ctx)
{
    // 普通帧只检查脏标记，不遍历批注或物件。
    // 目标位置变化与批注文本变化都必须由编辑入口置脏。
    if ( !ctx.isAnnotationRenderCacheDirty ) return;
    // 清空旧分组并推进版本，即使新状态没有批注也要通知消费端。
    // 先消费脏标记，空项目的早退不会导致每次 update 重复重建。
    ctx.annotationRenderCache.clear();
    ctx.isAnnotationRenderCacheDirty = false;
    ++ctx.annotationRenderCacheRevision;
    if ( !ctx.currentBeatmap || ctx.currentBeatmap->m_annotations.empty() ) {
        return;
    }

    /// @brief 一次批注重建期间的目标位置与本地拾取身份。
    struct ObjectPosition {
        /// @brief 目标可见时间，单位为秒。
        double timestamp{ 0.0 };
        /// @brief 统一轨号；默认值表示没有解析到目标轨道。
        std::int32_t track{ -1 };
        /// @brief 普通实体或折线父实体，不拥有 registry 内容。
        entt::entity entity{ entt::null };
        /// @brief 折线子项索引，普通物件使用负值。
        std::int32_t subIndex{ -1 };
    };
    // 临时索引只在本次重建期间存在，不跨实体生命周期缓存裸地址。
    // 键采用协作 ID，使批注目标不依赖加载时分配的 entt 实体号。
    // try_emplace 保留第一次遇到的相同键。
    std::unordered_map<std::string, ObjectPosition> playerObjects;
    const auto noteView = ctx.noteRegistry.view<const NoteComponent>();
    playerObjects.reserve(noteView.size());
    for ( const auto entity : noteView ) {
        const auto& note = noteView.get<const NoteComponent>(entity);
        if ( !note.m_collaborationId.empty() ) {
            // 子实体定位折线父实体及子索引，交互命令仍以父折线为入口。
            // 普通音符则直接记录自身实体，子索引保持负值。
            const bool isSubNote =
                note.m_isSubNote && note.m_parentPolyline != entt::null;
            playerObjects.try_emplace(
                note.m_collaborationId,
                ObjectPosition{ note.m_timestamp,
                                note.m_trackIndex,
                                isSubNote ? note.m_parentPolyline : entity,
                                isSubNote ? note.m_subIndex : -1 });
        }
        // 仅对顶层折线展开内嵌子项。
        // 子实体若再次展开会重复收集同一组目标。
        if ( note.m_type != ::MMM::NoteType::POLYLINE || note.m_isSubNote ) {
            continue;
        }
        for ( std::size_t index = 0U; index < note.m_subNotes.size();
              ++index ) {
            const auto& subNote = note.m_subNotes[index];
            // 内嵌子项可能尚无独立 ECS 实体，仍可通过父实体和数组下标定位。
            // 没有稳定 ID 的子项不构造占位键，避免所有空键互相覆盖。
            if ( !subNote.collaborationId.empty() ) {
                playerObjects.try_emplace(
                    subNote.collaborationId,
                    ObjectPosition{ subNote.timestamp,
                                    subNote.trackIndex,
                                    entity,
                                    static_cast<std::int32_t>(index) });
            }
        }
    }

    // 采样与玩家物件分开建表，目标种类决定在哪个 registry 查询。
    // 同一个字符串键在不同对象种类中不会误互相匹配。
    std::unordered_map<std::string, ObjectPosition> audioSamples;
    const auto sampleView = ctx.sampleRegistry.view<const SampleComponent>();
    audioSamples.reserve(sampleView.size());
    for ( const auto entity : sampleView ) {
        const auto& sample = sampleView.get<const SampleComponent>(entity);
        if ( !sample.m_collaborationId.empty() ) {
            // 批注跟随实际触发位置，而不是采样视觉锚点。
            // 偏移改变后需重建批注时间分组，否则提示会留在旧位置。
            audioSamples.try_emplace(
                sample.m_collaborationId,
                ObjectPosition{ sample.effectiveTime(),
                                static_cast<std::int32_t>(sample.m_track),
                                entity,
                                -1 });
        }
    }

    /// @brief 待排序的批注与最终显示时间。
    struct ResolvedAnnotation {
        /// @brief 解析目标后选定的分组时间，单位为秒。
        double timestamp{ 0.0 };
        /// @brief 批注文本、作者及可选目标身份，随后移动到缓存。
        AnnotationRenderItem item;
    };
    // 先解析目标位置，再统一排序分组。
    // 批注源容器顺序不保证与动态目标时间顺序相同。
    std::vector<ResolvedAnnotation> resolved;
    resolved.reserve(ctx.currentBeatmap->m_annotations.size());
    for ( const auto& annotation : ctx.currentBeatmap->m_annotations ) {
        ResolvedAnnotation entry;
        // 批注自身时间作为目标缺失时的回退；持久化毫秒在此转为秒。
        // 文本与作者复制到渲染项，消费端不再借用可编辑的批注对象。
        entry.timestamp       = annotation.m_timestamp / 1000.0;
        entry.item.id         = annotation.m_id;
        entry.item.targetKind = annotation.m_targetKind;
        entry.item.author     = annotation.m_author;
        entry.item.content    = annotation.m_content;
        if ( annotation.m_targetKind ==
             ::MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT ) {
            const auto target = playerObjects.find(annotation.m_targetId);
            // 先记录缺失状态，只有找到目标才覆盖回退时间。
            // 删除目标不会自动删除批注，其内容仍可在原时间被查看。
            entry.item.targetMissing = target == playerObjects.end();
            if ( target != playerObjects.end() ) {
                entry.timestamp           = target->second.timestamp;
                entry.item.track          = target->second.track;
                entry.item.targetEntity   = target->second.entity;
                entry.item.targetSubIndex = target->second.subIndex;
            }
        } else if ( annotation.m_targetKind ==
                    ::MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE ) {
            const auto target = audioSamples.find(annotation.m_targetId);
            // 音频采样也保留失效批注，但不套用玩家子索引规则。
            // 找到时只填写采样实体、统一轨道和触发时间。
            entry.item.targetMissing = target == audioSamples.end();
            if ( target != audioSamples.end() ) {
                entry.timestamp         = target->second.timestamp;
                entry.item.track        = target->second.track;
                entry.item.targetEntity = target->second.entity;
            }
        }
        // 非有限时间不能参与排序与可见区查询。
        // 丢弃的仅是本次渲染条目，不改写谱面持久化批注。
        if ( std::isfinite(entry.timestamp) ) {
            resolved.push_back(std::move(entry));
        }
    }
    // 同时间候选按批注 ID 排列，降低加载顺序变化造成的提示抖动。
    // 排序只在脏分支发生，不随播放逐帧重排。
    std::stable_sort(
        resolved.begin(), resolved.end(), [](const auto& lhs, const auto& rhs) {
            if ( std::abs(lhs.timestamp - rhs.timestamp) > 1e-7 ) {
                return lhs.timestamp < rhs.timestamp;
            }
            return lhs.item.id < rhs.item.id;
        });
    // 相近时间归入一个标记，具体文本项仍各自保留。
    // 分组容差与排序容差分别沿用当前实现，不据此合并持久化身份。
    for ( auto& entry : resolved ) {
        if ( ctx.annotationRenderCache.empty() ||
             std::abs(ctx.annotationRenderCache.back().timestamp -
                      entry.timestamp) > 1e-6 ) {
            ctx.annotationRenderCache.push_back(
                { .timestamp = entry.timestamp, .items = {} });
        }
        // 把拥有型文本移动进缓存，临时解析列表不再保留副本。
        // UI 后续只需按可见时间范围复制分组。
        ctx.annotationRenderCache.back().items.push_back(std::move(entry.item));
    }
}

/// @brief 谱面状态栏统计缓存。
struct BeatmapStatusStats {
    /// @brief 当前谱面的可计数物件数量。
    size_t noteCount{ 0 };

    /// @brief 当前谱面的最大连击数。
    size_t maxCombo{ 0 };
};

/// @brief 查询当前检视类型实际对应的悬浮部件拍位。
/// @param inspect 当前结构化悬浮检视信息。
/// @return 存在可显示拍位时返回对应部件，否则返回空指针。
/// @warning 快照热路径：仅执行固定分支选择，不得扫描 ECS 或分配内存。
/// @note 返回 inspect 内部观察指针，不能跨该值对象生命周期使用。
/// @note 长条身体只显示时长，不具有唯一可强调的拍点。
/// @note 采样触发点与锚点重合时可以回退到头部拍点。
/// @note 返回值只表达部件语义，不负责检查分母与时间是否有效。
/// @note 调用方生成分拍线前仍须执行拍区间校验。
const HoverBeatPoint* inspectedHoverBeatPoint(const HoverInspectInfo& inspect)
{
    // 部件类型决定唯一拍位来源，不能总取 Note 的头部时间。
    // 长条身体的检视信息表示区间，没有单个可强调拍点。
    switch ( inspect.kind ) {
    case HoverInspectKind::Note:
    case HoverInspectKind::HoldHead:
    case HoverInspectKind::FlickHead: return &inspect.head;
    case HoverInspectKind::HoldEnd:
    case HoverInspectKind::FlickEnd:
    case HoverInspectKind::PolylineHoldEnd:
    case HoverInspectKind::PolylineFlickEnd: return &inspect.end;
    case HoverInspectKind::FlickBody:
    case HoverInspectKind::PolylineHead:
    case HoverInspectKind::PolylineNode:
    case HoverInspectKind::PolylineFlickBody: return &inspect.body;
    case HoverInspectKind::AudioSampleAnchor: return &inspect.head;
    // 零偏移采样未必生成独立 end，触发点此时与锚点共用 head。
    // 回退只选择已有值，不临时构造一个悬空返回对象。
    case HoverInspectKind::AudioSampleTrigger:
        return inspect.end.show ? &inspect.end : &inspect.head;
    case HoverInspectKind::HoldBody:
    case HoverInspectKind::PolylineHoldBody:
    case HoverInspectKind::None: return nullptr;
    }
    return nullptr;
}

/// @brief 根据悬浮 Note 拍位生成单轨单拍临时分拍预览。
/// @param snapshot 待写入的渲染快照。
/// @param inspect 当前结构化悬浮检视信息。
/// @param currentBeatDivisor 当前全局分拍数。
/// @warning 快照热路径：仅执行常数次整数与浮点运算，不得访问完整注册表。
/// @note 会先清空旧预览，任何失败条件都保留关闭状态。
/// @note 已被当前网格整除的拍点无需再绘制额外分拍线。
/// @note 仅为玩家或草稿物件提供单轨预览，不用于自动采样。
/// @note 轨号校验使用快照轨道范围，因此必须先填写 trackCount。
/// @note 分母来自检视拟合，不能直接替换用户配置的全局分拍数。
void updateHoverSubdivisionPreview(RenderSnapshot&         snapshot,
                                   const HoverInspectInfo& inspect,
                                   int                     currentBeatDivisor)
{
    // 先清除复用快照上的旧预览。
    // 后续任何条件不满足时，都不能继续显示上一物件的分拍。
    snapshot.hoverSubdivisionPreview = HoverSubdivisionPreview{};
    if ( inspect.objectKind != ChartObjectKind::PlayerNote &&
         inspect.objectKind != ChartObjectKind::DraftNote ) {
        return;
    }

    // 只接受有意义的拍区间与范围内轨号。
    // 有限性检查先于后面的比例与整除计算。
    // 分母一已经是整拍，不需要额外细分提示。
    const auto* point = inspectedHoverBeatPoint(inspect);
    if ( !point || !point->show || point->track < -snapshot.trackCount ||
         point->track >= snapshot.trackCount || point->denominator <= 1 ||
         !std::isfinite(point->beatStartTime) ||
         !std::isfinite(point->beatEndTime) ||
         !std::isfinite(point->beatDuration) ||
         point->beatEndTime <= point->beatStartTime ||
         point->beatDuration <= 0.0 ) {
        return;
    }

    const int divisor = currentBeatDivisor > 0 ? currentBeatDivisor : 4;
    // 通过整数交叉相乘判断该拍点能否落在当前网格。
    // 提升到 64 位后再乘，避免普通 int 乘法过早溢出。
    const auto scaledNumerator =
        static_cast<std::int64_t>(point->numerator) * divisor;
    // 现有网格已能表达该位置时，不叠加另一套临时拍线。
    // 判定针对有理数位置，不用屏幕像素近似判断重合。
    if ( scaledNumerator % point->denominator == 0 ) return;

    snapshot.hoverSubdivisionPreview = {
        .show                  = true,
        .track                 = point->track,
        .numerator             = point->numerator,
        .denominator           = point->denominator,
        .commonBeatDivisorMask = 0U,
        .focusTime             = point->time,
        .beatStartTime         = point->beatStartTime,
        .beatEndTime           = point->beatEndTime,
        .beatDuration          = point->beatDuration,
    };
}

/// @brief 根据编辑手势目标拍位生成常用分拍线并集预览。
/// @param snapshot 待写入的渲染快照。
/// @param point 当前编辑目标对应的拍位。
/// @param commonBeatDivisorMask 用户启用的常用分拍线位掩码。
/// @warning 快照热路径：最多检查 23
/// 个固定分拍选项，不得访问完整注册表或分配内存。
/// @note 有效请求覆盖已有预览，失败请求不清除调用方现有内容。
/// @note 位掩码允许显示多个分拍层级，而 denominator 用于焦点高亮。
/// @note 此入口不改变实际编辑吸附结果，只准备绘制提示。
/// @note 未启用的掩码位不会传给渲染端。
/// @note 调用方负责选择拖动头、尾或画笔末梢，本函数不读取工具状态。
void updateCommonSubdivisionPreview(RenderSnapshot&       snapshot,
                                    const HoverBeatPoint& point,
                                    std::uint32_t         commonBeatDivisorMask)
{
    // 先剔除未定义位，避免配置中的旧位或损坏位扩展遍历范围。
    // 没有任何合法分拍时保持调用方现有预览不变。
    const auto validMask =
        commonBeatDivisorMask & Config::COMMON_BEAT_DIVISOR_MASK_ALL;
    if ( validMask == 0U || !point.show || point.track < -snapshot.trackCount ||
         point.track >= snapshot.trackCount || !std::isfinite(point.time) ||
         !std::isfinite(point.beatStartTime) ||
         !std::isfinite(point.beatEndTime) ||
         !std::isfinite(point.beatDuration) ||
         point.beatEndTime <= point.beatStartTime ||
         point.beatDuration <= 0.0 ) {
        return;
    }

    // 常用网格可以同时显示多组线，焦点颜色仍需一个分母。
    // 整拍位置优先取已启用的最小分母作为高亮层级。
    int highlightDenominator = point.denominator;
    if ( highlightDenominator <= 1 ) {
        // 遍历固定配置范围，最大工作量与谱面物件数无关。
        // 遇到第一个启用选项后停止，不为显示提示建立动态集合。
        for ( int divisor = Config::COMMON_BEAT_DIVISOR_MIN;
              divisor <= Config::COMMON_BEAT_DIVISOR_MAX;
              ++divisor ) {
            if ( Config::isCommonBeatDivisorEnabled(validMask, divisor) ) {
                highlightDenominator = divisor;
                break;
            }
        }
    }

    snapshot.hoverSubdivisionPreview = {
        .show                  = true,
        .track                 = point.track,
        .numerator             = point.numerator,
        .denominator           = highlightDenominator,
        .commonBeatDivisorMask = validMask,
        .focusTime             = point.time,
        .beatStartTime         = point.beatStartTime,
        .beatEndTime           = point.beatEndTime,
        .beatDuration          = point.beatDuration,
    };
}

/// @brief 判断视图是否属于播放态可背压的辅助画布。
/// @param cameraId 当前画布 ID。
/// @return Preview 或 Timeline 返回 true。
/// @warning 逻辑热路径：只做固定字符串比较。
bool isPlaybackSecondaryCameraId(const std::string& cameraId)
{
    return cameraId == "Preview" || cameraId == "Timeline";
}

/// @brief 计算 Hold 区间内的 1/4 拍连击增量。
/// @param startTime Hold 起始时间。
/// @param endTime Hold 结束时间。
/// @param beatmap 当前谱面数据。
/// @return 区间内按 BPM 分段累计的连击增量。
/// @warning 逻辑热路径低频分支：仅在音符/时间线脏标记触发统计重算时执行；禁止每
/// update 无条件调用。
/// @pre 起止时间与谱面 Timing 使用一致的秒单位。
/// @pre Timing 按时间排列，BPM 生效范围以相邻 BPM 为界。
/// @note 按四分之一拍累计连续数量，最后一次取整，避免逐段截断丢失。
/// @note 末段 BPM 决定三毫秒容差换算，不逐个端点额外计数。
size_t calculateIntervalCombos(double startTime, double endTime,
                               const ::MMM::BeatMap* beatmap)
{
    // 空谱面或非正区间没有持续连击增量。
    // 长条头部计数由调用方处理，本函数不额外加一。
    if ( !beatmap || endTime <= startTime ) {
        return 0;
    }

    // 跨 BPM 段先积累连续拍数，再统一取整。
    // 逐段取整会丢失跨边界拼接的不足一拍部分。
    double totalQuarterBeats = 0.0;
    double currTime          = startTime;

    // 第一条生效 BPM 以前使用谱面偏好值。
    // 统一规范化保护异常 BPM，不直接拿零或负值作节拍率。
    double currentBpm =
        ::MMM::normalizeBpmValue(beatmap->m_baseMapMetadata.preference_bpm);

    // 先确定区间起点处已生效的 BPM。
    // 同时定位后续查找起点，避免每段重新从列表首项开始。
    size_t nextTimingIdx = 0;
    for ( size_t i = 0; i < beatmap->m_timings.size(); ++i ) {
        const auto& timing = beatmap->m_timings[i];
        if ( timing.m_timingEffect == ::MMM::TimingEffect::BPM ) {
            // 起点恰有 BPM 时使用新值。
            // 其他 Timing 类型不改变连击增长速度。
            if ( timing.m_timestamp <= startTime ) {
                currentBpm = ::MMM::normalizeBpmValue(timing.m_bpm, currentBpm);
            } else {
                nextTimingIdx = i;
                break;
            }
        }
    }

    // 这是按数据段前进的循环，不等待真实播放时钟。
    // 每次推进到下一个 BPM 或区间末端。
    while ( currTime < endTime ) {
        // 默认本段覆盖剩余区间；只有内部出现 BPM 才截断。
        // 恰在 endTime 的事件不影响已结束区间的累计。
        double nextEventTime = endTime;
        double nextBpm       = currentBpm;
        size_t foundIdx      = beatmap->m_timings.size();

        for ( size_t i = nextTimingIdx; i < beatmap->m_timings.size(); ++i ) {
            const auto& timing = beatmap->m_timings[i];
            if ( timing.m_timingEffect == ::MMM::TimingEffect::BPM &&
                 timing.m_timestamp > currTime ) {
                if ( timing.m_timestamp < endTime ) {
                    nextEventTime = timing.m_timestamp;
                    nextBpm =
                        ::MMM::normalizeBpmValue(timing.m_bpm, currentBpm);
                    foundIdx = i + 1;
                }
                break;
            }
        }

        // 每秒拍数为 BPM/60，四分之一拍频率因此为 BPM/15。
        // 时间差与 BPM 必须保持同一秒单位。
        double dt = nextEventTime - currTime;
        totalQuarterBeats += dt * (currentBpm / 15.0);

        currTime   = nextEventTime;
        currentBpm = nextBpm;
        // 发现内部 BPM 后，下次从其后一项继续。
        // 未找到内部事件时本轮已到末端，无需再扫描。
        if ( foundIdx < beatmap->m_timings.size() ) {
            nextTimingIdx = foundIdx;
        }
    }

    // 把三毫秒转换成末段四分之一拍数，再补偿边界舍入。
    // 只在最终 floor 前应用一次，避免每段累积容差。
    double tolerance = 0.003 * (currentBpm / 15.0);
    return static_cast<size_t>(std::floor(totalQuarterBeats + tolerance));
}

/// @brief 将单个音符组件累计到状态栏统计。
/// @param note 当前音符组件。
/// @param beatmap 当前谱面数据。
/// @param stats 待更新的统计缓存。
/// @note 草稿过滤由调用方完成，本函数处理一个有效组件。
/// @note 折线子实体不再独立累计连击，防止与父折线子列表重复。
/// @note 物件数与连击数是不同统计口径，长条可产生多个连击。
/// @warning 仅在音符统计脏分支调用，可遍历折线子项和 BPM 段。
/// @note 连击依赖 BPM 数据，修改 Timing 后同样需要使统计失效。
/// @note 统计结果不依赖当前视口范围，不能只传入可见实体。
void accumulateNoteStats(const NoteComponent&  note,
                         const ::MMM::BeatMap* beatmap,
                         BeatmapStatusStats&   stats)
{
    // 折线容器不算一个独立可计数音符。
    // 物件数来自其中 Note、Hold、Flick 子项。
    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        for ( const auto& sub : note.m_subNotes ) {
            if ( sub.type == ::MMM::NoteType::NOTE ||
                 sub.type == ::MMM::NoteType::HOLD ||
                 sub.type == ::MMM::NoteType::FLICK ) {
                ++stats.noteCount;
            }
        }
    } else if ( !note.m_isSubNote ) {
        if ( note.m_type == ::MMM::NoteType::NOTE ||
             note.m_type == ::MMM::NoteType::HOLD ||
             note.m_type == ::MMM::NoteType::FLICK ) {
            ++stats.noteCount;
        }
    }

    // 子实体已由顶层折线统计，连击分支直接跳过。
    // 否则父子实体同时存在时会重复累计。
    if ( note.m_isSubNote ) {
        return;
    }

    if ( note.m_type == ::MMM::NoteType::NOTE ||
         note.m_type == ::MMM::NoteType::FLICK ) {
        ++stats.maxCombo;
    } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
        // 普通长条先计头部，再加持续区间的四分之一拍连击。
        // 持续区间长度不等于物件数量。
        ++stats.maxCombo;
        stats.maxCombo += calculateIntervalCombos(
            note.m_timestamp, note.m_timestamp + note.m_duration, beatmap);
    } else if ( note.m_type == ::MMM::NoteType::POLYLINE &&
                !note.m_subNotes.empty() ) {
        ++stats.maxCombo;
        // 首个折线子项已由折线头统一加过一次。
        // 若它是 Hold，只追加持续连击，不再次计算头部。
        if ( note.m_subNotes[0].type == ::MMM::NoteType::HOLD ) {
            stats.maxCombo += calculateIntervalCombos(
                note.m_subNotes[0].timestamp,
                note.m_subNotes[0].timestamp + note.m_subNotes[0].duration,
                beatmap);
        }
        // 后续 Flick 独立加一次，后续 Hold 只累计持续区间。
        // 普通连接节点不再增加连击，保持折线统计口径。
        for ( size_t i = 1; i < note.m_subNotes.size(); ++i ) {
            const auto& sub = note.m_subNotes[i];
            if ( sub.type == ::MMM::NoteType::FLICK ) {
                ++stats.maxCombo;
            } else if ( sub.type == ::MMM::NoteType::HOLD ) {
                stats.maxCombo += calculateIntervalCombos(
                    sub.timestamp, sub.timestamp + sub.duration, beatmap);
            }
        }
    }
}

/// @brief 判断物件类型是否应计入预览密度统计。
/// @param type 物件类型。
/// @return 可计数的 Note、Hold 或 Flick 返回 true。
/// @note 折线容器本身不计数，具体子物件类型由调用方展开。
/// @warning 密度缓存重建路径的固定枚举判断，不进行资源查询。
bool isPreviewDensityObjectType(::MMM::NoteType type)
{
    return type == ::MMM::NoteType::NOTE || type == ::MMM::NoteType::HOLD ||
           type == ::MMM::NoteType::FLICK;
}

/// @brief 将单个顶层音符组件的可计数时间写入密度时间缓存。
/// @param note 当前音符组件。
/// @param objectTimes 输出的物件时间缓存。
/// @warning 逻辑低频缓存重建路径：仅随音符排序或删减脏标记调用。
/// @note 只收集有限且非负的起始时间，不按持续时间重复计数。
/// @note 保留同时间的多个物件，密度不是唯一时间点数量。
/// @note 输出只追加不排序，统一排序由完成本批收集的调用方负责。
/// @note 调用方应排除草稿对象，使密度与正式谱面的计数口径一致。
/// @note 此缓存不包含自动采样；自动采样播放频率不代表玩家物件密度。
void appendPreviewDensityObjectTimes(const NoteComponent& note,
                                     std::vector<double>& objectTimes)
{
    if ( note.m_isSubNote ) {
        return;
    }

    // 密度按子项真实起点统计，不把折线容器重复记为一个物件。
    // 每个子项独立检查类型及时间合法性。
    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        for ( const auto& subNote : note.m_subNotes ) {
            if ( isPreviewDensityObjectType(subNote.type) &&
                 std::isfinite(subNote.timestamp) &&
                 subNote.timestamp >= 0.0 ) {
                objectTimes.push_back(subNote.timestamp);
            }
        }
        return;
    }

    // 同一时间可以出现多个物件，直接追加而不去重。
    // 持续时长影响连击统计，但不增加密度的起点数量。
    if ( isPreviewDensityObjectType(note.m_type) &&
         std::isfinite(note.m_timestamp) && note.m_timestamp >= 0.0 ) {
        objectTimes.push_back(note.m_timestamp);
    }
}

/// @brief 在物件或全谱时长变化时重建预览密度样本。
/// @param ctx 当前会话上下文。
/// @param totalDuration 当前谱面与音轨合并后的总时长，单位秒。
/// @warning 逻辑热路径低频分支：每个 update 只做常量级脏标记检查；实际
/// O(N+B) 重建仅在物件缓存或总时长变化时执行。
/// @pre previewDensityObjectTimes 已排序，其末项是最大合法物件时间。
/// @note 有效范围至少包含最后一个物件，避免音频较短时裁掉尾部密度。
/// @note 缓存更新后消费脏标记，普通 update 只比较范围。
/// @note 总时长变化即使没有新增物件，也可能改变密度分桶。
/// @note 缓存归会话所有，独立于某一个 Preview 相机的像素高度。
void rebuildPreviewDensitySnapshotIfNeeded(SessionContext& ctx,
                                           double          totalDuration)
{
    // 无效或非正总时长先按零处理。
    // 已有物件起点仍可把密度范围向后延伸。
    double effectiveDuration =
        std::isfinite(totalDuration) && totalDuration > 0.0 ? totalDuration
                                                            : 0.0;
    if ( !ctx.previewDensityObjectTimes.empty() ) {
        effectiveDuration =
            std::max(effectiveDuration, ctx.previewDensityObjectTimes.back());
    }

    // 极小范围变化不必重建同一密度图。
    // 物件内容变化仍由独立脏标记强制重建，不被该容差吞掉。
    constexpr double DURATION_EPSILON = 1e-6;
    const bool durationChanged = std::abs(ctx.previewDensityCache.duration -
                                          effectiveDuration) > DURATION_EPSILON;
    if ( !ctx.isPreviewDensityDirty && !durationChanged ) {
        return;
    }

    // 输入起点列表已经维护好，只重新生成分桶结果。
    // 完成后消费脏标记，使后续静止帧保持缓存命中。
    ctx.previewDensityCache = buildPreviewDensitySnapshot(
        ctx.previewDensityObjectTimes, effectiveDuration);
    ctx.isPreviewDensityDirty = false;
}

/// @brief 计算预览区相对主画布的纵向渲染缩放倍率。
/// @param ctx 当前会话上下文。
/// @param previewCamera 预览区相机信息。
/// @param config 当前编辑器配置。
/// @return 可用的预览区纵向缩放倍率；输入尺寸无效时返回 0。
/// @warning 逻辑/渲染热路径：每个 Session update 的预览相关分支调用；只允许
/// 常量时间计算，禁止 ECS 遍历、文件系统访问和阻塞操作。
/// @note 无主相机时使用兼容高度，只用于相机初始化过渡期。
/// @note Preview 使用像素边距，主画布使用归一化轨道上下边界。
/// @note 返回零表示不能进行逆映射，调用方不得拿它作为除数。
float calculatePreviewRenderScaleY(const SessionContext&       ctx,
                                   const CameraInfo&           previewCamera,
                                   const Config::EditorConfig& config)
{
    // 主相机尚未注册时使用固定兼容高度。
    // 相机存在后按实际像素高度计算，不保留旧回退值。
    const auto* mainCamera = SessionUtils::findMainCanvasCamera(ctx.cameras);
    float       mainViewportHeight =
        mainCamera ? mainCamera->viewportHeight : 1000.0f;
    float mainEffectiveH =
        (config.visual.trackLayout.bottom - config.visual.trackLayout.top) *
        mainViewportHeight;
    float previewDrawH = previewCamera.viewportHeight -
                         (config.visual.previewConfig.margin.top +
                          config.visual.previewConfig.margin.bottom);
    float areaRatio    = config.visual.previewConfig.areaRatio;

    // 没有有效绘制面积时拒绝比例换算。
    // 返回零让上层跳过除法，不把退化视图放大到异常比例。
    if ( mainEffectiveH <= 0.0001f || previewDrawH <= 0.0001f ||
         areaRatio <= 0.0001f ) {
        return 0.0f;
    }

    return previewDrawH / (mainEffectiveH * areaRatio);
}

/// @brief 将当前动画缩放比例同步到 ScrollCache。
/// @param ctx 当前会话上下文。
/// @param config 当前编辑器配置。
/// @warning 逻辑/渲染热路径：每个 Session update 调用；只做常量级查找和赋值。
/// @note 缓存已含目标缩放，这里只同步动画值相对目标值的比率。
/// @note 非有限或过小的目标缩放回退一，动画异常时回退目标值。
/// @note 不在渲染准备阶段重建全部滚动段。
/// @note 动画倍率对后续正向与逆向映射共同生效。
/// @note 同帧生成悬浮位置前必须先同步，避免拾取与可见几何使用不同缩放。
void syncScrollCacheAnimatedZoom(SessionContext&             ctx,
                                 const Config::EditorConfig& config)
{
    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) {
        return;
    }

    // 滚动缓存已按目标缩放构建。
    // 本次只提供动画相对目标的比例，不重复乘目标 zoom。
    double targetZoom = static_cast<double>(config.visual.timelineZoom);
    if ( !std::isfinite(targetZoom) || targetZoom <= 1e-9 ) {
        targetZoom = 1.0;
    }

    // 动画状态可能处于初始化阶段，非法值回退到目标。
    // 这样得到比例一，而不是让几何位置坍缩为零。
    double animateZoom = static_cast<double>(ctx.animatedTimelineZoom);
    if ( !std::isfinite(animateZoom) || animateZoom <= 1e-9 ) {
        animateZoom = targetZoom;
    }

    cache->setAnimatedZoomScale(animateZoom / targetZoom);
}

/// @brief 在生成视口快照前同步预览拖拽目标时间。
/// @param ctx 当前会话上下文。
/// @param config 当前编辑器配置。
/// @warning 逻辑/渲染热路径：每个 Session update 调用；只读取预览相机、
/// ScrollCache 和最后鼠标坐标，禁止引入 ECS 遍历或共享所有权复制。
/// @note 在遍历相机之前更新共享目标，避免相机遍历顺序影响主画布反馈。
/// @note 仅处理 Preview 直接拖动；波形与频谱路径由它们的输入更新目标时间。
/// @note 失败时保持上一有效目标，不写入不可用的时间值。
/// @note 鼠标坐标属于 Preview 视口局部空间，而非整个应用窗口。
/// @note 目标时间通过 ScrollCache 逆映射，不能用固定像素每秒替代。
void syncPreviewDragHoverTime(SessionContext&             ctx,
                              const Config::EditorConfig& config)
{
    // 只在预览手势期间换算最后鼠标位置。
    // 普通悬浮由每相机检视分支处理，不覆盖共享拖动目标。
    if ( !ctx.isDragging ||
         (ctx.dragCameraId != "Preview" && ctx.mouseCameraId != "Preview") ) {
        return;
    }

    // 借用已有预览相机，不因拖动输入而临时创建视口。
    // 相机可能在切换布局时尚未就绪，缺失时保持旧目标。
    auto cameraIt = ctx.cameras.find("Preview");
    if ( cameraIt == ctx.cameras.end() ) {
        return;
    }

    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) {
        return;
    }

    const auto& previewCamera = cameraIt->second;
    float       renderScaleY =
        calculatePreviewRenderScaleY(ctx, previewCamera, config);
    if ( std::abs(renderScaleY) <= 0.0001f ) {
        return;
    }

    float judgmentLineY =
        previewCamera.viewportHeight * config.visual.judgeline_pos;
    double currentAbsY = cache->getAbsY(ctx.animateTime);
    // 先把压缩后的屏幕距离还原成滚动距离，再逆映射到时间。
    // 不能直接将鼠标 Y 当作毫秒或线性时间差。
    double deltaY        = (judgmentLineY - ctx.lastMousePos.y) /
                           static_cast<double>(renderScaleY);
    ctx.previewHoverTime = cache->getTime(currentAbsY + deltaY);
}
}  // namespace

/// @brief 更新 ECS 状态并为当前 Session 的视口生成渲染快照。
/// @warning 逻辑/渲染热路径：每个 Session update 执行；后台 Session
/// 不生成拾取/悬浮交互数据。
/// @param config 本次会话更新的编辑器配置。
/// @param isActiveSession 当前会话是否拥有共享辅助视图与交互焦点。
/// @pre m_ctx 与交互控制器已初始化，调用期间实体修改由逻辑线程串行处理。
/// @note 一次更新先刷新脏索引，再把共用状态分发到各相机快照。
/// @note 辅助视图背压只跳过本次生成，不 sleep，也不阻塞主画布。
/// @note 完整快照在生成结束后发布，UI 不得读取仍在填写的工作缓冲。
/// @warning 背景路径这里只拼接字符串，不得加入文件存在性或目录查询。
/// @warning 同步缓冲首次缓存会复制共享所有权以覆盖跨线程消费生命周期；
/// 后续 update 借用缓存槽，不能改为每帧重新向注册表申请所有权。
/// @note 相机快照共用本轮谱面元数据，但保留各自视口坐标和裁剪范围。
/// @note 编辑脏标记的生产由命令入口负责，本函数只消费并发布派生数据。
void BeatmapSession::updateECSAndRender(const Config::EditorConfig& config,
                                        bool isActiveSession)
{
    /// @brief 根据已排序音符刷新结束时间前缀及按需统计。
    /// @param rebuildStats 是否同步状态栏物件数与连击数。
    /// @param rebuildDensity 是否重新收集密度起始时间。
    /// @warning 仅在排序、删减或统计脏分支调用，不能每帧无条件执行。
    auto rebuildNotePrefixAndStats = [this](bool rebuildStats,
                                            bool rebuildDensity) {
        // 前缀最大结束时间支持查找从窗口前开始、但仍延伸到窗口内的长物件。
        // 它与排序实体列表必须在同一次重建中更新。
        m_ctx->sortedNoteMaxEndPrefix.clear();
        m_ctx->sortedNoteMaxEndPrefix.reserve(m_ctx->sortedNoteEntities.size());
        // 统计变化不一定影响密度，按调用需求分别重建。
        // 重用容量，避免每次编辑都丢弃此前分配。
        if ( rebuildDensity ) {
            m_ctx->previewDensityObjectTimes.clear();
            m_ctx->previewDensityObjectTimes.reserve(
                m_ctx->sortedNoteEntities.size());
        }

        BeatmapStatusStats stats;
        // 只借用当前谱面以读取 BPM，不复制共享所有权。
        // 物件本身以 ECS 为准，未写回的编辑也必须立即反映到统计。
        const auto* beatmap    = m_ctx->currentBeatmap.get();
        double      maxEndTime = 0.0;
        for ( auto entity : m_ctx->sortedNoteEntities ) {
            if ( !m_ctx->noteRegistry.valid(entity) ||
                 !m_ctx->noteRegistry.all_of<NoteComponent>(entity) ) {
                continue;
            }

            const auto& note =
                m_ctx->noteRegistry.get<const NoteComponent>(entity);
            // 草稿不纳入玩家状态栏统计和预览密度。
            // 可见性前缀仍包含草稿，渲染不能因此丢掉草稿物件。
            if ( rebuildStats && !note.m_isDraft ) {
                accumulateNoteStats(note, beatmap, stats);
            }
            if ( rebuildDensity && !note.m_isDraft ) {
                appendPreviewDensityObjectTimes(
                    note, m_ctx->previewDensityObjectTimes);
            }

            // 负时长不能让结束位置落在起点之前。
            // 折线需要同时检查全部子项，父组件时长不足以代表最远范围。
            double noteEnd = note.m_timestamp + std::max(0.0, note.m_duration);
            if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
                for ( const auto& sub : note.m_subNotes ) {
                    noteEnd = std::max(
                        noteEnd, sub.timestamp + std::max(0.0, sub.duration));
                }
            }
            // 保存前缀最大值而非当前实体结束值。
            // 长物件位于更早位置时，后续短物件不能把可见候选范围缩短。
            maxEndTime = std::max(maxEndTime, noteEnd);
            m_ctx->sortedNoteMaxEndPrefix.push_back(maxEndTime);
        }

        // 一次性发布两个统计结果并消费脏标记。
        // 避免使用一新一旧的物件数与最大连击。
        if ( rebuildStats ) {
            m_ctx->noteCount        = stats.noteCount;
            m_ctx->maxCombo         = stats.maxCombo;
            m_ctx->isNoteStatsDirty = false;
        }
        // 顶层音符有序不保证折线子项展开后全局有序。
        // 统一排序起点列表，再让后面分桶缓存按脏标记更新。
        if ( rebuildDensity ) {
            std::sort(m_ctx->previewDensityObjectTimes.begin(),
                      m_ctx->previewDensityObjectTimes.end());
            m_ctx->isPreviewDensityDirty = true;
        }
    };

    // 在需要时重建已排序音符实体缓存。
    // 新增或时间变化可能改变顺序，必须重建完整排序索引。
    // 删除分支不能代替这里的重排。
    if ( m_ctx->isNoteOrderDirty ) {
        auto noteView = m_ctx->noteRegistry.view<const NoteComponent>();
        m_ctx->sortedNoteEntities.assign(noteView.begin(), noteView.end());
        std::sort(m_ctx->sortedNoteEntities.begin(),
                  m_ctx->sortedNoteEntities.end(),
                  [this](entt::entity a, entt::entity b) {
                      return m_ctx->noteRegistry.get<const NoteComponent>(a)
                                 .m_timestamp <
                             m_ctx->noteRegistry.get<const NoteComponent>(b)
                                 .m_timestamp;
                  });
        // 状态栏统计直接以当前 ECS 为准，不依赖延迟写回的 BeatMap 音符容器。
        // 否则 m_needsNotesSync 等待空闲期间 isNoteStatsDirty 无法消费，
        // 会让每个逻辑 update 都强制生成全部视口的渲染快照。
        rebuildNotePrefixAndStats(true, true);
        // 修订号通知可见性消费端旧索引位置已失效。
        // 完整重建也覆盖了删除修剪，两个脏标记一起清除。
        ++m_ctx->noteVisibilityIndexRevision;
        m_ctx->isNoteOrderDirty = false;
        m_ctx->isNotePruneDirty = false;
        // 纯删除不改变剩余元素时间顺序。
        // 原地过滤失效实体即可，随后重新累计前缀和统计。
    } else if ( m_ctx->isNotePruneDirty ) {
        /// @brief 判断排序索引中的实体是否已被删除或失去音符组件。
        /// @param entity 待过滤的本地实体。
        /// @return 无法安全读取 NoteComponent 时返回 true。
        /// @warning 删除后的低频索引修剪，不扫描其他 registry。
        auto isEntityInvalid = [this](entt::entity entity) {
            return !m_ctx->noteRegistry.valid(entity) ||
                   !m_ctx->noteRegistry.all_of<NoteComponent>(entity);
        };
        m_ctx->sortedNoteEntities.erase(
            std::remove_if(m_ctx->sortedNoteEntities.begin(),
                           m_ctx->sortedNoteEntities.end(),
                           isEntityInvalid),
            m_ctx->sortedNoteEntities.end());
        rebuildNotePrefixAndStats(true, true);
        ++m_ctx->noteVisibilityIndexRevision;
        m_ctx->isNotePruneDirty = false;
        // 只有统计配置变化时沿用实体顺序和密度起点。
        // 仍刷新结束前缀，使该助手输出保持统一结构。
    } else if ( m_ctx->isNoteStatsDirty ) {
        rebuildNotePrefixAndStats(true, false);
    }

    // registry 上下文保存会话缓存的观察入口。
    // 指向 vector 对象而非其内部数据，容量变化不会改变对象地址。
    if ( auto** sortedEntitiesPtr =
             m_ctx->noteRegistry.ctx()
                 .find<const std::vector<entt::entity>*>() ) {
        *sortedEntitiesPtr = &m_ctx->sortedNoteEntities;
    } else {
        m_ctx->noteRegistry.ctx().emplace<const std::vector<entt::entity>*>(
            &m_ctx->sortedNoteEntities);
    }
    // 结束时间前缀与实体索引由不同类型槽访问。
    // 两个缓存生命周期都覆盖本次系统调用。
    if ( auto** maxEndPrefixPtr =
             m_ctx->noteRegistry.ctx().find<const std::vector<double>*>() ) {
        *maxEndPrefixPtr = &m_ctx->sortedNoteMaxEndPrefix;
    } else {
        m_ctx->noteRegistry.ctx().emplace<const std::vector<double>*>(
            &m_ctx->sortedNoteMaxEndPrefix);
    }
    // 消费端可用修订号识别索引内容是否变化。
    // 借用标量地址，避免每帧复制一份全量索引来比较。
    if ( auto** revisionPtr =
             m_ctx->noteRegistry.ctx().find<const std::uint64_t*>() ) {
        *revisionPtr = &m_ctx->noteVisibilityIndexRevision;
    } else {
        m_ctx->noteRegistry.ctx().emplace<const std::uint64_t*>(
            &m_ctx->noteVisibilityIndexRevision);
    }
    // 拖动物件可能暂时离开普通可见候选范围。
    // 独立固定列表确保拖动预览仍能参与绘制。
    if ( auto* pinnedEntities =
             m_ctx->noteRegistry.ctx().find<DragRenderPinnedEntities>() ) {
        pinnedEntities->entities = &m_ctx->dragRenderPinnedEntities;
    } else {
        auto& pinnedEntityView =
            m_ctx->noteRegistry.ctx().emplace<DragRenderPinnedEntities>();
        pinnedEntityView.entities = &m_ctx->dragRenderPinnedEntities;
    }

    /// @brief 为排序后的自动采样建立区间结束位置前缀。
    /// @note 采样视觉区间覆盖锚点与实际触发点，而不是完整媒体时长。
    /// @warning 仅随采样排序或删除刷新，禁止在每帧渲染中重建。
    auto rebuildSamplePrefix = [this]() {
        m_ctx->sortedSampleMaxEndPrefix.clear();
        m_ctx->sortedSampleMaxEndPrefix.reserve(
            m_ctx->sortedSampleEntities.size());
        // 采样起点允许为负，初始前缀不能固定为零。
        // 否则所有零点前采样都会得到虚假的结束上界。
        double maxEndTime = -std::numeric_limits<double>::infinity();
        for ( const auto entity : m_ctx->sortedSampleEntities ) {
            if ( !m_ctx->sampleRegistry.valid(entity) ||
                 !m_ctx->sampleRegistry.all_of<SampleComponent>(entity) ) {
                continue;
            }
            const auto& sample =
                m_ctx->sampleRegistry.get<const SampleComponent>(entity);
            maxEndTime =
                std::max(maxEndTime,
                         // 视觉采样区间包含锚点与触发点两端。
                         // 正负偏移都以较远端作为结束位置。
                         std::max(sample.m_timestamp, sample.effectiveTime()));
            m_ctx->sortedSampleMaxEndPrefix.push_back(maxEndTime);
        }
    };

    // 自动采样拥有独立的低频可见性索引，普通渲染路径不扫描完整 Registry。
    // 采样和音符分开维护索引，改变一种对象不迫使另一种重排。
    // 排序键采用两端较早时间，支持负偏移向前延伸。
    if ( m_ctx->isSampleOrderDirty ) {
        const auto sampleView =
            m_ctx->sampleRegistry.view<const SampleComponent>();
        m_ctx->sortedSampleEntities.assign(sampleView.begin(),
                                           sampleView.end());
        std::sort(m_ctx->sortedSampleEntities.begin(),
                  m_ctx->sortedSampleEntities.end(),
                  [this](entt::entity lhs, entt::entity rhs) {
                      const auto& left =
                          m_ctx->sampleRegistry.get<const SampleComponent>(lhs);
                      const auto& right =
                          m_ctx->sampleRegistry.get<const SampleComponent>(rhs);
                      const double leftStart =
                          std::min(left.m_timestamp, left.effectiveTime());
                      const double rightStart =
                          std::min(right.m_timestamp, right.effectiveTime());
                      // 先比较视觉区间起点，同起点再按实体整数值打破平局。
                      // 确定次序让缓存修订后的候选遍历可重现。
                      if ( leftStart != rightStart )
                          return leftStart < rightStart;
                      return entt::to_integral(lhs) < entt::to_integral(rhs);
                  });
        rebuildSamplePrefix();
        ++m_ctx->sampleVisibilityIndexRevision;
        m_ctx->isSampleOrderDirty = false;
        m_ctx->isSamplePruneDirty = false;
        // 纯删除保留剩余采样顺序，只剔除失效条目。
        // 修剪后必须重建前缀，旧位置与新实体下标不再对应。
    } else if ( m_ctx->isSamplePruneDirty ) {
        m_ctx->sortedSampleEntities.erase(
            std::remove_if(
                m_ctx->sortedSampleEntities.begin(),
                m_ctx->sortedSampleEntities.end(),
                [this](entt::entity entity) {
                    return !m_ctx->sampleRegistry.valid(entity) ||
                           !m_ctx->sampleRegistry.all_of<SampleComponent>(
                               entity);
                }),
            m_ctx->sortedSampleEntities.end());
        rebuildSamplePrefix();
        ++m_ctx->sampleVisibilityIndexRevision;
        m_ctx->isSamplePruneDirty = false;
    }

    // 索引和目标位置准备后，再处理可能依赖它们的批注。
    // 批注未置脏时这里只做一次布尔检查。
    rebuildAnnotationRenderCacheIfNeeded(*m_ctx);

    // 在坐标投影和鼠标逆映射前同步动画比例。
    // 同一 update 内不能让绘制与交互使用不同缩放阶段。
    syncScrollCacheAnimatedZoom(*m_ctx, config);

    // 1. 调用 ECS System 更新全局物理位置 (Logical Transform)
    // 注意：物理位置更新应基于逻辑时间 m_ctx->currentTime
    // 逻辑变换使用真实会话时间，快照投影随后使用动画时间。
    // 两者职责不同，不能为了平滑显示改写逻辑播放位置。
    System::NoteTransformSystem::update(m_ctx->noteRegistry,
                                        m_ctx->timelineRegistry,
                                        m_ctx->currentTime,
                                        config,
                                        m_ctx->currentBeatmap.get(),
                                        m_ctx->isTransformDirty);
    m_ctx->isTransformDirty = false;

    // 0. 更新 BPM 缓存（仅在脏时执行 O(N log N) 操作）
    SessionUtils::ensureBpmEvents(*m_ctx);

    // 筛选出所有 BPM 标记供后续视口处理（磁轴、智能拟合等）
    const auto& bpmEvents = m_ctx->bpmEvents;

    // 0. 框选区域变化时更新选中状态，避免旧框选框每帧覆盖手动选择。
    // 只在选框数据变化时重新应用选中结果。
    // 旧选框不能在每帧覆盖用户后来手动修改的选择。
    if ( m_ctx->isMarqueeSelectionDirty ) {
        m_interaction->updateMarqueeSelection();
    }

    syncPreviewDragHoverTime(*m_ctx, config);

    auto&        engine = EditorEngine::instance();
    const double secondaryCameraSnapshotMinInterval =
        engine.adaptiveRenderSnapshotMinInterval(config, true);
    // 同步跟随会话虽不直接驱动音频，也需要发布播放态视觉快照。
    // 该标志控制补间和辅助视图限频，不能只看本地 isPlaying。
    const bool snapshotIsPlaying =
        m_ctx->isPlaying || m_ctx->isAudioTimelineSyncFollower;
    const double snapshotTotalTime =
        SessionUtils::getEffectiveTotalTimeSeconds(*m_ctx);
    rebuildPreviewDensitySnapshotIfNeeded(*m_ctx, snapshotTotalTime);
    const double renderSnapshotNow =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const double visualClockTime =
        m_ctx->playbackVisualClock.lastResolvedSteadyTime();
    // 播放快照沿用视觉时钟的已解析基准，避免把生成耗时误计为播放延迟。
    // 上限钳到当前单调时钟，不发布未来时间戳。
    // 暂停或基准无效时直接使用当前生成时刻。
    const double snapshotSysTime =
        snapshotIsPlaying && std::isfinite(visualClockTime) &&
                visualClockTime > 0.0
            ? std::min(visualClockTime, renderSnapshotNow)
            : renderSnapshotNow;
    const double snapshotPlaybackSpeed =
        Audio::AudioManager::instance().getPlaybackSpeed();

    const bool hasBeatmap = (m_ctx->currentBeatmap != nullptr);
    // 快照只保存不可解引用的地址令牌，用于 UI 排除会话切换后的旧快照。
    // 地址只作不透明身份令牌，UI 不得解引用成谱面指针。
    // 切换会话后可据此拒绝旧缓冲的画面。
    const std::uintptr_t snapshotBeatmapInstanceId =
        reinterpret_cast<std::uintptr_t>(m_ctx->currentBeatmap.get());
    // 相机共用的元信息先收集一次，再复制到各视图。
    // 默认空值用于无谱面状态，防止沿用上一次封面和标题。
    std::string snapshotBackgroundPath;
    bool        snapshotBackgroundIsVideo        = false;
    double      snapshotBackgroundVideoStartTime = 0.0;
    std::string snapshotBeatmapPathKey;
    std::string snapshotBeatmapName;
    bool        snapshotIsDirty     = false;
    double      snapshotFallbackBpm = ::MMM::DEFAULT_NORMALIZED_BPM;
    if ( m_ctx->currentBeatmap ) {
        const auto& metadata = m_ctx->currentBeatmap->m_baseMapMetadata;
        snapshotFallbackBpm = ::MMM::normalizeBpmValue(metadata.preference_bpm);

        if ( !metadata.main_cover_path.empty() ) {
            std::filesystem::path bgPath;
            // 协作项目优先使用其独立资源根。
            // 没有项目时才回退到谱面父目录，支持单文件载入。
            auto* project = m_ctx->collaborationProject
                                ? m_ctx->collaborationProject.get()
                                : engine.getCurrentProject();
            std::filesystem::path resourcePath = metadata.main_cover_path;
            const auto resourceKey = Config::pathToUtf8(resourcePath);
            // 协作资源映射可能改变本地文件位置。
            // 这里只查内存映射并拼路径，不进行文件存在性检查。
            if ( const auto iterator =
                     m_ctx->collaborationPathRemap.find(resourceKey);
                 iterator != m_ctx->collaborationPathRemap.end() ) {
                resourcePath = Config::utf8ToPath(iterator->second);
            }
            if ( project ) {
                bgPath = project->m_projectRoot / resourcePath;
            } else {
                bgPath = metadata.map_path.parent_path() / resourcePath;
            }
            snapshotBackgroundPath = Config::pathToUtf8(bgPath);
        }
        snapshotBackgroundIsVideo =
            metadata.cover_type == MMM::CoverType::VIDEO;
        // 视频起始时间从持久化毫秒转成秒。
        // 与动画播放时间保持同一单位，具体帧解码留给背景服务。
        snapshotBackgroundVideoStartTime =
            static_cast<double>(metadata.video_starttime) / 1000.0;
        snapshotBeatmapPathKey = Config::pathToUtf8(metadata.map_path);
        snapshotBeatmapName    = metadata.name;
        snapshotIsDirty        = m_ctx->actionStack.isDirty();
    }

    // 先给状态栏可用默认值，再从缓存取当前位置的 Timing。
    // 无缓存时仍可显示谱面偏好 BPM 和单位 SV。
    double    snapshotCurrentBpm       = snapshotFallbackBpm;
    double    snapshotCurrentSv        = 1.0;
    const int snapshotCurrentBeatIndex = SessionUtils::calculateBeatIndex(
        m_ctx->animateTime, bpmEvents, snapshotFallbackBpm);
    if ( const auto* cache =
             m_ctx->timelineRegistry.ctx().find<System::ScrollCache>() ) {
        // 状态栏使用动画时间，与可见拍线保持一致。
        // SV 非有限时保留默认值，避免 UI 显示无意义数值。
        const auto timingState = cache->getTimingStateAt(m_ctx->animateTime);
        snapshotCurrentBpm =
            ::MMM::normalizeBpmValue(timingState.bpm, snapshotFallbackBpm);
        if ( std::isfinite(timingState.sv) ) {
            snapshotCurrentSv = timingState.sv;
        }
    }

    // 2. 遍历所有注册的视口 (Camera) 进行独立的视口剔除和坐标映射
    // 每个相机有独立几何与同步缓冲。
    // 共用状态已在循环外收集，视口相关状态在这里单独投影。
    for ( auto& [cameraId, camera] : m_ctx->cameras ) {
        // 只有活跃 Session 才能往 Preview 和 Timeline 缓冲写入，避免后台
        // Session 覆盖
        // Preview 与 Timeline 是活跃会话共享视图。
        // 后台会话可以保留自己的主画布，但不得覆盖共享辅助缓冲。
        if ( (cameraId == "Preview" || cameraId == "Timeline") &&
             !isActiveSession ) {
            continue;
        }

        // 只对播放中的辅助视图应用背压。
        // 主画布和暂停态交互不会因为这条限频规则等待。
        const bool isSecondaryPlaybackCamera =
            snapshotIsPlaying && isPlaybackSecondaryCameraId(cameraId);
        // 波形或频谱正在处理手势时，Preview 也有即时反馈责任。
        // 即使鼠标不在 Preview 内，也要绕过其普通播放限频。
        const bool isPreviewExternalDrag =
            cameraId == "Preview" && (m_ctx->dragCameraId == "AudioWaveform" ||
                                      m_ctx->dragCameraId == "AudioSpectrum" ||
                                      m_ctx->mouseCameraId == "AudioWaveform" ||
                                      m_ctx->mouseCameraId == "AudioSpectrum");
        // 鼠标、拖动、框选、画笔与橡皮擦都视为连续交互。
        // 活动会话优先保证本地反馈，不等辅助快照间隔结束。
        const bool isCameraInteractionActive =
            isActiveSession &&
            (m_ctx->mouseCameraId == cameraId ||
             m_ctx->dragCameraId == cameraId || isPreviewExternalDrag ||
             m_ctx->isSelecting || m_ctx->brushState.isActive ||
             m_ctx->eraserState.isActive);
        // 时间未到只跳过本次辅助快照，不 sleep、不忙等。
        // 逻辑线程继续更新其他相机及会话状态。
        if ( isSecondaryPlaybackCamera && !isCameraInteractionActive ) {
            // 保存的是每个相机的生成基准，而不是网络确认时间。
            // 首次没有历史值时允许立即生成。
            auto& lastCameraSnapshotTime =
                m_ctx->lastCameraSnapshotTimes[cameraId];
            if ( lastCameraSnapshotTime > 0.0 &&
                 snapshotSysTime - lastCameraSnapshotTime <
                     secondaryCameraSnapshotMinInterval ) {
                continue;
            }
            // 通过限频门槛后推进该相机基准。
            // 这里仍可能因工作缓冲不可用而跳过，保持既有调度行为。
            lastCameraSnapshotTime = snapshotSysTime;
        } else {
            m_ctx->lastCameraSnapshotTimes[cameraId] = snapshotSysTime;
        }

        // 从 Session 本地缓存获取该 Camera 专属的同步缓冲，避免每帧查注册表锁。
        // 借用缓存中的 shared_ptr 槽，普通帧不产生所有权增减。
        // 首次缺失才从引擎取共享缓冲，保证 UI 消费期的生命周期。
        auto& syncBuffer = m_ctx->syncBuffers[cameraId];
        if ( !syncBuffer ) {
            syncBuffer = engine.getSyncBuffer(cameraId);
        }
        // 相机可能尚未完成渲染端注册，缺少缓冲时跳过即可。
        // 不能在逻辑线程等待 UI 创建资源。
        if ( !syncBuffer ) continue;

        // 工作快照由同步缓冲管理，本地只持有本次写入观察指针。
        // 消费者未释放可写槽时返回空，生产者继续处理其他视图。
        RenderSnapshot* snapshot = syncBuffer->getWorkingSnapshot();
        if ( !snapshot ) continue;

        // 复用工作槽前重置逐帧状态，避免旧悬浮、选择或几何残留。
        // 容量及可复用缓存的具体保留规则由 RenderSnapshot::clear 管理。
        snapshot->clear();

        // 注入该 Camera 特有的 UV 映射到快照
        // 图集映射按相机与修订同步。
        // 不能把其他相机的 UV 或字体指标用于当前纹理资源。
        engine.updateSnapshotAtlasUVMap(cameraId,
                                        snapshot->uvMap,
                                        snapshot->atlasUvRevision,
                                        snapshot->asciiFontAtlasMetrics,
                                        snapshot->unicodeFontMetrics);
        snapshot->isPlaying       = snapshotIsPlaying;
        snapshot->isSeekScrubbing = m_ctx->isSeekScrubbing;
        snapshot->currentTime     = m_ctx->animateTime;  // 快照使用动画时间
        // 只有主画布使用水平平移。
        // Preview 和 Timeline 维持各自布局原点，不能被主画布拖动带偏。
        snapshot->canvasHorizontalOffsetX =
            SessionUtils::isMainCanvasCameraId(cameraId)
                ? camera.horizontalOffsetX
                : 0.0F;
        // 真实播放时间与动画显示时间同时发布。
        // 需要音频进度的 UI 读取 playbackTime，需要绘制的位置读取 currentTime。
        snapshot->playbackTime = m_ctx->currentTime;
        // 总时长来自统一的谱面/音轨范围，供进度条和视图边界共同使用。
        // 不使用当前可见音符末端，避免滚动画布时进度范围跳变。
        snapshot->totalTime = snapshotTotalTime;
        // 时钟锚点与播放速度配对，供消费端推算生成快照之后的进度。
        // 不能混入墙钟时间，否则系统校时会改变插值结果。
        snapshot->snapshotSysTime  = snapshotSysTime;
        snapshot->playbackSpeed    = snapshotPlaybackSpeed;
        snapshot->fallbackBpm      = snapshotFallbackBpm;
        snapshot->currentBpm       = snapshotCurrentBpm;
        snapshot->currentBeatIndex = snapshotCurrentBeatIndex;
        snapshot->currentSv        = snapshotCurrentSv;
        snapshot->hasBeatmap       = hasBeatmap;
        // 实例标识只在本进程内区分谱面对象，不能作为保存或协作 ID。
        // UI 可用它区分内容更新与整个谱面替换。
        snapshot->beatmapInstanceId = snapshotBeatmapInstanceId;
        // 批注版本和批注可见项来自同一会话缓存。
        // 消费端可据版本判断提示数据是否已更新。
        snapshot->annotationRevision = m_ctx->annotationRenderCacheRevision;
        // 操作反馈按快照复制，避免 UI 跨线程借用会被下一命令覆盖的字符串。
        // 此处只传递已有消息，不在每个相机上重新执行操作。
        snapshot->lastActionMessage = m_ctx->lastActionMessage;
        // 密度图只随 Preview 快照传输。
        // 其他画布不承担这份统计数据的复制成本。
        if ( cameraId == "Preview" ) {
            snapshot->previewDensity = m_ctx->previewDensityCache;
        }

        // 只在有谱面时复制封面、标题、路径和修改标记。
        // 空会话依靠前面的 clear 保持缺省状态。
        if ( hasBeatmap ) {
            snapshot->backgroundPath    = snapshotBackgroundPath;
            snapshot->bgSize            = m_ctx->bgSize;
            snapshot->backgroundIsVideo = snapshotBackgroundIsVideo;
            // 视频起播偏移已在公共元数据阶段转换为秒。
            // 分发到多个相机时不要再次转换持久化的毫秒单位。
            snapshot->backgroundVideoStartTime =
                snapshotBackgroundVideoStartTime;
            snapshot->beatmapPathKey = snapshotBeatmapPathKey;
            snapshot->beatmapName    = snapshotBeatmapName;
            snapshot->isDirty        = snapshotIsDirty;
        }

        // 计算可见时间范围 (基于动画时间)
        // 可见范围也使用动画位置，避免交互提示与画面错位。
        // 借用同一缓存供后面批注投影，不重新构建映射。
        auto* cache = m_ctx->timelineRegistry.ctx().find<System::ScrollCache>();
        if ( cache ) {
            float judgmentLineY =
                camera.viewportHeight * config.visual.judgeline_pos;
            double currentAbsY = cache->getAbsY(m_ctx->animateTime);
            // osu! 模式: timelineZoom 已写入 absY 流速，不在此处重复除以 scale
            // 主画布的 timelineZoom 已体现在缓存绝对位置中。
            // 再次除目标 zoom 会把可见时间范围缩放两次。
            double scale = snapshot->renderScaleY;
            if ( !SessionUtils::isMainCanvasCameraId(cameraId) &&
                 std::abs(scale) > 0.0001f ) {
                if ( std::abs(scale) < 1e-6 ) scale = 1.0;
            } else if ( SessionUtils::isMainCanvasCameraId(cameraId) ) {
                scale = 1.0;
            }

            // 两端各自逆映射；反向滚动时数值顺序可能相反。
            // 消费方应按用途取 min/max，而不能假设起点永远更早。
            snapshot->visibleTimeStart = cache->getTime(
                currentAbsY - (camera.viewportHeight - judgmentLineY) / scale);
            snapshot->visibleTimeEnd =
                cache->getTime(currentAbsY + judgmentLineY / scale);
        }
        if ( SessionUtils::isMainCanvasCameraId(cameraId) &&
             !m_ctx->annotationRenderCache.empty() ) {
            // 批注可见时间窗两端额外扩展少量余量。
            // 该数值只扩大候选范围，不延迟交互或阻塞等待。
            const double visibleStart =
                std::min(snapshot->visibleTimeStart, snapshot->visibleTimeEnd) -
                0.25;
            const double visibleEnd =
                std::max(snapshot->visibleTimeStart, snapshot->visibleTimeEnd) +
                0.25;
            // 缓存按时间排序，从可见起点二分定位。
            // 只复制窗口附近的分组，不每帧传输整张批注表。
            const auto first = std::lower_bound(
                m_ctx->annotationRenderCache.begin(),
                m_ctx->annotationRenderCache.end(),
                visibleStart,
                [](const AnnotationRenderMarker& marker, double timestamp) {
                    return marker.timestamp < timestamp;
                });
            // 达到可见末端后停止扫描。
            // 复制分组值使 UI 文本不借用仍可能编辑的逻辑缓存。
            for ( auto marker = first;
                  marker != m_ctx->annotationRenderCache.end() &&
                  marker->timestamp <= visibleEnd;
                  ++marker ) {
                snapshot->annotationMarkers.push_back(*marker);
            }
        }

        // --- 注入交互状态 ---
        // 交互许可与工具状态属于当前快照，后台主画布可显示但不拾取。
        // 统计值已在脏分支算好，此处只复制标量。
        // 工具类型供渲染端选择反馈外观，输入处理仍由逻辑线程负责。
        // 非活动会话可显示已有内容，但不应据此接收编辑命令。
        snapshot->currentTool        = m_ctx->currentTool;
        snapshot->acceptsInteraction = isActiveSession;
        snapshot->noteCount          = m_ctx->noteCount;
        snapshot->maxCombo           = m_ctx->maxCombo;
        // 三类轨道数量共同定义统一轨号空间。
        // 草稿与采样不能简单按玩家轨道数量裁剪。
        snapshot->trackCount      = m_ctx->trackCount;
        snapshot->draftTrackCount = m_ctx->draftTrackCount;
        snapshot->bgmTrackCount   = m_ctx->bgmTrackCount;
        // 区域开关来自会话最后接收的配置状态。
        // 它们必须与统一轨号解释一起交给消费端，不能只发布玩家轨数。
        snapshot->bmsEditingEnabled =
            m_ctx->lastConfig.settings.enableBmsEditing;
        snapshot->draftLanesEnabled =
            m_ctx->lastConfig.settings.professionalMode;
        snapshot->isHoveringCanvas =
            m_ctx->isMouseInCanvas && (m_ctx->mouseCameraId == cameraId);

        // 核心修复：预览区的拖拽状态广播
        // 如果预览区正在拖拽，所有视口的渲染快照都需要知道预览区当前的悬停时间点。
        // 共享目标时间广播到需要反馈的各个视图。
        // 活动会话限制避免后台会话把自己的手势标志传播到共享界面。
        snapshot->isPreviewDragging = isActiveSession && m_ctx->isDragging &&
                                      (m_ctx->dragCameraId == "Preview" ||
                                       m_ctx->mouseCameraId == "Preview" ||
                                       m_ctx->dragCameraId == "AudioWaveform" ||
                                       m_ctx->dragCameraId == "AudioSpectrum");
        snapshot->previewHoverTime  = m_ctx->previewHoverTime;

        // --- 注入框选状态 ---
        snapshot->isSelecting = m_ctx->isSelecting;
        // 最后一个框是当前活动手势，旧框仍保留作为历史选择区域。
        // 先检查非空再取 back，框选刚开始但尚无区域时也可安全发布。
        if ( m_ctx->isSelecting && !m_ctx->marqueeBoxes.empty() ) {
            snapshot->activeSelectionCameraId =
                m_ctx->marqueeBoxes.back().cameraId;
        }

        // 只复制绘制所需端点与相机身份，不复制选中的实体集合。
        // 屏幕坐标留给渲染系统按当前视口换算。
        for ( const auto& box : m_ctx->marqueeBoxes ) {
            RenderSnapshot::MarqueeBoxSnapshot boxSnap;
            boxSnap.startTime  = box.startTime;
            boxSnap.endTime    = box.endTime;
            boxSnap.startTrack = box.startTrack;
            boxSnap.endTrack   = box.endTrack;
            boxSnap.cameraId   = box.cameraId;
            snapshot->marqueeBoxes.push_back(boxSnap);
        }

        // 完整悬浮拟合只为鼠标所在视图生成。
        // 非当前视图仍接收必要的共享预览拖动状态。
        if ( snapshot->isHoveringCanvas ) {
            auto* cache =
                m_ctx->timelineRegistry.ctx().find<System::ScrollCache>();
            if ( cache ) {
                float judgmentLineY =
                    camera.viewportHeight * config.visual.judgeline_pos;

                double currentAbsY = cache->getAbsY(m_ctx->animateTime);
                // 从判定线到鼠标的像素差表达滚动距离方向。
                // 不能直接把屏幕 Y 加到当前时间。
                double deltaY = (judgmentLineY - m_ctx->lastMousePos.y);

                float renderScaleY = 1.0f;
                // 核心修复：预览区的坐标是经过压缩的，计算时间时需要除以缩放比例
                // 兼容两种预览标识，逆映射都需要解除纵向压缩。
                // 主画布比例保持一，缩放动画已由 ScrollCache 表达。
                if ( cameraId == "Preview" || cameraId == "PreviewCanvas" ) {
                    renderScaleY =
                        calculatePreviewRenderScaleY(*m_ctx, camera, config);
                }

                // 避免退化预览面积形成除零。
                // 此处沿用既有回退：比例不可用时保留未压缩的像素差。
                if ( std::abs(renderScaleY) > 0.0001f ) {
                    deltaY /= renderScaleY;
                }

                // 先得到目标绝对滚动位置，再由缓存解出时间。
                // 跳跃和滚速变化不能用固定像素每秒近似处理。
                double targetAbsY     = currentAbsY + deltaY;
                snapshot->hoveredTime = cache->getTime(targetAbsY);

                // 计算轨道；主画布统一应用相机横向偏移，预览区保持独立布局。
                // 轨号映射与时间逆映射独立。
                // 只有目标区域有效时才允许显示主画布吸附提示。
                CanvasTrackProjection trackProjection;
                bool                  isInsideTrack = false;
                if ( cameraId == "Preview" || cameraId == "PreviewCanvas" ) {
                    const float leftX = config.visual.previewConfig.margin.left;
                    const float rightX =
                        camera.viewportWidth -
                        config.visual.previewConfig.margin.right;
                    trackProjection.leftX  = leftX;
                    trackProjection.rightX = rightX;
                    // 预览只按玩家轨数分区，零轨数保留无效投影。
                    // contains 与 trackAt
                    // 分开读取，防止区域外的边缘轨号被当成真实命中。
                    trackProjection.singleTrackWidth =
                        m_ctx->trackCount > 0
                            ? (rightX - leftX) /
                                  static_cast<float>(m_ctx->trackCount)
                            : 0.0F;
                    trackProjection.valid =
                        trackProjection.singleTrackWidth > 0.0F;
                    snapshot->hoveredTrack = trackProjection.trackAt(
                        m_ctx->lastMousePos.x, m_ctx->trackCount);
                    isInsideTrack =
                        trackProjection.contains(m_ctx->lastMousePos.x);
                } else {
                    const auto laneProjection = calculateCanvasLaneProjection(
                        camera.viewportWidth,
                        m_ctx->trackCount,
                        m_ctx->bgmTrackCount,
                        config.visual.trackLayout,
                        camera.horizontalOffsetX,
                        true,
                        config.settings.enableBmsEditing,
                        config.settings.professionalMode,
                        m_ctx->draftTrackCount,
                        true);
                    trackProjection = laneProjection.player;
                    // 主画布使用统一投影识别玩家、草稿与 BGM。
                    // 水平平移已经包含在投影中，不再单独减一次偏移。
                    const auto lane =
                        laneProjection.laneAt(m_ctx->lastMousePos.x);
                    if ( lane ) {
                        // 局部区域轨号在此转换为全局统一轨号。
                        // 草稿轨数独立参与转换，不能永远按玩家轨数计算负轨范围。
                        snapshot->hoveredTrack =
                            lane->absoluteTrack(laneProjection.playerLaneCount,
                                                laneProjection.draftLaneCount);
                        isInsideTrack = true;
                    } else if ( m_ctx->lastMousePos.x >=
                                    laneProjection.annotationLeftX &&
                                m_ctx->lastMousePos.x <
                                    laneProjection.annotationRightX ) {
                        // 批注栏与物件轨道共用同一时间吸附预览，但不映射为轨道号。
                        isInsideTrack = true;
                    }
                }

                // --- 磁吸拍线时间戳预览 ---
                // 吸附入口同时接收时间、屏幕位置和相机布局。
                // 这让普通网格与压缩视图的磁吸判定共用同一规则。
                auto snap = SessionUtils::getSnapResult(snapshot->hoveredTime,
                                                        m_ctx->lastMousePos.y,
                                                        camera,
                                                        config,
                                                        bpmEvents,
                                                        m_ctx->timelineRegistry,
                                                        m_ctx->animateTime,
                                                        m_ctx->cameras,
                                                        snapshotFallbackBpm);

                // 判断是否在轨道框内
                // Timeline 没有普通音符轨道限制。
                // 其他视图必须实际位于可交互轨道或批注区域内才显示吸附状态。
                if ( snap.isSnapped ) {
                    if ( cameraId == "Timeline" || isInsideTrack ) {
                        snapshot->isSnapped          = true;
                        snapshot->snappedTime        = snap.snappedTime;
                        snapshot->snappedNumerator   = snap.numerator;
                        snapshot->snappedDenominator = snap.denominator;
                    }
                }
                snapshot->currentBeatDivisor = config.settings.beatDivisor;

                // --- 预览区悬停状态 ---
                // 鼠标位于 Preview 时直接保存屏幕 Y，不从时间再次往返换算。
                // 同步共享目标给主画布，使本地视觉在本次逻辑更新立即跟随。
                if ( cameraId == "Preview" ) {
                    // 预览悬浮保留鼠标像素位置，以便提示框直接贴合指针。
                    // 跨视图共享的是时间，其他视口不能直接复用这个 Y 坐标。
                    snapshot->isPreviewHovered  = true;
                    snapshot->isPreviewDragging = m_ctx->isDragging;
                    snapshot->previewHoverY     = m_ctx->lastMousePos.y;

                    // 核心逻辑：拖动预览区时，主画布应该渲染拖拽处的内容
                    snapshot->previewHoverTime = snapshot->hoveredTime;
                    m_ctx->previewHoverTime    = snapshot->hoveredTime;
                }

                /// @brief 根据当前 BPM 网格拟合用于检视的拍位描述。
                /// @param time 目标事件时间，单位为秒。
                /// @param track 用于单轨提示的统一轨号。
                /// @return 包含分数、拍区间与可见标志的值对象。
                /// @note 无 BPM 时仍保留时间与轨道，其他字段使用类型默认值。
                /// @warning 悬浮热路径，可遍历有序 BPM
                /// 缓存和固定分母集合；不得扫描 ECS。
                auto makeBeatPoint = [&](double time, int32_t track) {
                    // 先保留原始时间和轨道。
                    // 后面即便没有 BPM 可用于拟合，仍可返回基本检视位置。
                    HoverBeatPoint point;
                    point.show  = true;
                    point.time  = time;
                    point.track = track;

                    // 从有序 BPM 缓存找不晚于目标的最后一项。
                    // 保留下标供后续截断拍区间，避免再次搜索下一 BPM。
                    const TimelineComponent* activeBpm      = nullptr;
                    size_t                   activeBpmIndex = 0;
                    for ( size_t bpmIndex = 0; bpmIndex < bpmEvents.size();
                          ++bpmIndex ) {
                        const auto* bpmEv = bpmEvents[bpmIndex];
                        // 小容差允许边界附近使用该 BPM。
                        // 遇到未来项就停止，因为缓存已按时间排列。
                        if ( bpmEv->m_timestamp <= time + 1e-4 ) {
                            activeBpm      = bpmEv;
                            activeBpmIndex = bpmIndex;
                        } else {
                            break;
                        }
                    }
                    // 首 BPM 以前仍可沿用它的节拍向前拟合。
                    // 这里不创建新的 Timing 事件，也不改变谱面数据。
                    if ( !activeBpm && !bpmEvents.empty() &&
                         time < bpmEvents.front()->m_timestamp ) {
                        activeBpm = bpmEvents.front();
                    }
                    // 缺少 BPM 时不伪造一个已拟合分数。
                    // 下游依据拍区间有效性决定是否绘制额外分拍线。
                    if ( !activeBpm ) return point;
                    bool isBeforeFirstBpm = !bpmEvents.empty() &&
                                            activeBpm == bpmEvents.front() &&
                                            time < activeBpm->m_timestamp;

                    // 优先采用当前谱面偏好，空谱面使用统一默认值。
                    // 规范化再计算节拍长度，避免直接除异常 BPM。
                    const double fallbackBpm =
                        m_ctx->currentBeatmap
                            ? m_ctx->currentBeatmap->m_baseMapMetadata
                                  .preference_bpm
                            : ::MMM::DEFAULT_NORMALIZED_BPM;
                    const double bpmVal = ::MMM::normalizeBpmValue(
                        activeBpm->m_value, fallbackBpm);

                    double beatDuration = 60.0 / bpmVal;
                    // 候选集合固定，包含常用低分母和高精度细分。
                    // 不会按鼠标误差动态扩张搜索空间。
                    static const int denominators[] = { 1,  2,  3,  4,  5,  6,
                                                        7,  8,  9,  10, 11, 12,
                                                        13, 14, 15, 16, 24, 32,
                                                        48, 64, 96, 128 };
                    // 候选初值只是搜索占位，最终分数由固定候选集合更新。
                    // 拟合结果与鼠标所在原始时间同时保留，便于分别显示。
                    int    bestNum   = 0;
                    int    bestDen   = 1;
                    double bestScore = 1e9;
                    // 对每个候选网格选最近的整数步。
                    // 拟合只描述当前位置，不把物件时间修改到拟合时间。
                    for ( int den : denominators ) {
                        double stepDuration = beatDuration / den;
                        double relative     = time - activeBpm->m_timestamp;
                        // 先按候选网格量化，再计算真实时间误差。
                        // 不同分母的误差以秒为同一单位比较。
                        double steps = std::round(relative / stepDuration);
                        double fitTime =
                            activeBpm->m_timestamp + steps * stepDuration;
                        double error = std::abs(time - fitTime);

                        int64_t totalSteps = static_cast<int64_t>(steps);
                        int     beatIndex  = totalSteps % den;
                        // 首 BPM 前的步数可以为负。
                        // 把余数归一到拍内范围，便于统一约分。
                        if ( beatIndex < 0 ) beatIndex += den;

                        // 整拍沿用 1/1 的显示约定。
                        // 非整拍再约分，避免把同一个半拍写成多个等价分数。
                        int finalNum = 1;
                        int finalDen = 1;
                        if ( beatIndex != 0 ) {
                            int common = std::gcd(beatIndex, den);
                            finalNum   = beatIndex / common;
                            finalDen   = den / common;
                        }

                        // 误差之外加入分母惩罚，微小时间误差下优先更简单的分数。
                        // 这是显示拟合规则，不等同于编辑吸附的距离阈值。
                        double score = error + (double)finalDen * 0.0002;
                        // 只在严格更优时替换候选，相同分数保持遍历中的先选结果。
                        // 这样重复生成同一位置的提示不会随机跳变。
                        if ( score < bestScore ) {
                            bestScore = score;
                            bestNum   = finalNum;
                            bestDen   = finalDen;
                        }
                    }

                    double rel = time - activeBpm->m_timestamp;
                    // 整数拍定位与前面的最简分数拟合分开计算。
                    // 拍区间必须锚定 BPM 原点，不能跟随候选分母改变相位。
                    int64_t beatsInActive = static_cast<int64_t>(
                        std::floor(rel / beatDuration + 1e-6));

                    // 首 BPM 前的整拍使用零分子兼容负拍显示。
                    // 只改显示分数，不改变实际时间或拍区间。
                    if ( isBeforeFirstBpm && bestNum == 1 && bestDen == 1 ) {
                        bestNum = 0;
                    }
                    // 首 BPM 前用相对负拍数，之后使用跨段的全局拍号。
                    // 不能把每个 BPM 段的局部拍数都直接显示为全谱拍号。
                    point.beatIndex =
                        isBeforeFirstBpm
                            ? static_cast<int>(beatsInActive)
                            : SessionUtils::calculateBeatIndex(
                                  time, bpmEvents, snapshotFallbackBpm);
                    point.numerator   = bestNum;
                    point.denominator = bestDen;
                    point.beatStartTime =
                        activeBpm->m_timestamp +
                        static_cast<double>(beatsInActive) * beatDuration;
                    point.beatEndTime = point.beatStartTime + beatDuration;
                    // 原始拍长保留用于细分间距，区间末端随后可以被 BPM
                    // 边界截短。 截断范围不等于重新定义该段的节拍速度。
                    point.beatDuration = beatDuration;
                    // 拍区间若跨过下一 BPM，要在变化点截断。
                    // 分拍提示不能把旧 BPM 的拍长延伸进新段。
                    if ( !isBeforeFirstBpm &&
                         activeBpmIndex + 1 < bpmEvents.size() ) {
                        const double nextBpmTime =
                            bpmEvents[activeBpmIndex + 1]->m_timestamp;
                        if ( nextBpmTime > point.beatStartTime &&
                             nextBpmTime < point.beatEndTime ) {
                            point.beatEndTime = nextBpmTime;
                        }
                    }
                    return point;
                };

                // --- 智能拟合：计算当前悬停物件的最简分拍 ---
                // 优先检视当前悬浮目标，无悬浮时回退正在拖动的目标。
                // 实体与对象种类必须成对选择，避免到错误 registry 读取。
                const entt::entity inspectEntity =
                    (m_ctx->hoveredEntity != entt::null) ? m_ctx->hoveredEntity
                                                         : m_ctx->draggedEntity;
                const ChartObjectKind inspectObjectKind =
                    (m_ctx->hoveredEntity != entt::null)
                        ? m_ctx->hoveredObjectKind
                        : m_ctx->draggedObjectKind;
                // 只有检视目标就是当前拖动实体，才使用锁定的拖动部件。
                // 鼠标路过其他实体时不能错误套用旧子项索引。
                const bool useDragState = m_ctx->isDragging &&
                                          inspectEntity != entt::null &&
                                          inspectEntity == m_ctx->draggedEntity;
                // 按照对象种类选择 registry，并先确认实体仍有效。
                // 返回的是本次 update 内借用组件，不保存到跨线程快照。
                const auto* inter =
                    ((inspectObjectKind == ChartObjectKind::PlayerNote ||
                      inspectObjectKind == ChartObjectKind::DraftNote) &&
                     inspectEntity != entt::null &&
                     m_ctx->noteRegistry.valid(inspectEntity))
                        ? m_ctx->noteRegistry
                              .try_get<const InteractionComponent>(
                                  inspectEntity)
                    : (inspectObjectKind == ChartObjectKind::AudioSample &&
                       inspectEntity != entt::null &&
                       m_ctx->sampleRegistry.valid(inspectEntity))
                        ? m_ctx->sampleRegistry
                              .try_get<const InteractionComponent>(
                                  inspectEntity)
                        : nullptr;
                // 拖动状态可独立支持检视，不要求此时仍有悬浮组件标记。
                // 普通悬浮则必须有有效交互组件并处于悬浮或拖动态。
                const bool shouldInspect =
                    useDragState ||
                    (inter && (inter->isHovered || inter->isDragging));
                // 只在 Note 类别里读取 NoteComponent。
                // 采样实体号可能与音符实体号数值相同，种类判断不能省略。
                const auto* inspectNote =
                    shouldInspect &&
                            (inspectObjectKind == ChartObjectKind::PlayerNote ||
                             inspectObjectKind == ChartObjectKind::DraftNote)
                        ? m_ctx->noteRegistry.try_get<const NoteComponent>(
                              inspectEntity)
                        : nullptr;
                if ( inspectNote ) {
                    const auto& note = *inspectNote;
                    // 拖动期间保持抓取开始时选定的部件。
                    // 普通悬浮读取最新命中部件，允许头、尾和身体之间切换。
                    const auto hoveredPart =
                        useDragState
                            ? m_ctx->draggedPart
                            : static_cast<HoverPart>(inter->hoveredPart);
                    // 折线子索引同样随拖动目标锁定。
                    // 不能因为鼠标暂时离开子项几何就让属性提示跳到另一段。
                    const int32_t hoveredSubIndex =
                        useDragState ? m_ctx->draggedSubIndex
                                     : inter->hoveredSubIndex;

                    // 结构化信息同时保留多个拍点及持续区间。
                    // 旧版单拍提示在后面从这些字段选择，不在此丢弃头尾信息。
                    HoverInspectInfo inspect;
                    inspect.show       = true;
                    inspect.entity     = inspectEntity;
                    inspect.objectKind = inspectObjectKind;

                    /// @brief 将一个已显示拍点同步到旧版悬浮提示字段。
                    /// @param point 从结构化检视中选出的拍点。
                    /// @note 只更新可见拍点，不覆盖结构化的头、身、尾信息。
                    /// @warning 快照热路径，只复制固定数量标量。
                    auto setLegacyPoint = [&](const HoverBeatPoint& point) {
                        // 未显示拍点不覆盖旧版兼容字段。
                        // 结构化消费者仍可读取当前 inspect 的全部类型信息。
                        if ( !point.show ) return;
                        snapshot->hoveredNoteNumerator   = point.numerator;
                        snapshot->hoveredNoteDenominator = point.denominator;
                        snapshot->hoveredNoteBeatIndex   = point.beatIndex;
                        snapshot->hoveredNoteTime        = point.time;
                        snapshot->hoveredNoteTrack       = point.track;
                    };

                    if ( note.m_type == ::MMM::NoteType::POLYLINE &&
                         hoveredSubIndex >= 0 &&
                         hoveredSubIndex <
                             static_cast<int>(note.m_subNotes.size()) ) {
                        // 先检查子索引范围再读取，折线编辑可能改变子项数量。
                        // 子项的时间和轨道独立于父容器，不能直接使用父头位置。
                        const auto& sub = note.m_subNotes[hoveredSubIndex];
                        inspect.track   = sub.trackIndex;
                        // 首节点与中间节点采用不同检视类型。
                        // 都以节点自身拍点为主体，不把连接段时长当作节点时间。
                        if ( hoveredPart == HoverPart::PolylineNode ) {
                            inspect.kind = (hoveredSubIndex == 0)
                                               ? HoverInspectKind::PolylineHead
                                               : HoverInspectKind::PolylineNode;
                            inspect.body =
                                makeBeatPoint(sub.timestamp, sub.trackIndex);
                            inspect.showTrack = true;
                        } else if ( hoveredPart == HoverPart::HoldEnd &&
                                    sub.type == ::MMM::NoteType::HOLD ) {
                            // 长条尾部同时保留头与尾拍点，用于显示时长和结束拍位。
                            // 尾时间由子项起点加持续时间计算。
                            inspect.kind = HoverInspectKind::PolylineHoldEnd;
                            inspect.head =
                                makeBeatPoint(sub.timestamp, sub.trackIndex);
                            inspect.end = makeBeatPoint(
                                sub.timestamp + sub.duration, sub.trackIndex);
                            inspect.showDuration = true;
                            inspect.duration     = sub.duration;
                            inspect.showTrack    = true;
                        } else if ( hoveredPart == HoverPart::FlickArrow &&
                                    sub.type == ::MMM::NoteType::FLICK ) {
                            // Flick 的结束位置改变轨道而不改变时间。
                            // 显示轨号采用起轨加轨差，保持箭头端点含义。
                            inspect.kind = HoverInspectKind::PolylineFlickEnd;
                            inspect.end  = makeBeatPoint(
                                sub.timestamp, sub.trackIndex + sub.dtrack);
                            inspect.showDtrack = true;
                            inspect.dtrack     = sub.dtrack;
                            inspect.showTrack  = true;
                            inspect.track      = sub.trackIndex + sub.dtrack;
                        } else if ( sub.type == ::MMM::NoteType::FLICK ) {
                            // 身体检视强调原始节点与轨差。
                            // 不把整个横向区域伪装成另一个持续时间区间。
                            inspect.kind = HoverInspectKind::PolylineFlickBody;
                            inspect.body =
                                makeBeatPoint(sub.timestamp, sub.trackIndex);
                            inspect.showDtrack = true;
                            inspect.dtrack     = sub.dtrack;
                        } else {
                            // 持续身体只报告时长与轨道。
                            // 单个拍点由后面的编辑预览回退规则按需要补充。
                            inspect.kind = HoverInspectKind::PolylineHoldBody;
                            inspect.showDuration = true;
                            inspect.duration     = sub.duration;
                            inspect.showTrack    = true;
                        }
                        // 独立 Hold 使用组件自身的起点与持续时间。
                        // 头、尾和身体选择不同检视类型，但共享同一个持续区间。
                    } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
                        // 时长保持逻辑秒值，单位展示由消费端决定。
                        // 身体没有唯一拍点，因此其提示不能从默认头字段取值。
                        inspect.duration = note.m_duration;
                        inspect.track    = note.m_trackIndex;
                        if ( hoveredPart == HoverPart::HoldEnd ) {
                            inspect.kind = HoverInspectKind::HoldEnd;
                            inspect.head = makeBeatPoint(note.m_timestamp,
                                                         note.m_trackIndex);
                            inspect.end  = makeBeatPoint(
                                note.m_timestamp + note.m_duration,
                                note.m_trackIndex);
                        } else if ( hoveredPart == HoverPart::HoldBody ) {
                            inspect.kind = HoverInspectKind::HoldBody;
                        } else {
                            inspect.kind = HoverInspectKind::HoldHead;
                            inspect.head = makeBeatPoint(note.m_timestamp,
                                                         note.m_trackIndex);
                        }
                        inspect.showDuration = true;
                        inspect.showTrack    = true;
                        // 独立 Flick 同时保留起始轨与轨差。
                        // 箭头端点显示目标轨，身体提示则保留起点作为参考。
                    } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
                        // 轨差保持符号，负值表示向较小轨号方向移动。
                        // 不能取绝对值，否则端点提示会丢失方向。
                        inspect.dtrack = note.m_dtrack;
                        if ( hoveredPart == HoverPart::FlickArrow ) {
                            inspect.kind = HoverInspectKind::FlickEnd;
                            inspect.end  = makeBeatPoint(
                                note.m_timestamp,
                                note.m_trackIndex + note.m_dtrack);
                            inspect.track = note.m_trackIndex + note.m_dtrack;
                            inspect.showTrack = true;
                        } else if ( hoveredPart == HoverPart::HoldBody ) {
                            inspect.kind = HoverInspectKind::FlickBody;
                            inspect.body = makeBeatPoint(note.m_timestamp,
                                                         note.m_trackIndex);
                        } else {
                            inspect.kind  = HoverInspectKind::FlickHead;
                            inspect.head  = makeBeatPoint(note.m_timestamp,
                                                          note.m_trackIndex);
                            inspect.track = note.m_trackIndex;
                            inspect.showTrack = true;
                        }
                        inspect.showDtrack = true;
                    } else {
                        // 普通点击只有一个可显示拍点。
                        // 不人为附加持续时间或轨差字段。
                        inspect.kind = HoverInspectKind::Note;
                        inspect.head =
                            makeBeatPoint(note.m_timestamp, note.m_trackIndex);
                        inspect.track     = note.m_trackIndex;
                        inspect.showTrack = true;
                    }

                    // 先借用顶层绑定，再按合法折线子索引切换到子项绑定。
                    // 子项没有绑定时保持空，不隐式继承父对象的音效。
                    const ::MMM::AudioSampleBinding* sampleBinding =
                        note.m_sampleBinding ? &*note.m_sampleBinding : nullptr;
                    if ( note.m_type == ::MMM::NoteType::POLYLINE &&
                         hoveredSubIndex >= 0 &&
                         hoveredSubIndex <
                             static_cast<int>(note.m_subNotes.size()) ) {
                        const auto& subNote = note.m_subNotes[hoveredSubIndex];
                        sampleBinding       = subNote.sampleBinding
                                                  ? &*subNote.sampleBinding
                                                  : nullptr;
                    }
                    // 非空资源 ID 才提供试听属性。
                    // 这里只发布引用和音量，不能在快照线程临时解码音效。
                    if ( sampleBinding &&
                         !sampleBinding->m_audioResourceId.empty() ) {
                        inspect.showAudioPreview = true;
                        inspect.audioResourceId =
                            sampleBinding->m_audioResourceId;
                        inspect.volume = sampleBinding->m_volume;
                        if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
                            // 子索引用于后续试听操作定位该段绑定。
                            // UI 命令执行时仍需重新校验实体与下标是否有效。
                            inspect.sampleBindingSubIndex = hoveredSubIndex;
                        }
                    }

                    // 先发布结构化值，再派生临时分拍预览和旧字段。
                    // 后续 UI 不持有 registry 或绑定 optional 的地址。
                    snapshot->hoverInspect = inspect;
                    // 无手势时显示当前网格无法表达的悬浮细分。
                    // 拖动态改用当前编辑吸附模式决定提示，避免两套线混杂。
                    if ( !m_ctx->isDragging ) {
                        updateHoverSubdivisionPreview(
                            *snapshot, inspect, config.settings.beatDivisor);
                    } else if ( useDragState &&
                                config.settings.objectPlacementSnap &&
                                config.settings.objectPlacementSnapMode ==
                                    Config::ObjectPlacementSnapMode::
                                        CommonBeatDivisors ) {
                        const HoverBeatPoint* editPoint =
                            inspectedHoverBeatPoint(inspect);
                        // 某些身体检视没有唯一拍点，编辑时借用一个局部回退值。
                        // 观察指针只在本分支内使用，不写进快照。
                        HoverBeatPoint fallbackPoint;
                        // 独立 Hold 身体用头部拍位作为移动参考。
                        // 结束部件已有自己的拍点，不经过这个回退。
                        if ( !editPoint &&
                             inspect.kind == HoverInspectKind::HoldBody ) {
                            fallbackPoint = makeBeatPoint(note.m_timestamp,
                                                          note.m_trackIndex);
                            editPoint     = &fallbackPoint;
                        } else if ( !editPoint &&
                                    inspect.kind ==
                                        HoverInspectKind::PolylineHoldBody &&
                                    hoveredSubIndex >= 0 &&
                                    hoveredSubIndex <
                                        static_cast<int>(
                                            note.m_subNotes.size()) ) {
                            const auto& sub = note.m_subNotes[hoveredSubIndex];
                            // 整段平移使用段起点作吸附参考，保持其原有持续长度。
                            // 尾部缩放由前面的尾检视分支提供独立结束拍点。
                            fallbackPoint =
                                makeBeatPoint(sub.timestamp, sub.trackIndex);
                            editPoint = &fallbackPoint;
                        }
                        // 只有找到有效编辑拍位才请求常用分拍并集。
                        // 配置位掩码的合法性由专用助手处理。
                        if ( editPoint ) {
                            updateCommonSubdivisionPreview(
                                *snapshot,
                                *editPoint,
                                config.settings.commonBeatDivisorMask);
                        }
                    }
                    // 旧提示优先采用头，其次身体，最后尾。
                    // 这是兼容字段选择顺序，不覆盖结构化检视当前部件类型。
                    // 兼容字段只容纳一个拍点，不能用它恢复完整长条区间。
                    // 需要区分抓取部件的 UI 应读取 hoverInspect.kind。
                    if ( inspect.head.show ) {
                        setLegacyPoint(inspect.head);
                    } else if ( inspect.body.show ) {
                        setLegacyPoint(inspect.body);
                    } else if ( inspect.end.show ) {
                        setLegacyPoint(inspect.end);
                    }
                    // 采样有独立 registry 和锚点/触发点语义。
                    // 不能沿用 Note 的持续时间或折线子项规则。
                } else if ( inspectObjectKind == ChartObjectKind::AudioSample &&
                            shouldInspect ) {
                    // 删除命令可能已移除组件；没有组件时让本轮快照保持无检视状态。
                    // 不尝试用相同实体号从音符 Registry 回退取值。
                    const auto* sample =
                        m_ctx->sampleRegistry.try_get<const SampleComponent>(
                            inspectEntity);
                    if ( sample ) {
                        const auto hoveredPart =
                            useDragState
                                ? m_ctx->draggedPart
                                : static_cast<HoverPart>(inter->hoveredPart);
                        HoverInspectInfo inspect;
                        inspect.show       = true;
                        inspect.entity     = inspectEntity;
                        inspect.objectKind = ChartObjectKind::AudioSample;
                        inspect.kind =
                            hoveredPart == HoverPart::SampleOffset
                                ? HoverInspectKind::AudioSampleTrigger
                                : HoverInspectKind::AudioSampleAnchor;
                        inspect.head =
                            makeBeatPoint(sample->m_timestamp, sample->m_track);
                        // 零偏移时头尾重合，只保留头部描述。
                        // 非零偏移按 effectiveTime
                        // 生成触发拍点，允许位于锚点之前。
                        if ( sample->m_offsetMs != 0 ) {
                            inspect.end = makeBeatPoint(sample->effectiveTime(),
                                                        sample->m_track);
                        }
                        inspect.showTrack = true;
                        inspect.track     = sample->m_track;
                        // 试听所需的资源
                        // ID、实例音量和偏移一次复制到结构化快照。 播放行为仍由
                        // UI 命令入口请求，不在这里触发。
                        // 采样属性面板与试听入口有各自开关，不能由物件类型推断其中一个。
                        // 偏移在面板中仍使用毫秒，与拍点的秒时间分开传递。
                        inspect.showAudioSample  = true;
                        inspect.showAudioPreview = true;
                        inspect.audioResourceId  = sample->m_audioResourceId;
                        inspect.volume           = sample->m_volume;
                        inspect.offsetMs         = sample->m_offsetMs;
                        // 资源字符串随值对象移交给快照，局部对象后续不再读取。
                        // 兼容提示必须从快照内的新对象取字段。
                        snapshot->hoverInspect = std::move(inspect);

                        // 偏移手柄优先显示触发拍位，其余部件显示锚点。
                        // 没有独立触发拍点时回退头部，避免读取缺省 end。
                        const auto& legacyPoint =
                            hoveredPart == HoverPart::SampleOffset &&
                                    snapshot->hoverInspect.end.show
                                ? snapshot->hoverInspect.end
                                : snapshot->hoverInspect.head;
                        snapshot->hoveredNoteNumerator = legacyPoint.numerator;
                        snapshot->hoveredNoteDenominator =
                            legacyPoint.denominator;
                        snapshot->hoveredNoteBeatIndex = legacyPoint.beatIndex;
                        snapshot->hoveredNoteTime      = legacyPoint.time;
                        snapshot->hoveredNoteTrack     = legacyPoint.track;
                    }
                }

                if ( SessionUtils::isMainCanvasCameraId(cameraId) &&
                     m_ctx->brushState.isActive &&
                     !m_ctx->brushState.createsAudioSample &&
                     config.settings.objectPlacementSnap &&
                     config.settings.objectPlacementSnapMode ==
                         Config::ObjectPlacementSnapMode::CommonBeatDivisors ) {
                    // 画笔未提交对象也需要当前编辑目标的分拍提示。
                    // 预览位置取手势末端，不固定为最初按下的位置。
                    // 这份提示是派生显示数据，不会生成临时 ECS 实体。
                    // 取消画笔只需清除工具状态，无需撤销一个预览物件。
                    double  previewTime  = m_ctx->brushState.time;
                    int32_t previewTrack = m_ctx->brushState.track;
                    // 折线画笔以最后一段为正在编辑的末梢。
                    // Hold 末梢推进时间，Flick 末梢推进轨道，二者不能混算。
                    if ( !m_ctx->brushState.polylineSegments.empty() ) {
                        const auto& tip =
                            m_ctx->brushState.polylineSegments.back();
                        previewTime  = tip.timestamp;
                        previewTrack = tip.trackIndex;
                        if ( tip.type == ::MMM::NoteType::HOLD ) {
                            previewTime += tip.duration;
                        } else if ( tip.type == ::MMM::NoteType::FLICK ) {
                            previewTrack += tip.dtrack;
                        }
                    } else if ( m_ctx->brushState.type ==
                                ::MMM::NoteType::HOLD ) {
                        previewTime += m_ctx->brushState.duration;
                    } else if ( m_ctx->brushState.type ==
                                ::MMM::NoteType::FLICK ) {
                        previewTrack += m_ctx->brushState.dtrack;
                    }
                    updateCommonSubdivisionPreview(
                        *snapshot,
                        makeBeatPoint(previewTime, previewTrack),
                        config.settings.commonBeatDivisorMask);
                }
            }
        }

        // 非预览区拖拽时（AudioWaveform/AudioSpectrum），从 previewHoverTime
        // 反算 previewHoverY
        // 外部视图拖动只提供目标时间，Preview 需要反算目标框的屏幕位置。
        // 实际鼠标就在 Preview 时沿用直接鼠标坐标，避免覆盖它。
        if ( cameraId == "Preview" && snapshot->isPreviewDragging &&
             !snapshot->isHoveringCanvas ) {
            auto* cache =
                m_ctx->timelineRegistry.ctx().find<System::ScrollCache>();
            if ( cache ) {
                float judgmentLineY =
                    camera.viewportHeight * config.visual.judgeline_pos;
                double currentAbsY = cache->getAbsY(m_ctx->animateTime);
                // 源时间与目标时间都经同一动画缓存转换。
                // 即使滚速变化，也能保持预览框与主画布跳转一致。
                double targetAbsY = cache->getAbsY(snapshot->previewHoverTime);

                float renderScaleY =
                    calculatePreviewRenderScaleY(*m_ctx, camera, config);

                if ( std::abs(renderScaleY) > 0.0001f ) {
                    snapshot->previewHoverY =
                        judgmentLineY -
                        static_cast<float>((targetAbsY - currentAbsY) *
                                           renderScaleY);
                }
            }
        }

        // 判定线高度比例计算
        float judgmentLineY =
            camera.viewportHeight * config.visual.judgeline_pos;

        // 获取主视口高度用于预览区比例对齐
        float finalMainHeight =
            camera.viewportHeight;  // 默认为当前视口高度，防止除以 0 或比例错乱
        // 预览比例需要真正主画布高度。
        // 缺省回退保留当前视口高度，只服务主相机尚未就绪的状态。
        const auto* mainCameraFinal =
            SessionUtils::findMainCanvasCamera(m_ctx->cameras);
        if ( mainCameraFinal ) {
            finalMainHeight = mainCameraFinal->viewportHeight;
        }

        // --- 注入画笔预览状态 ---
        // 只有活跃会话发布未提交画笔状态。
        // 几何数据按值复制，UI 不借用正在追加的折线段数组。
        if ( isActiveSession && m_ctx->brushState.isActive ) {
            snapshot->brush.isActive = true;
            snapshot->brush.createsAudioSample =
                m_ctx->brushState.createsAudioSample;
            snapshot->brush.time         = m_ctx->brushState.time;
            snapshot->brush.duration     = m_ctx->brushState.duration;
            snapshot->brush.track        = m_ctx->brushState.track;
            snapshot->brush.dtrack       = m_ctx->brushState.dtrack;
            snapshot->brush.type         = m_ctx->brushState.type;
            snapshot->brush.customColors = m_ctx->brushState.customColors;
            snapshot->brush.audioResourceId =
                m_ctx->brushState.activeAudioResourceId;
            snapshot->brush.polylineSegments =
                m_ctx->brushState.polylineSegments;
        }

        // --- 注入橡皮擦预览状态 ---
        // 橡皮擦快照记录目标集合及对象种类。
        // 先将子索引设为负值，默认表达整个目标高亮。
        if ( isActiveSession && m_ctx->eraserState.isActive ) {
            snapshot->erasingEntities   = m_ctx->eraserState.targetEntities;
            snapshot->erasingObjectKind = m_ctx->eraserState.targetObjectKind;
            snapshot->erasingSubIndex   = -1;

            // Shift 模式下保持 erasingSubIndex = -1，使整个 Polyline 标红
            // 非 Shift 才允许缩小到合法折线子项。
            // 无效子索引继续保留整条高亮，不能访问越界子数组。
            if ( !m_ctx->eraserState.isShiftDown ) {
                // 非 Shift：悬停在 Polyline 的任意子物件时，允许局部高亮红色
                if ( m_ctx->hoveredEntity != entt::null &&
                     (m_ctx->hoveredObjectKind == ChartObjectKind::PlayerNote ||
                      m_ctx->hoveredObjectKind == ChartObjectKind::DraftNote) &&
                     m_ctx->noteRegistry.all_of<NoteComponent>(
                         m_ctx->hoveredEntity) ) {
                    const auto& nc = m_ctx->noteRegistry.get<NoteComponent>(
                        m_ctx->hoveredEntity);
                    if ( nc.m_type == ::MMM::NoteType::POLYLINE &&
                         !nc.m_subNotes.empty() ) {
                        if ( m_ctx->hoveredSubIndex >= 0 &&
                             m_ctx->hoveredSubIndex <
                                 static_cast<int>(nc.m_subNotes.size()) ) {
                            snapshot->erasingSubIndex = m_ctx->hoveredSubIndex;
                        }
                    }
                }
            }
        }


        // 3. 调用 ECS System 针对当前 Camera 生成渲染快照
        // 使用动画时间 m_ctx->animateTime 进行剔除和位置映射
        // 所有交互、元数据和画笔状态准备好后才生成几何。
        // 音符及采样索引作为借用参数传入，不在渲染器里全量重建。
        System::NoteRenderSystem::generateSnapshot(
            m_ctx->noteRegistry,
            m_ctx->sampleRegistry,
            m_ctx->sortedSampleEntities,
            m_ctx->sortedSampleMaxEndPrefix,
            m_ctx->timelineRegistry,
            bpmEvents,
            snapshot,
            cameraId,
            m_ctx->animateTime,
            camera.viewportWidth,
            camera.viewportHeight,
            judgmentLineY,
            m_ctx->trackCount,
            m_ctx->bgmTrackCount,
            m_ctx->draftTrackCount,
            config,
            finalMainHeight,
            &m_ctx->hitFXSystem);

        // 布局生成后才有最终 renderScaleY，批注屏幕坐标在此统一补齐。
        // 批注时间使用自身段的 HS 参考，保持与对应目标的显示映射一致。
        if ( SessionUtils::isMainCanvasCameraId(cameraId) && cache ) {
            const double currentAbsY =
                cache->getVisualAnchorAbsY(m_ctx->animateTime);
            for ( auto& marker : snapshot->annotationMarkers ) {
                marker.canvasY =
                    judgmentLineY -
                    static_cast<float>(cache->getDisplayDelta(
                        marker.timestamp, currentAbsY, marker.timestamp)) *
                        snapshot->renderScaleY;
            }
        }

        // 5. 提交专属快照
        // 所有几何和交互数据完成后一次发布。
        // 发布后工作槽归同步机制管理，不能继续通过本地指针修改。
        syncBuffer->pushWorkingSnapshot();
    }
}


}  // namespace MMM::Logic
