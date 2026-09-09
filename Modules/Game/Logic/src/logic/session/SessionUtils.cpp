#include "logic/session/SessionUtils.h"
#include "audio/AudioManager.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "mmm/timing/BpmNormalization.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

namespace MMM::Logic::SessionUtils
{

namespace
{
/// @brief 单个顶层音符对状态栏统计的贡献。
/// @note 两个字段使用相同快照计算，供批次汇总后统一更新会话统计。
struct NoteStatisticsContribution {
    /// @brief 可计数物件数量。
    std::size_t noteCount{ 0U };
    /// @brief 最大连击数量。
    std::size_t maxCombo{ 0U };
};

/// @brief 计算 Hold 区间内的四分之一拍连击增量。
/// @param startTime 长条起点，单位为秒。
/// @param endTime 长条终点，起终点相同不产生区间连击。
/// @param beatmap 提供按时间排列的 BPM 事件及首段默认 BPM。
/// @return 跨 BPM 分段积分后取整的增量，不包含长条头部自身的连击。
/// @pre 起终点有限，节奏事件按时间递增；循环不对输入重新排序。
/// @warning 编辑统计更新路径扫描时间点，不应作为逐帧物件绘制查询。
std::size_t calculateIntervalCombos(double startTime, double endTime,
                                    const ::MMM::BeatMap* beatmap)
{
    if ( !beatmap || endTime <= startTime ) return 0U;

    double totalQuarterBeats = 0.0;
    double currentTime       = startTime;
    double currentBpm =
        ::MMM::normalizeBpmValue(beatmap->m_baseMapMetadata.preference_bpm);
    // 首个生效 BPM 之前按偏好值积分，不能用列表末尾的 BPM 填充整条长条。
    std::size_t nextTimingIndex = 0U;

    // 起点恰好命中 BPM 时立即使用新值；SV 等非 BPM 效果不改变节奏长度。
    for ( std::size_t index = 0U; index < beatmap->m_timings.size(); ++index ) {
        const auto& timing = beatmap->m_timings[index];
        if ( timing.m_timingEffect != ::MMM::TimingEffect::BPM ) continue;
        if ( timing.m_timestamp <= startTime ) {
            currentBpm = ::MMM::normalizeBpmValue(timing.m_bpm, currentBpm);
        } else {
            // 留下首个起点之后的 BPM 供分段循环消费，不在这里预先生效。
            nextTimingIndex = index;
            break;
        }
    }

    while ( currentTime < endTime ) {
        // 先假定剩余部分共用当前 BPM，只有区间内部的下一 BPM 才拆段。
        double      nextEventTime = endTime;
        double      nextBpm       = currentBpm;
        std::size_t foundIndex    = beatmap->m_timings.size();
        for ( std::size_t index = nextTimingIndex;
              index < beatmap->m_timings.size();
              ++index ) {
            const auto& timing = beatmap->m_timings[index];
            if ( timing.m_timingEffect == ::MMM::TimingEffect::BPM &&
                 timing.m_timestamp > currentTime ) {
                if ( timing.m_timestamp < endTime ) {
                    // 只在当前长条内部切段；终点处的 BPM 对前面区间没有贡献。
                    nextEventTime = timing.m_timestamp;
                    nextBpm =
                        ::MMM::normalizeBpmValue(timing.m_bpm, currentBpm);
                    foundIndex = index + 1U;
                }
                break;
            }
        }
        // 每分钟拍数除以 15 得到每秒四分之一拍数，先累计再统一取整。
        // 若逐段取整，跨 BPM 的不足一格部分会被多次丢弃。
        totalQuarterBeats +=
            (nextEventTime - currentTime) * (currentBpm / 15.0);
        currentTime = nextEventTime;
        currentBpm  = nextBpm;
        if ( foundIndex < beatmap->m_timings.size() ) {
            // 下一轮从已使用事件之后继续，避免每个小段都从列表开头寻找。
            nextTimingIndex = foundIndex;
        }
    }

    // 将末段 BPM 下的 3 ms 容差换算为格数，容忍边界时间的微小误差。
    const double tolerance = 0.003 * (currentBpm / 15.0);
    return static_cast<std::size_t>(std::floor(totalQuarterBeats + tolerance));
}

/// @brief 计算单个音符组件对状态栏统计的贡献。
/// @param note 变更前或变更后的组件快照，不要求仍位于注册表中。
/// @param beatmap 长条连击所需的节奏数据，缺失时仅保留头部贡献。
/// @return 物件数与最大连击独立统计，不把两者视为同一个指标。
/// @note 本函数只返回单物件贡献，调用方负责从总数撤去旧值并加入新值。
/// @warning 变更统计可能随长条数量重复扫描节奏数据，不能放入逐帧渲染循环。
NoteStatisticsContribution calculateNoteStatistics(
    const NoteComponent& note, const ::MMM::BeatMap* beatmap)
{
    NoteStatisticsContribution result;
    // 草稿不计正式成绩，派生子实体由其所属折线统一统计，避免重复。
    if ( note.m_isDraft || note.m_isSubNote ) return result;

    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        // 折线容器本身不算物件，只有可计数类型的节点贡献物件数量。
        for ( const auto& sub : note.m_subNotes ) {
            if ( sub.type == ::MMM::NoteType::NOTE ||
                 sub.type == ::MMM::NoteType::HOLD ||
                 sub.type == ::MMM::NoteType::FLICK ) {
                ++result.noteCount;
            }
        }
    } else if ( note.m_type == ::MMM::NoteType::NOTE ||
                note.m_type == ::MMM::NoteType::HOLD ||
                note.m_type == ::MMM::NoteType::FLICK ) {
        ++result.noteCount;
    }

    if ( note.m_type == ::MMM::NoteType::NOTE ||
         note.m_type == ::MMM::NoteType::FLICK ) {
        ++result.maxCombo;
    } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
        ++result.maxCombo;
        // 头部贡献与区间贡献分离，零长度长条仍保留一次基础连击。
        result.maxCombo += calculateIntervalCombos(
            note.m_timestamp, note.m_timestamp + note.m_duration, beatmap);
    } else if ( note.m_type == ::MMM::NoteType::POLYLINE &&
                !note.m_subNotes.empty() ) {
        // 此处依据连接数组的首项判定头部，不通过最小时间重新选择头节点。
        // 物件总数按类型计数，而连击还受角色影响，两次遍历不能简单合并为计长。
        // 非空折线先计一次头部；内部普通节点不再各自增加基础连击。
        ++result.maxCombo;
        if ( note.m_subNotes.front().type == ::MMM::NoteType::HOLD ) {
            const auto& first = note.m_subNotes.front();
            result.maxCombo += calculateIntervalCombos(
                first.timestamp, first.timestamp + first.duration, beatmap);
        }
        for ( std::size_t index = 1U; index < note.m_subNotes.size();
              ++index ) {
            // 后续滑键增加单次连击，长条只增加持续区间产生的格数。
            const auto& sub = note.m_subNotes[index];
            if ( sub.type == ::MMM::NoteType::FLICK ) {
                ++result.maxCombo;
            } else if ( sub.type == ::MMM::NoteType::HOLD ) {
                // 内部长条不重复补头部基础连击，只计这一节点的持续区间。
                result.maxCombo += calculateIntervalCombos(
                    sub.timestamp, sub.timestamp + sub.duration, beatmap);
            }
        }
    }
    return result;
}

/// @brief 将单个顶层音符的可计数时间追加到输出缓存。
/// @param output 追加目标，不清空、不排序，也不消除同时刻的多个物件。
/// @param note 顶层组件快照；派生子实体和草稿由入口统一排除。
/// @note 时间重复次数就是密度贡献，不能转换为唯一时间集合。
/// @warning 编辑缓存维护路径会向量追加，调用方不能把它用于逐帧只读密度查询。
void appendDensityTimes(const NoteComponent& note, std::vector<double>& output)
{
    if ( note.m_isDraft || note.m_isSubNote ) return;
    /// @brief 过滤非计数类型及非法时间，保留可用于密度窗口的起始时刻。
    /// @param type 独立物件或折线节点的具体类型，不传父折线类型代替节点。
    /// @param timestamp 组件中的绝对秒数，函数不应用视觉偏移。
    const auto append = [&output](::MMM::NoteType type, double timestamp) {
        // 密度窗口只统计可用的非负起始时刻，长条尾部不额外贡献一次点击。
        // 非有限值不能进入后续依赖有序比较的时间数组。
        if ( (type == ::MMM::NoteType::NOTE || type == ::MMM::NoteType::HOLD ||
              type == ::MMM::NoteType::FLICK) &&
             std::isfinite(timestamp) && timestamp >= 0.0 ) {
            output.push_back(timestamp);
        }
    };
    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        // 密度按节点发生时刻统计，不以父容器起点代替整条折线。
        for ( const auto& sub : note.m_subNotes ) {
            append(sub.type, sub.timestamp);
        }
    } else {
        append(note.m_type, note.m_timestamp);
    }
}

/// @brief 计算音符用于可见性前缀的结束时间。
/// @return 父物件与全部折线节点的最大结束时刻，负持续时间按零处理。
/// @param note 用于裁剪范围的组件值，函数不改变其原始时长。
/// @note 返回结束范围，不代表该物件在此期间每一时刻都能被击中。
/// @warning 可见性索引维护会遍历当前折线节点，不读取全局物件或创建资源。
double noteEndTime(const NoteComponent& note)
{
    double result = note.m_timestamp + std::max(0.0, note.m_duration);
    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        // 节点顺序不作为终点依据，防止较早起始的长节点被末节点遮蔽。
        for ( const auto& sub : note.m_subNotes ) {
            result =
                std::max(result, sub.timestamp + std::max(0.0, sub.duration));
        }
    }
    return result;
}

/// @brief 判断两个可选采样绑定是否相同。
/// @param lhs 待移除事件保留的旧绑定。
/// @param rhs 缓存中候选事件的绑定。
/// @return 是否可按相同绑定状态参与事件匹配，不查询资源实际内容。
/// @note 两个空绑定相等；资源标识与音量均精确比较，用于移除旧缓存项。
/// @note 这是状态差分匹配，不是音频响度近似比较，不能给音量加入容差。
bool sameSampleBinding(const std::optional<::MMM::AudioSampleBinding>& lhs,
                       const std::optional<::MMM::AudioSampleBinding>& rhs)
{
    if ( lhs.has_value() != rhs.has_value() ) return false;
    // 只有存在性一致后才能解引用右侧；两个空值通过短路直接匹配。
    return !lhs || (lhs->m_audioResourceId == rhs->m_audioResourceId &&
                    lhs->m_volume == rhs->m_volume);
}

/// @brief 将单个正式或草稿顶层音符转换为打击事件。
/// @param output 追加目标，由外部维护事件排序和播放游标。
/// @param note 已带有节点值和绑定信息的组件快照，不借用派生子实体内容。
/// @note 草稿状态保留给事件消费者，不在转换阶段过滤。
/// @warning 编辑差分准备会复制绑定并追加事件，调用方负责限制到变更物件集合。
void appendHitEvents(const NoteComponent&                        note,
                     std::vector<System::HitFXSystem::HitEvent>& output)
{
    using HitEvent = System::HitFXSystem::HitEvent;
    using HitRole  = HitEvent::Role;
    // 派生子实体已由父折线的节点数组展开，不再生成第二份事件。
    if ( note.m_isSubNote ) return;

    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        for ( std::size_t index = 0U; index < note.m_subNotes.size();
              ++index ) {
            const auto& sub = note.m_subNotes[index];
            // 单节点折线优先标为头部；其余节点才区分尾部与内部。
            HitRole   role = index == 0U ? HitRole::Head
                                         : (index + 1U == note.m_subNotes.size()
                                                ? HitRole::Tail
                                                : HitRole::Internal);
            const int span = sub.type == ::MMM::NoteType::FLICK
                                 ? std::abs(sub.dtrack) + 1
                                 : 1;
            // span 表示包含起始轨的覆盖数量，带符号轨差另存用于左右方向。
            // 两者不能互换，否则向左滑键的覆盖数量会变成负值。
            auto binding = sub.sampleBinding;
            // 只有无独立绑定的头部继承父绑定，不能将父采样扩散到所有节点。
            if ( !binding && role == HitRole::Head ) {
                binding = note.m_sampleBinding;
            }
            output.push_back({ sub.timestamp,
                               sub.type,
                               role,
                               span,
                               sub.trackIndex,
                               sub.dtrack,
                               sub.duration,
                               true,
                               std::move(binding),
                               note.m_isDraft });
        }
        // 父折线不追加独立打击，空节点数组也不伪造一个头部事件。
        return;
    }

    const int span =
        note.m_type == ::MMM::NoteType::FLICK ? std::abs(note.m_dtrack) + 1 : 1;
    output.push_back({ note.m_timestamp,
                       note.m_type,
                       HitRole::None,
                       span,
                       note.m_trackIndex,
                       note.m_dtrack,
                       note.m_duration,
                       false,
                       note.m_sampleBinding,
                       note.m_isDraft });
}

/// @brief 判断两个打击事件是否来自同一个音符状态。
/// @param lhs 差分生成的旧事件描述。
/// @param rhs 时间排序范围内的缓存候选。
/// @return 类型、几何、角色、草稿状态和采样绑定是否全部匹配。
/// @note 时间排序只能缩小候选范围，移除时还需匹配轨道、角色及绑定等属性。
/// @note 事件不带实体身份；完全相同的重叠事件可互相匹配，但只能移除一项。
bool sameHitEvent(const System::HitFXSystem::HitEvent& lhs,
                  const System::HitFXSystem::HitEvent& rhs)
{
    return lhs.timestamp == rhs.timestamp && lhs.type == rhs.type &&
           lhs.role == rhs.role && lhs.trackSpan == rhs.trackSpan &&
           lhs.trackIndex == rhs.trackIndex &&
           lhs.trackOffset == rhs.trackOffset && lhs.duration == rhs.duration &&
           lhs.isSubNote == rhs.isSubNote && lhs.isDraft == rhs.isDraft &&
           sameSampleBinding(lhs.sampleBinding, rhs.sampleBinding);
}
}  // namespace

/// @brief 用音符变更前后快照更新排序、可见性、统计、密度及打击事件缓存。
/// @param ctx 已应用实体变更、但派生缓存仍对应变更前状态的会话。
/// @param mutations 每个实体至多出现一次；before/after
/// 分别描述删除前和更新后值。
/// @return true 表示基础缓存已更新；false 表示需要调用方执行完整重建。
/// @note 失败不保证回滚；密度匹配失败时，排序与统计可能已经发生变化。
/// @pre mutations 的快照在调用结束前有效，after 与当前 Registry 内容一致。
/// @pre 注册表和缓存由调用方串行访问，本函数不自行取得会话锁。
/// @note 新建可只给 after，删除可只给 before，更新同时提供二者。
/// @note 返回 true 也可能把打击事件表标脏；它允许该类缓存独立降级重建。
/// @note 此入口只处理 NoteRegistry，不维护独立自动采样的有序索引或统计。
/// @warning 编辑提交路径会分配、归并和移动缓存元素，不是逐帧增量查询接口。
bool applyNoteCacheMutationsIncrementally(
    SessionContext& ctx, std::span<const NoteCacheMutationView> mutations)
{
    if ( mutations.empty() ) return true;
    // 空批次是无操作成功，不借此检查或修复会话现有缓存状态。
    // 增量算法必须建立在一致的旧缓存上，不能借一次局部变更修复已有脏状态。
    if ( ctx.isNoteOrderDirty || ctx.isNotePruneDirty || ctx.isNoteStatsDirty ||
         ctx.sortedNoteEntities.size() != ctx.sortedNoteMaxEndPrefix.size() ) {
        return false;
    }

    std::unordered_set<entt::entity>           affectedEntities;
    std::vector<entt::entity>                  replacementEntities;
    std::vector<double>                        removedDensityTimes;
    std::vector<double>                        addedDensityTimes;
    std::vector<System::HitFXSystem::HitEvent> removedHitEvents;
    std::vector<System::HitFXSystem::HitEvent> addedHitEvents;
    NoteStatisticsContribution                 beforeStatistics;
    NoteStatisticsContribution                 afterStatistics;
    // 统计累计值按批次汇总，避免更新同一组对象时让中间总数被重复计入。
    double earliestTimestamp   = std::numeric_limits<double>::infinity();
    bool   touchesHitEventNote = false;

    // 准备阶段只收集差分，不在这里逐个改写会话统计。
    // 这样重复实体、无效新实体和旧索引缺项可在基础缓存变动之前拒绝。
    affectedEntities.reserve(mutations.size());
    replacementEntities.reserve(mutations.size());
    for ( const auto& mutation : mutations ) {
        // 重复实体会重复扣减统计或密度，先拒绝不满足批次唯一性的输入。
        if ( mutation.entity == entt::null ||
             !affectedEntities.insert(mutation.entity).second ) {
            return false;
        }
        if ( mutation.before ) {
            // 旧值来自调用方快照，不能回到已修改的注册表中读取。
            earliestTimestamp =
                std::min(earliestTimestamp, mutation.before->m_timestamp);
            const auto contribution = calculateNoteStatistics(
                *mutation.before, ctx.currentBeatmap.get());
            beforeStatistics.noteCount += contribution.noteCount;
            beforeStatistics.maxCombo += contribution.maxCombo;
            appendDensityTimes(*mutation.before, removedDensityTimes);
            if ( !mutation.before->m_isSubNote ) {
                // 草稿没有正式统计贡献，但其播放预览事件仍需跟随变更。
                // 只排除派生子实体，不能直接用 noteCount 是否为零决定事件更新。
                touchesHitEventNote = true;
                if ( !ctx.isHitEventsDirty ) {
                    appendHitEvents(*mutation.before, removedHitEvents);
                }
            }
        }
        if ( mutation.after ) {
            // 新值参与排序时需要访问当前组件，因此不能接受已失效的实体。
            if ( !ctx.noteRegistry.valid(mutation.entity) ||
                 !ctx.noteRegistry.all_of<NoteComponent>(mutation.entity) ) {
                return false;
            }
            replacementEntities.push_back(mutation.entity);
            // 更新的起点可能向前或向后移动，取新旧最早位置确定受影响后缀。
            // 即使只改持续时间，仍需从起点重算可见性终点前缀。
            earliestTimestamp =
                std::min(earliestTimestamp, mutation.after->m_timestamp);
            const auto contribution = calculateNoteStatistics(
                *mutation.after, ctx.currentBeatmap.get());
            afterStatistics.noteCount += contribution.noteCount;
            afterStatistics.maxCombo += contribution.maxCombo;
            appendDensityTimes(*mutation.after, addedDensityTimes);
            if ( !mutation.after->m_isSubNote ) {
                // 若父子表示发生切换，旧侧与新侧分别判断，不能复用旧角色结论。
                touchesHitEventNote = true;
                if ( !ctx.isHitEventsDirty ) {
                    appendHitEvents(*mutation.after, addedHitEvents);
                }
            }
        }
    }

    for ( const auto& mutation : mutations ) {
        // 在改写排序缓存前确认旧实体存在，否则局部移除无法对应完整旧状态。
        if ( mutation.before &&
             std::find(ctx.sortedNoteEntities.begin(),
                       ctx.sortedNoteEntities.end(),
                       mutation.entity) == ctx.sortedNoteEntities.end() ) {
            return false;
        }
    }

    // 仅排序替换集合，再与剩余有序集合归并；无需重新排序全部音符。
    // 先移除所有受影响实体，避免更新后实体同时留在旧位置和替换集合中。
    // std::erase_if 保留其余实体的原顺序，为后面的有序归并提供前提。
    std::erase_if(ctx.sortedNoteEntities, [&](entt::entity entity) {
        return affectedEntities.contains(entity);
    });
    /// @brief 按更新后的组件起始时间排序，与原缓存使用相同排序口径。
    /// @param lhs 已验证具有 NoteComponent 的候选实体。
    /// @param rhs 与 lhs 同属本会话 Registry 的候选实体。
    /// @return 是否严格早于另一实体；同刻不追加实体 ID 优先级。
    const auto entityLess = [&ctx](entt::entity lhs, entt::entity rhs) {
        return ctx.noteRegistry.get<const NoteComponent>(lhs).m_timestamp <
               ctx.noteRegistry.get<const NoteComponent>(rhs).m_timestamp;
    };
    std::sort(
        replacementEntities.begin(), replacementEntities.end(), entityLess);
    // 独立输出容器避免归并过程中覆写尚未读取的旧元素。
    // 比较器读取的是更新后的 Registry，不读取已经用于差分扣减的 before。
    std::vector<entt::entity> mergedEntities;
    mergedEntities.reserve(ctx.sortedNoteEntities.size() +
                           replacementEntities.size());
    std::merge(ctx.sortedNoteEntities.begin(),
               ctx.sortedNoteEntities.end(),
               replacementEntities.begin(),
               replacementEntities.end(),
               std::back_inserter(mergedEntities),
               entityLess);
    ctx.sortedNoteEntities.swap(mergedEntities);

    // 插入、删除或移动都可能改变后缀最大终点，重算起点取新旧最早时间。
    // 该时间之前的实体序列未受影响，可以继续复用其累计最大值。
    const auto prefixBegin = std::lower_bound(
        ctx.sortedNoteEntities.begin(),
        ctx.sortedNoteEntities.end(),
        earliestTimestamp,
        [&ctx](entt::entity entity, double timestamp) {
            return ctx.noteRegistry.get<const NoteComponent>(entity)
                       .m_timestamp < timestamp;
        });
    const auto prefixIndex = static_cast<std::size_t>(
        std::distance(ctx.sortedNoteEntities.begin(), prefixBegin));
    // 前缀表长度必须跟随新序列，包括尾部删除导致的缩短。
    // 从首项重算时使用零基线；否则接续未受影响的上一项最大终点。
    ctx.sortedNoteMaxEndPrefix.resize(ctx.sortedNoteEntities.size());
    double maximumEnd =
        prefixIndex == 0U ? 0.0 : ctx.sortedNoteMaxEndPrefix[prefixIndex - 1U];
    for ( std::size_t index = prefixIndex;
          index < ctx.sortedNoteEntities.size();
          ++index ) {
        const auto entity = ctx.sortedNoteEntities[index];
        maximumEnd        = std::max(
            maximumEnd,
            noteEndTime(ctx.noteRegistry.get<const NoteComponent>(entity)));
        ctx.sortedNoteMaxEndPrefix[index] = maximumEnd;
    }

    // 防止无符号统计下溢；先撤去旧贡献，再加入新贡献。
    ctx.noteCount = ctx.noteCount >= beforeStatistics.noteCount
                        ? ctx.noteCount - beforeStatistics.noteCount
                        : 0U;
    ctx.maxCombo  = ctx.maxCombo >= beforeStatistics.maxCombo
                        ? ctx.maxCombo - beforeStatistics.maxCombo
                        : 0U;
    ctx.noteCount += afterStatistics.noteCount;
    ctx.maxCombo += afterStatistics.maxCombo;

    // 密度时间表保留逐物件的重复值；窗口级密度图在本表更新后再按脏标记生成。
    // 不把多个新增物件合并成一个时间戳，否则峰值会低估同刻叠键。
    for ( const double timestamp : removedDensityTimes ) {
        // 同刻多个物件各占一项，每次只删除一个匹配值而非整组时间。
        const auto found =
            std::lower_bound(ctx.previewDensityObjectTimes.begin(),
                             ctx.previewDensityObjectTimes.end(),
                             timestamp);
        if ( found == ctx.previewDensityObjectTimes.end() ||
             *found != timestamp ) {
            // 旧贡献与缓存不一致时不能继续猜测移除位置。
            // 此时基础索引和统计已经更新，调用方须标脏并完整重建而非重试差分。
            return false;
        }
        ctx.previewDensityObjectTimes.erase(found);
    }
    for ( const double timestamp : addedDensityTimes ) {
        // 插到同时间组末尾以维持有序性，重复值仍须保留。
        const auto insertion =
            std::upper_bound(ctx.previewDensityObjectTimes.begin(),
                             ctx.previewDensityObjectTimes.end(),
                             timestamp);
        ctx.previewDensityObjectTimes.insert(insertion, timestamp);
    }
    ctx.isPreviewDensityDirty =
        !removedDensityTimes.empty() || !addedDensityTimes.empty();

    // 事件更新的失败处理比基础密度宽松：旧事件表失配后标脏即可，
    // 下一次 ensureHitEvents 会从当前组件完整重建，不返回基础更新失败。
    if ( touchesHitEventNote && !ctx.isHitEventsDirty ) {
        // 已脏事件表留给重建处理，不能在不可信的旧表上继续应用差分。
        for ( const auto& event : removedHitEvents ) {
            const auto range = std::equal_range(
                ctx.hitEvents.begin(), ctx.hitEvents.end(), event);
            // 排序等价不等于完整状态相等，同刻不同轨或不同绑定仍需区分。
            // 即使有多个完全相同的事件，这次变更也只扣除一个贡献。
            const auto found = std::find_if(
                range.first, range.second, [&](const auto& current) {
                    return sameHitEvent(current, event);
                });
            if ( found == range.second ) {
                // 事件差分失败只降级此缓存，不否定已经完成的基础索引更新。
                ctx.isHitEventsDirty = true;
                break;
            }
            ctx.hitEvents.erase(found);
        }
        if ( !ctx.isHitEventsDirty ) {
            // 只有所有旧事件都匹配成功后才插入新事件。
            // 降级重建时保留脏标志，不把一份不完整的事件表当作可读缓存发布。
            for ( auto& event : addedHitEvents ) {
                const auto insertion = std::upper_bound(
                    ctx.hitEvents.begin(), ctx.hitEvents.end(), event);
                ctx.hitEvents.insert(insertion, std::move(event));
            }
            syncHitIndex(ctx);
            // 事件插入或删除会移动下标，成功后按当前播放时间重新定位游标。
        }
    }

    // 发布索引版本供读取端失效旧视野结果；不在这里触发资源重载。
    // 版本号在整条成功路径末尾推进，中途失败须由调用方接管完整重建。
    ++ctx.noteVisibilityIndexRevision;
    ctx.isNoteOrderDirty = false;
    ctx.isNotePruneDirty = false;
    ctx.isNoteStatsDirty = false;
    // 此处不清模型回写标志；派生缓存一致不意味着 BeatMap 已接收组件变更。
    return true;
}

/// @brief 根据共用专业模式与独立折线编辑开关判断音符是否可编辑。
/// @param note 需要判断的物件或派生子实体，不涉及音频样本 Registry。
/// @param settings 当前会话已解析的编辑设置。
/// @return 是否通过物件类型开关，不包含协作权限或项目只读状态判断。
/// @warning 逻辑与渲染热路径：只读取组件属性和配置布尔值。
bool isNoteEditable(const NoteComponent&          note,
                    const Config::EditorSettings& settings)
{
    // 专业模式限制优先于折线编辑开关，启用后者不会开放隐藏的草稿轨。
    if ( note.m_isDraft && !settings.professionalMode ) return false;
    if ( settings.enablePolylineEditing ) return true;
    // 基础模式只允许独立单键与长条；独立滑键也不在这个允许集合中。
    return !note.m_isSubNote && (note.m_type == ::MMM::NoteType::NOTE ||
                                 note.m_type == ::MMM::NoteType::HOLD);
}

/// @brief 排除已知辅助视图，判断相机标识是否属于主画布候选。
/// @param cameraId 内部相机标识，不是本地化窗口标题。
/// @return 是否不属于辅助视图集合；未知标识也可能返回 true。
/// @note 使用排除规则兼容多谱面动态标识，不只接受旧的固定窗口名。
/// @warning 视图查询热路径仅比较标识，不查找或创建窗口。
bool isMainCanvasCameraId(const std::string& cameraId)
{
    return cameraId != "Preview" && cameraId != "PreviewCanvas" &&
           cameraId != "Timeline" && cameraId != "AudioWaveform" &&
           cameraId != "AudioSpectrum";
}

/// @brief 从已登记相机中借用一个主画布相机，找不到时返回空指针。
/// @param cameras 调用方拥有的相机表，返回值不得跨越条目删除使用。
/// @return 借用的候选相机，空表或只有辅助相机时返回 nullptr。
/// @note 优先兼容旧固定标识，其余候选按容器遍历顺序选择，不代表焦点顺序。
/// @warning 相机查询会遍历已登记视图，不承担相机资源初始化。
const CameraInfo* findMainCanvasCamera(
    const std::unordered_map<std::string, CameraInfo>& cameras)
{
    // 旧固定 ID 优先保持历史布局的选择语义；没有它才兼容动态画布 ID。
    // 这里不检查窗口可见性，不能用返回值断言该画布当前在前台。
    auto itLegacy = cameras.find("Basic2DCanvas");
    if ( itLegacy != cameras.end() ) {
        return &itLegacy->second;
    }

    // 多个动态主画布都合法时不排序候选，避免查询辅助函数引入焦点管理职责。
    for ( const auto& [cameraId, camera] : cameras ) {
        if ( isMainCanvasCameraId(cameraId) ) {
            return &camera;
        }
    }

    return nullptr;
}

/// @brief 将绝对时间转换为跨 BPM 段连续编号的整拍拍号。
/// @param time 查询时间，单位为秒。
/// @param bpmEvents 已按时间排序的 BPM 组件借用列表。
/// @param fallbackBpm 传递给统一 BPM 规范化入口的后备值。
/// @pre 段间时间差和累计拍数在返回整型的可表示范围内。
/// @return 从 1 开始的拍号；无事件、非有限时间或首段之前返回 0。
/// @note BPM 切换前不足一整拍的尾段仍占一个拍号，不沿用旧 BPM 的小数相位。
/// @warning 时间定位查询遍历 BPM 列表，不应在逐音符绘制中反复调用。
int calculateBeatIndex(double                                       time,
                       const std::vector<const TimelineComponent*>& bpmEvents,
                       double                                       fallbackBpm)
{
    if ( bpmEvents.empty() || !std::isfinite(time) ) {
        return 0;
    }

    int64_t totalBeats = 0;
    for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
        const auto* currentBpm = bpmEvents[i];
        if ( !currentBpm ) {
            // 允许跳过空条目，但不修复相邻段关系；正常调用应提供连续有效列表。
            continue;
        }

        const double bpm =
            ::MMM::normalizeBpmValue(currentBpm->m_value, fallbackBpm);
        // 各段独立使用传入的 fallback，不以上一段 BPM 覆写调用方后备策略。

        // 段落右端不包含下一 BPM 起点，边界时刻归入新节奏段。
        const double nextBpmTime =
            i + 1 < bpmEvents.size() && bpmEvents[i + 1]
                ? bpmEvents[i + 1]->m_timestamp
                : std::numeric_limits<double>::infinity();
        if ( time >= currentBpm->m_timestamp && time < nextBpmTime ) {
            // 当前段向下取整获取所在拍，微小容差避免整拍边界落回前一拍。
            const double beatDuration = 60.0 / bpm;
            const auto   beatsInBpm   = static_cast<int64_t>(std::floor(
                (time - currentBpm->m_timestamp) / beatDuration + 1e-6));
            return static_cast<int>(totalBeats + beatsInBpm + 1);
        }
        if ( time >= nextBpmTime ) {
            // 已走完的段向上取整计入总拍数，正长度短段至少贡献一个编号。
            const double beatDuration = 60.0 / bpm;
            const double segmentDuration =
                nextBpmTime - currentBpm->m_timestamp;
            auto beatsInBpm = static_cast<int64_t>(
                std::ceil(segmentDuration / beatDuration - 1e-6));
            if ( segmentDuration > 0.0 ) {
                // 防止正长度极短段受浮点容差影响被向上取整为零。
                beatsInBpm = std::max<int64_t>(beatsInBpm, 1);
            }
            totalBeats += beatsInBpm;
            continue;
        }
        // 查询位于当前段之前且没有命中更早段时，不向首段之前外推拍号。
        break;
    }
    return 0;
}

/// @brief 合并谱面内容长度、会话缓存长度和当前匹配音频时间线的长度。
/// @param ctx 提供内容描述及最近已知音频总长，不转移音频控制权。
/// @return 非负总秒数；未匹配的全局音频不能延长当前谱面的时间范围。
/// @warning 播放更新路径只读取现有描述与音频状态，不扫描音符或解码文件。
double getEffectiveTotalTimeSeconds(const SessionContext& ctx)
{
    // 谱面内容决定最低可导航范围，音频尾部只扩展这个范围，不裁掉尾部时间点。
    double totalTime = ctx.audioTimelineDescriptor.m_chartEndSeconds;
    if ( ctx.audioTimelineTotalTime > 0.0 &&
         std::isfinite(ctx.audioTimelineTotalTime) ) {
        totalTime = std::max(totalTime, ctx.audioTimelineTotalTime);
    }

    // 没有非空指纹时只相信会话侧长度，不借用设备中可能仍保留的上一首音频。
    const auto& audio = Audio::AudioManager::instance();
    // 多会话共用音频管理器，必须通过指纹确认长度确实属于本会话。
    if ( !ctx.audioTimelineDescriptor.m_fingerprint.empty() &&
         audio.getLoadedAudioTimelineFingerprint() ==
             ctx.audioTimelineDescriptor.m_fingerprint ) {
        totalTime = std::max(totalTime, audio.getTotalTime());
    }
    return std::max(0.0, totalTime);
}

/// @brief 扫描谱面模型，取得时间点与可计数物件覆盖的最晚时间。
/// @param beatMap 使用毫秒存储时间的谱面模型，不是 ECS 秒单位组件。
/// @pre 折线引用指向有效的模型物件，HOLD 标记与实际派生类型一致。
/// @return 最晚内容位置换算后的秒数；没有正时间内容时返回零。
/// @warning 描述重建路径完整遍历模型，不用于每帧计算总时长。
double calculateChartContentEndSeconds(const MMM::BeatMap& beatMap)
{
    // 使用最大终点而非物件遍历末项，因此不要求各类型容器已按时间排序。
    // 自动采样与主音轨尾部由音频描述负责，不在本函数重复计算媒体持续时间。
    double chartEndMs = 0.0;
    /// @brief 将有限的单点时间并入最晚内容位置，负时间不缩短零基线。
    /// @param timestampMs 原始模型毫秒时间，单位转换在整个范围计算结束后进行。
    const auto includeTimestamp = [&chartEndMs](double timestampMs) {
        if ( std::isfinite(timestampMs) ) {
            chartEndMs = std::max(chartEndMs, timestampMs);
        }
    };
    /// @brief 将合法起点和持续时间组成的终点并入范围，负时长按零处理。
    /// @param timestampMs 模型起点，保留允许为负的前导时间。
    /// @param durationMs 模型持续时间，不是绝对终点。
    const auto includeDuration = [&chartEndMs](double timestampMs,
                                               double durationMs) {
        if ( std::isfinite(timestampMs) && std::isfinite(durationMs) ) {
            // 负时长退为零以保留起点；非有限时长则整项不贡献范围。
            chartEndMs =
                std::max(chartEndMs, timestampMs + std::max(durationMs, 0.0));
        }
    };

    for ( const auto& timing : beatMap.m_timings ) {
        // 空白尾部仍可能有编辑时间点，不能只用最后一个音符界定内容范围。
        includeTimestamp(timing.m_timestamp);
    }
    for ( const auto& note : beatMap.m_noteData.notes ) {
        includeTimestamp(note.m_timestamp);
    }
    for ( const auto& hold : beatMap.m_noteData.holds ) {
        includeDuration(hold.m_timestamp, hold.m_duration);
    }
    for ( const auto& flick : beatMap.m_noteData.flicks ) {
        includeTimestamp(flick.m_timestamp);
    }
    for ( const auto& polyline : beatMap.m_noteData.polylines ) {
        // 父起点和每个子节点都参与范围，长条子节点使用终点而非起点。
        includeTimestamp(polyline.m_timestamp);
        for ( const auto& subNoteReference : polyline.m_subNotes ) {
            // 节点也可能已在类型容器中扫描过；最大值归并允许重复访问，
            // 不需要为范围计算维护一套额外的节点去重集合。
            const auto& subNote = subNoteReference.get();
            if ( subNote.m_type == NoteType::HOLD ) {
                const auto& subHold = static_cast<const Hold&>(subNote);
                includeDuration(subHold.m_timestamp, subHold.m_duration);
            } else {
                includeTimestamp(subNote.m_timestamp);
            }
        }
    }
    return std::max(chartEndMs, 0.0) / 1000.0;
}

/// @brief 从当前谱面重建音频描述，登记后续激活和指纹发布需求。
/// @param ctx 描述输出及脏状态所属会话。
/// @param project 资源解析环境；为空时使用谱面所在目录构造最小环境。
/// @return 音频描述指纹是否发生变化，不表示音频已加载或正在播放。
/// @note 即使新旧指纹相同，也保留之前未消费的激活请求。
/// @pre 需要反映最新编辑时，调用方应先将相关 ECS 变更同步到模型。
/// @warning 低频脏状态处理会扫描内容、解析资源并输出诊断，不可无条件逐帧重建。
bool rebuildAudioTimelineDescriptor(SessionContext&       ctx,
                                    const ::MMM::Project* project)
{
    const std::string previousFingerprint =
        ctx.audioTimelineDescriptor.m_fingerprint;
    const bool activationWasPending = ctx.isAudioTimelineActivationPending;
    // 先保存旧指纹与激活意图再替换描述，否则无法区分内容变化和未处理请求。
    if ( !ctx.currentBeatmap ) {
        // 空会话也需发布清空结果，后续激活流程负责卸载旧音频。
        ctx.audioTimelineDescriptor                  = {};
        ctx.audioTimelineTotalTime                   = 0.0;
        ctx.isAudioTimelineDescriptorDirty           = false;
        ctx.isAudioTimelineActivationPending         = true;
        ctx.isAudioTimelineFingerprintPublishPending = true;
        return !previousFingerprint.empty();
    }

    // 描述由当前内存模型构造，不直接扫描 Registry，也不触发谱面保存。
    // 资源上下文只在构造期间借用，后备项目离开函数后可立即销毁。
    Project fallbackProject;
    if ( !project ) {
        // 后备项目只提供相对路径基准，不伪造完整的项目资源索引。
        fallbackProject.m_projectRoot =
            ctx.currentBeatmap->m_baseMapMetadata.map_path.parent_path();
    }
    const Project& descriptorProject = project ? *project : fallbackProject;
    ctx.audioTimelineDescriptor      = buildAudioTimelineDescriptor(
        *ctx.currentBeatmap,
        descriptorProject,
        ctx.currentBeatmap->m_baseMapMetadata.map_path,
        calculateChartContentEndSeconds(*ctx.currentBeatmap));
    ctx.audioTimelineTotalTime = ctx.audioTimelineDescriptor.m_chartEndSeconds;
    // 先用内容长度初始化，实际解码后才由激活流程补足音频尾部长度。
    const bool fingerprintChanged =
        previousFingerprint != ctx.audioTimelineDescriptor.m_fingerprint;
    // 指纹相同不撤销旧请求：例如描述已构造而活动会话尚未完成激活。
    // 描述脏标志与激活待处理标志分别表示数据准备和设备消费两个阶段。
    ctx.isAudioTimelineDescriptorDirty = false;
    ctx.isAudioTimelineActivationPending =
        activationWasPending || fingerprintChanged;
    ctx.isAudioTimelineFingerprintPublishPending = true;

    // 诊断不改变返回值语义：这里返回指纹是否变化，而不是资源解析成功与否。
    // 调用方不能以 false 推断无需处理已有的激活或指纹发布标记。
    for ( const auto& diagnostic : ctx.audioTimelineDescriptor.m_diagnostics ) {
        XWARN("Audio timeline descriptor: {}", diagnostic.m_message);
    }
    return fingerprintChanged;
}

/// @brief 让活动会话的描述成为全局音频时间线，并对齐播放及视觉时钟。
/// @param ctx 需要拥有全局音频传输控制权的活动会话。
/// @param shouldPlay 定位到会话当前时间后播放还是暂停。
/// @return 是否成功完成激活；非活动会话、无谱面或加载失败返回 false。
/// @note 成功也可能存在缺失音频片段，数量与诊断由加载结果独立反馈。
/// @note 此函数控制底层传输，但不直接将 ctx.isPlaying 设为 shouldPlay。
/// @warning 会话切换或显式激活路径可能加载资源，不可作为逐帧时钟读取接口。
bool activateAudioTimeline(SessionContext& ctx, bool shouldPlay)
{
    if ( !ctx.isActiveSession ) {
        // 后台谱面不得覆盖正在由活动会话使用的全局音频时间线。
        return false;
    }

    if ( ctx.isAudioTimelineDescriptorDirty ) {
        // 协作会话优先使用自己的项目环境，避免解析到本地另一项目的资源。
        const auto* project =
            ctx.collaborationProject
                ? ctx.collaborationProject.get()
                : EditorEngine::instance().getCurrentProject();
        rebuildAudioTimelineDescriptor(ctx, project);
    }

    auto& audio = Audio::AudioManager::instance();
    if ( !ctx.currentBeatmap ) {
        // 活动空会话明确卸载全局时间线；后台空会话已由入口挡住，不能卸载它。
        audio.unloadAudioTimeline();
        ctx.audioTimelineTotalTime           = 0.0;
        ctx.missingAudioTimelineClipCount    = 0U;
        ctx.isAudioTimelineActivationPending = false;
        return false;
    }

    const auto& descriptor = ctx.audioTimelineDescriptor;
    // 指纹相同仍可能有显式待激活请求，不能仅靠指纹跳过加载。
    const bool needsReload =
        ctx.isAudioTimelineActivationPending ||
        !audio.hasLoadedAudioTimeline() ||
        audio.getLoadedAudioTimelineFingerprint() != descriptor.m_fingerprint;
    if ( needsReload ) {
        // 传入事件与内容末尾两部分：没有音频事件的尾部也应保留可定位区间。
        const auto result =
            audio.loadAudioTimeline(descriptor.m_events,
                                    descriptor.m_chartEndSeconds,
                                    descriptor.m_fingerprint);
        // 本次激活请求已尝试消费，失败诊断通过结果返回，不留标志导致逐帧重试。
        // 后续是否重新请求加载由上层交互或描述变化决定。
        ctx.isAudioTimelineActivationPending = false;
        ctx.missingAudioTimelineClipCount    = result.missingClipCount;
        if ( !result.success ) {
            // 加载失败保留谱面内容范围供界面导航，不将旧音频长度带入新会话。
            ctx.audioTimelineTotalTime = descriptor.m_chartEndSeconds;
            for ( const auto& diagnostic : result.diagnostics ) {
                XWARN("Audio timeline load: {}", diagnostic.message);
            }
            return false;
        }
        for ( const auto& diagnostic : result.diagnostics ) {
            XWARN("Audio timeline load: {}", diagnostic.message);
        }
    }

    ctx.audioTimelineTotalTime = audio.getTotalTime();
    // 复用同一描述时也要重新定位，否则会保留上一会话的传输位置。
    audio.seek(ctx.currentTime);
    if ( shouldPlay ) {
        audio.play();
    } else {
        // 暂停仍保留刚设定的 seek 位置，不能替换成会重置位置的停止语义。
        audio.pause();
    }
    const double activationTime =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    // 时间基准取自激活动作之后，避免把可能耗时的资源加载算进视觉推演跨度。
    // 清除上次传输的平滑历史，用同一定位时间建立新的单调时钟基准。
    ctx.playbackVisualClock.reset();
    ctx.playbackVisualClock.rebase(
        ctx.currentTime, activationTime, audio.getPlaybackSpeed(), shouldPlay);
    return true;
}

/// @brief 计算会话切换时目标时间与是否续播，不修改任何会话或音频状态。
/// @param previousSyncFingerprint 原会话的同步时间线指纹。
/// @param targetSyncFingerprint 目标会话的同步时间线指纹。
/// @param previousTime 原会话当前时间。
/// @param targetTime 目标会话原有时间。
/// @param previousWasPlaying 原会话切换前是否播放。
/// @param stopPlaybackOnScroll 是否要求切换定位时停止续播。
/// @param synchronizeMatchingTimelines 是否同步相同时间线的定位时间。
/// @note 时间同步开关只决定定位来源，续播条件另行按同指纹与停止策略判断。
/// @return 定位和续播决策的值对象，不包含任何 seek、play 或 pause 副作用。
AudioTimelineSwitchDecision resolveAudioTimelineSwitch(
    std::string_view previousSyncFingerprint,
    std::string_view targetSyncFingerprint, double previousTime,
    double targetTime, bool previousWasPlaying, bool stopPlaybackOnScroll,
    bool synchronizeMatchingTimelines)
{
    // 两个空指纹不能说明共享时间线；无可验证来源时保留目标会话自己的定位。
    const bool sameTimeline = !previousSyncFingerprint.empty() &&
                              previousSyncFingerprint == targetSyncFingerprint;
    // 即使不复制上一会话的位置，同一时间线也可以按目标自己的位置继续播放。
    // 因而 m_resumePlayback 不能简单复用“是否复制 previousTime”的条件。
    return {
        .m_targetTime = sameTimeline && synchronizeMatchingTimelines
                            ? previousTime
                            : targetTime,
        .m_resumePlayback =
            sameTimeline && previousWasPlaying && !stopPlaybackOnScroll,
    };
}

/// @brief 消费匹配时间线的传输快照，更新会话时间、播放与跟随状态。
/// @param ctx 要更新的会话；跟随会话通过其来源指纹匹配传输。
/// @param loadedFingerprint 当前音频管理器实际加载的时间线指纹。
/// @param snapshot 本次音频时钟观测，不保证有效或正在播放。
/// @param nowSteadySeconds 本次更新的单调时间，供视觉时钟插值使用。
/// @param syncConfig 非跟随会话对音频时钟应用的同步策略。
/// @return 更新后会话是否仍处于播放或时间线跟随状态。
/// @pre nowSteadySeconds 与视觉时钟重定位使用相同的单调时钟来源。
/// @warning 播放更新热路径，不应添加资源重载、睡眠或等待传输完成的循环。
bool applyAudioTimelineTransportSnapshot(
    SessionContext& ctx, std::string_view loadedFingerprint,
    const Audio::AudioTimelineClockSnapshot& snapshot, double nowSteadySeconds,
    const Config::SyncConfig& syncConfig)
{
    // 这里只借用本次调用期间的字符串，不复制共享对象或延长来源会话生命周期。
    const std::string_view expectedFingerprint =
        ctx.isAudioTimelineSyncFollower
            ? std::string_view(ctx.m_audioTimelineSyncSourceFingerprint)
            : std::string_view(ctx.audioTimelineDescriptor.m_fingerprint);
    const bool readsCurrentTransport = !expectedFingerprint.empty() &&
                                       loadedFingerprint == expectedFingerprint;
    if ( !readsCurrentTransport ) {
        // 全局音频已切换到别的来源，解除跟随并清掉旧插值基准。
        ctx.isPlaying                   = false;
        ctx.isAudioTimelineSyncFollower = false;
        ctx.m_audioTimelineSyncSourceFingerprint.clear();
        ctx.playbackVisualClock.reset();
        return false;
    }

    // 来源身份匹配只允许消费传输信息，不意味着某个后台会话取得音频控制权。
    // 跟随路径只推演本地时钟，不能在这里 seek 全局音频来追赶自身视野。
    // 跟随者使用自身既有视觉时钟，主会话才直接消费音频采样进行校正。
    const double currentTime =
        ctx.isAudioTimelineSyncFollower
            ? (ctx.playbackVisualClock.initialized()
                   ? ctx.playbackVisualClock.resolveAt(nowSteadySeconds)
                   : ctx.currentTime)
            : ctx.playbackVisualClock.update(
                  snapshot, nowSteadySeconds, syncConfig);
    // 跟随者还没有时钟基线时沿用已有定位，不能以默认零时间制造一次视觉跳变。
    if ( std::isfinite(currentTime) ) {
        // 有已知长度时只限制上界，负时间仍可用于谱面开始前的定位。
        const double totalTime = getEffectiveTotalTimeSeconds(ctx);
        ctx.currentTime =
            totalTime > 0.0 ? std::min(currentTime, totalTime) : currentTime;
    }
    // 推演结果不可用时保留最后一个有效定位，但仍继续处理有效快照的停止状态。
    // 停止判定要求有效快照；读取不到一次音频观测不等于已经自然播放结束。
    if ( ctx.isPlaying && snapshot.valid &&
         snapshot.state == Audio::AudioTimelinePlaybackState::Stopped ) {
        // 自然播完与普通停止分开记录，是否重启留给后续播放控制流程。
        ctx.restartPlaybackAfterFinishPending = snapshot.finished;
        ctx.isPlaying                         = false;
        return false;
    }
    if ( ctx.isAudioTimelineSyncFollower && snapshot.valid &&
         snapshot.state != Audio::AudioTimelinePlaybackState::Playing ) {
        // 来源暂停或停止后不再持续外推；无效快照则不据此推断来源已停止。
        ctx.isAudioTimelineSyncFollower = false;
        ctx.m_audioTimelineSyncSourceFingerprint.clear();
        return false;
    }
    // 返回的是会话是否继续推进，不是本次快照是否有效；缺一帧观测不强行暂停。
    return ctx.isPlaying || ctx.isAudioTimelineSyncFollower;
}

/// @brief 在单个 BPM 段中选取最近的启用分拍位置。
/// @param rawTime 待定位的原始秒数，可位于段落起点之前。
/// @param timingTime 分拍网格的相位原点，单位为秒。
/// @param nextTimingTime 下一段起点，尾段允许传正无穷。
/// @param bpm 当前段每分钟拍数，必须有限且为正。
/// @param settings 吸附开关、向下取整规则及启用的分拍方案。
/// @return 成功时携带绝对秒数和最简拍位；无候选时 isSnapped 为 false。
/// @note 候选不受像素距离阈值约束；本函数只做时间网格选择。
/// @pre nextTimingTime 不早于 timingTime，时间差与步号处于可表示范围。
/// @note 不强制候选非负；首段之前是否允许放置由调用入口决定。
/// @note 拍内分数只描述所选时间的网格类别，不包含绝对拍号。
/// @warning 连续编辑热路径只枚举有限分拍方案，不遍历物件或加载配置。
SnapResult calculateObjectPlacementSnap(double rawTime, double timingTime,
                                        double nextTimingTime, double bpm,
                                        const Config::EditorSettings& settings)
{
    SnapResult result;
    // 禁用吸附和非法输入共用空结果，由调用方决定如何保留原始定位。
    if ( !settings.objectPlacementSnap || !std::isfinite(rawTime) ||
         !std::isfinite(timingTime) || !std::isfinite(bpm) || bpm <= 0.0 ) {
        return result;
    }

    const double beatDuration = 60.0 / bpm;
    // 有限正 BPM 仍可能使倒数溢出或步长小到不可用，先验证秒单位拍长。
    // 此入口拒绝不可用的网格，不在失败结果中伪造一次整拍吸附。
    if ( !std::isfinite(beatDuration) || beatDuration <= 1e-9 ) {
        return result;
    }

    // 各方案的候选按时间距离竞争；近似等距时保留先枚举到的方案。
    double bestDistance = std::numeric_limits<double>::infinity();
    /// @brief 计算一个分母下的候选，仅在严格更近时替换当前结果。
    /// @param divisor 每拍的等分数，不是待返回分数的固定分母。
    /// @note 捕获结果与最优距离，在同一次查询的全部启用方案间归并候选。
    auto considerDivisor = [&](int divisor) {
        if ( divisor <= 0 ) return;

        const double stepDuration = beatDuration / divisor;
        // 细分后的步长要再次验证，拍长可用并不保证任意细分仍可稳定运算。
        if ( !std::isfinite(stepDuration) || stepDuration <= 1e-9 ) return;

        // 始终相对 BPM 起点取整，不能相对零时刻生成另一套拍位相位。
        const double relativeTime = rawTime - timingTime;
        // floor 模式对负时间也向负无穷取整，不可用整数截断代替，
        // 否则原点之前的位置会被错误吸到右侧网格。
        const double stepCount =
            settings.snapFloor ? std::floor(relativeTime / stepDuration + 1e-6)
                               : std::round(relativeTime / stepDuration);
        double candidate = timingTime + stepCount * stepDuration;
        // 先重建绝对时间再限制右端，保留非零 BPM 相位；
        // 单纯对 rawTime/stepDuration 取整会把有偏移的谱面吸到另一套网格。
        // 旧段网格不能越过下一时间点，但允许恰好吸附到段落边界。
        if ( candidate > nextTimingTime ) candidate = nextTimingTime;
        if ( !std::isfinite(candidate) ) return;

        // 比较的是最终可放置位置，因此下一段边界截断也要在距离计算之前完成。
        // snapFloor 改变各分母内部的取整方向，不改变跨分母比较的距离口径。
        const double distance = std::abs(candidate - rawTime);
        // bestDistance 是所有已启用方案共享的比较基准，不能在每个分母重置。
        // 微小容差让等价网格交点保持稳定，不因浮点误差来回切换分拍标签。
        if ( distance >= bestDistance - 1e-12 ) return;

        result.isSnapped   = true;
        result.snappedTime = candidate;
        bestDistance       = distance;

        if ( std::isfinite(nextTimingTime) &&
             std::abs(candidate - nextTimingTime) <= 1e-9 ) {
            // 新段起点按整拍报告，不拿旧段步长解释被截断后的分数。
            result.numerator   = 1;
            result.denominator = 1;
            return;
        }

        // 拍内编号只需余数，完整步号用于定位而不直接作为分数分子。
        // 例如四等分网格的第五步仍报告 1/4，不报告 5/4。
        // 重算截断后的步号，并将负步号映射到非负拍内索引。
        auto stepIndex = static_cast<std::int64_t>(
            std::llround((candidate - timingTime) / stepDuration));
        int beatIndex = static_cast<int>(stepIndex % divisor);
        if ( beatIndex < 0 ) beatIndex += divisor;
        // C++ 负数取模保留负号；修正后分子落在 [0, divisor)，
        // 首段前的拍位也能复用正时间分拍的标签与配色。
        if ( beatIndex == 0 ) {
            // 整拍统一表示为 1/1，而非 0/divisor，便于样式与提示使用。
            result.numerator   = 1;
            result.denominator = 1;
            return;
        }
        // 等价分拍约分后共享同一标签与颜色，例如 2/4 显示为 1/2。
        const int factor   = std::gcd(beatIndex, divisor);
        result.numerator   = beatIndex / factor;
        result.denominator = divisor / factor;
    };

    if ( settings.objectPlacementSnapMode ==
         Config::ObjectPlacementSnapMode::CommonBeatDivisors ) {
        // 按分母升序遍历，固定等距时的选择顺序；未启用项不参与竞争。
        for ( int divisor = Config::COMMON_BEAT_DIVISOR_MIN;
              divisor <= Config::COMMON_BEAT_DIVISOR_MAX;
              ++divisor ) {
            if ( Config::isCommonBeatDivisorEnabled(
                     settings.commonBeatDivisorMask, divisor) ) {
                considerDivisor(divisor);
            }
        }
    } else {
        // 单分拍模式至少保留整拍，不让损坏的非正配置成为取模基数。
        considerDivisor(std::max(settings.beatDivisor, 1));
    }

    // 多分拍模式下没有启用项时保持空结果，不暗中补一个未启用的整拍方案。
    return result;
}

/// @brief 为指定相机与时间查询可用分拍吸附，并验证投影坐标可用。
/// @param rawTime 原始时间定位，单位为秒。
/// @param mouseY 用于有限值校验的鼠标纵坐标，不是吸附半径阈值。
/// @param camera 接收定位的画布或预览相机。
/// @param config 首段前网格、预览布局与分拍设置。
/// @param bpmEvents 已排序且指针有效的 BPM 列表。
/// @param timelineRegistry 提供已建立的 ScrollCache，不在此重建缓存。
/// @param animateTime 当前画布时间锚点，单位为秒。
/// @param cameras 已登记相机，用于预览与主画布高度换算。
/// @param fallbackBpm 当前 BPM 非法时使用的后备正值。
/// @return 第一个有效 BPM 段给出的吸附结果，缺少必要上下文则返回空结果。
/// @pre BPM 列表及 ScrollCache 与本次查询的时间线一致，调用期间不并发变更。
/// @note 本函数不补建首个 BPM；没有节奏上下文时，即使有 fallback 也不吸附。
/// @warning 交互更新路径会查询滚动映射，不得增加磁盘读取或阻塞等待。
SnapResult getSnapResult(
    double rawTime, float mouseY, const CameraInfo& camera,
    const Config::EditorConfig&                  config,
    const std::vector<const TimelineComponent*>& bpmEvents,
    entt::registry& timelineRegistry, double animateTime,
    const std::unordered_map<std::string, CameraInfo>& cameras,
    double                                             fallbackBpm)
{
    SnapResult result;

    auto* cache = timelineRegistry.ctx().find<System::ScrollCache>();
    // 只借用已准备好的滚动映射，输入查询不能把首次构建成本带入连续拖动。
    if ( !cache ) return result;

    if ( bpmEvents.empty() ) return result;

    /// @brief 首个 BPM 前是否沿用首个 BPM 向前反推分拍网格。
    const bool allowBeforeFirstTiming =
        config.visual.drawBeatLinesBeforeFirstTiming;
    if ( rawTime < bpmEvents[0]->m_timestamp && !allowBeforeFirstTiming )
        return result;

    // 画面上的纵坐标相对当前动画时间投影，而非逻辑播放时间。
    // 这样缓动期间的吸附有效性检查使用的仍是正在显示的视野基准。
    float  judgmentLineY = camera.viewportHeight * config.visual.judgeline_pos;
    double currentAbsY   = cache->getAbsY(animateTime);

    float renderScaleY = 1.0f;
    if ( camera.id == "Preview" || camera.id == "PreviewCanvas" ) {
        // 预览按主画布可绘制高度与显示范围压缩，没有主相机时退回自身高度。
        const auto* mainCamera = findMainCanvasCamera(cameras);
        float       mainViewportHeight =
            mainCamera ? mainCamera->viewportHeight : camera.viewportHeight;

        float mainEffectiveH =
            (config.visual.trackLayout.bottom - config.visual.trackLayout.top) *
            mainViewportHeight;
        float ty = config.visual.previewConfig.margin.top;
        float by =
            camera.viewportHeight - config.visual.previewConfig.margin.bottom;
        float previewDrawH = by - ty;

        // 主画布只取轨道上下边界之间的有效高度，不把工具栏留白算入压缩比例。
        // 预览边距已经是其自身坐标，不能再乘一次主画布高度。
        // 边距只缩小预览绘图区，areaRatio 决定它覆盖多少主画布高度。
        renderScaleY = previewDrawH /
                       (mainEffectiveH * config.visual.previewConfig.areaRatio);
    }

    for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
        const auto* currentBPM  = bpmEvents[i];
        double      bpmTime     = currentBPM->m_timestamp;
        double      bpmVal      = currentBPM->m_value;
        double      nextBpmTime = (i + 1 < bpmEvents.size())
                                      ? bpmEvents[i + 1]->m_timestamp
                                      : std::numeric_limits<double>::infinity();

        if ( rawTime < bpmTime && i > 0 ) continue;
        // 仅首段允许向前外推；其他段不得用自己的网格抢占更早段的查询时间。
        // 相等边界允许旧段产生被截到下一 BPM 的候选，候选本身按整拍报告。
        if ( rawTime > nextBpmTime ) continue;

        // 选择段落依据原始定位，不重新按候选时间二次寻找 BPM。
        // 因而旧段候选落到下一段起点时，由候选计算中的边界规则标成整拍。
        double bVal = bpmVal;
        // 此入口只处理非有限与非正 BPM，保留现有正值供候选计算使用。
        if ( !std::isfinite(bVal) || bVal <= 0.0 ) {
            bVal = fallbackBpm;
        }
        if ( !std::isfinite(bVal) || bVal <= 0.0 ) bVal = 120.0;
        // fallback 本身也可能不可用，最后一步保证传给候选算法的是有限正数。
        auto candidate = calculateObjectPlacementSnap(
            rawTime, bpmTime, nextBpmTime, bVal, config.settings);
        if ( !candidate.isSnapped ) continue;

        // SV 可使时间到纵坐标的映射非匀速，必须通过 ScrollCache 转换候选。
        // 不能以 BPM 拍长直接推算像素高度替代滚动积分后的绝对坐标。
        // 投影仅作有效性检查，不在多候选间比较鼠标的像素距离。
        double snapAbsY = cache->getAbsY(candidate.snappedTime);
        float  snapY    = judgmentLineY -
                      static_cast<float>(snapAbsY - currentAbsY) * renderScaleY;
        if ( !std::isfinite(snapY) || !std::isfinite(mouseY) ) continue;

        // mouseY 不参与候选距离竞争；这里只要求输入坐标和输出投影都可用。
        // 不再寻找屏幕上更近但属于另一时间段的交点。
        result = candidate;
        break;
    }

    return result;
}


/// @brief 将实际打击、预测打击和绑定声音预读游标同步到动画时间。
/// @param ctx 含事件表与当前显示时间的会话，允许事件表尚未重建。
/// @note lower_bound 保留恰好位于当前时刻的事件，避免定位后漏掉边界音符。
/// @note 只改变缓存下标，不清空音频设备中已排定的播放任务。
/// @warning 播放定位与事件更新路径；脏表会触发重建，清洁表只做二分查询。
void syncHitIndex(SessionContext& ctx)
{
    ensureHitEvents(ctx);
    // 定位基准是 animateTime，包含会话视觉偏移；不能改成设备采样时间。
    // 空事件表的查询结果为 end，距离为零，三个消费者均从空区间开始。
    auto it                 = std::lower_bound(ctx.hitEvents.begin(),
                               ctx.hitEvents.end(),
                               System::HitFXSystem::HitEvent{
                                   ctx.animateTime, ::MMM::NoteType::NOTE });
    ctx.nextHitIndex        = std::distance(ctx.hitEvents.begin(), it);
    ctx.nextPredictHitIndex = ctx.nextHitIndex;
    // 预测和预读各自随后向前推进，重新定位时必须先归到同一事件边界，
    // 否则向后 seek 后可能沿用旧的远端下标而漏掉需要重新排定的内容。
    ctx.nextBoundSoundPrefetchIndex = ctx.nextHitIndex;
}

/// @brief 仅在脏状态下重新收集 BPM 组件并按时间稳定排序。
/// @param ctx 提供时间线 Registry、借用指针缓存及其脏标记。
/// @note 缓存借用注册表组件地址，组件生命周期变化必须同步标记此缓存失效。
/// @pre 由会话串行访问，重建期间不可删除或搬移时间线组件。
/// @warning 逻辑更新中的条件重建入口；清洁时直接返回，禁止移除脏标记门控。
void ensureBpmEvents(SessionContext& ctx)
{
    if ( !ctx.isBpmEventsDirty ) return;

    ctx.bpmEvents.clear();
    // 重新收集地址而非仅重排旧指针，才能覆盖时间点的新增与删除。
    // SV、Jump 等效果不加入节奏列表，滚动积分由独立缓存负责。
    auto tlView = ctx.timelineRegistry.view<const TimelineComponent>();
    for ( auto entity : tlView ) {
        const auto& tl = tlView.get<const TimelineComponent>(entity);
        if ( tl.m_effect == ::MMM::TimingEffect::BPM ) {
            ctx.bpmEvents.push_back(&tl);
        }
    }
    // 同时间 BPM 保留本次收集顺序，不在这里另行定义覆盖优先级。
    std::stable_sort(
        ctx.bpmEvents.begin(),
        ctx.bpmEvents.end(),
        [](const TimelineComponent* a, const TimelineComponent* b) {
            return a->m_timestamp < b->m_timestamp;
        });
    // 排序完成才允许消费方复用缓存；清洁标志不表示时间点数值已经规范化。
    ctx.isBpmEventsDirty = false;
}

/// @brief 使打击事件缓存失效，将实际重建推迟到消费入口。
/// @param ctx 被编辑物件所属的会话，不影响其他谱面的事件缓存。
/// @note 多次标脏合并为一次后续重建，不计数也不触发即时音频加载。
/// @warning 编辑通知路径仅写会话标记，不在每次组件回调中全量扫描。
void markHitEventsDirty(SessionContext& ctx)
{
    ctx.isHitEventsDirty = true;
}

/// @brief 确保打击事件可读，已有清洁缓存时不做额外工作。
/// @param ctx 事件缓存及重建数据来源，返回后事件表按时间有序。
/// @pre 组件修改必须先标脏，本函数不会通过全表比对自动发现遗漏的通知。
/// @warning 逻辑消费路径可能因脏标记触发全量重建，避免无变更时反复标脏。
void ensureHitEvents(SessionContext& ctx)
{
    if ( ctx.isHitEventsDirty ) {
        rebuildHitEvents(ctx);
    }
}

/// @brief 从顶层音符重建打击事件，重新定位播放游标并扩展模型长度。
/// @param ctx 组件是事件真源，当前模型仅用于维护内容长度。
/// @note 草稿事件保留其标志，但草稿末尾不参与正式谱面长度计算。
/// @note 不负责保存模型、重建 BPM 列表或提交音频时间线描述。
/// @pre 组件访问由会话串行化；本函数不为独立调用方补充互斥保护。
/// @warning 编辑后的低频重建会完整遍历组件并排序事件，不可逐帧无条件调用。
void rebuildHitEvents(SessionContext& ctx)
{
    // 全量替换事件值，不保留对 NoteComponent 或父节点数组的借用引用。
    // 组件容器随后变化时，事件内容通过脏标记或差分机制更新。
    ctx.hitEvents.clear();
    ctx.nextHitIndex                = 0;
    ctx.nextBoundSoundPrefetchIndex = 0;

    double maxEndTime = 0.0;
    // 零基线不因负时间前导物件向前收缩，模型长度仍描述非负的尾部范围。

    auto view     = ctx.noteRegistry.view<NoteComponent>();
    using HitRole = System::HitFXSystem::HitEvent::Role;

    for ( auto entity : view ) {
        const auto& note = view.get<NoteComponent>(entity);
        // 子实体只是交互投影，事件统一由父节点数组展开，避免重复发声。
        if ( note.m_isSubNote ) continue;

        double noteEndTime = note.m_timestamp + note.m_duration;
        if ( !note.m_isDraft && noteEndTime > maxEndTime ) {
            maxEndTime = noteEndTime;
        }

        // 草稿仍能生成试听反馈，但它的节点也不能延长正式谱面的保存长度。
        if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
            // 父组件及节点时间已经是秒，不走模型加载时的毫秒换算。
            // 重建依据编辑后的节点数组，不能再回到尚未同步的 BeatMap
            // 读取旧节点。
            size_t subNoteCount = note.m_subNotes.size();
            for ( size_t i = 0; i < subNoteCount; ++i ) {
                const auto& sn   = note.m_subNotes[i];
                HitRole     role = HitRole::Internal;
                if ( i == 0 )
                    role = HitRole::Head;
                else if ( i == subNoteCount - 1 )
                    role = HitRole::Tail;

                // 单节点时头部优先；不能同时生成尾事件，否则会产生双重反馈。
                int span = 1;
                if ( sn.type == ::MMM::NoteType::FLICK ) {
                    span = std::abs(sn.dtrack) + 1;
                }

                auto sampleBinding = sn.sampleBinding;
                // 父采样仅作为头部后备，节点独立绑定优先于父绑定。
                if ( !sampleBinding && role == HitRole::Head ) {
                    sampleBinding = note.m_sampleBinding;
                }
                ctx.hitEvents.push_back({ sn.timestamp,
                                          sn.type,
                                          role,
                                          span,
                                          sn.trackIndex,
                                          sn.dtrack,
                                          sn.duration,
                                          true,
                                          std::move(sampleBinding),
                                          note.m_isDraft });

                // 折线总范围取所有节点终点，不假定最后一个连接节点结束得最晚。
                // 节点可能包含长条，因此仅以 timestamp
                // 最大值计算会截掉持续区间。
                double snEndTime = sn.timestamp + sn.duration;
                if ( !note.m_isDraft && snEndTime > maxEndTime ) {
                    maxEndTime = snEndTime;
                }
            }
        } else {
            int span = 1;
            if ( note.m_type == ::MMM::NoteType::FLICK ) {
                span = std::abs(note.m_dtrack) + 1;
            }

            ctx.hitEvents.push_back({ note.m_timestamp,
                                      note.m_type,
                                      HitRole::None,
                                      span,
                                      note.m_trackIndex,
                                      note.m_dtrack,
                                      note.m_duration,
                                      false,
                                      note.m_sampleBinding,
                                      note.m_isDraft });
        }
    }
    // 播放游标要求事件有序，必须先排序再重新按时间定位。
    std::sort(ctx.hitEvents.begin(), ctx.hitEvents.end());
    // syncHitIndex 会调用 ensureHitEvents；先清脏标记避免递归重建。
    ctx.isHitEventsDirty = false;
    // 游标定位在模型长度扩展之前完成；事件顺序决定下标，不依赖长度元数据。
    syncHitIndex(ctx);

    if ( ctx.currentBeatmap ) {
        // ECS 时间使用秒而模型长度使用毫秒；这里只扩展，不自动截短既有长度。
        double maxEndTimeMs = maxEndTime * 1000.0;
        if ( maxEndTimeMs > ctx.currentBeatmap->m_baseMapMetadata.map_length ) {
            // 删除末尾物件不在本入口缩短既有长度，避免把用户保留的尾部空白抹掉。
            ctx.currentBeatmap->m_baseMapMetadata.map_length = maxEndTimeMs;
        }
    }
}

}  // namespace MMM::Logic::SessionUtils
