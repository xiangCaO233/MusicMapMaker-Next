#include "config/CreatorIdentity.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/ActionController.h"
#include "logic/session/NoteAction.h"
#include "logic/session/NoteIdentity.h"
#include "logic/session/SampleAction.h"
#include "logic/session/SelectionState.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/TimelineAction.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/timing/BpmNormalization.h"
#include "runtime/AppThreadPool.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
#include <ice/thread/ThreadPool.hpp>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * @file ActionController_Editing.cpp
 * @brief 实现可撤销编辑命令、剪贴板节拍换算和协作权威数据替换。
 *
 * 本文件只处理会改变谱面领域数据的动作。瞬时鼠标、相机和播放状态由其他
 * Controller 管理；这里生成的修改必须进入 ActionStack，或者在权威远端替换
 * 场景中明确绕过本地撤销历史。
 *
 * Note、自动采样、Timing 与批注是彼此独立的数据域。每个处理器需要精确标记
 * 自己影响的 BeatmapMutationFlags，使协作层只编码必要类别。ECS 是编辑期间的
 * 权威表示，BeatMap 是保存和同步边界使用的领域表示，两者之间的转换必须保留
 * 稳定逻辑 ID、格式私有元数据和父子结构。
 *
 * 剪贴板同时保存秒偏移和复制时的连续 beat 位置。普通粘贴可以沿用秒偏移；
 * “按分拍粘贴”则在目标谱面的 BPM 分段时间线上重新求秒时间，确保跨不同 BPM
 * 谱面仍保持音乐节拍间距。所有换算都使用规范 BPM，拒绝 NaN 和负时间结果。
 *
 * Polyline 在领域层以内嵌子 Note 表示，在 ECS 中同时存在父实体与用于拾取的
 * 子实体。任何整体替换、删除、颜色更新和剪贴板操作都必须维护这一双重结构：
 * 父组件保存持久化子数组，子实体只作为可交互投影，不能被当作独立根物件保存。
 *
 * 协作权威替换优先复用稳定 ID 相同且内容未变化的 ECS 实体，以保留本地选择、
 * 悬停和画笔引用。已变化实体按差量重建；不再存在的实体连同折线子实体销毁。
 * 后台接收的旧 BeatMap 容器在替换后交给线程池释放，避免大型析构阻塞逻辑帧。
 *
 * 注释中的“低频路径”指用户显式编辑或远端同步触发，并不允许在每帧渲染链路
 * 调用文件系统、等待线程或完整排序。需要全量扫描的操作必须集中在一次动作内，
 * 不得按选中实体重复扫描 Registry。
 *
 * 动作快照使用值语义，避免 ActionStack 持有 Registry 组件地址或来源 BeatMap
 * 内部指针。唯一跨线程共享所有权是退役 BeatMap 容器，它只用于把大型析构移出
 * 逻辑线程，并在对应函数的 @warning 中明确说明生命周期原因。
 *
 * 时间单位边界必须显式：BeatMap 的 Note、Timing 与批注锚点通常以毫秒保存，
 * SessionContext 组件以秒编辑，ClipboardBeatTimeline 以连续拍数计算。每次跨层
 * 转换都在专用助手中完成，不能让 UI 显示单位渗入领域动作。
 *
 * 稳定 ID 用于协作身份而非 ECS 身份。entt::entity 只在当前 Registry 生命周期
 * 内有效；整体替换可以按 collaborationId 复用实体以保留交互，但保存和网络
 * 编码不能持久化实体数值。Polyline 子节点同样拥有独立协作 ID。
 *
 * 数据替换与用户编辑采用不同历史策略。用户编辑必须可撤销；远端权威状态不能
 * 被普通 Undo 恢复为服务端已淘汰内容。只有纯 Note/批注更新可保留能按稳定 ID
 * 重放的历史，跨 Timing、Metadata 或 Sample 替换会清除历史。
 *
 * 所有缓存更新遵循“能证明则增量，不能证明则标脏”的原则。对象差量携带前后
 * 组件视图时尝试增量维护排序和统计；完整替换或复杂结构变化统一清空索引并在
 * 后续系统重建，不能为了避免重建而保留可能过期的数据。
 *
 * 修改本文件时应逐项维持以下约束：
 *
 * - 新建 Note 必须验证根时间以及所有 Polyline 子节点时间。
 * - 新建 Sample 必须验证时间、音量和绝对轨道加法范围。
 * - 批量创建实体必须在任何结构性失败之前完成预演。
 * - 预分配实体只能由随后提交的 Action 安装组件。
 * - 空动作不得进入 ActionStack，以免无故改变保存点。
 * - 同一用户操作跨 Note 与 Sample 时必须使用复合动作。
 * - 删除 Polyline 根必须同时包含全部 ECS 子投影实体。
 * - 修改 Polyline 子内容必须同时更新父内嵌数组。
 * - 子实体命令必须先解析稳定父实体和有效子索引。
 * - Draft 不得被远端正式对象整体替换或差量删除。
 * - 隐藏或禁用类型不得通过复制粘贴绕过编辑权限。
 * - Sample BGM lane 必须相对玩家轨道数保存到剪贴板。
 * - beat 粘贴必须整批具备拍位缓存，否则统一按秒处理。
 * - Hold 和子 Hold 跨 BPM 时必须分别换算起点与终点。
 * - 粘贴任何 Note/Sample 失败时不得留下部分创建结果。
 * - Timeline 独立条目允许过滤，但合法项仍需形成单一动作。
 * - 所有 Timeline 值必须有限，BPM 还必须满足非负约束。
 * - Timing 移动后必须清除不再有效的 Malody beat 缓存。
 * - BPM 修改与配套保速 SCROLL 必须处于同一撤销边界。
 * - 替换 BPM 列表时至少需要一个有效来源 BPM。
 * - preference_bpm 必须与替换方向对应的 BPM 快照同步。
 * - 有限越界 BPM 使用最近合法边界，不回退任意默认值。
 * - 元数据替换不得接管目标项目的文件和媒体资源路径。
 * - 玩家轨道变化必须保持 Sample 的相对 BGM lane。
 * - Sample 轨道迁移必须在修改前以更宽整数验证上溢。
 * - 谱面级批注和 Note 内嵌注释必须报告正确变更类别。
 * - 批注目标必须使用 collaborationId，不能持久化 ECS 句柄。
 * - 新增批注前必须保证目标稳定 ID 已经写入对应组件。
 * - 更新既有谱面批注不能静默改变原作者或目标身份。
 * - 整体替换前后快照不得借用来源 BeatMap 内部地址。
 * - 权威远端替换不能作为普通本地动作压入撤销栈。
 * - 跨数据域权威替换必须清除无法安全重放的本地历史。
 * - 纯对象差量只允许在编码基线与身份集合已准备时使用。
 * - 完整对象替换应按稳定 ID 复用仍存在的 ECS 实体。
 * - 无稳定 ID 的兼容复用必须要求完整持久化内容相等。
 * - 一个现有实体最多只能匹配一个新权威目标。
 * - 根与子角色变化时必须新建实体，不能带着旧父关系复用。
 * - 对象删除后必须同步清理选择、悬停和拖动裸句柄。
 * - 保留框选几何时，对象集合变化必须使命中缓存失效。
 * - 完整替换应统一标脏排序、统计、密度和打击事件缓存。
 * - 差量缓存更新失败时必须安全退化为完整缓存重建。
 * - 退役领域容器只能在逻辑线程不再读取后移交线程池。
 * - 后台释放任务必须拥有容器生命周期，不能捕获裸指针。
 * - Action 的 execute、undo 与 redo 必须采用对称提交顺序。
 * - 任何状态栏反馈都不能代替领域脏标记或观察者类别。
 * - 实体扫描和排序只允许发生在用户编辑或权威同步低频路径。
 * - 新增控制器分支不得把文件 IO 或线程等待带入逻辑更新。
 */

namespace MMM::Logic
{

/// @brief 获取当前会话可用于轨道镜像的轨道数量。
/// @param ctx 当前会话上下文。
/// @return 优先采用领域谱面中的正玩家轨道数，否则回退运行时轨道数。
/// @details
/// 镜像属于持久化编辑，应以谱面元数据为基准；空会话或尚未同步的构造阶段才
/// 使用 SessionContext 的运行时值，避免零轨导致所有镜像静默失效。
int getMirrorTrackCount(const SessionContext& ctx)
{
    if ( ctx.currentBeatmap &&
         ctx.currentBeatmap->m_baseMapMetadata.track_count > 0 ) {
        return ctx.currentBeatmap->m_baseMapMetadata.track_count;
    }
    return ctx.trackCount;
}

/// @brief 对单个 NoteComponent 应用轨道镜像变换。
/// @param note 待原地镜像的根音符组件。
/// @param playerTrackCount 玩家轨道数量。
/// @param draftTrackCount 草稿轨道数量。
/// @details
/// 正轨按 `[0, count)` 反转，负草稿轨按 `[-count, -1]` 反转。Flick 的横向方向
/// 随轨道镜像取反；Polyline 的每个内嵌子物件采用相同轨道域和方向规则。
void mirrorNoteComponent(NoteComponent& note, int playerTrackCount,
                         int draftTrackCount)
{
    const auto trackCount = note.m_isDraft ? draftTrackCount : playerTrackCount;
    // 轨道域尚未建立时保持输入不变，调用方可把命令视为无操作。
    if ( trackCount <= 0 ) return;

    const auto mirrorTrack = [trackCount, isDraft = note.m_isDraft](int track) {
        // 草稿轨以 -1 为最靠近玩家区的第一轨，公式需额外减一保持闭区间对称。
        return isDraft ? -trackCount - 1 - track : trackCount - 1 - track;
    };
    note.m_trackIndex = mirrorTrack(note.m_trackIndex);
    if ( note.m_type == ::MMM::NoteType::FLICK ) {
        note.m_dtrack = -note.m_dtrack;
    }

    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        // 子物件轨道持久化在父组件数组中，不能只镜像用于拾取的 ECS 子实体。
        for ( auto& sub : note.m_subNotes ) {
            sub.trackIndex = mirrorTrack(sub.trackIndex);
            if ( sub.type == ::MMM::NoteType::FLICK ) {
                sub.dtrack = -sub.dtrack;
            }
        }
    }
}

/// @brief 判断新建物件是否允许落在谱面时间线上。
/// @param note 待创建物件。
/// @return 物件和所有折线子物件的时间戳均非负且有限时返回 true。
/// @details
/// 该校验只约束创建时间，不验证轨道和资源权限；那些约束由生成命令负责。根时间
/// 合法但任一 Polyline 子节点非法时仍整体拒绝，避免创建不可完整渲染的折线。
bool isPlaceableCreatedNote(const NoteComponent& note)
{
    // 创建结果必须落在歌曲原点之后；移动已有物件的负时间策略由对应动作决定。
    if ( !std::isfinite(note.m_timestamp) || note.m_timestamp < 0.0 ) {
        return false;
    }

    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        // 父时间合法仍不足以证明折线合法，所有内嵌节点都要独立验证。
        for ( const auto& subNote : note.m_subNotes ) {
            if ( !std::isfinite(subNote.timestamp) ||
                 subNote.timestamp < 0.0 ) {
                return false;
            }
        }
    }

    return true;
}

/// @brief 判断新建自动采样锚点是否允许落在谱面时间线上。
/// @param sample 待创建自动采样。
/// @return 锚点时间非负且有限、音量合法时返回 true；资源可为空以表示静音草稿。
/// @details
/// 轨道上溢在相对 BGM lane 换算处检查。这里保留空资源 ID，因为对象可在跨域
/// 拖动时作为尚未绑定音频的 Sample 草稿存在。
bool isPlaceableCreatedSample(const SampleComponent& sample)
{
    // 空资源 ID 是允许跨轨拖动形成的静音草稿，不在此作为非法引用拒绝。
    return std::isfinite(sample.m_timestamp) && sample.m_timestamp >= 0.0 &&
           std::isfinite(sample.m_volume);
}

/// @brief 移除 Malody timing 拍位缓存。
/// @param metadata 待修改的 Timing 元数据。
/// @details
/// `beat`
/// 是来源秒时间对应的派生字段，移动事件后继续保留会让再次导出使用旧拍位。 其他
/// Malody 属性不受影响，属性表为空时才删除整个来源域。
void clearMalodyTimingBeatMetadata(::MMM::TimingMetadata& metadata)
{
    // beat 是来源格式的派生定位缓存，时间被编辑后必须删除以便保存器重新生成。
    auto sourceIt =
        metadata.timing_properties.find(::MMM::TimingMetadataType::MALODY);
    if ( sourceIt == metadata.timing_properties.end() ) {
        return;
    }

    sourceIt->second.erase("beat");
    if ( sourceIt->second.empty() ) {
        // 属性域不再含其他 Malody 字段时一并移除空映射，保持序列化输出简洁。
        metadata.timing_properties.erase(sourceIt);
    }
}

/// @brief 复制粘贴分拍换算使用的 BPM 时间点。
struct ClipboardBeatTimelinePoint {
    double timestamp{ 0.0 };  ///< BPM 时间点，单位秒
    double bpm{ 120.0 };      ///< 当前段 BPM
    double beat{ 0.0 };       ///< 该时间点对应的连续拍数
};

/// @brief 复制粘贴分拍换算用的连续 BPM 时间线。
/// @details 每个点保存该 BPM 段起点的秒时间、段速率和累计连续拍数。
using ClipboardBeatTimeline = std::vector<ClipboardBeatTimelinePoint>;

/// @brief 获取复制粘贴分拍换算使用的默认 BPM。
/// @param ctx 当前会话上下文。
/// @return 有效首选 BPM，缺失时返回 120。
/// @details
/// 首选 BPM 只作为无红线或异常红线的回退速率，真实分段仍来自 bpmEvents。所有
/// 返回值都经过统一规范，调用方可安全用于乘除。
double getClipboardFallbackBpm(const SessionContext& ctx)
{
    if ( ctx.currentBeatmap ) {
        // 元数据首选 BPM 也必须规范化，不能让异常值进入除法或拍数积分。
        return ::MMM::normalizeBpmValue(
            ctx.currentBeatmap->m_baseMapMetadata.preference_bpm);
    }
    return ::MMM::DEFAULT_NORMALIZED_BPM;
}

/// @brief 按原值方向规整 BPM，保证分拍换算使用安全数值。
/// @param bpm 待规整 BPM。
/// @param fallbackBpm 原值为 NaN 时使用的回退 BPM。
/// @return 位于安全计算范围内的 BPM。
/// @details
/// 有限越界值夹到最接近原值的合法边界；非有限值使用调用方回退。该语义与编辑
/// 和保存层一致，避免剪贴板换算出现另一套 BPM 规则。
double sanitizeClipboardBpm(double bpm, double fallbackBpm)
{
    // 统一复用领域 BPM 边界语义：有限越界值夹到最近边界，NaN 使用回退值。
    return ::MMM::normalizeBpmValue(bpm, fallbackBpm);
}

/// @brief 从当前 BPM 缓存构建可双向换算的连续 beat 时间线。
/// @param ctx 当前会话上下文。
/// @param fallbackBpm BPM 无效时使用的默认 BPM。
/// @return 按时间排序的 BPM/beat 锚点列表。
/// @warning 低频编辑路径：复制或按分拍粘贴时调用，允许在 BPM 脏时重建缓存。
ClipboardBeatTimeline buildClipboardBeatTimeline(SessionContext& ctx,
                                                 double          fallbackBpm)
{
    // BPM 缓存可能因同批 Timing 动作变脏，构建前强制得到按时间排序的最新指针。
    SessionUtils::ensureBpmEvents(ctx);

    ClipboardBeatTimeline timeline;
    timeline.reserve(ctx.bpmEvents.size());
    for ( const auto* event : ctx.bpmEvents ) {
        // 空指针和非有限时间无法形成有序分段，跳过而不污染后续累计拍数。
        if ( !event || !std::isfinite(event->m_timestamp) ) continue;

        const double bpm = sanitizeClipboardBpm(event->m_value, fallbackBpm);
        if ( timeline.empty() ) {
            // 第一段定义连续拍零点；其时间可以不是歌曲零秒。
            timeline.push_back({ event->m_timestamp, bpm, 0.0 });
            continue;
        }

        const auto& previous = timeline.back();
        // 使用上一段 BPM 对两个事件之间的秒差积分，得到新段起点累计拍数。
        const double beat =
            previous.beat +
            (event->m_timestamp - previous.timestamp) * previous.bpm / 60.0;
        timeline.push_back({ event->m_timestamp, bpm, beat });
    }
    return timeline;
}

/// @brief 将秒时间转换为连续 beat 位置。
/// @param timeline 按时间排序的 BPM 分段。
/// @param timestamp 待转换的秒时间。
/// @param fallbackBpm 无分段时使用的 BPM。
/// @return 跨越全部 BPM 段后的连续拍数；非法时间返回零。
double clipboardTimeToBeat(const ClipboardBeatTimeline& timeline,
                           double timestamp, double fallbackBpm)
{
    if ( !std::isfinite(timestamp) ) return 0.0;

    const double bpm = sanitizeClipboardBpm(fallbackBpm, 120.0);
    if ( timeline.empty() ) {
        // 无红线时整张谱面按首选 BPM 视为单一线性分段。
        return timestamp * bpm / 60.0;
    }

    auto it = std::upper_bound(
        timeline.begin(),
        timeline.end(),
        timestamp,
        [](double value, const ClipboardBeatTimelinePoint& point) {
            return value < point.timestamp;
        });
    // upper_bound 找到首个更晚段；时间位于首段前时向前线性外推。
    const auto& point = it == timeline.begin() ? timeline.front() : *(it - 1);
    return point.beat + (timestamp - point.timestamp) * point.bpm / 60.0;
}

/// @brief 将连续 beat 位置转换为秒时间。
/// @param timeline 按累计 beat 排序的 BPM 分段。
/// @param beat 待转换的连续拍数。
/// @param fallbackBpm 无分段时使用的 BPM。
/// @return 对应秒时间；非法拍数返回零。
double clipboardBeatToTime(const ClipboardBeatTimeline& timeline, double beat,
                           double fallbackBpm)
{
    if ( !std::isfinite(beat) ) return 0.0;

    const double bpm = sanitizeClipboardBpm(fallbackBpm, 120.0);
    if ( timeline.empty() ) {
        // 与 timeToBeat 使用同一回退 BPM，保证无 Timing 谱面可逆。
        return beat * 60.0 / bpm;
    }

    auto it = std::upper_bound(
        timeline.begin(),
        timeline.end(),
        beat,
        [](double value, const ClipboardBeatTimelinePoint& point) {
            return value < point.beat;
        });
    // 连续拍数单调递增，可采用与时间查询对称的上界搜索定位分段。
    const auto& point = it == timeline.begin() ? timeline.front() : *(it - 1);
    return point.timestamp + (beat - point.beat) * 60.0 / point.bpm;
}

/// @brief 为剪贴板条目记录复制瞬间的 beat 位置。
/// @param item 待补充拍位锚点的音符剪贴板条目。
/// @param timeline 复制来源的连续 BPM 时间线。
/// @param fallbackBpm 无 BPM 事件时采用的首选值。
/// @details
/// 根物件保存起止拍，Polyline 还为每个内嵌子物件保存起止拍。持续时间不能仅按
/// 一个 BPM 比例换算，因为 Hold 可能跨越多个 BPM 分段。
void populateClipboardBeatPositions(ClipboardItem&               item,
                                    const ClipboardBeatTimeline& timeline,
                                    double                       fallbackBpm)
{
    item.startBeat =
        clipboardTimeToBeat(timeline, item.note.m_timestamp, fallbackBpm);
    item.endBeat = clipboardTimeToBeat(
        timeline, item.note.m_timestamp + item.note.m_duration, fallbackBpm);
    item.subStartBeats.clear();
    item.subEndBeats.clear();

    if ( item.note.m_type == ::MMM::NoteType::POLYLINE ) {
        // 两个数组始终与 m_subNotes 同长同序，粘贴时才能无歧义恢复每个节点。
        item.subStartBeats.reserve(item.note.m_subNotes.size());
        item.subEndBeats.reserve(item.note.m_subNotes.size());
        for ( const auto& sub : item.note.m_subNotes ) {
            item.subStartBeats.push_back(
                clipboardTimeToBeat(timeline, sub.timestamp, fallbackBpm));
            item.subEndBeats.push_back(clipboardTimeToBeat(
                timeline, sub.timestamp + sub.duration, fallbackBpm));
        }
    }

    item.hasBeatPositions = true;
    // 标志最后提交，避免中途扩容失败时留下看似完整的拍位缓存。
}

/// @brief 为自动采样剪贴板条目记录复制瞬间的 beat 锚点。
/// @param item 待写入拍位的自动采样条目。
/// @param timeline 复制来源的连续 BPM 时间线。
/// @param fallbackBpm BPM 缺失时的默认值。
void populateSampleClipboardBeatPosition(SampleClipboardItem&         item,
                                         const ClipboardBeatTimeline& timeline,
                                         double fallbackBpm)
{
    // 自动采样没有持续时间和内嵌节点，只需记录单一锚点拍位。
    item.startBeat =
        clipboardTimeToBeat(timeline, item.sample.m_timestamp, fallbackBpm);
    item.hasBeatPosition = true;
}

/// @brief 按 beat 偏移将剪贴板条目落到新的粘贴时间。
/// @param note 待原地定位的复制音符。
/// @param item 保存来源拍位的剪贴板条目。
/// @param timeline 粘贴目标谱面的 BPM 时间线。
/// @param fallbackBpm 目标无 BPM 事件时使用的值。
/// @param pasteBeat 用户选择的目标锚点拍位。
/// @param minBeat 本批剪贴板内容的最早拍位。
/// @details
/// 所有条目共享 `pasteBeat - minBeat` 偏移，保持批次相对拍距。起点和终点分别
/// 换算，才能正确处理跨 BPM 的持续时间；Polyline 子物件采用同一规则。
void applyBeatPastePosition(NoteComponent& note, const ClipboardItem& item,
                            const ClipboardBeatTimeline& timeline,
                            double fallbackBpm, double pasteBeat,
                            double minBeat)
{
    const double startBeat = pasteBeat + item.startBeat - minBeat;
    const double endBeat   = pasteBeat + item.endBeat - minBeat;
    note.m_timestamp = clipboardBeatToTime(timeline, startBeat, fallbackBpm);
    const double endTime = clipboardBeatToTime(timeline, endBeat, fallbackBpm);
    // 浮点边界或异常分段不能产生负 duration，最小夹到零。
    note.m_duration = std::max(0.0, endTime - note.m_timestamp);

    if ( note.m_type != ::MMM::NoteType::POLYLINE ||
         item.subStartBeats.size() != note.m_subNotes.size() ||
         item.subEndBeats.size() != note.m_subNotes.size() ) {
        // 旧剪贴板缺少完整子拍位缓存时只定位根物件，避免数组越界猜测节点。
        return;
    }

    for ( std::size_t i = 0; i < note.m_subNotes.size(); ++i ) {
        // 索引与复制时 m_subNotes 顺序一致，分别恢复每个节点的起止秒时间。
        auto&        sub          = note.m_subNotes[i];
        const double subStartBeat = pasteBeat + item.subStartBeats[i] - minBeat;
        const double subEndBeat   = pasteBeat + item.subEndBeats[i] - minBeat;
        sub.timestamp =
            clipboardBeatToTime(timeline, subStartBeat, fallbackBpm);
        const double subEndTime =
            clipboardBeatToTime(timeline, subEndBeat, fallbackBpm);
        sub.duration = std::max(0.0, subEndTime - sub.timestamp);
    }
}

/// @brief 将调色盘命令颜色转换为 NoteColorOverrides。
/// @param colors 按 NoteColorSlot 枚举顺序排列的颜色数组。
/// @return 所有颜色槽均显式赋值的覆写对象。
NoteColorOverrides makeNoteColorOverrides(
    const std::array<glm::vec4, NOTE_COLOR_SLOT_COUNT>& colors)
{
    NoteColorOverrides overrides;
    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        // 枚举和数组共享固定槽位顺序，由统一 setter 写入对应 optional 成员。
        auto slot = static_cast<NoteColorSlot>(i);
        setNoteColorOverride(overrides, slot, colors[i]);
    }
    return overrides;
}

/// @brief 更新整体替换涉及音符实体的基础辅助组件。
/// @param registry 目标 ECS 注册表。
/// @param entity 目标音符实体。
/// @param preserveInteraction 是否保留实体已有的本地交互状态。
/// @details
/// Transform 是纯派生投影，权威组件变化后必须重建。Interaction 只有在稳定实体
/// 被复用且允许保留时沿用，否则用默认组件清除旧选择与拖动标志。
void ensureReplacementNoteAuxiliaryComponents(entt::registry& registry,
                                              entt::entity    entity,
                                              bool preserveInteraction)
{
    // Transform 是替换后重新投影的派生缓存，始终用默认值重建。
    registry.emplace_or_replace<TransformComponent>(entity);
    if ( !preserveInteraction ||
         !registry.all_of<InteractionComponent>(entity) ) {
        // 仅复用未变化实体时保留选择/悬停；新建或变化实体从干净交互状态开始。
        registry.emplace_or_replace<InteractionComponent>(entity);
    }
}

/// @brief 标记整体替换后需要重建音符排序和统计缓存。
/// @param ctx 当前会话上下文。
/// @details
/// 清空拥有实体句柄的缓存，并设置相应 dirty 位。prune 缓存不能在集合已整体变化
/// 后继续增量裁剪，因此重置为 false，等待排序重建建立新基线。
void markReplacementNoteOrderDirty(SessionContext& ctx)
{
    // 排序、区间前缀、密度和批注渲染都依赖实体集合，统一失效防止局部漏刷。
    ctx.sortedNoteEntities.clear();
    ctx.sortedNoteMaxEndPrefix.clear();
    ctx.previewDensityObjectTimes.clear();
    ctx.isNoteOrderDirty             = true;
    ctx.isNotePruneDirty             = false;
    ctx.isNoteStatsDirty             = true;
    ctx.isPreviewDensityDirty        = true;
    ctx.isAnnotationRenderCacheDirty = true;
}

/// @brief 将折线子物件点击目标解析到父折线实体。
/// @param ctx 当前会话上下文。
/// @param entity 点击或命令给出的 Note ECS 实体。
/// @return 可持久化颜色覆写的根实体；目标无效时返回 entt::null。
entt::entity resolveNoteColorTargetEntity(SessionContext& ctx,
                                          entt::entity    entity)
{
    if ( entity == entt::null || !ctx.noteRegistry.valid(entity) ||
         !ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
        return entt::null;
    }

    const auto& note = ctx.noteRegistry.get<NoteComponent>(entity);
    if ( note.m_isSubNote && note.m_parentPolyline != entt::null &&
         ctx.noteRegistry.valid(note.m_parentPolyline) &&
         ctx.noteRegistry.all_of<NoteComponent>(note.m_parentPolyline) ) {
        // 子实体颜色不独立持久化，颜色操作统一作用于父折线组件。
        return note.m_parentPolyline;
    }
    return entity;
}

/// @brief 判断两个可选颜色是否完全相同。
/// @param lhs 左侧颜色。
/// @param rhs 右侧颜色。
/// @return optional 状态及 RGBA 四通道都相等时返回 true。
bool isSameOptionalColor(const std::optional<glm::vec4>& lhs,
                         const std::optional<glm::vec4>& rhs)
{
    if ( lhs.has_value() != rhs.has_value() ) return false;
    if ( !lhs.has_value() ) return true;
    return lhs->r == rhs->r && lhs->g == rhs->g && lhs->b == rhs->b &&
           lhs->a == rhs->a;
}

/// @brief 判断两个音符配色覆写缓存是否完全相同。
/// @param lhs 左侧全部颜色槽。
/// @param rhs 右侧全部颜色槽。
/// @return 每个可选槽位均相同时返回 true。
/// @details
/// 显式逐槽比较避免依赖结构填充或 optional 容器布局，并与持久化颜色槽集合保持
/// 一一对应。该判等只用于跳过无变化颜色动作。
bool isSameNoteColorOverrides(const NoteColorOverrides& lhs,
                              const NoteColorOverrides& rhs)
{
    return isSameOptionalColor(lhs.tap, rhs.tap) &&
           isSameOptionalColor(lhs.head, rhs.head) &&
           isSameOptionalColor(lhs.hold, rhs.hold) &&
           isSameOptionalColor(lhs.end, rhs.end) &&
           isSameOptionalColor(lhs.flickArrow, rhs.flickArrow) &&
           isSameOptionalColor(lhs.node, rhs.node);
}

/// @brief 判断两个可选命中采样绑定是否完全相同。
/// @param lhs 左侧绑定。
/// @param rhs 右侧绑定。
/// @return optional 状态、资源 ID 和音量均相同时返回 true。
bool isSameSampleBinding(const std::optional<::MMM::AudioSampleBinding>& lhs,
                         const std::optional<::MMM::AudioSampleBinding>& rhs)
{
    if ( lhs.has_value() != rhs.has_value() ) return false;
    return !lhs.has_value() ||
           (lhs->m_audioResourceId == rhs->m_audioResourceId &&
            lhs->m_volume == rhs->m_volume);
}

/// @brief 判断两个折线子物件组件是否完全相同。
/// @param lhs 左侧子物件。
/// @param rhs 右侧子物件。
/// @return 所有持久化字段及派生编辑覆写均相同时返回 true。
/// @details
/// 父实体句柄和 subIndex 不属于内嵌持久化子组件，因此不参与比较；几何、元数据、
/// 批注、采样绑定和颜色必须全部相同才能兼容复用。
bool isSameSubNoteComponent(const NoteComponent::SubNote& lhs,
                            const NoteComponent::SubNote& rhs)
{
    return lhs.type == rhs.type && lhs.timestamp == rhs.timestamp &&
           lhs.duration == rhs.duration && lhs.trackIndex == rhs.trackIndex &&
           lhs.dtrack == rhs.dtrack &&
           lhs.metadata.note_properties == rhs.metadata.note_properties &&
           lhs.annotation == rhs.annotation &&
           isSameSampleBinding(lhs.sampleBinding, rhs.sampleBinding) &&
           isSameNoteColorOverrides(lhs.customColors, rhs.customColors);
}

/// @brief 判断两个根音符组件是否可在权威同步后保留同一实体身份。
/// @param lhs 当前 ECS 根组件。
/// @param rhs 新权威状态根组件。
/// @return 全部持久化内容相同且两者均非子实体时返回 true。
/// @details
/// 稳定 ID 相同但内容变化的实体不能保留交互组件，因为选择中的旧几何可能与新
/// 内容不一致。Polyline 还要求内嵌子数组等长且逐项相同。
bool isSameRootNoteComponent(const NoteComponent& lhs, const NoteComponent& rhs)
{
    return !lhs.m_isSubNote && !rhs.m_isSubNote && lhs.m_type == rhs.m_type &&
           lhs.m_timestamp == rhs.m_timestamp &&
           lhs.m_duration == rhs.m_duration &&
           lhs.m_trackIndex == rhs.m_trackIndex &&
           lhs.m_dtrack == rhs.m_dtrack &&
           lhs.m_metadata.note_properties == rhs.m_metadata.note_properties &&
           isSameSampleBinding(lhs.m_sampleBinding, rhs.m_sampleBinding) &&
           isSameNoteColorOverrides(lhs.m_customColors, rhs.m_customColors) &&
           lhs.m_subNotes.size() == rhs.m_subNotes.size() &&
           std::equal(lhs.m_subNotes.begin(),
                      lhs.m_subNotes.end(),
                      rhs.m_subNotes.begin(),
                      isSameSubNoteComponent);
}

/// @brief 为待删除折线父实体批量追加其子物件删除条目。
/// @param ctx 当前会话上下文。
/// @param deletedEntities 已经加入删除操作的实体集合。
/// @param entries 待追加的批量 note action 条目。
/// @details
/// 删除父 Polyline 时，BatchNoteAction 还必须包含其 ECS 子实体，否则父组件虽然
/// 从领域模型消失，用于拾取和渲染的子实体仍会残留。实现先收集全部目标父实体，
/// 再对 Registry 做一次扫描，并避开调用方已经加入的重复条目。
/// @warning 逻辑热路径低频分支：删除命令执行时最多完整扫描一次 note ECS，禁止按
/// 父折线数量重复扫描。
void appendDeletedPolylineChildren(
    SessionContext&                         ctx,
    const std::unordered_set<entt::entity>& deletedEntities,
    std::vector<BatchNoteAction::Entry>&    entries)
{
    // 空删除集合无需构建父索引或扫描 Registry。
    if ( deletedEntities.empty() ) return;

    std::unordered_set<entt::entity> polylineParents;
    for ( auto entity : deletedEntities ) {
        // 命令队列中的实体可能在此前动作中失效，所有组件读取前重新验证。
        if ( !ctx.noteRegistry.valid(entity) ||
             !ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
            continue;
        }

        const auto& note = ctx.noteRegistry.get<NoteComponent>(entity);
        if ( note.m_type == ::MMM::NoteType::POLYLINE &&
             !note.m_subNotes.empty() ) {
            // 没有内嵌子数组的折线无需寻找投影子实体。
            polylineParents.insert(entity);
        }
    }
    if ( polylineParents.empty() ) return;

    std::unordered_set<entt::entity> existingEntries;
    // 调用方可能已因显式选择把某个子实体加入 action，先建立去重集合。
    existingEntries.reserve(entries.size());
    for ( const auto& entry : entries ) {
        if ( entry.entity != entt::null ) {
            existingEntries.insert(entry.entity);
        }
    }

    auto noteView = ctx.noteRegistry.view<NoteComponent>();
    // 单次全表扫描按 parentPolyline 关系定位所有目标子实体。
    for ( auto entity : noteView ) {
        const auto& note = noteView.get<NoteComponent>(entity);
        if ( note.m_isSubNote &&
             polylineParents.find(note.m_parentPolyline) !=
                 polylineParents.end() &&
             existingEntries.insert(entity).second ) {
            entries.push_back({ entity, note, std::nullopt });
        }
    }
}

/// @brief 收集当前 Timeline Registry 中的全部事件并按时间排序。
/// @param ctx 当前会话上下文。
/// @return 时间升序、同时间按特效枚举稳定排列的组件副本。
/// @warning 仅用于显式 Timeline 编辑和撤销动作，不得在每帧更新中调用。
std::vector<TimelineComponent> collectSortedTimelineComponents(
    SessionContext& ctx)
{
    std::vector<TimelineComponent> timelines;
    // Registry 迭代顺序不是持久化顺序，先完整复制再统一排序。
    auto view = ctx.timelineRegistry.view<TimelineComponent>();
    for ( auto entity : view ) {
        timelines.push_back(view.get<TimelineComponent>(entity));
    }

    std::stable_sort(
        timelines.begin(),
        timelines.end(),
        [](const auto& lhs, const auto& rhs) {
            // 一纳秒容差内视为同一时间点，再以类型得到确定输出顺序。
            if ( std::abs(lhs.m_timestamp - rhs.m_timestamp) > 1e-9 ) {
                return lhs.m_timestamp < rhs.m_timestamp;
            }
            return static_cast<int>(lhs.m_effect) <
                   static_cast<int>(rhs.m_effect);
        });
    return timelines;
}

/// @brief 将指定 Timeline 列表整体写回当前会话。
/// @param ctx 当前会话上下文。
/// @param timelines 已规范化并按顺序排列的目标列表。
/// @details
/// Timeline 实体没有跨整体替换保留的交互身份，因此先销毁旧实体再重建。完成后
/// 同时标记领域同步、BPM 指针缓存、坐标变换、统计和 ScrollCache，避免任一派生
/// 视图继续引用旧事件。
void replaceTimelineComponents(SessionContext&                       ctx,
                               const std::vector<TimelineComponent>& timelines)
{
    std::vector<entt::entity> entities;
    // 不能在 view 迭代期间直接销毁，先复制实体句柄避免迭代器失效。
    auto view = ctx.timelineRegistry.view<TimelineComponent>();
    for ( auto entity : view ) {
        entities.push_back(entity);
    }
    for ( auto entity : entities ) {
        ctx.timelineRegistry.destroy(entity);
    }

    for ( const auto& timeline : timelines ) {
        // 每个目标组件获得新实体，列表顺序由后续缓存而非 entt 实体值表达。
        auto entity = ctx.timelineRegistry.create();
        ctx.timelineRegistry.emplace<TimelineComponent>(entity, timeline);
    }

    ctx.m_needsTimingsSync = true;
    ctx.isBpmEventsDirty   = true;
    ctx.isTransformDirty   = true;
    ctx.isNoteStatsDirty   = true;
    if ( auto* cache =
             ctx.timelineRegistry.ctx().find<System::ScrollCache>() ) {
        // ScrollCache 可能尚未安装；存在时只标脏，重建留给统一时间线系统。
        cache->isDirty = true;
    }
}

/// @brief 判断 Timeline 数值是否满足可编辑域约束。
/// @param effect Timeline 类型。
/// @param value 待写入数值。
/// @return 数值有限，且 BPM 不小于零时返回 true。
bool isValidTimelineValue(::MMM::TimingEffect effect, double value)
{
    // 非 BPM 特效允许负值；BPM 的统一底线仅排除负数。
    return std::isfinite(value) &&
           (effect != ::MMM::TimingEffect::BPM || value >= 0.0);
}

/// @brief 归一化批量替换后的 Timeline 列表。
/// @param timelines 来自命令或权威谱面的未校验组件列表。
/// @return 删除非法项、稳定排序并合并同时间 BPM 后的列表。
/// @details
/// 非有限时间和值直接删除。BPM 同一时间只允许一项，排序后按输入稳定顺序用后项
/// 覆盖前项；其他 TimingEffect 即使同时间也保留，因为它们可能表达独立效果。
std::vector<TimelineComponent> normalizeReplacementTimelines(
    std::vector<TimelineComponent> timelines)
{
    // 先过滤再排序，比较器永远不会遇到破坏严格弱序的 NaN。
    std::erase_if(timelines, [](const auto& timeline) {
        // 批量替换同样经过统一数值约束，防止绕过普通编辑命令。
        return !std::isfinite(timeline.m_timestamp) ||
               !isValidTimelineValue(timeline.m_effect, timeline.m_value);
    });

    std::stable_sort(
        timelines.begin(),
        timelines.end(),
        [](const auto& lhs, const auto& rhs) {
            if ( std::abs(lhs.m_timestamp - rhs.m_timestamp) > 1e-9 ) {
                return lhs.m_timestamp < rhs.m_timestamp;
            }
            return static_cast<int>(lhs.m_effect) <
                   static_cast<int>(rhs.m_effect);
        });

    std::vector<TimelineComponent> normalized;
    normalized.reserve(timelines.size());
    for ( const auto& timeline : timelines ) {
        if ( timeline.m_effect == ::MMM::TimingEffect::BPM ) {
            // 只在已规范列表中寻找同时间 BPM，非 BPM 无需线性去重。
            auto duplicateIt = std::find_if(
                normalized.begin(),
                normalized.end(),
                [&](const auto& existing) {
                    return existing.m_effect == ::MMM::TimingEffect::BPM &&
                           std::abs(existing.m_timestamp -
                                    timeline.m_timestamp) < 1e-6;
                });
            if ( duplicateIt != normalized.end() ) {
                // 后出现的事件代表调用方最终值，同时保留排序后的目标位置。
                *duplicateIt = timeline;
                continue;
            }
        }
        normalized.push_back(timeline);
    }
    return normalized;
}

/// @brief 收集当前会话中可作为整体替换快照的物件组件。
/// @param ctx 当前会话上下文。
/// @return 非子物件的物件组件列表。
/// @details
/// 草稿和 Polyline 子实体不属于正式谱面整体替换快照。根组件复制后采用稳定顺序，
/// 便于撤销动作比较和协作编码得到确定结果，排序不改变 ECS 实体。
std::vector<NoteComponent> collectEditableNoteComponents(SessionContext& ctx)
{
    std::vector<NoteComponent> notes;
    auto                       view = ctx.noteRegistry.view<NoteComponent>();
    for ( auto entity : view ) {
        const auto& note = view.get<NoteComponent>(entity);
        if ( note.m_isSubNote || note.m_isDraft ) continue;
        // 根 Polyline 的内嵌子数组随父组件一起复制，无需收集投影子实体。
        notes.push_back(note);
    }

    std::stable_sort(
        notes.begin(), notes.end(), [](const auto& lhs, const auto& rhs) {
            if ( std::abs(lhs.m_timestamp - rhs.m_timestamp) > 1e-9 ) {
                return lhs.m_timestamp < rhs.m_timestamp;
            }
            if ( lhs.m_trackIndex != rhs.m_trackIndex ) {
                return lhs.m_trackIndex < rhs.m_trackIndex;
            }
            return static_cast<int>(lhs.m_type) < static_cast<int>(rhs.m_type);
        });
    return notes;
}

/// @brief 从谱面 Note 构建 ECS 音符组件。
/// @param note 来源谱面物件。
/// @return 对应的 ECS 组件。
/// @details
/// 领域层使用毫秒和无符号轨道，ECS 使用秒和编辑器整数轨道。类型专有字段从
/// Hold/Flick 派生对象读取；采样绑定、批注、协作 ID 与颜色覆写完整保留。
NoteComponent makeNoteComponentFromBeatmapNote(const ::MMM::Note& note)
{
    NoteComponent component;
    // 公共字段先复制，类型分支只补充 duration 或 dtrack。
    component.m_type            = note.m_type;
    component.m_timestamp       = note.m_timestamp / 1000.0;
    component.m_trackIndex      = static_cast<int>(note.m_track);
    component.m_metadata        = note.m_metadata;
    component.m_annotation      = note.m_annotation;
    component.m_sampleBinding   = note.getSampleBinding();
    component.m_collaborationId = note.m_collaborationId;

    if ( note.m_type == ::MMM::NoteType::HOLD ) {
        // 领域 Hold 持续时间同样由毫秒换算为秒。
        component.m_duration =
            static_cast<const ::MMM::Hold&>(note).m_duration / 1000.0;
    } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
        // Flick 横向跨度已经以轨道单位保存，不做单位换算。
        component.m_dtrack = static_cast<const ::MMM::Flick&>(note).m_dtrack;
    }

    loadNoteColorOverridesFromMetadata(component);
    // 颜色覆写持久化在格式元数据中，加载为 ECS 快速访问缓存。
    return component;
}

/// @brief 从谱面折线子物件构建 ECS 子物件组件。
/// @param note 来源谱面子物件。
/// @return 对应的 ECS 折线子物件。
/// @details
/// 返回值是父 NoteComponent 内嵌的 SubNote DTO，不创建 ECS 实体。父实体和子实体
/// 的关系由替换函数在掌握目标父句柄后建立。
NoteComponent::SubNote makeSubNoteComponentFromBeatmapNote(
    const ::MMM::Note& note)
{
    NoteComponent::SubNote subNote;
    // 与根组件采用相同单位和字段转换，保证往返不会改变节点语义。
    subNote.type            = note.m_type;
    subNote.timestamp       = note.m_timestamp / 1000.0;
    subNote.duration        = 0.0;
    subNote.trackIndex      = static_cast<int>(note.m_track);
    subNote.dtrack          = 0;
    subNote.metadata        = note.m_metadata;
    subNote.annotation      = note.m_annotation;
    subNote.sampleBinding   = note.getSampleBinding();
    subNote.collaborationId = note.m_collaborationId;

    if ( note.m_type == ::MMM::NoteType::HOLD ) {
        subNote.duration =
            static_cast<const ::MMM::Hold&>(note).m_duration / 1000.0;
    } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
        subNote.dtrack = static_cast<const ::MMM::Flick&>(note).m_dtrack;
    }

    loadNoteColorOverridesFromMetadata(subNote);
    return subNote;
}

/// @brief 收集谱面中已经被 Polyline 引用的子物件地址。
/// @param beatMap 来源谱面。
/// @return Polyline 子物件地址集合。
/// @details
/// 某些格式同时把子 Note 暴露在类型容器与 Polyline 引用中。地址集合用于识别
/// 这些别名，防止整体转换时又把同一子物件加入根物件列表。
std::unordered_set<const ::MMM::Note*> collectBeatmapPolylineSubNotePointers(
    const ::MMM::BeatMap& beatMap)
{
    std::unordered_set<const ::MMM::Note*> subNotePointers;
    for ( const auto& polyline : beatMap.m_noteData.polylines ) {
        // reference_wrapper 指向 m_allNotes
        // 中的稳定对象，函数期间地址可用作身份。
        for ( const auto& subNoteRef : polyline.m_subNotes ) {
            subNotePointers.insert(&subNoteRef.get());
        }
    }
    return subNotePointers;
}

/// @brief 从谱面数据构建可整体替换到当前会话的物件组件列表。
/// @param beatMap 来源谱面。
/// @return 非子物件的物件组件列表。
/// @details
/// 各类型容器可能包含 Polyline 已引用的子对象，先用地址集合排除别名。Polyline
/// 单独重建内嵌子数组，并以首节点修正父级时间和轨道锚点。最终稳定排序使完整
/// 替换与协作序列化不依赖来源容器排列。
std::vector<NoteComponent> makeNoteComponentsFromBeatMap(
    const ::MMM::BeatMap& beatMap)
{
    std::vector<NoteComponent> notes;
    notes.reserve(
        beatMap.m_noteData.notes.size() + beatMap.m_noteData.holds.size() +
        beatMap.m_noteData.flicks.size() + beatMap.m_noteData.polylines.size());

    const auto subNotePointers = collectBeatmapPolylineSubNotePointers(beatMap);
    // 普通 Note、Hold 和 Flick 只加入真正的根对象。
    for ( const auto& note : beatMap.m_noteData.notes ) {
        if ( note.m_isSubNote || subNotePointers.contains(&note) ) continue;
        notes.push_back(makeNoteComponentFromBeatmapNote(note));
    }
    for ( const auto& hold : beatMap.m_noteData.holds ) {
        if ( hold.m_isSubNote || subNotePointers.contains(&hold) ) continue;
        notes.push_back(makeNoteComponentFromBeatmapNote(hold));
    }
    for ( const auto& flick : beatMap.m_noteData.flicks ) {
        if ( flick.m_isSubNote || subNotePointers.contains(&flick) ) continue;
        notes.push_back(makeNoteComponentFromBeatmapNote(flick));
    }
    for ( const auto& polyline : beatMap.m_noteData.polylines ) {
        // Polyline 根的类型和子数组由领域引用关系重新构造。
        auto component   = makeNoteComponentFromBeatmapNote(polyline);
        component.m_type = ::MMM::NoteType::POLYLINE;
        component.m_subNotes.clear();
        component.m_subNotes.reserve(polyline.m_subNotes.size());
        for ( const auto& subNoteRef : polyline.m_subNotes ) {
            component.m_subNotes.push_back(
                makeSubNoteComponentFromBeatmapNote(subNoteRef.get()));
        }
        if ( !component.m_subNotes.empty() ) {
            // 父锚点始终跟随第一节点，供排序、投影和范围查询使用。
            component.m_timestamp  = component.m_subNotes.front().timestamp;
            component.m_trackIndex = component.m_subNotes.front().trackIndex;
        }
        notes.push_back(std::move(component));
    }

    std::stable_sort(
        notes.begin(), notes.end(), [](const auto& lhs, const auto& rhs) {
            if ( std::abs(lhs.m_timestamp - rhs.m_timestamp) > 1e-9 ) {
                return lhs.m_timestamp < rhs.m_timestamp;
            }
            if ( lhs.m_trackIndex != rhs.m_trackIndex ) {
                return lhs.m_trackIndex < rhs.m_trackIndex;
            }
            return static_cast<int>(lhs.m_type) < static_cast<int>(rhs.m_type);
        });
    return notes;
}

/// @brief 从完整谱面中只构建指定稳定标识对应的根物件组件。
/// @param beatMap 后台已经物化的最新可见谱面。
/// @param identities 本轮发生增删改的根物件稳定标识。
/// @return 最新谱面中仍存在的目标根物件组件。
/// @details
/// 该函数用于后台差量应用，只复制身份集合命中的根对象。集合中已被删除的身份
/// 不会出现在结果中，调用方随后据此销毁现有 ECS 实体。Polyline 仍需携带完整
/// 子数组，因为一个节点变化会改变父折线的整体几何。
std::vector<NoteComponent> makeChangedNoteComponentsFromBeatMap(
    const ::MMM::BeatMap&                  beatMap,
    const std::unordered_set<std::string>& identities)
{
    std::vector<NoteComponent> notes;
    notes.reserve(identities.size());
    const auto appendRoot = [&](const ::MMM::Note& note) {
        // 子对象即使拥有 ID 也由父 Polyline 差量统一承载，不作为根匹配。
        if ( note.m_isSubNote ||
             !identities.contains(note.m_collaborationId) ) {
            return;
        }
        notes.push_back(makeNoteComponentFromBeatmapNote(note));
    };
    for ( const auto& note : beatMap.m_noteData.notes ) appendRoot(note);
    for ( const auto& hold : beatMap.m_noteData.holds ) appendRoot(hold);
    for ( const auto& flick : beatMap.m_noteData.flicks ) appendRoot(flick);
    for ( const auto& polyline : beatMap.m_noteData.polylines ) {
        // 根 ID 不在目标集合时完全跳过，避免复制未变化的大折线。
        if ( !identities.contains(polyline.m_collaborationId) ) continue;
        auto component   = makeNoteComponentFromBeatmapNote(polyline);
        component.m_type = ::MMM::NoteType::POLYLINE;
        component.m_subNotes.reserve(polyline.m_subNotes.size());
        for ( const auto& subNote : polyline.m_subNotes ) {
            component.m_subNotes.push_back(
                makeSubNoteComponentFromBeatmapNote(subNote.get()));
        }
        if ( !component.m_subNotes.empty() ) {
            component.m_timestamp  = component.m_subNotes.front().timestamp;
            component.m_trackIndex = component.m_subNotes.front().trackIndex;
        }
        notes.push_back(std::move(component));
    }
    return notes;
}

/// @brief 从谱面 Timing 构建可替换到当前会话的 Timeline 组件。
/// @param beatMap 来源谱面。
/// @return Timeline 组件列表。
/// @details
/// 时间从领域毫秒换算为 ECS 秒。BPM 使用统一边界规范，格式私有 TimingMetadata
/// 原样保留；最后复用批量归一化规则处理排序、非法值和同时间重复 BPM。
std::vector<TimelineComponent> makeTimelineComponentsFromBeatMap(
    const ::MMM::BeatMap& beatMap)
{
    std::vector<TimelineComponent> timelines;
    timelines.reserve(beatMap.m_timings.size());
    for ( const auto& timing : beatMap.m_timings ) {
        // 每个领域 Timing 转成值组件，替换阶段不借用源对象地址。
        TimelineComponent timeline;
        timeline.m_timestamp = timing.m_timestamp / 1000.0;
        timeline.m_effect    = timing.m_timingEffect;
        timeline.m_value     = timing.m_timingEffectParameter;
        if ( timeline.m_effect == ::MMM::TimingEffect::BPM ) {
            // timing.m_bpm 是异常值回退参考，有限越界值仍夹到最近合法边界。
            timeline.m_value =
                ::MMM::normalizeBpmValue(timeline.m_value, timing.m_bpm);
        }
        timeline.m_metadata = timing.m_metadata;
        timelines.push_back(std::move(timeline));
    }
    return normalizeReplacementTimelines(std::move(timelines));
}

/// @brief 按协作逻辑标识合并权威物件，并保留仍存在物件的 ECS 实体。
/// @param ctx 当前会话上下文。
/// @param notes 替换后的非子物件组件列表。
/// @param preserveInteraction 是否保留仍存在实体的本地交互状态。
/// @details
/// 完整替换先为现有正式 Note 建立稳定 ID 索引，草稿不参与远端替换。目标根与
/// 子物件优先按 collaborationId 复用实体；旧数据没有 ID 时，根物件可以用完整
/// 内容相等进行作为兼容匹配，并继承之前生成的稳定 ID。
///
/// retained 集合保证一个现有实体最多匹配一个目标组件。目标不存在时创建实体；
/// 旧实体未被保留时销毁。Transform 总是重建，Interaction 只在调用方允许且实体
/// 确实复用时保留，从而兼顾权威一致性与本地选择连续性。
///
/// Polyline 的每个内嵌子组件需要单独建立 ECS 子实体，并记录父句柄与子索引。
/// 完成集合替换后，选择、悬停、框选、拖动、画笔和橡皮状态按 preserveInteraction
/// 清理，最后统一标记 Note 同步及全部依赖排序、统计、打击事件的缓存。
///
/// preserveInteraction 只保留“同一实体仍被权威状态复用”的交互组件。内容变化
/// 但 ID 相同的实体仍会覆盖 NoteComponent；调用方通过同步边界保证不存在会把
/// 旧几何提交回去的活动手势。未复用实体从默认 InteractionComponent 开始。
///
/// legacy 根匹配只在目标没有 collaborationId 时启用，并要求所有持久化字段及
/// Polyline 子数组完全一致。它用于给旧数据继承已有 ID，不会把两个内容不同的
/// 无 ID 对象仅按时间或轨道错误合并。
///
/// 本地 Draft 被完整排除：不进入 existing 索引、不参与 retained 差集，也不会
/// 被远端正式谱面替换清除。选择整理中同样特别保留有效 Draft 实体。
/// @warning 低频权威同步路径：会完整扫描一次音符 Registry 并整理交互缓存，
/// 禁止从每帧更新路径调用。
void replaceNoteComponents(SessionContext&                   ctx,
                           const std::vector<NoteComponent>& notes,
                           bool preserveInteraction)
{
    if ( !preserveInteraction ) {
        // 不保留交互时先通过统一索引助手清除正式 Note 选择状态。
        clearChartObjectSelectionIndex(ctx, ChartObjectKind::PlayerNote);
    }

    struct ExistingNote {
        /// @brief 同步前的实体。
        entt::entity entity{ entt::null };
        /// @brief 同步前的组件快照。
        NoteComponent component;
    };

    std::vector<ExistingNote> existing;
    const auto view = ctx.noteRegistry.view<const NoteComponent>();
    // 一次扫描同时保存旧组件快照和稳定 ID 到数组索引的映射。
    existing.reserve(view.size());
    std::unordered_map<std::string, std::size_t> identityIndex;
    identityIndex.reserve(view.size());
    for ( const auto entity : view ) {
        const auto& component = view.get<const NoteComponent>(entity);
        // 草稿属于本地工作区，不允许权威正式谱面替换删除或复用。
        if ( component.m_isDraft ) continue;
        existing.push_back({ entity, component });
        const auto& identity = existing.back().component.m_collaborationId;
        if ( !identity.empty() ) {
            // 重复稳定 ID 保留首次出现项，后续目标仍会因 retained
            // 保护不重复复用。
            identityIndex.try_emplace(identity, existing.size() - 1U);
        }
    }

    std::unordered_set<entt::entity> retained;
    retained.reserve(existing.size());
    const auto findByIdentity = [&](std::string_view identity,
                                    bool expectedSubNote) -> entt::entity {
        // 空 ID 交给旧格式内容匹配；稳定 ID 还必须匹配根/子角色。
        if ( identity.empty() ) return entt::null;
        const auto found = identityIndex.find(std::string(identity));
        if ( found == identityIndex.end() ) return entt::null;
        const auto& candidate = existing[found->second];
        if ( candidate.component.m_isSubNote != expectedSubNote ||
             retained.contains(candidate.entity) ) {
            // 一个实体不能同时表示两个目标，角色变化也必须新建以清理父子关系。
            return entt::null;
        }
        return candidate.entity;
    };
    const auto findLegacyRoot = [&](const NoteComponent& desired) {
        // 旧谱面没有协作 ID 时只允许完整内容相等的根实体兼容复用。
        const auto found = std::find_if(
            existing.begin(), existing.end(), [&](const ExistingNote& entry) {
                return !entry.component.m_isSubNote &&
                       !retained.contains(entry.entity) &&
                       isSameRootNoteComponent(entry.component, desired);
            });
        return found == existing.end() ? entt::null : found->entity;
    };

    for ( const auto& note : notes ) {
        // applied 是目标值副本，兼容继承 ID 不会回写调用方输入列表。
        NoteComponent applied = note;
        auto          entity = findByIdentity(applied.m_collaborationId, false);
        if ( entity == entt::null && applied.m_collaborationId.empty() ) {
            entity = findLegacyRoot(applied);
        }
        const bool retainedExistingEntity = entity != entt::null;
        if ( entity == entt::null ) {
            // 未匹配目标创建干净实体，辅助组件将在写入 Note 后补齐。
            entity = ctx.noteRegistry.create();
        } else if ( applied.m_collaborationId.empty() ) {
            // 内容相等的旧格式目标继承现有根 ID，保持后续增量同步身份稳定。
            const auto& previous = ctx.noteRegistry.get<NoteComponent>(entity);
            applied.m_collaborationId = previous.m_collaborationId;
            if ( applied.m_subNotes.size() == previous.m_subNotes.size() ) {
                // 子数组等长时按结构位置继承缺失的节点 ID。
                for ( std::size_t index = 0; index < applied.m_subNotes.size();
                      ++index ) {
                    if ( applied.m_subNotes[index].collaborationId.empty() ) {
                        applied.m_subNotes[index].collaborationId =
                            previous.m_subNotes[index].collaborationId;
                    }
                }
            }
        }
        retained.insert(entity);
        ctx.noteRegistry.emplace_or_replace<NoteComponent>(entity, applied);
        // 先写目标组件再配置派生组件，后者可依据是否复用决定保留交互。
        ensureReplacementNoteAuxiliaryComponents(
            ctx.noteRegistry,
            entity,
            preserveInteraction && retainedExistingEntity);

        if ( applied.m_type != ::MMM::NoteType::POLYLINE ) continue;

        // 父组件内嵌数组是持久化权威值，下面为每项建立对应拾取实体。
        for ( std::size_t index = 0; index < applied.m_subNotes.size();
              ++index ) {
            const auto&   sub = applied.m_subNotes[index];
            NoteComponent subComponent;
            // 子实体复制完整节点字段，并显式记录父实体与数组位置。
            subComponent.m_type            = sub.type;
            subComponent.m_timestamp       = sub.timestamp;
            subComponent.m_duration        = sub.duration;
            subComponent.m_trackIndex      = sub.trackIndex;
            subComponent.m_dtrack          = sub.dtrack;
            subComponent.m_isSubNote       = true;
            subComponent.m_parentPolyline  = entity;
            subComponent.m_subIndex        = static_cast<int>(index);
            subComponent.m_metadata        = sub.metadata;
            subComponent.m_annotation      = sub.annotation;
            subComponent.m_sampleBinding   = sub.sampleBinding;
            subComponent.m_customColors    = sub.customColors;
            subComponent.m_collaborationId = sub.collaborationId;

            auto subEntity =
                findByIdentity(subComponent.m_collaborationId, true);
            const bool retainedExistingSubEntity = subEntity != entt::null;
            if ( subEntity == entt::null ) {
                // 新节点没有可复用身份时创建独立投影实体。
                subEntity = ctx.noteRegistry.create();
            }
            retained.insert(subEntity);
            ctx.noteRegistry.emplace_or_replace<NoteComponent>(subEntity,
                                                               subComponent);
            ensureReplacementNoteAuxiliaryComponents(
                ctx.noteRegistry,
                subEntity,
                preserveInteraction && retainedExistingSubEntity);
        }
    }

    for ( const auto& entry : existing ) {
        // 目标列表未保留的正式根或子实体代表权威删除。
        if ( !retained.contains(entry.entity) &&
             ctx.noteRegistry.valid(entry.entity) ) {
            ctx.noteRegistry.destroy(entry.entity);
        }
    }

    if ( preserveInteraction ) {
        // 选择集合只保留仍存在、组件仍标记选中的正式实体；本地草稿永远保留。
        std::erase_if(ctx.selectedNoteEntities, [&](entt::entity entity) {
            if ( ctx.noteRegistry.valid(entity) &&
                 ctx.noteRegistry.all_of<NoteComponent>(entity) &&
                 ctx.noteRegistry.get<const NoteComponent>(entity).m_isDraft ) {
                return false;
            }
            return !retained.contains(entity) ||
                   !ctx.noteRegistry.valid(entity) ||
                   !ctx.noteRegistry.all_of<InteractionComponent>(entity) ||
                   !ctx.noteRegistry.get<const InteractionComponent>(entity)
                        .isSelected;
        });
        if ( ctx.hoveredObjectKind == ChartObjectKind::PlayerNote &&
             (ctx.hoveredEntity == entt::null ||
              !retained.contains(ctx.hoveredEntity) ||
              !ctx.noteRegistry.valid(ctx.hoveredEntity)) ) {
            // 已删除悬停实体不能跨帧保留裸 entt 句柄。
            ctx.hoveredEntity     = entt::null;
            ctx.hoveredObjectKind = ChartObjectKind::PlayerNote;
            ctx.hoveredPart       = static_cast<std::int32_t>(HoverPart::None);
            ctx.hoveredSubIndex   = -1;
        }
        if ( !ctx.marqueeBoxes.empty() ) {
            // 框选几何不变但目标集合改变，需要下一帧重新求命中集合。
            ctx.isMarqueeSelectionDirty = true;
        }
    } else {
        // 不保留交互时清空所有正式 Note 的全局悬停与框选会话状态。
        ctx.hoveredEntity       = entt::null;
        ctx.hoveredObjectKind   = ChartObjectKind::PlayerNote;
        ctx.hoveredPart         = static_cast<std::int32_t>(HoverPart::None);
        ctx.hoveredSubIndex     = -1;
        ctx.isSelecting         = false;
        ctx.hasMarqueeSelection = false;
        ctx.isMarqueeSelectionDirty = false;
        ctx.marqueeBoxes.clear();
    }
    ctx.draggedEntity = entt::null;
    // 权威集合变化会让拖动初始组件和固定实体失效，无条件终止正式 Note 拖动。
    ctx.draggedObjectKind = ChartObjectKind::PlayerNote;
    ctx.draggedPart       = HoverPart::None;
    ctx.draggedSubIndex   = -1;
    ctx.dragInitialNote.reset();
    ctx.dragInitialSample.reset();
    ctx.dragRenderPinnedEntities.clear();
    ctx.isDragging = false;
    if ( ctx.brushState.isActive && !ctx.brushState.createsAudioSample ) {
        // Note 画笔依赖旧对象集合；自动采样画笔属于独立数据域，可继续保留。
        ctx.brushState.isActive = false;
        ctx.brushState.polylineSegments.clear();
        ctx.brushState.activeAudioResourceId.clear();
        ctx.brushState.activeSampleBinding.reset();
        ctx.brushState.holdStartTime = -1.0;
        ctx.brushState.duration      = 0.0;
        ctx.brushState.dtrack        = 0;
    }
    if ( ctx.eraserState.isActive &&
         ctx.eraserState.targetObjectKind == ChartObjectKind::PlayerNote ) {
        // 只取消作用于被替换 Note 域的橡皮，不影响 Sample 域手势。
        ctx.eraserState.isActive = false;
        ctx.eraserState.targetEntities.clear();
    }
    ctx.m_needsNotesSync = true;
    // 完整替换无法经济地维护所有增量缓存，统一标脏并在后续系统重建。
    SessionUtils::markHitEventsDirty(ctx);
    markReplacementNoteOrderDirty(ctx);
}

/// @brief 按后台差量只更新指定稳定标识对应的 ECS 物件。
/// @param ctx 当前会话上下文。
/// @param source 后台已经物化的最新可见谱面。
/// @param changedIdentities 本轮增删改的根物件稳定标识。
/// @details
/// 差量路径只读取 changedIdentities 对应的正式根物件及其 Polyline 子实体。现有
/// 目标先建立 ID 索引和 before 快照，新目标写入后记录 after；未保留目标销毁。
/// 这些前后视图用于 SessionUtils 增量更新排序、统计和打击事件缓存。
///
/// 若增量缓存助手发现不支持的结构变化，会返回 false，调用方退化为完整缓存
/// 失效。无论采用哪种缓存路径，ECS 领域结果一致，草稿和未命中的正式物件不变。
///
/// changedIdentities 表示根对象身份。Polyline 任一子节点变化都应由上游把父根 ID
/// 放入集合，本函数再完整物化该父的全部子数组。这样缓存和几何不会出现只有一个
/// 节点来自新修订、其余节点仍来自旧修订的混合状态。
///
/// cacheMutations 对已有目标记录 before，对保留或新建目标记录 after。删除只有
/// before，创建只有 after，修改两者都有。转换为 NoteCacheMutationView 后，指针
/// 只在同步调用期间借用 optional 内值，不会逃逸本函数。
///
/// 差量路径不结束未命中对象上的选择或手势，只清理实际删除实体的选择与悬停。
/// 有框选几何时标脏命中结果，使下一帧把新对象集合纳入选择计算。
/// @warning 远端物件提交路径：会扫描 Registry 建立目标实体索引，但只复制、
/// 创建或销毁实际变化的物件，禁止从每帧更新路径调用。
void applyIncrementalNoteComponents(
    SessionContext& ctx, const ::MMM::BeatMap& source,
    const std::vector<std::string>& changedIdentities)
{
    std::unordered_set<std::string> identities(changedIdentities.begin(),
                                               changedIdentities.end());
    // 去重后的空集合是无操作，不扫描 Registry。
    if ( identities.empty() ) return;

    struct ExistingNote {
        /// @brief 同步前的实体。
        entt::entity entity{ entt::null };
        /// @brief 同步前的目标组件快照。
        NoteComponent component;
    };

    struct OwnedCacheMutation {
        /// @brief 差量对应的实体。
        entt::entity entity{ entt::null };
        /// @brief 应用远端差量前的组件。
        std::optional<NoteComponent> before;
        /// @brief 应用远端差量后的组件。
        std::optional<NoteComponent> after;
    };

    std::vector<ExistingNote>                     existing;
    std::unordered_map<std::string, entt::entity> existingByIdentity;
    std::unordered_set<entt::entity>              targetRoots;
    const auto view = ctx.noteRegistry.view<const NoteComponent>();
    // 第一遍收集命中的根实体，并记录父集合供第二遍定位其子实体。
    for ( const auto entity : view ) {
        const auto& note = view.get<const NoteComponent>(entity);
        if ( note.m_isDraft || note.m_isSubNote ||
             !identities.contains(note.m_collaborationId) ) {
            continue;
        }
        existing.push_back({ entity, note });
        targetRoots.insert(entity);
        if ( !note.m_collaborationId.empty() ) {
            existingByIdentity.try_emplace(note.m_collaborationId, entity);
        }
    }
    for ( const auto entity : view ) {
        // 第二遍只收集目标根当前拥有的子实体，不触及其他 Polyline。
        const auto& note = view.get<const NoteComponent>(entity);
        if ( note.m_isDraft || !note.m_isSubNote ||
             !targetRoots.contains(note.m_parentPolyline) ) {
            continue;
        }
        existing.push_back({ entity, note });
        if ( !note.m_collaborationId.empty() ) {
            existingByIdentity.try_emplace(note.m_collaborationId, entity);
        }
    }

    std::vector<OwnedCacheMutation>               cacheMutations;
    std::unordered_map<entt::entity, std::size_t> cacheMutationByEntity;
    cacheMutations.reserve(existing.size() + identities.size());
    cacheMutationByEntity.reserve(existing.size() + identities.size());
    for ( const auto& entry : existing ) {
        // 所有旧目标先记录 before；之后保留项补 after，删除项保持空 after。
        cacheMutationByEntity.emplace(entry.entity, cacheMutations.size());
        cacheMutations.push_back({
            .entity = entry.entity,
            .before = entry.component,
        });
    }
    const auto recordAfter = [&](entt::entity         entity,
                                 const NoteComponent& component) {
        // 复用实体更新既有记录，新建实体追加只有 after 的缓存变化。
        const auto found = cacheMutationByEntity.find(entity);
        if ( found != cacheMutationByEntity.end() ) {
            cacheMutations[found->second].after = component;
            return;
        }
        cacheMutationByEntity.emplace(entity, cacheMutations.size());
        cacheMutations.push_back({
            .entity = entity,
            .after  = component,
        });
    };

    std::unordered_set<entt::entity> retained;
    retained.reserve(existing.size() + identities.size());
    const auto acquireEntity = [&](std::string_view identity,
                                   bool expectedSubNote) -> entt::entity {
        // 差量协议要求稳定 ID；空 ID 不进行内容猜测，直接创建新实体。
        if ( identity.empty() ) return entt::null;
        const auto found = existingByIdentity.find(std::string(identity));
        if ( found == existingByIdentity.end() ||
             retained.contains(found->second) ||
             !ctx.noteRegistry.valid(found->second) ) {
            return entt::null;
        }
        const auto& current =
            ctx.noteRegistry.get<const NoteComponent>(found->second);
        // 根/子角色不一致时拒绝复用，避免遗留 parentPolyline 关系。
        return current.m_isSubNote == expectedSubNote ? found->second
                                                      : entt::null;
    };

    const auto desiredNotes =
        makeChangedNoteComponentsFromBeatMap(source, identities);
    // 结果不含已删除身份；这些实体会在写入目标后由 retained 差集销毁。
    for ( const auto& desired : desiredNotes ) {
        auto       root = acquireEntity(desired.m_collaborationId, false);
        const bool retainedRoot = root != entt::null;
        if ( root == entt::null ) root = ctx.noteRegistry.create();
        // 根实体写入后立即登记 after，使后续缓存更新拥有完整组件生命周期。
        retained.insert(root);
        ctx.noteRegistry.emplace_or_replace<NoteComponent>(root, desired);
        recordAfter(root, desired);
        ensureReplacementNoteAuxiliaryComponents(
            ctx.noteRegistry, root, retainedRoot);

        if ( desired.m_type != ::MMM::NoteType::POLYLINE ) continue;
        // Polyline 差量以父为最小单位，所有新节点随父完整物化。
        for ( std::size_t index = 0; index < desired.m_subNotes.size();
              ++index ) {
            const auto&   sub = desired.m_subNotes[index];
            NoteComponent child;
            // 子组件显式绑定本轮实际根句柄，不能沿用后台 BeatMap 的地址关系。
            child.m_type            = sub.type;
            child.m_timestamp       = sub.timestamp;
            child.m_duration        = sub.duration;
            child.m_trackIndex      = sub.trackIndex;
            child.m_dtrack          = sub.dtrack;
            child.m_isSubNote       = true;
            child.m_parentPolyline  = root;
            child.m_subIndex        = static_cast<int>(index);
            child.m_metadata        = sub.metadata;
            child.m_annotation      = sub.annotation;
            child.m_sampleBinding   = sub.sampleBinding;
            child.m_customColors    = sub.customColors;
            child.m_collaborationId = sub.collaborationId;

            auto childEntity = acquireEntity(child.m_collaborationId, true);
            const bool retainedChild = childEntity != entt::null;
            if ( childEntity == entt::null ) {
                childEntity = ctx.noteRegistry.create();
            }
            retained.insert(childEntity);
            ctx.noteRegistry.emplace_or_replace<NoteComponent>(childEntity,
                                                               child);
            recordAfter(childEntity, child);
            ensureReplacementNoteAuxiliaryComponents(
                ctx.noteRegistry, childEntity, retainedChild);
        }
    }

    std::unordered_set<entt::entity> removed;
    for ( const auto& entry : existing ) {
        // 旧目标中未被复用的实体包括权威删除和结构变化后被替换的节点。
        if ( retained.contains(entry.entity) ||
             !ctx.noteRegistry.valid(entry.entity) ) {
            continue;
        }
        removed.insert(entry.entity);
        ctx.noteRegistry.destroy(entry.entity);
    }
    std::erase_if(ctx.selectedNoteEntities, [&](entt::entity entity) {
        // 只从选择索引移除已销毁实体，保留未变化目标和本地草稿选择。
        return removed.contains(entity);
    });
    if ( removed.contains(ctx.hoveredEntity) ) {
        // 悬停句柄被删除时同步重置类型和部件身份。
        ctx.hoveredEntity     = entt::null;
        ctx.hoveredObjectKind = ChartObjectKind::PlayerNote;
        ctx.hoveredPart       = static_cast<std::int32_t>(HoverPart::None);
        ctx.hoveredSubIndex   = -1;
    }
    if ( !ctx.marqueeBoxes.empty() ) ctx.isMarqueeSelectionDirty = true;

    ctx.m_needsNotesSync = true;
    // 将拥有 optional 快照转换为只在调用期间有效的轻量观察视图。
    std::vector<SessionUtils::NoteCacheMutationView> cacheMutationViews;
    cacheMutationViews.reserve(cacheMutations.size());
    for ( const auto& mutation : cacheMutations ) {
        cacheMutationViews.push_back({
            .entity = mutation.entity,
            .before = mutation.before ? &*mutation.before : nullptr,
            .after  = mutation.after ? &*mutation.after : nullptr,
        });
    }
    if ( !SessionUtils::applyNoteCacheMutationsIncrementally(
             ctx, cacheMutationViews) ) {
        // 复杂结构或缓存前置条件不满足时退化完整重建，优先保证一致性。
        SessionUtils::markHitEventsDirty(ctx);
        markReplacementNoteOrderDirty(ctx);
    }
}

/// @brief 在线程池释放已经被最新权威状态换出的旧领域物件容器。
/// @param retiredBeatmap 已不再由逻辑线程读取、内部持有旧物件容器的谱面。
/// @warning 联机物件提交路径每次调用一次；这里保留 shared_ptr 是为了保证旧
/// 容器跨线程释放期间的生命周期。若直接在逻辑线程析构，大谱面的逐元素释放
/// 仍会造成可见掉帧，目前没有不转移所有权且能安全后台释放的替代方案。
void retireBeatmapObjectStorage(std::shared_ptr<::MMM::BeatMap> retiredBeatmap)
{
    // 空容器无需投递任务，避免线程池调度开销。
    if ( !retiredBeatmap ) return;
    auto release = [beatmap = std::move(retiredBeatmap)]() mutable {
        // 先清空所有权容器，再重置类型索引；lambda 结束后释放 BeatMap 本身。
        beatmap->m_allNotes.clear();
        beatmap->m_noteData = {};
    };
    auto* threadPool = Runtime::AppThreadPool::instance().get();
    if ( threadPool ) {
        // 共享指针移动进任务，保证旧对象直到后台析构完成都保持有效。
        static_cast<void>(threadPool->enqueue(std::move(release)));
    } else {
        // 应用关闭阶段线程池可能已销毁，只能同步释放以避免泄漏。
        release();
    }
}

/// @brief 收集当前会话中全部自动采样组件。
/// @param ctx 当前会话上下文。
/// @return 按时间和轨道稳定排序的自动采样快照。
/// @details
/// Registry 迭代顺序不稳定，值副本按时间、绝对轨道和资源 ID 排序，使撤销快照
/// 与协作比较具有确定顺序。函数不修改组件或选择状态。
std::vector<SampleComponent> collectSampleComponents(SessionContext& ctx)
{
    std::vector<SampleComponent> samples;
    const auto view = ctx.sampleRegistry.view<const SampleComponent>();
    samples.reserve(view.size());
    for ( const auto entity : view ) {
        // 自动采样没有父子投影实体，视图中每项都属于可持久化根对象。
        samples.push_back(view.get<const SampleComponent>(entity));
    }
    std::stable_sort(
        samples.begin(), samples.end(), [](const auto& lhs, const auto& rhs) {
            if ( lhs.m_timestamp != rhs.m_timestamp ) {
                return lhs.m_timestamp < rhs.m_timestamp;
            }
            if ( lhs.m_track != rhs.m_track ) return lhs.m_track < rhs.m_track;
            return lhs.m_audioResourceId < rhs.m_audioResourceId;
        });
    return samples;
}

/// @brief 从谱面领域对象构建自动采样组件快照。
/// @param beatMap 来源谱面。
/// @return 可直接替换到 ECS 的自动采样组件。
/// @details
/// AudioSample 到 SampleComponent 的单位、资源和格式元数据转换由类型工厂统一
/// 维护。结果保持领域数组顺序，整体替换函数负责创建 ECS 实体。
std::vector<SampleComponent> makeSampleComponentsFromBeatMap(
    const ::MMM::BeatMap& beatMap)
{
    std::vector<SampleComponent> samples;
    samples.reserve(beatMap.m_audioSamples.size());
    for ( const auto& sample : beatMap.m_audioSamples ) {
        samples.push_back(SampleComponent::fromAudioSample(sample));
    }
    return samples;
}

/// @brief 整体重建当前会话的自动采样 ECS。
/// @param ctx 当前会话上下文。
/// @param samples 替换后的自动采样组件列表。
/// @details
/// Sample 没有稳定交互身份复用需求，整体替换直接清空 Registry 并重建组件。
/// 所有选择、悬停、拖动和依赖音频描述符的缓存随之失效；领域同步标记保证之后
/// 保存时将新 ECS 数据写回 BeatMap。
void replaceSampleComponents(SessionContext&                     ctx,
                             const std::vector<SampleComponent>& samples)
{
    // 先从统一选择索引移除 Sample，避免 clear 后保留失效实体句柄。
    clearChartObjectSelectionIndex(ctx, ChartObjectKind::AudioSample);
    ctx.sampleRegistry.clear();
    for ( const auto& sample : samples ) {
        // 每个值组件配套干净 Interaction，权威替换不继承本地 Sample 手势。
        const auto entity = ctx.sampleRegistry.create();
        ctx.sampleRegistry.emplace<SampleComponent>(entity, sample);
        ctx.sampleRegistry.emplace<InteractionComponent>(entity);
    }
    ctx.hoveredEntity = entt::null;
    // Sample Registry 重建后，全局悬停与拖动句柄均不再有效。
    ctx.draggedEntity = entt::null;
    ctx.dragInitialSample.reset();
    ctx.isSampleOrderDirty = true;
    // 排序、批注、密度和音频播放描述符都直接依赖 Sample 集合。
    ctx.isSamplePruneDirty               = false;
    ctx.isAnnotationRenderCacheDirty     = true;
    ctx.m_needsSamplesSync               = true;
    ctx.isPreviewDensityDirty            = true;
    ctx.isAudioTimelineDescriptorDirty   = true;
    ctx.isAudioTimelineActivationPending = true;
}

/// @brief 谱面元数据替换快照。
/// @details 基础元数据和格式扩展属性作为一个可撤销值对象共同保存。
struct BeatmapMetadataSnapshot {
    /// @brief 基础谱面元数据。
    ::MMM::BaseMapMeta baseMeta;

    /// @brief 扩展谱面元数据。
    ::MMM::MapMetadata mapMetadata;
};

/// @brief 从当前谱面构建元数据快照。
/// @param beatMap 来源谱面。
/// @return 当前元数据快照。
/// @details 快照按值复制，不借用
/// BeatMap；动作撤销期间即使谱面对象更换也不悬空。
BeatmapMetadataSnapshot makeMetadataSnapshot(const ::MMM::BeatMap& beatMap)
{
    return BeatmapMetadataSnapshot{
        .baseMeta    = beatMap.m_baseMapMetadata,
        .mapMetadata = beatMap.m_metadata,
    };
}

/// @brief 构建元数据替换后的快照，保留当前谱面的文件、资源路径及背景语义。
/// @param current 当前谱面。
/// @param source 来源谱面。
/// @return 替换后的元数据快照。
/// @details
/// “替换元数据”导入歌曲、作者、难度和格式属性，但当前项目的文件身份、音频、
/// 背景和持久 BGM 轨道属于目标环境，必须保留。OSU 扩展字段同步改写成保留值，
/// 防止之后打开元数据编辑器又从旧私有属性反向覆盖基础字段。
BeatmapMetadataSnapshot makeReplacementMetadataSnapshot(
    const ::MMM::BeatMap& current, const ::MMM::BeatMap& source)
{
    auto base = source.m_baseMapMetadata;
    // map_path 决定当前会话保存目标，不能被来源谱面的机器路径接管。
    base.map_path = current.m_baseMapMetadata.map_path;
    // main_audio_path 是旧单音轨字段，统一清空并保留当前 song_file_hint。
    base.main_audio_path.clear();
    base.song_file_hint  = current.m_baseMapMetadata.song_file_hint;
    base.bgm_track_count = current.m_baseMapMetadata.bgm_track_count;
    base.main_cover_path = current.m_baseMapMetadata.main_cover_path;
    // 背景路径、类型、视频起点和偏移必须作为同一语义组保留。
    base.cover_path      = current.m_baseMapMetadata.cover_path;
    base.cover_type      = current.m_baseMapMetadata.cover_type;
    base.video_starttime = current.m_baseMapMetadata.video_starttime;
    base.bgxoffset       = current.m_baseMapMetadata.bgxoffset;
    base.bgyoffset       = current.m_baseMapMetadata.bgyoffset;

    auto mapMetadata = source.m_metadata;
    // 其他来源格式私有属性保留，只有 OSU 中重复表达的资源字段需要校正。
    if ( auto osuIt =
             mapMetadata.map_properties.find(::MMM::MapMetadataType::OSU);
         osuIt != mapMetadata.map_properties.end() ) {
        // 原始 OSU 属性也必须与保留的资源一致，避免元数据编辑器反向覆盖。
        osuIt->second["General::AudioFilename"] =
            Config::pathToUtf8(base.song_file_hint);
        osuIt->second["Events::background"] =
            base.cover_type == ::MMM::CoverType::VIDEO
                ? fmt::format("Video,{},\"{}\"",
                              base.video_starttime,
                              Config::pathToUtf8(base.main_cover_path))
                : fmt::format("0,0,\"{}\",{},{}",
                              Config::pathToUtf8(base.main_cover_path),
                              base.bgxoffset,
                              base.bgyoffset);
    }
    return BeatmapMetadataSnapshot{
        .baseMeta    = std::move(base),
        .mapMetadata = std::move(mapMetadata),
    };
}

/// @brief 将元数据快照应用到当前谱面。
/// @param ctx 当前会话上下文。
/// @param snapshot 元数据快照。
/// @details
/// 快照写入后同步 SessionContext 的运行时轨道数，并再次写回夹取后的合法值。
/// 轨道和首选 BPM 可能改变投影与统计，因此相关派生缓存统一标脏。
void applyMetadataSnapshot(SessionContext&                ctx,
                           const BeatmapMetadataSnapshot& snapshot)
{
    // 空会话没有元数据承载对象，动作保持无操作。
    if ( !ctx.currentBeatmap ) return;

    ctx.currentBeatmap->m_baseMapMetadata = snapshot.baseMeta;
    ctx.currentBeatmap->m_metadata        = snapshot.mapMetadata;
    ctx.trackCount = std::max(1, snapshot.baseMeta.track_count);
    // 玩家轨至少一轨，BGM 持久轨允许为零；运行时与领域值必须保持一致。
    ctx.bgmTrackCount = std::max(0, snapshot.baseMeta.bgm_track_count);
    ctx.currentBeatmap->m_baseMapMetadata.track_count     = ctx.trackCount;
    ctx.currentBeatmap->m_baseMapMetadata.bgm_track_count = ctx.bgmTrackCount;
    ctx.isTransformDirty                                  = true;
    ctx.isNoteStatsDirty                                  = true;
}

/// @brief 从其他谱面替换当前谱面部分数据的可撤销动作。
/// @details
/// 动作一次保存所有被选择数据域的 before/after 快照。execute、undo 和 redo
/// 通过同一 apply 顺序写入，保证对象、Timing、自动采样、批注和元数据的组合
/// 替换始终是 ActionStack 中的单个原子事务。
///
/// 动作不自行判断快照内容是否相等，创建者根据命令类别决定替换范围。这样可以
/// 保留“来源明确要求覆盖但当前值恰好相同”的事务语义，同时 mutationFlags 只按
/// 实际选择的数据域报告。
///
/// preserveInteraction 是执行策略而非数据快照的一部分，正向、撤销和重做都采用
/// 相同策略。用于权威替换时动作通常直接执行而不入栈；用于本地替换时一致策略
/// 避免 Undo 额外清除用户仍然有效的选择。
class ReplaceBeatmapDataAction : public IEditorAction
{
public:
    /// @brief 构造数据替换动作。
    /// @param replaceObjects 是否替换玩家物件。
    /// @param replaceTimelines 是否替换 Timing。
    /// @param replaceMetadata 是否替换谱面元数据。
    /// @param replaceAnnotations 是否按稳定标识替换玩家物件注释。
    /// @param preserveInteraction 是否保留仍存在物件的本地交互状态。
    /// @param beforeNotes 替换前的玩家物件快照。
    /// @param afterNotes 替换后的玩家物件快照。
    /// @param beforeTimelines 替换前的 Timeline 快照。
    /// @param afterTimelines 替换后的 Timeline 快照。
    /// @param beforeMetadata 替换前的元数据快照。
    /// @param afterMetadata 替换后的元数据快照。
    /// @param sampleTrackChanges 玩家轨道数变化对应的自动采样轨道迁移表。
    /// @param beforePreferenceBpm 替换前的首选 BPM。
    /// @param afterPreferenceBpm 替换后的首选 BPM。
    /// @details
    /// 未启用的数据域仍可传入空快照，但不会在 apply 中访问。sampleTrackChanges
    /// 单独保存元数据改键引起的 Sample 绝对轨道变化，使撤销可恢复原相对 BGM
    /// 位置而不重新计算当前 Registry。
    ReplaceBeatmapDataAction(
        bool replaceObjects, bool replaceTimelines, bool replaceMetadata,
        bool replaceAudioSamples, bool replaceAnnotations,
        bool preserveInteraction, std::vector<NoteComponent> beforeNotes,
        std::vector<NoteComponent>                       afterNotes,
        std::vector<TimelineComponent>                   beforeTimelines,
        std::vector<TimelineComponent>                   afterTimelines,
        std::vector<SampleComponent>                     beforeSamples,
        std::vector<SampleComponent>                     afterSamples,
        std::vector<::MMM::BeatmapAnnotation>            beforeAnnotations,
        std::vector<::MMM::BeatmapAnnotation>            afterAnnotations,
        BeatmapMetadataSnapshot                          beforeMetadata,
        BeatmapMetadataSnapshot                          afterMetadata,
        std::vector<TrackCountAction::SampleTrackChange> sampleTrackChanges,
        double beforePreferenceBpm, double afterPreferenceBpm)
        : m_replaceObjects(replaceObjects)
        , m_replaceTimelines(replaceTimelines)
        , m_replaceMetadata(replaceMetadata)
        , m_replaceAudioSamples(replaceAudioSamples)
        , m_replaceAnnotations(replaceAnnotations)
        , m_preserveInteraction(preserveInteraction)
        , m_beforeNotes(std::move(beforeNotes))
        , m_afterNotes(std::move(afterNotes))
        , m_beforeTimelines(std::move(beforeTimelines))
        , m_afterTimelines(std::move(afterTimelines))
        , m_beforeSamples(std::move(beforeSamples))
        , m_afterSamples(std::move(afterSamples))
        , m_beforeAnnotations(std::move(beforeAnnotations))
        , m_afterAnnotations(std::move(afterAnnotations))
        , m_beforeMetadata(std::move(beforeMetadata))
        , m_afterMetadata(std::move(afterMetadata))
        , m_sampleTrackChanges(std::move(sampleTrackChanges))
        , m_beforePreferenceBpm(beforePreferenceBpm)
        , m_afterPreferenceBpm(afterPreferenceBpm)
    {
    }

    /// @brief 执行替换。
    /// @param ctx 当前会话上下文。
    /// @note 正向应用全部 after 快照。
    void execute(SessionContext& ctx) override { apply(ctx, true); }

    /// @brief 撤销替换。
    /// @param ctx 当前会话上下文。
    /// @note 反向应用全部 before 快照。
    void undo(SessionContext& ctx) override { apply(ctx, false); }

    /// @brief 重做替换。
    /// @param ctx 当前会话上下文。
    /// @note 与首次执行共享正向路径，避免两套提交顺序漂移。
    void redo(SessionContext& ctx) override { execute(ctx); }

    /// @brief 获取动作名称。
    /// @return ActionStack 与 UI 展示使用的稳定英文名称。
    std::string getName() const override { return "Replace Beatmap Data"; }

    /// @brief 返回本次替换实际覆盖的全部谱面数据类别。
    /// @return 按构造选项及采样轨道迁移组合的变更位。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override
    {
        auto flags = ::MMM::BeatmapMutationFlags::None;
        // 每个独立领域映射到一个观察者类别，组合动作按位合并。
        if ( m_replaceObjects ) flags |= ::MMM::BeatmapMutationFlags::Objects;
        if ( m_replaceTimelines ) {
            flags |= ::MMM::BeatmapMutationFlags::Timelines;
        }
        if ( m_replaceAudioSamples || !m_sampleTrackChanges.empty() ) {
            // 即使未整体替换 Sample，改键迁移绝对轨道也属于 AudioSamples 变化。
            flags |= ::MMM::BeatmapMutationFlags::AudioSamples;
        }
        if ( m_replaceMetadata ) {
            flags |= ::MMM::BeatmapMutationFlags::Metadata;
        }
        if ( m_replaceAnnotations ) {
            flags |= ::MMM::BeatmapMutationFlags::Annotations;
        }
        return flags;
    }

private:
    /// @brief 应用指定方向的替换快照。
    /// @param ctx 当前会话上下文。
    /// @param forward 是否应用替换后的快照。
    /// @details
    /// 应用顺序先重建对象和 Timing，再处理 Sample 与批注，最后写元数据和轨道
    /// 迁移。Timing 替换但元数据未替换时单独维护 preference_bpm，保持其派生值
    /// 与红线一致。
    void apply(SessionContext& ctx, bool forward)
    {
        if ( m_replaceObjects ) {
            // 完整对象替换可按动作构造策略保留稳定实体交互状态。
            replaceNoteComponents(ctx,
                                  forward ? m_afterNotes : m_beforeNotes,
                                  m_preserveInteraction);
        }
        if ( m_replaceTimelines ) {
            // Timeline 重建会标记全部 BPM 与滚动派生缓存。
            replaceTimelineComponents(
                ctx, forward ? m_afterTimelines : m_beforeTimelines);
            if ( ctx.currentBeatmap && !m_replaceMetadata ) {
                // 元数据未整体替换时仍需恢复对应方向的首选 BPM。
                ctx.currentBeatmap->m_baseMapMetadata.preference_bpm =
                    forward ? m_afterPreferenceBpm : m_beforePreferenceBpm;
            }
        }
        if ( m_replaceAudioSamples ) {
            // 自动采样整体替换拥有独立 Registry 生命周期。
            replaceSampleComponents(ctx,
                                    forward ? m_afterSamples : m_beforeSamples);
        }
        if ( m_replaceAnnotations && ctx.currentBeatmap ) {
            // 谱面级批注直接存于 BeatMap，替换后只需刷新批注渲染缓存。
            ctx.currentBeatmap->m_annotations =
                forward ? m_afterAnnotations : m_beforeAnnotations;
            ctx.isAnnotationRenderCacheDirty = true;
        }
        if ( m_replaceMetadata ) {
            // 元数据先决定新玩家轨道数，随后按预先验证的表迁移 Sample
            // 绝对轨道。
            applyMetadataSnapshot(ctx,
                                  forward ? m_afterMetadata : m_beforeMetadata);
            for ( const auto& change : m_sampleTrackChanges ) {
                // 动作执行时实体可能因其他替换失效，跳过缺失项而不访问悬空句柄。
                if ( !ctx.sampleRegistry.valid(change.entity) ||
                     !ctx.sampleRegistry.all_of<SampleComponent>(
                         change.entity) ) {
                    continue;
                }
                ctx.sampleRegistry.get<SampleComponent>(change.entity).m_track =
                    forward ? change.afterTrack : change.beforeTrack;
            }
            ctx.m_needsSamplesSync = true;
            // 轨道迁移直接修改 Sample ECS，保存前必须同步领域数组。
        }
    }

    /// @brief 是否替换物件数据。
    /// @note 控制 beforeNotes/afterNotes 是否应用。
    bool m_replaceObjects{ false };

    /// @brief 是否替换时间线数据。
    /// @note 启用时同时维护对应方向的 preference_bpm。
    bool m_replaceTimelines{ false };

    /// @brief 是否替换谱面元数据。
    /// @note 可附带自动采样绝对轨道迁移。
    bool m_replaceMetadata{ false };

    /// @brief 是否替换自动采样对象。
    /// @note 与仅迁移现有 Sample 轨道是两个独立选项。
    bool m_replaceAudioSamples{ false };

    /// @brief 是否按稳定标识替换玩家物件注释。
    /// @note 此字段实际控制谱面级批注列表快照。
    bool m_replaceAnnotations{ false };

    /// @brief 是否保留稳定身份仍存在物件的本地交互状态。
    bool m_preserveInteraction{ false };

    /// @brief 替换前物件组件。
    /// @note 仅保存正式根对象，Polyline 子节点内嵌于父组件。
    std::vector<NoteComponent> m_beforeNotes;

    /// @brief 替换后物件组件。
    /// @note 来源 BeatMap 已转换成 ECS 秒单位。
    std::vector<NoteComponent> m_afterNotes;

    /// @brief 替换前 Timeline 组件。
    /// @note 列表已经稳定排序并规范化。
    std::vector<TimelineComponent> m_beforeTimelines;

    /// @brief 替换后 Timeline 组件。
    /// @note 同时间重复 BPM 已按规则合并。
    std::vector<TimelineComponent> m_afterTimelines;

    /// @brief 替换前自动采样组件。
    /// @note 值快照不持有 Registry 实体身份。
    std::vector<SampleComponent> m_beforeSamples;

    /// @brief 替换后自动采样组件。
    /// @note apply 时为每项创建新的 Sample 实体。
    std::vector<SampleComponent> m_afterSamples;

    /// @brief 替换前批注。
    /// @note 保存完整稳定 ID、目标、作者和正文。
    std::vector<::MMM::BeatmapAnnotation> m_beforeAnnotations;

    /// @brief 替换后批注。
    /// @note 与 Note 内嵌注释属于不同领域集合。
    std::vector<::MMM::BeatmapAnnotation> m_afterAnnotations;

    /// @brief 替换前元数据。
    /// @note 用于撤销恢复基础字段和格式扩展属性。
    BeatmapMetadataSnapshot m_beforeMetadata;

    /// @brief 替换后元数据。
    /// @note 已保留当前项目文件和资源路径语义。
    BeatmapMetadataSnapshot m_afterMetadata;

    /// @brief 玩家轨道数变化时自动采样绝对轨道的双向迁移表。
    /// @note 每项以当前 Sample ECS 实体为动作内身份。
    std::vector<TrackCountAction::SampleTrackChange> m_sampleTrackChanges;

    /// @brief 替换前首选 BPM。
    double m_beforePreferenceBpm{ 120.0 };

    /// @brief 替换后首选 BPM。
    double m_afterPreferenceBpm{ 120.0 };
};

/// @brief 按稳定 ID 增删改单条谱面批注的可撤销动作。
/// @details
/// before/after 的 optional 组合表达新增、修改和删除。动作始终通过稳定字符串 ID
/// 定位当前列表，不依赖 vector 下标，因此其他不相关批注插入后仍可正确撤销。
///
/// 四种组合的语义为：空到有值是新增，有值到有值是修改，有值到空是删除，两侧
/// 都空不会由命令处理器构造。apply 本身保持幂等，找不到删除目标时安全无操作。
class BeatmapAnnotationAction : public IEditorAction
{
public:
    /// @brief 构造按稳定 ID 定位的单条批注动作。
    /// @param before 修改前批注；新增时为空。
    /// @param after 修改后批注；删除时为空。
    /// @param name 动作名称。
    BeatmapAnnotationAction(std::optional<::MMM::BeatmapAnnotation> before,
                            std::optional<::MMM::BeatmapAnnotation> after,
                            std::string                             name)
        : m_before(std::move(before))
        , m_after(std::move(after))
        , m_name(std::move(name))
    {
        // 新值优先提供 ID；删除动作只存在 before 时从旧值取得身份。
        m_annotationId =
            m_after ? m_after->m_id : (m_before ? m_before->m_id : "");
    }

    /// @brief 应用修改后的批注列表。
    /// @param ctx 当前会话上下文。
    /// @note after 为空时执行删除。
    void execute(SessionContext& ctx) override { apply(ctx, m_after); }

    /// @brief 恢复修改前的批注列表。
    /// @param ctx 当前会话上下文。
    /// @note before 为空时撤销新增。
    void undo(SessionContext& ctx) override { apply(ctx, m_before); }

    /// @brief 重新应用修改后的批注列表。
    /// @param ctx 当前会话上下文。
    void redo(SessionContext& ctx) override { execute(ctx); }

    /// @brief 返回动作名称。
    /// @return 构造时提供的本地动作名称。
    std::string getName() const override { return m_name; }

    /// @brief 返回批注数据类别。
    /// @return 固定为 Annotations，不把附着音符批注误报为对象几何变化。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override
    {
        return ::MMM::BeatmapMutationFlags::Annotations;
    }

private:
    /// @brief 按稳定 ID 写回或删除单条批注。
    /// @param ctx 当前会话上下文。
    /// @param annotation 待应用批注；空值表示删除。
    /// @details
    /// 有目标值时按 ID 覆盖或追加；空目标值时按 ID 删除。无论列表是否实际找到
    /// 对象，都把渲染缓存标脏，使撤销重做后的可见状态从领域列表重新派生。
    void apply(SessionContext&                                ctx,
               const std::optional<::MMM::BeatmapAnnotation>& annotation)
    {
        if ( !ctx.currentBeatmap ) return;
        // 列表属于当前 BeatMap，动作不缓存其地址以支持会话内重做。
        auto&      annotations = ctx.currentBeatmap->m_annotations;
        const auto found       = std::find_if(
            annotations.begin(), annotations.end(), [&](auto& item) {
                return item.m_id == m_annotationId;
            });
        if ( annotation ) {
            if ( found == annotations.end() ) {
                // 新增或目标曾被外部删除时追加完整批注值。
                annotations.push_back(*annotation);
            } else {
                // 修改保持 vector 位置，只替换该稳定 ID 的内容。
                *found = *annotation;
            }
        } else if ( found != annotations.end() ) {
            // 删除不存在的 ID 是幂等无操作。
            annotations.erase(found);
        }
        ctx.isAnnotationRenderCacheDirty = true;
    }

    /// @brief 本动作唯一影响的批注稳定 ID。
    /// @note 从 after 或 before 提取，整个动作生命周期内不变。
    std::string m_annotationId;

    /// @brief 修改前单条批注。
    /// @note 新增动作中为空。
    std::optional<::MMM::BeatmapAnnotation> m_before;

    /// @brief 修改后单条批注。
    /// @note 删除动作中为空。
    std::optional<::MMM::BeatmapAnnotation> m_after;

    /// @brief 动作名称。
    std::string m_name;
};

/// @brief 批量替换 Timeline 的可撤销动作。
/// @details
/// before/after 列表已在创建动作前规范化。动作还保存两侧 preference_bpm，
/// 因为首选 BPM 是 Timing 红线变化的元数据派生值，必须与列表在同一撤销边界
/// 恢复。
///
/// replaceTimelineComponents 会重建 Timeline Registry 并标脏全部派生缓存，动作
/// 因此不保存实体句柄。列表值和首选 BPM 足以在任意方向重建等价领域状态。
class ReplaceTimelinesAction : public IEditorAction
{
public:
    /// @brief 构造批量替换 Timeline 动作。
    /// @param before 替换前的 Timeline 列表。
    /// @param after 替换后的 Timeline 列表。
    /// @param beforePreferenceBpm 替换前的首选 BPM。
    /// @param afterPreferenceBpm 替换后的首选 BPM。
    ReplaceTimelinesAction(std::vector<TimelineComponent> before,
                           std::vector<TimelineComponent> after,
                           double                         beforePreferenceBpm,
                           double                         afterPreferenceBpm)
        : m_before(std::move(before))
        , m_after(std::move(after))
        , m_beforePreferenceBpm(beforePreferenceBpm)
        , m_afterPreferenceBpm(afterPreferenceBpm)
    {
    }

    /// @brief 执行批量替换。
    /// @param ctx 会话上下文引用。
    void execute(SessionContext& ctx) override
    {
        // 先重建 Timeline 和派生缓存，再提交与 after 列表对应的首选 BPM。
        replaceTimelineComponents(ctx, m_after);
        if ( ctx.currentBeatmap ) {
            ctx.currentBeatmap->m_baseMapMetadata.preference_bpm =
                m_afterPreferenceBpm;
        }
    }

    /// @brief 撤销批量替换。
    /// @param ctx 会话上下文引用。
    void undo(SessionContext& ctx) override
    {
        // 撤销采用完全对称顺序恢复 before 列表和元数据。
        replaceTimelineComponents(ctx, m_before);
        if ( ctx.currentBeatmap ) {
            ctx.currentBeatmap->m_baseMapMetadata.preference_bpm =
                m_beforePreferenceBpm;
        }
    }

    /// @brief 重做批量替换。
    /// @param ctx 会话上下文引用。
    void redo(SessionContext& ctx) override { execute(ctx); }

    /// @brief 获取动作名称。
    /// @return 撤销历史显示的稳定动作名称。
    std::string getName() const override { return "Replace Timings"; }

    /// @brief Timing 替换同时更新从 BPM 推导的首选 BPM 元数据。
    /// @return Timelines 与 Metadata 两类组合标志。
    [[nodiscard]] ::MMM::BeatmapMutationFlags mutationFlags() const override
    {
        return ::MMM::BeatmapMutationFlags::Timelines |
               ::MMM::BeatmapMutationFlags::Metadata;
    }

private:
    /// @brief 替换前的 Timeline 列表。
    std::vector<TimelineComponent> m_before;

    /// @brief 替换后的 Timeline 列表。
    std::vector<TimelineComponent> m_after;

    /// @brief 替换前的谱面首选 BPM。
    double m_beforePreferenceBpm{ 120.0 };

    /// @brief 替换后的谱面首选 BPM。
    double m_afterPreferenceBpm{ 120.0 };
};

/**
 * @brief 编辑命令处理区。
 *
 * 所有用户可撤销修改都遵循相同提交协议：先读取 before 值，再完整验证输入和
 * 目标，随后构造 after 值，最后把一个 Action 或 CompositeEditorAction 推入
 * ActionStack。收集与验证阶段不应留下部分领域修改。
 *
 * 批量命令在结构完整性无法保证时整体拒绝。明确允许跳过非法项的入口会在函数
 * 注释中说明；Note/Sample 混合操作使用复合动作，使一次撤销恢复整个用户意图。
 * Polyline 根和投影子实体必须加入同一个 BatchNoteAction，不能留下孤儿节点。
 *
 * 命令携带的实体来自 UI 快照，执行时必须重新验证 Registry 及组件存在性。
 * 无载荷命令仍保留参数名，以便注释和所有 LogicCommand 重载保持统一表达。
 */

/// @brief 撤销 ActionStack 顶部已执行动作。
/// @param cmd 无附加参数的撤销命令。
/// @note 撤销可能涉及 Timing，统一将 BPM 指针缓存标脏。
void ActionController::handleCommand(const CmdUndo& cmd)
{
    // Action 自身负责恢复领域状态，会话这里只处理共享 BPM 派生缓存。
    m_ctx.actionStack.undo(m_ctx);
    m_ctx.isBpmEventsDirty = true;
}

/// @brief 重做 ActionStack 中最近撤销的动作。
/// @param cmd 无附加参数的重做命令。
/// @note 重做可能涉及 Timing，统一将 BPM 指针缓存标脏。
void ActionController::handleCommand(const CmdRedo& cmd)
{
    // redo 复用动作正向路径，完成后下一次 BPM 查询必须重建事件指针。
    m_ctx.actionStack.redo(m_ctx);
    m_ctx.isBpmEventsDirty = true;
}

/// @brief 将当前可编辑选择复制为 Note 与 Sample 双域剪贴板快照。
/// @param cmd 无附加参数的复制命令。
/// @details
/// Note 保存完整组件及秒/拍两套定位信息；Sample 把绝对轨道转换为相对 BGM
/// lane，使目标谱面玩家轨道数不同时仍能落到相同 BGM 区位置。结果同时写入
/// 会话本地缓存和 EditorEngine 跨会话剪贴板。
/// @warning 用户显式低频路径：会扫描一次 Note 和一次 Sample Registry。
void ActionController::handleCommand(const CmdCopy& cmd)
{
    // 新复制事务完全替换旧载荷，避免两次不同选择意外合并。
    m_ctx.clipboard.clear();
    m_ctx.sampleClipboard.clear();
    const double fallbackBpm  = getClipboardFallbackBpm(m_ctx);
    auto         beatTimeline = buildClipboardBeatTimeline(m_ctx, fallbackBpm);
    auto view = m_ctx.noteRegistry.view<NoteComponent, InteractionComponent>();
    for ( auto entity : view ) {
        // 只复制当前配置仍允许编辑的已选对象，隐藏类型不泄漏到剪贴板。
        const auto& ic   = view.get<InteractionComponent>(entity);
        const auto& note = view.get<NoteComponent>(entity);
        if ( ic.isSelected &&
             SessionUtils::isNoteEditable(note, m_ctx.lastConfig.settings) ) {
            ClipboardItem item;
            // 组件按值复制，之后源实体变化不会修改剪贴板快照。
            item.note = note;
            populateClipboardBeatPositions(item, beatTimeline, fallbackBpm);
            m_ctx.clipboard.push_back(std::move(item));
        }
    }
    const std::uint32_t playerTrackCount =
        static_cast<std::uint32_t>(std::max(m_ctx.trackCount, 0));
    auto sampleView =
        m_ctx.sampleRegistry.view<SampleComponent, InteractionComponent>();
    for ( auto entity : sampleView ) {
        // Sample 选择来自独立 Registry，不与同值 entt 句柄的 Note 混淆。
        const auto& interaction = sampleView.get<InteractionComponent>(entity);
        if ( !interaction.isSelected ) continue;

        SampleClipboardItem item;
        item.sample  = sampleView.get<SampleComponent>(entity);
        item.bgmLane = item.sample.m_track >= playerTrackCount
                           ? item.sample.m_track - playerTrackCount
                           : 0U;
        // 异常落入玩家区的旧 Sample 保守映射到首个 BGM lane，避免无符号下溢。
        item.sample.m_track = item.bgmLane;
        populateSampleClipboardBeatPosition(item, beatTimeline, fallbackBpm);
        m_ctx.sampleClipboard.push_back(std::move(item));
    }

    EditorEngine::instance().setChartObjectClipboard(
        m_ctx.clipboard, m_ctx.sampleClipboard, &m_ctx, false);
    // 全局载荷记录来源会话和非 Cut 属性，供跨标签权限策略判断。
    const std::size_t itemCount =
        m_ctx.clipboard.size() + m_ctx.sampleClipboard.size();
    XINFO("Copied {} chart objects to clipboard", itemCount);
    m_ctx.lastActionMessage = fmt::format("{} {} {} {}",
                                          TR("ui.status.category.clipboard"),
                                          TR("ui.status.clipboard.copied"),
                                          itemCount,
                                          TR("ui.status.info.items"));
}

/// @brief 复制当前选择并把来源实体标记为待剪切。
/// @param cmd 无附加参数的剪切命令。
/// @details
/// 剪切不立即删除对象，只在成功复制后设置 InteractionComponent::isCut。随后的
/// 本会话粘贴把删除旧对象和创建新对象合成一个动作；跨会话粘贴只创建副本。
void ActionController::handleCommand(const CmdCut& cmd)
{
    // 先复用复制流程建立完整载荷，避免出现 Cut 标记却没有剪贴板内容。
    handleCommand(CmdCopy{});
    auto view = m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : view ) {
        // 只标记新配置下仍可编辑的 Note，隐藏类型保持原状态。
        auto&       ic   = m_ctx.noteRegistry.get<InteractionComponent>(entity);
        const auto& note = view.get<NoteComponent>(entity);
        if ( ic.isSelected &&
             SessionUtils::isNoteEditable(note, m_ctx.lastConfig.settings) ) {
            ic.isCut = true;
        }
    }
    auto sampleView = m_ctx.sampleRegistry.view<InteractionComponent>();
    for ( auto entity : sampleView ) {
        // Sample 剪切标记独立保存，粘贴时可与 Note 组成复合动作。
        auto& interaction =
            m_ctx.sampleRegistry.get<InteractionComponent>(entity);
        if ( interaction.isSelected ) {
            interaction.isCut = true;
        }
    }
    EditorEngine::instance().setChartObjectClipboard(
        m_ctx.clipboard, m_ctx.sampleClipboard, &m_ctx, true);
    // 覆盖 Copy 写入的全局属性，把同一快照标记为 Cut 来源。
    const std::size_t itemCount =
        m_ctx.clipboard.size() + m_ctx.sampleClipboard.size();
    m_ctx.lastActionMessage = fmt::format("{} {} {} {}",
                                          TR("ui.status.category.clipboard"),
                                          TR("ui.status.clipboard.cut"),
                                          itemCount,
                                          TR("ui.status.info.items"));
}

/// @brief 删除当前可编辑选择；无选择时删除有效悬停目标。
/// @param cmd 无附加参数的删除命令。
/// @details
/// Note 与 Sample 分别收集为批量动作。Polyline 父被删除时追加全部投影子实体；
/// 两个域同时存在则包装 CompositeEditorAction，保证一次撤销完整恢复。
/// @warning 用户显式低频路径：最多各扫描一次 Note 和 Sample Registry。
void ActionController::handleCommand(const CmdDeleteSelected& cmd)
{
    // 收集阶段只构造 before->nullopt 条目，不立即销毁实体。
    std::vector<BatchNoteAction::Entry>   entries;
    std::vector<BatchSampleAction::Entry> sampleEntries;

    auto view = m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : view ) {
        const auto& ic   = view.get<InteractionComponent>(entity);
        const auto& note = view.get<NoteComponent>(entity);
        if ( ic.isSelected &&
             SessionUtils::isNoteEditable(note, m_ctx.lastConfig.settings) ) {
            entries.push_back({ entity, note, std::nullopt });
        }
    }

    auto sampleView =
        m_ctx.sampleRegistry.view<InteractionComponent, SampleComponent>();
    for ( auto entity : sampleView ) {
        const auto& interaction = sampleView.get<InteractionComponent>(entity);
        if ( interaction.isSelected ) {
            sampleEntries.push_back({
                entity,
                sampleView.get<SampleComponent>(entity),
                std::nullopt,
            });
        }
    }

    // 没有任何选择时才回退悬停目标，避免额外删除一个未选物件。
    if ( entries.empty() && sampleEntries.empty() &&
         m_ctx.hoveredEntity != entt::null ) {
        if ( (m_ctx.hoveredObjectKind == ChartObjectKind::PlayerNote ||
              m_ctx.hoveredObjectKind == ChartObjectKind::DraftNote) &&
             m_ctx.noteRegistry.valid(m_ctx.hoveredEntity) &&
             m_ctx.noteRegistry.all_of<NoteComponent>(m_ctx.hoveredEntity) &&
             SessionUtils::isNoteEditable(
                 m_ctx.noteRegistry.get<const NoteComponent>(
                     m_ctx.hoveredEntity),
                 m_ctx.lastConfig.settings) ) {
            entries.push_back(
                { m_ctx.hoveredEntity,
                  m_ctx.noteRegistry.get<NoteComponent>(m_ctx.hoveredEntity),
                  std::nullopt });
        } else if ( m_ctx.hoveredObjectKind == ChartObjectKind::AudioSample &&
                    m_ctx.sampleRegistry.valid(m_ctx.hoveredEntity) &&
                    m_ctx.sampleRegistry.all_of<SampleComponent>(
                        m_ctx.hoveredEntity) ) {
            sampleEntries.push_back({
                m_ctx.hoveredEntity,
                m_ctx.sampleRegistry.get<SampleComponent>(m_ctx.hoveredEntity),
                std::nullopt,
            });
        }
    }

    // 收集根实体 ID，供一次扫描追加被删除 Polyline 的全部子实体。
    std::unordered_set<entt::entity> deletedEntities;
    for ( const auto& entry : entries ) {
        deletedEntities.insert(entry.entity);
    }

    // 父与子进入同一批量动作，执行和撤销都不会出现孤儿投影实体。
    appendDeletedPolylineChildren(m_ctx, deletedEntities, entries);

    const size_t count = entries.size() + sampleEntries.size();
    if ( count > 0 ) {
        // 每个非空数据域构造一个子动作，再按数量决定是否需要复合包装。
        std::vector<std::unique_ptr<IEditorAction>> actions;
        actions.reserve(2);
        if ( !entries.empty() ) {
            actions.push_back(std::make_unique<BatchNoteAction>(
                std::move(entries), "Delete Selected"));
        }
        if ( !sampleEntries.empty() ) {
            actions.push_back(std::make_unique<BatchSampleAction>(
                std::move(sampleEntries), "删除已选自动采样"));
        }

        std::unique_ptr<IEditorAction> action;
        if ( actions.size() == 1 ) {
            // 单域删除保留原动作的精确 mutationFlags。
            action = std::move(actions.front());
        } else {
            // 混合选择只占一个撤销槽，按 Note、Sample 顺序执行。
            action = std::make_unique<CompositeEditorAction>(
                std::move(actions), "删除已选谱面物件");
        }
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
        XINFO("Deleted {} selected/hovered items", count);
    }
}

/// @brief 将已选可编辑 Note 在各自玩家或草稿轨道域内水平镜像。
/// @param cmd 无附加参数的镜像命令。
/// @details
/// 根 Polyline 被选中时，其 ECS 子实体也加入同一 BatchNoteAction。草稿使用独立
/// draftTrackCount，正式物件使用谱面玩家轨道数；Flick 横向方向随镜像反转。
void ActionController::handleCommand(const CmdMirrorSelected& cmd)
{
    // 空谱面没有稳定轨道域，拒绝构造镜像动作。
    if ( !m_ctx.currentBeatmap ) return;
    int trackCount = getMirrorTrackCount(m_ctx);

    std::vector<BatchNoteAction::Entry> entries;
    std::unordered_set<entt::entity>    toMirror;

    // 第一阶段收集全部已选根物件及其 Polyline 投影子实体。
    auto view = m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : view ) {
        const auto& ic = view.get<InteractionComponent>(entity);
        const auto& nc = view.get<NoteComponent>(entity);
        if ( ic.isSelected &&
             SessionUtils::isNoteEditable(nc, m_ctx.lastConfig.settings) ) {
            toMirror.insert(entity);

            // 如果是 Polyline，收集其所有子物件实体
            if ( nc.m_type == ::MMM::NoteType::POLYLINE ) {
                // 当前没有父到子直接索引，只在显式镜像时定位对应投影实体。
                for ( auto subEnt : m_ctx.noteRegistry.view<NoteComponent>() ) {
                    const auto& subNC =
                        m_ctx.noteRegistry.get<NoteComponent>(subEnt);
                    if ( subNC.m_isSubNote &&
                         subNC.m_parentPolyline == entity ) {
                        toMirror.insert(subEnt);
                    }
                }
            }
        }
    }

    // 第二阶段按值构造每个实体的 before/after 镜像条目。
    for ( auto entity : toMirror ) {
        if ( !m_ctx.noteRegistry.valid(entity) ||
             !m_ctx.noteRegistry.all_of<NoteComponent>(entity) )
            continue;

        const auto& oldNote = m_ctx.noteRegistry.get<NoteComponent>(entity);
        // 动作快照不借用 Registry 组件地址，支持之后安全撤销。
        auto newNote = oldNote;

        mirrorNoteComponent(newNote, trackCount, m_ctx.draftTrackCount);

        entries.push_back({ entity, oldNote, newNote });
    }

    if ( !entries.empty() ) {
        size_t count  = entries.size();
        auto   action = std::make_unique<BatchNoteAction>(std::move(entries),
                                                        "Mirror Selected");
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
        XINFO("Mirrored {} items (including sub-notes)", count);

        m_ctx.lastActionMessage = fmt::format("{} {} {} {}",
                                              TR("ui.status.category.action"),
                                              TR("ui.edit.mirror"),
                                              count,
                                              TR("ui.status.info.items"));
    }
}

/// @brief 对全部已选可编辑根 Note 设置或清除单一颜色槽。
/// @param cmd 目标颜色槽及可选颜色；空颜色表示清除该槽。
/// @note Polyline 子实体不独立持久化颜色，因此只处理根组件。
/// @details
/// 每个目标的完整 NoteComponent 作为 before/after 保存，统一 setter 同步 ECS
/// 颜色 缓存与格式元数据。没有有效选择时不创建空批量动作。
void ActionController::handleCommand(const CmdApplyNoteColorToSelection& cmd)
{
    // 同批选择合并成一个动作，使一次撤销恢复全部颜色。
    std::vector<BatchNoteAction::Entry> entries;

    auto view = m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : view ) {
        const auto& ic = view.get<InteractionComponent>(entity);
        if ( !ic.isSelected ) continue;

        const auto& oldNote = view.get<NoteComponent>(entity);
        if ( oldNote.m_isSubNote || !SessionUtils::isNoteEditable(
                                        oldNote, m_ctx.lastConfig.settings) ) {
            continue;
        }

        auto newNote = oldNote;
        setNoteColorOverride(newNote, cmd.slot, cmd.color);
        entries.push_back({ entity, oldNote, newNote });
    }

    if ( entries.empty() ) return;

    auto actionName =
        cmd.color.has_value() ? "Set Note Color" : "Clear Note Color";
    auto action =
        std::make_unique<BatchNoteAction>(std::move(entries), actionName);
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage =
        cmd.color.has_value() ? "Note color applied" : "Note color cleared";
}

/// @brief 对全部已选可编辑根 Note 应用完整调色盘。
/// @param cmd 按固定 NoteColorSlot 顺序提供的颜色数组。
/// @note 每个目标都保存完整 before/after 覆写。
/// @details
/// 颜色数组先转换为槽位对象，再对每个根组件应用。子实体的可见颜色由父折线
/// 结构派生，不能单独写入以免下次重建丢失。
void ActionController::handleCommand(const CmdApplyNotePaletteToSelection& cmd)
{
    // 命令颜色数组只转换一次，避免为每个选中实体重复构建覆写对象。
    std::vector<BatchNoteAction::Entry> entries;
    auto colors = makeNoteColorOverrides(cmd.colors);

    auto view = m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : view ) {
        const auto& ic = view.get<InteractionComponent>(entity);
        if ( !ic.isSelected ) continue;

        const auto& oldNote = view.get<NoteComponent>(entity);
        if ( oldNote.m_isSubNote || !SessionUtils::isNoteEditable(
                                        oldNote, m_ctx.lastConfig.settings) ) {
            continue;
        }

        auto newNote = oldNote;
        applyNoteColorOverrides(newNote, colors);
        entries.push_back({ entity, oldNote, newNote });
    }

    if ( entries.empty() ) return;

    auto action = std::make_unique<BatchNoteAction>(std::move(entries),
                                                    "Set Note Palette");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage = "Note palette applied";
}

/// @brief 将当前画笔调色盘应用到点击实体所属的根 Note。
/// @param cmd 点击实体；Polyline 子实体会解析到父实体。
/// @note 空调色盘或结果无变化时不创建撤销动作。
/// @details
/// 与批量调色入口不同，本命令用于颜色画笔的单次命中。目标解析到持久化根后，
/// 仍需检查当前编辑能力，防止颜色画笔修改已被设置隐藏的类型。
void ActionController::handleCommand(const CmdApplyBrushPaletteToEntity& cmd)
{
    // 目标解析同时验证实体和 Polyline 父关系。
    entt::entity target = resolveNoteColorTargetEntity(m_ctx, cmd.entity);
    if ( target == entt::null ) return;

    const auto& colors = m_ctx.brushState.customColors;
    // 空调色盘表示没有画笔覆写，不应被解释为清除已有颜色。
    if ( !hasAnyNoteColorOverride(colors) ) return;

    const auto& oldNote = m_ctx.noteRegistry.get<NoteComponent>(target);
    if ( !SessionUtils::isNoteEditable(oldNote, m_ctx.lastConfig.settings) ) {
        return;
    }
    auto newNote = oldNote;
    applyNoteColorOverrides(newNote, colors);
    if ( isSameNoteColorOverrides(oldNote.m_customColors,
                                  newNote.m_customColors) )
        // 相同结果不污染 ActionStack 保存点。
        return;

    std::vector<BatchNoteAction::Entry> entries;
    entries.push_back({ target, oldNote, newNote });

    auto action = std::make_unique<BatchNoteAction>(std::move(entries),
                                                    "Set Note Palette");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage = "Note palette applied";
}

/// @brief 清除点击实体所属根 Note 的全部颜色覆写。
/// @param cmd 点击实体；Polyline 子实体会解析到父实体。
/// @note 目标已经没有覆写时不创建动作。
/// @details
/// 清除后 Note 回退皮肤默认颜色，但不删除其他格式元数据、采样绑定或批注。动作
/// 仅包含这一根实体，支持精确撤销原调色盘。
void ActionController::handleCommand(const CmdClearNoteColorOverrides& cmd)
{
    // 空 NoteColorOverrides 经统一应用助手清除全部 optional 槽。
    entt::entity target = resolveNoteColorTargetEntity(m_ctx, cmd.entity);
    if ( target == entt::null ) return;

    const auto& oldNote = m_ctx.noteRegistry.get<NoteComponent>(target);
    if ( !SessionUtils::isNoteEditable(oldNote, m_ctx.lastConfig.settings) ) {
        return;
    }
    auto               newNote = oldNote;
    NoteColorOverrides emptyColors;
    applyNoteColorOverrides(newNote, emptyColors);
    if ( isSameNoteColorOverrides(oldNote.m_customColors,
                                  newNote.m_customColors) )
        return;

    std::vector<BatchNoteAction::Entry> entries;
    entries.push_back({ target, oldNote, newNote });

    auto action = std::make_unique<BatchNoteAction>(std::move(entries),
                                                    "Clear Note Palette");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage = "Note palette cleared";
}

/// @brief 粘贴剪贴板中的物件，并拒绝会落到负时间的创建结果。
/// @param cmd 粘贴指令。
/// @details
/// 优先读取 EditorEngine 的会话安全剪贴板，空时回退 SessionContext 本地副本。
/// Note、Sample 和 Timeline 可以同时存在；Note/Sample 共用最早时间或最早 beat
/// 锚点，Timeline 交由独立批量动作。
///
/// 按 beat 粘贴要求本批全部 Note/Sample 都带拍位缓存，否则整批回退秒偏移，
/// 避免部分对象按拍、部分按秒。创建实体前完整验证时间与轨道，任一结果非法即
/// 拒绝整批，不留下半次粘贴。
///
/// 本会话 Cut 粘贴将旧实体删除和新实体创建合成一个动作；普通 Copy 或跨会话
/// 粘贴只创建新实体。执行后可选中新对象，并清理已经消费的 Cut 来源状态。
///
/// 粘贴事务分为以下阶段：
///
/// - 载荷解析：按当前会话隔离规则取得三类剪贴板，并过滤已禁止编辑的 Note。
/// - 锚点计算：在 Note 和 Sample 的并集上求最早秒时间与最早连续拍数。
/// - 结果预演：对值副本应用秒/拍偏移、可选镜像和相对 BGM 轨道换算。
/// - 完整验证：检查所有根、Polyline 子节点、Sample 时间和 uint32 轨道范围。
/// - 动作构造：收集本地 Cut 删除条目，预分配新实体并追加创建条目。
/// - 原子提交：按数据域生成批量动作，必要时包装为一个复合动作。
/// - 选择收尾：清除旧框选，按预分配实体精确选中新建对象并消费 Cut 状态。
///
/// 预分配实体在动作执行前只存在句柄，没有 NoteComponent 或 SampleComponent。
/// Batch 动作负责安装组件与辅助交互状态。这样执行后无需扫描 Registry 猜测哪些
/// 对象刚被创建，也能让 redo 复用相同实体身份。
///
/// 本地 Cut 的删除条目保留 beforeSelected，使撤销可以恢复来源选择。Polyline
/// 父删除会补齐全部子投影实体。跨会话 Cut 不在目标 Session 操作来源 Registry，
/// 只通知 EditorEngine 按来源会话协议消费载荷。
///
/// Timeline 剪贴板允许独立条目过滤：无效时间或值不会破坏其他合法 Timing。
/// 这是因为每个 Timeline 事件相互独立；与之相对，Note/Sample 的混合布局必须
/// 保持共同锚点和结构完整，所以其中任一项失败会拒绝整个对象批次。
///
/// 秒模式保留对象间绝对时间差，适合音效与既有波形对齐；beat 模式保留音乐拍距，
/// 适合跨 BPM 谱面复用节奏。两种模式都以当前播放头 animateTime 为目标锚点，
/// 不读取相机像素位置，避免缩放或滚动影响粘贴结果。
/// @warning 低频编辑路径：用户触发粘贴时执行，可能批量创建实体。
void ActionController::handleCommand(const CmdPaste& cmd)
{
    // EditorEngine 按协作隔离和来源会话过滤跨标签载荷。
    auto noteClipboard   = EditorEngine::instance().getClipboard(&m_ctx);
    auto sampleClipboard = EditorEngine::instance().getSampleClipboard(&m_ctx);
    auto timelineClipboard =
        EditorEngine::instance().getTimelineClipboard(&m_ctx);
    if ( noteClipboard.empty() && sampleClipboard.empty() &&
         timelineClipboard.empty() ) {
        // 全局载荷不可用时，本会话副本仍支持旧入口和同标签操作。
        noteClipboard   = m_ctx.clipboard;
        sampleClipboard = m_ctx.sampleClipboard;
    }
    std::erase_if(noteClipboard, [&](const auto& item) {
        // 当前设置已禁用的类型不能借粘贴绕过编辑权限。
        return !SessionUtils::isNoteEditable(item.note,
                                             m_ctx.lastConfig.settings);
    });
    if ( noteClipboard.empty() && sampleClipboard.empty() &&
         timelineClipboard.empty() ) {
        return;
    }

    // 先预计算粘贴目标，确保不会在负时间创建新物件。
    double pasteTime = m_ctx.animateTime;

    if ( !noteClipboard.empty() || !sampleClipboard.empty() ) {
        // Note 与 Sample 共用同一个最早锚点，保持混合编排的相对时间。
        double minTime = std::numeric_limits<double>::infinity();
        double minBeat = std::numeric_limits<double>::infinity();
        for ( const auto& item : noteClipboard ) {
            // 秒与 beat 两套最早锚点同时统计，稍后按配置选择其中一套。
            minTime = std::min(minTime, item.note.m_timestamp);
            minBeat = std::min(minBeat, item.startBeat);
        }
        for ( const auto& item : sampleClipboard ) {
            minTime = std::min(minTime, item.sample.m_timestamp);
            minBeat = std::min(minBeat, item.startBeat);
        }

        const double timeOffset = pasteTime - minTime;
        // 只有每个条目都有复制时拍位，整批才能安全采用 beat 语义。
        const bool pasteByBeat =
            m_ctx.lastConfig.settings.copyPasteTimeBasis ==
                Config::CopyPasteTimeBasis::Beat &&
            std::all_of(
                noteClipboard.begin(),
                noteClipboard.end(),
                [](const auto& item) { return item.hasBeatPositions; }) &&
            std::all_of(sampleClipboard.begin(),
                        sampleClipboard.end(),
                        [](const auto& item) { return item.hasBeatPosition; });
        const double pasteFallbackBpm = getClipboardFallbackBpm(m_ctx);
        auto         pasteBeatTimeline =
            pasteByBeat ? buildClipboardBeatTimeline(m_ctx, pasteFallbackBpm)
                                : ClipboardBeatTimeline{};
        const double pasteBeat =
            // 播放头时间先映射到目标谱面的连续拍数，作为整批粘贴锚点。
            pasteByBeat ? clipboardTimeToBeat(
                              pasteBeatTimeline, pasteTime, pasteFallbackBpm)
                        : 0.0;

        int mirrorTrackCount = cmd.m_mirrored ? getMirrorTrackCount(m_ctx) : 0;
        std::vector<NoteComponent> notesToPaste;
        notesToPaste.reserve(noteClipboard.size());
        for ( const auto& item : noteClipboard ) {
            // 预计算阶段只修改局部副本，不创建实体或触碰 ActionStack。
            auto newNote = item.note;
            if ( pasteByBeat ) {
                applyBeatPastePosition(newNote,
                                       item,
                                       pasteBeatTimeline,
                                       pasteFallbackBpm,
                                       pasteBeat,
                                       minBeat);
            } else {
                // 秒模式保持复制内容内部的绝对秒间距。
                newNote.m_timestamp = item.note.m_timestamp + timeOffset;

                // Polyline 父与全部内嵌节点必须应用相同秒偏移。
                if ( newNote.m_type == ::MMM::NoteType::POLYLINE ) {
                    for ( auto& sub : newNote.m_subNotes ) {
                        sub.timestamp += timeOffset;
                    }
                }
            }

            if ( cmd.m_mirrored ) {
                // 镜像在时间定位后应用，两种变换互不依赖并共享一次撤销。
                mirrorNoteComponent(
                    newNote, mirrorTrackCount, m_ctx.draftTrackCount);
            }

            if ( !isPlaceableCreatedNote(newNote) ) {
                // 一个非法 Note 会拒绝整批，尚未分配实体所以无需回滚。
                XWARN("Paste blocked before 0s: target time={:.3f}",
                      newNote.m_timestamp);
                return;
            }

            notesToPaste.push_back(newNote);
        }

        const std::uint32_t playerTrackCount =
            static_cast<std::uint32_t>(std::max(m_ctx.trackCount, 0));
        std::vector<SampleComponent> samplesToPaste;
        samplesToPaste.reserve(sampleClipboard.size());
        for ( const auto& item : sampleClipboard ) {
            // Sample 与 Note 使用相同批次锚点，但只保存单一开始时间。
            auto newSample = item.sample;
            if ( pasteByBeat ) {
                newSample.m_timestamp =
                    clipboardBeatToTime(pasteBeatTimeline,
                                        pasteBeat + item.startBeat - minBeat,
                                        pasteFallbackBpm);
            } else {
                newSample.m_timestamp = item.sample.m_timestamp + timeOffset;
            }

            const std::uint64_t targetTrack =
                // 相对 BGM lane 加到目标玩家轨道数之后，适配不同 Key 数谱面。
                static_cast<std::uint64_t>(playerTrackCount) + item.bgmLane;
            if ( targetTrack >
                 static_cast<std::uint64_t>(
                     std::numeric_limits<std::uint32_t>::max()) ) {
                XWARN("Paste blocked: BGM lane {} exceeds track range",
                      item.bgmLane);
                return;
            }
            newSample.m_track = static_cast<std::uint32_t>(targetTrack);
            if ( !isPlaceableCreatedSample(newSample) ) {
                XWARN("Sample paste blocked before 0s: target time={:.3f}",
                      newSample.m_timestamp);
                return;
            }
            samplesToPaste.push_back(std::move(newSample));
        }

        std::vector<BatchNoteAction::Entry>   noteEntries;
        std::vector<BatchSampleAction::Entry> sampleEntries;
        /// @brief 本次粘贴预先分配的新 Note 实体，用于执行后选中新物件。
        std::vector<entt::entity> pastedNoteEntities;
        /// @brief 本次粘贴预先分配的新 Sample 实体，用于执行后选中新物件。
        std::vector<entt::entity> pastedSampleEntities;
        pastedNoteEntities.reserve(noteClipboard.size());
        pastedSampleEntities.reserve(sampleClipboard.size());
        const bool selectPastedObjects = cmd.m_selectPastedObjects;

        // 本会话 Cut 需要把旧实体删除并入同一动作；Copy 不收集删除条目。
        auto noteView   = m_ctx.noteRegistry.view<InteractionComponent>();
        auto sampleView = m_ctx.sampleRegistry.view<InteractionComponent>();
        bool isLocalCut = EditorEngine::instance().isClipboardCutFrom(&m_ctx);
        if ( isLocalCut ) {
            // Note Cut 只处理仍存在且当前可编辑的实体。
            std::unordered_set<entt::entity> deletedNoteEntities;
            for ( auto entity : noteView ) {
                auto& ic = m_ctx.noteRegistry.get<InteractionComponent>(entity);
                if ( ic.isCut ) {
                    if ( !m_ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
                        continue;
                    }

                    auto oldNote =
                        m_ctx.noteRegistry.get<NoteComponent>(entity);
                    if ( !SessionUtils::isNoteEditable(
                             oldNote, m_ctx.lastConfig.settings) ) {
                        continue;
                    }
                    noteEntries.push_back({
                        // 删除条目保留原选择状态，撤销时可恢复交互体验。
                        .entity         = entity,
                        .before         = oldNote,
                        .after          = std::nullopt,
                        .beforeSelected = ic.isSelected,
                    });
                    deletedNoteEntities.insert(entity);
                }
            }
            appendDeletedPolylineChildren(
                m_ctx, deletedNoteEntities, noteEntries);

            // Sample Cut 使用独立 Registry，但与 Note 删除共享最终复合动作。
            for ( auto entity : sampleView ) {
                auto& interaction =
                    m_ctx.sampleRegistry.get<InteractionComponent>(entity);
                if ( interaction.isCut &&
                     m_ctx.sampleRegistry.all_of<SampleComponent>(entity) ) {
                    sampleEntries.push_back({
                        .entity = entity,
                        .before =
                            m_ctx.sampleRegistry.get<SampleComponent>(entity),
                        .after          = std::nullopt,
                        .beforeSelected = interaction.isSelected,
                    });
                }
            }
        } else {
            // 跨会话 Cut 由来源会话消费协议处理，目标会话不能删除来源实体。
            EditorEngine::instance().consumeCrossSessionCutClipboard(&m_ctx);
        }

        for ( const auto& newNote : notesToPaste ) {
            // 为新粘贴物件预分配实体，避免执行后再从撤销栈动作反查实体。
            entt::entity pastedEntity = m_ctx.noteRegistry.create();
            pastedNoteEntities.push_back(pastedEntity);
            noteEntries.push_back({
                .entity        = pastedEntity,
                .before        = std::nullopt,
                .after         = newNote,
                .afterSelected = selectPastedObjects,
            });
        }
        for ( const auto& newSample : samplesToPaste ) {
            // Sample 同样预分配实体，供动作执行后精确建立新选择。
            entt::entity pastedEntity = m_ctx.sampleRegistry.create();
            pastedSampleEntities.push_back(pastedEntity);
            sampleEntries.push_back({
                .entity        = pastedEntity,
                .before        = std::nullopt,
                .after         = newSample,
                .afterSelected = selectPastedObjects,
            });
        }

        // 动作执行前清除所有临时剪切标记，避免实体销毁后访问失效 View。
        for ( auto entity : noteView ) {
            m_ctx.noteRegistry.get<InteractionComponent>(entity).isCut = false;
        }
        for ( auto entity : sampleView ) {
            m_ctx.sampleRegistry.get<InteractionComponent>(entity).isCut =
                false;
        }

        std::vector<std::unique_ptr<IEditorAction>> actions;
        // Note 与 Sample 条目分别交给其 Registry 专用动作。
        actions.reserve(2);
        if ( !noteEntries.empty() ) {
            actions.push_back(std::make_unique<BatchNoteAction>(
                std::move(noteEntries),
                cmd.m_mirrored ? "Mirror Paste" : "Paste"));
        }
        if ( !sampleEntries.empty() ) {
            actions.push_back(std::make_unique<BatchSampleAction>(
                std::move(sampleEntries), "粘贴自动采样"));
        }
        std::unique_ptr<IEditorAction> action;
        if ( actions.size() == 1 ) {
            // 单域粘贴无需复合包装。
            action = std::move(actions.front());
        } else {
            // 混合粘贴保持单一撤销边界，镜像属性只影响动作展示名和 Note。
            action = std::make_unique<CompositeEditorAction>(
                std::move(actions),
                cmd.m_mirrored ? "镜像粘贴谱面物件" : "粘贴谱面物件");
        }
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);

        if ( selectPastedObjects ) {
            // 选择新对象前终止框选手势，避免下一帧重新覆盖显式选择。
            m_ctx.isSelecting         = false;
            m_ctx.hasMarqueeSelection = false;
            m_ctx.marqueeIsAdditive   = false;
            m_ctx.marqueeBoxes.clear();
            // 先清空所有旧选择，再只选中本次粘贴创建出的实体。
            clearChartObjectSelection(m_ctx);

            for ( auto entity : pastedNoteEntities ) {
                // 预分配实体执行后应已拥有组件；类型决定正式或草稿选择索引。
                const auto* note =
                    m_ctx.noteRegistry.try_get<const NoteComponent>(entity);
                setChartObjectSelected(m_ctx,
                                       note && note->m_isDraft
                                           ? ChartObjectKind::DraftNote
                                           : ChartObjectKind::PlayerNote,
                                       entity,
                                       true);
            }
            for ( auto entity : pastedSampleEntities ) {
                // Sample 选择必须显式使用 AudioSample 类型，防止跨 Registry
                // 混淆。
                setChartObjectSelected(
                    m_ctx, ChartObjectKind::AudioSample, entity, true);
            }
        }

        if ( isLocalCut ) {
            // 动作成功提交后才标记 Cut 已消费，失败前仍允许用户重试。
            EditorEngine::instance().markCutClipboardConsumed();
        }
    }

    if ( !timelineClipboard.empty() ) {
        // Timeline 与对象域分别成动作，沿用同一播放头 pasteTime。
        const bool pasteByBeat =
            m_ctx.lastConfig.settings.copyPasteTimeBasis ==
                Config::CopyPasteTimeBasis::Beat &&
            std::all_of(timelineClipboard.begin(),
                        timelineClipboard.end(),
                        [](const auto& item) { return item.hasBeatPosition; });
        const double pasteFallbackBpm = getClipboardFallbackBpm(m_ctx);
        auto         pasteBeatTimeline =
            pasteByBeat ? buildClipboardBeatTimeline(m_ctx, pasteFallbackBpm)
                                : ClipboardBeatTimeline{};
        const double pasteBeat =
            pasteByBeat ? clipboardTimeToBeat(
                              pasteBeatTimeline, pasteTime, pasteFallbackBpm)
                        : 0.0;

        std::vector<BatchTimelineAction::Entry> timelineEntries;
        timelineEntries.reserve(timelineClipboard.size());
        for ( const auto& item : timelineClipboard ) {
            // 每个事件从相对秒或相对 beat 恢复目标时间，值与类型保持原样。
            auto   newTimeline = item.timeline;
            double targetTime  = pasteTime + item.relativeTime;
            if ( pasteByBeat ) {
                targetTime = clipboardBeatToTime(pasteBeatTimeline,
                                                 pasteBeat + item.relativeBeat,
                                                 pasteFallbackBpm);
            }
            // 粘贴创建也必须遵循非负 BPM 约束，零值仍可保留。
            if ( !std::isfinite(targetTime) ||
                 !isValidTimelineValue(newTimeline.m_effect,
                                       newTimeline.m_value) ) {
                continue;
            }

            newTimeline.m_timestamp = std::max(0.0, targetTime);
            // 负目标时间按零夹取；非法值则已在上方过滤，不加入部分动作。
            timelineEntries.push_back(
                { entt::null, std::nullopt, newTimeline });
        }

        if ( !timelineEntries.empty() ) {
            // 所有合法 Timing 合并为一次粘贴动作，并使 BPM 缓存失效。
            auto action = std::make_unique<BatchTimelineAction>(
                std::move(timelineEntries), "Paste");
            m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
            m_ctx.isBpmEventsDirty = true;
        }
    }
}

/**
 * @brief Timeline 编辑命令区。
 *
 * 所有入口复用 isValidTimelineValue，确保非有限值和负 BPM 无法绕过 UI 控件进入
 * ECS。时间发生变化且调用方没有提供完整元数据覆盖时，必须清除 Malody beat
 * 缓存，因为原拍位不再描述新的秒时间。
 *
 * 单项更新对无效目标整体无操作；批量更新允许跳过已失效或非法的独立条目，
 * 只要仍有合法项就提交一个 BatchTimelineAction。每次成功动作都将 bpmEvents
 * 标脏，具体 ScrollCache 和领域同步由 TimelineAction 维护。
 */

/// @brief 更新单个既有 Timeline 事件。
/// @param cmd 目标实体、新时间、新值及可选元数据覆盖。
/// @note 目标失效或值非法时不创建动作。
/// @details
/// 显式 metadataOverride 完整替换格式私有字段；否则只在时间变化时删除失效的
/// Malody beat。类型保持旧组件值，本入口不支持把 BPM 改成其他效果。
void ActionController::handleCommand(const CmdUpdateTimelineEvent& cmd)
{
    // UI 入队后实体可能被其他动作删除，读取前重新验证句柄。
    if ( m_ctx.timelineRegistry.valid(cmd.entity) ) {
        auto oldTl = m_ctx.timelineRegistry.get<TimelineComponent>(cmd.entity);
        // 单项编辑是表格与弹窗的最终入口，非法 BPM 不得进入撤销栈。
        if ( !isValidTimelineValue(oldTl.m_effect, cmd.newValue) ) return;
        auto newTl        = oldTl;
        newTl.m_timestamp = cmd.newTime;
        newTl.m_value     = cmd.newValue;
        if ( cmd.metadataOverride ) {
            // 显式覆盖代表调用方已经提供与新时间一致的格式私有元数据。
            newTl.m_metadata = *cmd.metadataOverride;
        } else if ( std::abs(newTl.m_timestamp - oldTl.m_timestamp) > 1e-6 ) {
            // 仅移动时间时旧 Malody beat 已失效，其他属性继续保留。
            clearMalodyTimingBeatMetadata(newTl.m_metadata);
        }

        auto action = std::make_unique<TimelineAction>(
            TimelineAction::Type::Update, cmd.entity, oldTl, newTl);
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
        m_ctx.isBpmEventsDirty = true;
    }
}

/// @brief 批量更新多个既有 Timeline 事件。
/// @param cmd 多个独立目标及其新时间、值和可选元数据。
/// @details 失效或非法条目逐项跳过，其余变化合并为一次撤销动作。
/// @note 时间和值在极小容差内相同且无元数据覆盖的条目视为无操作。
void ActionController::handleCommand(const CmdUpdateTimelineEvents& cmd)
{
    if ( cmd.events.empty() ) {
        return;
    }

    std::vector<BatchTimelineAction::Entry> entries;
    entries.reserve(cmd.events.size());
    for ( const auto& update : cmd.events ) {
        // 批次允许部分目标在排队期间失效，不因此放弃其他仍有效编辑。
        if ( !m_ctx.timelineRegistry.valid(update.entity) ||
             !m_ctx.timelineRegistry.all_of<TimelineComponent>(
                 update.entity) ) {
            continue;
        }

        const auto oldTimeline =
            m_ctx.timelineRegistry.get<TimelineComponent>(update.entity);
        // 批量编辑逐项过滤负 BPM，避免一项非法值污染整个动作。
        if ( !isValidTimelineValue(oldTimeline.m_effect, update.newValue) ) {
            continue;
        }
        auto newTimeline        = oldTimeline;
        newTimeline.m_timestamp = update.newTime;
        newTimeline.m_value     = update.newValue;
        if ( update.metadataOverride ) {
            newTimeline.m_metadata = *update.metadataOverride;
        } else if ( std::abs(newTimeline.m_timestamp -
                             oldTimeline.m_timestamp) > 1e-6 ) {
            clearMalodyTimingBeatMetadata(newTimeline.m_metadata);
        }

        const bool coreFieldsChanged =
            std::abs(newTimeline.m_timestamp - oldTimeline.m_timestamp) >
                1e-12 ||
            std::abs(newTimeline.m_value - oldTimeline.m_value) > 1e-12;
        if ( !coreFieldsChanged && !update.metadataOverride ) {
            // 时间和值均相同且没有私有元数据替换时不产生空动作条目。
            continue;
        }
        entries.push_back(
            { update.entity, oldTimeline, std::move(newTimeline) });
    }

    if ( entries.empty() ) {
        return;
    }
    auto action = std::make_unique<BatchTimelineAction>(
        std::move(entries), "Batch Timeline Update");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.isBpmEventsDirty = true;
}

/// @brief 修改 BPM，并在同一时间点创建或更新配套保速 SCROLL 事件。
/// @param cmd BPM 实体、新时间、新 BPM 和计算出的 SCROLL 值。
/// @details
/// 配套 SCROLL 以新 BPM 时间为键；存在多个候选时选择实体值最大的最后项，保持
/// 与当前表格覆盖顺序一致。BPM 与 SCROLL 作为一个 BatchTimelineAction 提交。
void ActionController::handleCommand(const CmdUpdateBpmWithKeepSpeedSv& cmd)
{
    // 目标必须仍是 BPM，不能把其他 Timing 类型按 BPM 结构修改。
    auto& registry = m_ctx.timelineRegistry;
    if ( !registry.valid(cmd.bpmEntity) ||
         !registry.all_of<TimelineComponent>(cmd.bpmEntity) ) {
        return;
    }

    const auto bpmBefore = registry.get<TimelineComponent>(cmd.bpmEntity);
    if ( bpmBefore.m_effect != ::MMM::TimingEffect::BPM ) {
        return;
    }
    // 保速联动仍是 BPM 修改入口，负数不得连带生成或修改 SV。
    if ( !isValidTimelineValue(::MMM::TimingEffect::BPM, cmd.newBpm) ||
         !isValidTimelineValue(::MMM::TimingEffect::SCROLL, cmd.scrollValue) ) {
        return;
    }

    auto bpmAfter = bpmBefore;
    // BPM 的时间和值作为同一 before/after 组件更新。
    bpmAfter.m_timestamp = cmd.newTime;
    bpmAfter.m_value     = cmd.newBpm;
    if ( std::abs(bpmAfter.m_timestamp - bpmBefore.m_timestamp) > 1e-6 ) {
        clearMalodyTimingBeatMetadata(bpmAfter.m_metadata);
    }

    std::vector<BatchTimelineAction::Entry> entries;
    // 批次固定最多包含目标 BPM 和一个同时间 SCROLL。
    entries.reserve(2U);
    entries.push_back({ cmd.bpmEntity, bpmBefore, std::move(bpmAfter) });

    entt::entity companionScrollEntity = entt::null;
    const auto   timelines = registry.view<const TimelineComponent>();
    for ( const auto entity : timelines ) {
        // 全表寻找新时间点的配套 SCROLL，不复用旧 BPM 时间上的无关事件。
        const auto& timeline = timelines.get<const TimelineComponent>(entity);
        if ( timeline.m_effect == ::MMM::TimingEffect::SCROLL &&
             std::abs(timeline.m_timestamp - cmd.newTime) <= 1e-6 &&
             (companionScrollEntity == entt::null ||
              entt::to_integral(entity) >
                  entt::to_integral(companionScrollEntity)) ) {
            companionScrollEntity = entity;
        }
    }

    if ( companionScrollEntity != entt::null ) {
        // 现有配套事件保留格式私有元数据，只在时间变化时清理派生 beat。
        const auto scrollBefore =
            registry.get<TimelineComponent>(companionScrollEntity);
        auto scrollAfter        = scrollBefore;
        scrollAfter.m_timestamp = cmd.newTime;
        scrollAfter.m_value     = cmd.scrollValue;
        if ( std::abs(scrollAfter.m_timestamp - scrollBefore.m_timestamp) >
             1e-12 ) {
            clearMalodyTimingBeatMetadata(scrollAfter.m_metadata);
        }
        entries.push_back(
            { companionScrollEntity, scrollBefore, std::move(scrollAfter) });
    } else {
        // 没有候选时创建新 SCROLL，并与 BPM 更新共用一次撤销。
        TimelineComponent newScroll{ cmd.newTime,
                                     ::MMM::TimingEffect::SCROLL,
                                     cmd.scrollValue };
        entries.push_back({ entt::null, std::nullopt, std::move(newScroll) });
    }

    auto action = std::make_unique<BatchTimelineAction>(std::move(entries),
                                                        "BPM Keep Speed SV");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.isBpmEventsDirty = true;
}

/// @brief 删除单个有效 Timeline 实体。
/// @param cmd 待删除实体。
/// @note 删除动作保存完整旧组件，支持原位置撤销恢复。
/// @details
/// TimelineAction 负责销毁实体和标记 Timing 同步；处理器只在成功提交后失效
/// bpmEvents 指针缓存，删除非 BPM 时同样采用统一保守策略。
void ActionController::handleCommand(const CmdDeleteTimelineEvent& cmd)
{
    // 无效实体是幂等无操作，命令排队期间可能已被其他批次删除。
    if ( m_ctx.timelineRegistry.valid(cmd.entity) ) {
        auto oldTl  = m_ctx.timelineRegistry.get<TimelineComponent>(cmd.entity);
        auto action = std::make_unique<TimelineAction>(
            TimelineAction::Type::Delete, cmd.entity, oldTl, std::nullopt);
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
        m_ctx.isBpmEventsDirty = true;
    }
}

/// @brief 创建单个 Timeline 事件。
/// @param cmd 目标时间、类型和值。
/// @note 非有限值或负 BPM 被统一拒绝。
/// @details
/// 命令时间由上游定位逻辑提供，本入口保持其值；创建动作以 null 实体表示需要在
/// execute 阶段分配，随后可由同一动作撤销和重做。
void ActionController::handleCommand(const CmdCreateTimelineEvent& cmd)
{
    // 所有单项创建入口统一拒绝负 BPM 与非有限数值。
    if ( !isValidTimelineValue(cmd.type, cmd.value) ) return;
    TimelineComponent newTl{ cmd.time, cmd.type, cmd.value };
    // 新建条目使用 null 实体，由 TimelineAction 执行时创建稳定句柄。
    auto action = std::make_unique<TimelineAction>(
        TimelineAction::Type::Create, entt::null, std::nullopt, newTl);
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.isBpmEventsDirty = true;
}

/// @brief 批量创建多个 Timeline 事件。
/// @param cmd 待创建事件列表。
/// @details 单个非法值被跳过，其余合法事件合并成一次 Paste 撤销动作。
/// @note 每个条目的 TimingMetadata 随值组件复制，格式来源信息不会丢失。
void ActionController::handleCommand(const CmdCreateTimelineEvents& cmd)
{
    if ( cmd.events.empty() ) {
        return;
    }

    std::vector<BatchTimelineAction::Entry> entries;
    // 预留命令容量，过滤非法项不会要求容器缩容。
    entries.reserve(cmd.events.size());
    for ( const auto& event : cmd.events ) {
        // 批量创建只跳过非法项，合法的零 BPM 与其他事件继续提交。
        if ( !isValidTimelineValue(event.type, event.value) ) continue;
        entries.push_back(
            { entt::null,
              std::nullopt,
              TimelineComponent{
                  event.time, event.type, event.value, event.metadata } });
    }

    if ( entries.empty() ) return;
    // 全部非法时不污染 ActionStack；至少一项合法才提交动作。
    auto action =
        std::make_unique<BatchTimelineAction>(std::move(entries), "Paste");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.isBpmEventsDirty = true;
}

/// @brief 用外部 Timing 列表替换当前谱面的全部 BPM 红线。
/// @param cmd 来源 Timing 及是否保留现有非 BPM 事件。
/// @details
/// 来源只采纳有效 BPM，时间从毫秒换算为秒并规范 BPM 边界。可选保留当前 SV 等
/// 非 BPM 事件；第一条有效新 BPM 同步成为 afterPreferenceBpm。完整 before/after
/// 列表和首选 BPM 共同进入 ReplaceTimelinesAction。
///
/// 该入口不会导入来源中的其他 TimingEffect，即使 keepNonBpmTimings 为 false；
/// 该标志只决定是否保留目标会话已有非 BPM 事件。至少需要一个有效来源 BPM，
/// 否则拒绝动作，避免因为空或损坏输入意外清空全部红线。
void ActionController::handleCommand(const CmdReplaceBeatmapTimings& cmd)
{
    // 空会话没有可关联的首选 BPM 元数据，拒绝替换。
    if ( !m_ctx.currentBeatmap ) {
        return;
    }

    const std::vector<TimelineComponent> before =
        collectSortedTimelineComponents(m_ctx);
    std::vector<TimelineComponent> after;
    if ( cmd.keepNonBpmTimings ) {
        // 只复制非 BPM 现有事件，新 BPM 始终来自命令输入。
        for ( const auto& timeline : before ) {
            if ( timeline.m_effect != ::MMM::TimingEffect::BPM ) {
                after.push_back(timeline);
            }
        }
    }

    double afterPreferenceBpm =
        m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm > 0.0
            ? m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm
            : 120.0;
    bool hasBpm = false;
    for ( const auto& timing : cmd.timings ) {
        // 外部列表中的非 BPM 数据不由此入口导入。
        if ( timing.m_timingEffect != ::MMM::TimingEffect::BPM ) {
            continue;
        }

        double bpm = timing.m_timingEffectParameter > 0.0
                         ? timing.m_timingEffectParameter
                         : timing.m_bpm;
        if ( !(bpm > 0.0) || !std::isfinite(bpm) ||
             !std::isfinite(timing.m_timestamp) ) {
            continue;
        }

        bpm = ::MMM::normalizeBpmValue(bpm);
        // 有限越界 BPM 夹到最近合法边界，NaN 已在上方过滤。
        TimelineComponent timeline;
        timeline.m_timestamp = timing.m_timestamp / 1000.0;
        timeline.m_effect    = ::MMM::TimingEffect::BPM;
        timeline.m_value     = bpm;
        timeline.m_metadata  = timing.m_metadata;
        after.push_back(timeline);
        if ( !hasBpm ) {
            // 排序前的首个有效来源 BPM 定义谱面首选值。
            afterPreferenceBpm = bpm;
            hasBpm             = true;
        }
    }

    if ( !hasBpm ) {
        // 没有有效 BPM 时不提交仅删除旧红线的破坏性替换。
        return;
    }

    after = normalizeReplacementTimelines(std::move(after));
    // 规范化处理同时间重复 BPM，并给动作稳定的目标顺序。
    const double beforePreferenceBpm =
        m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm > 0.0
            ? m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm
            : afterPreferenceBpm;

    auto action = std::make_unique<ReplaceTimelinesAction>(
        before, after, beforePreferenceBpm, afterPreferenceBpm);
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.isBpmEventsDirty = true;
}

/// @brief 设置根 Note 或 Polyline 指定子节点的内嵌注释文本。
/// @param cmd 目标实体、可选子索引和新文本。
/// @details
/// 子实体先解析为父 Polyline 与稳定数组索引；持久化值写入父组件内嵌子数组，
/// 同时更新对应 ECS 子实体投影。两项合并为一个 BatchNoteAction，并将变更类别
/// 明确设为 Annotations，避免协作层误报几何对象变化。
///
/// 注释允许为空以表达清除，但字节长度不能超过领域上限。命令给出的 subIndex
/// 优先；未给出时从子投影组件恢复。根或子目标在排队期间失效都会返回明确状态，
/// 不尝试按时间和轨道猜测替代对象。
///
/// 某些导入谱面可能缺少对应 ECS 子实体，此时父内嵌值仍可更新并持久化；只有找到
/// 同父同索引投影时才追加子条目。这样数据正确性不依赖当前投影是否完整。
void ActionController::handleCommand(const CmdSetNoteAnnotation& cmd)
{
    // 大小上限和实体有效性在任何组件复制前统一验证。
    if ( cmd.annotation.size() > ::MMM::MAX_NOTE_ANNOTATION_BYTES ||
         cmd.entity == entt::null || !m_ctx.noteRegistry.valid(cmd.entity) ||
         !m_ctx.noteRegistry.all_of<NoteComponent>(cmd.entity) ) {
        m_ctx.lastActionMessage = "注释目标无效或内容超过 8192 字节";
        return;
    }

    entt::entity rootEntity = cmd.entity;
    std::int32_t subIndex   = cmd.subIndex;
    const auto&  requested  = m_ctx.noteRegistry.get<NoteComponent>(cmd.entity);
    if ( requested.m_isSubNote ) {
        // 命令未显式提供子索引时采用投影实体记录的原数组位置。
        rootEntity = requested.m_parentPolyline;
        if ( subIndex < 0 ) subIndex = requested.m_subIndex;
    }
    if ( rootEntity == entt::null || !m_ctx.noteRegistry.valid(rootEntity) ||
         !m_ctx.noteRegistry.all_of<NoteComponent>(rootEntity) ) {
        m_ctx.lastActionMessage = "注释目标已经失效";
        return;
    }

    const auto beforeRoot = m_ctx.noteRegistry.get<NoteComponent>(rootEntity);
    // 根组件始终是持久化 before/after 的主体。
    auto afterRoot = beforeRoot;
    if ( subIndex < 0 ) {
        if ( beforeRoot.m_annotation == cmd.annotation ) return;
        afterRoot.m_annotation = cmd.annotation;
    } else {
        // 子索引只能用于 Polyline 且必须仍落在当前内嵌数组范围内。
        const auto index = static_cast<std::size_t>(subIndex);
        if ( beforeRoot.m_type != ::MMM::NoteType::POLYLINE ||
             index >= beforeRoot.m_subNotes.size() ) {
            m_ctx.lastActionMessage = "折线子音符注释目标已经失效";
            return;
        }
        if ( beforeRoot.m_subNotes[index].annotation == cmd.annotation ) return;
        afterRoot.m_subNotes[index].annotation = cmd.annotation;
    }

    std::vector<BatchNoteAction::Entry> entries;
    entries.push_back({
        .entity = rootEntity,
        .before = beforeRoot,
        .after  = afterRoot,
    });

    if ( subIndex >= 0 ) {
        // 为保持当前帧投影一致，定位同父同索引的 ECS 子实体并加入批次。
        const auto view = m_ctx.noteRegistry.view<const NoteComponent>();
        for ( const auto entity : view ) {
            const auto& note = view.get<const NoteComponent>(entity);
            if ( !note.m_isSubNote || note.m_parentPolyline != rootEntity ||
                 note.m_subIndex != subIndex ) {
                continue;
            }
            auto afterChild = note;
            // 子实体只更新镜像注释字段，父关系、几何和身份保持不变。
            afterChild.m_annotation = cmd.annotation;
            entries.push_back({
                .entity = entity,
                .before = note,
                .after  = std::move(afterChild),
            });
            break;
        }
    }

    auto action = std::make_unique<BatchNoteAction>(
        std::move(entries),
        "Set Note Annotation",
        ::MMM::BeatmapMutationFlags::Annotations);
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage = "已更新物件注释";
}

/// @brief 新增谱面级批注或按稳定 ID 更新既有批注正文。
/// @param cmd 批注身份、目标类型、目标实体、作者和正文。
/// @details
/// annotationId 非空表示修改，只允许更新仍存在项的正文。空 ID 表示新增，需规范
/// Creator 身份并检查数量上限，再按时间戳、Sample 或 Player Object 三种目标
/// 建立稳定 targetId 和毫秒时间锚点。
///
/// 物件目标若尚无协作 ID，会在创建批注前生成并把对应 ECS 域标脏，使批注引用
/// 能随谱面持久化。最终 before/after optional 交给 BeatmapAnnotationAction。
///
/// 修改既有批注刻意只更新正文，保持原作者、目标类型、targetId 和时间锚点不变。
/// 需要改变目标时应删除并新增，避免一个稳定批注 ID 在协作历史中突然改指对象。
///
/// PLAYER_OBJECT 的子节点目标取父内嵌 SubNote collaborationId，而不是临时 ECS
/// 子实体值；AUDIO_SAMPLE 取 SampleComponent collaborationId。时间锚点是创建时
/// 快照，用于排序和目标缺失后的上下文展示，不代替稳定 targetId。
void ActionController::handleCommand(const CmdUpsertBeatmapAnnotation& cmd)
{
    // 正文为空或超限都不能创建可序列化批注。
    if ( !m_ctx.currentBeatmap || cmd.content.empty() ||
         cmd.content.size() > ::MMM::MAX_BEATMAP_ANNOTATION_CONTENT_BYTES ) {
        m_ctx.lastActionMessage = "批注正文不能为空且不能超过 8192 字节";
        return;
    }

    std::optional<::MMM::BeatmapAnnotation> before;
    std::optional<::MMM::BeatmapAnnotation> after;
    if ( !cmd.annotationId.empty() ) {
        // 修改路径不允许用不存在 ID 隐式新增，避免丢失原作者和目标。
        const auto found =
            std::find_if(m_ctx.currentBeatmap->m_annotations.begin(),
                         m_ctx.currentBeatmap->m_annotations.end(),
                         [&](const auto& annotation) {
                             return annotation.m_id == cmd.annotationId;
                         });
        if ( found == m_ctx.currentBeatmap->m_annotations.end() ) {
            m_ctx.lastActionMessage = "要修改的批注已经不存在";
            return;
        }
        if ( found->m_content == cmd.content ) return;
        before           = *found;
        after            = *found;
        after->m_content = cmd.content;
    } else {
        // 新增路径先验证作者和全谱数量，再生成新的稳定批注 ID。
        const std::string author = Config::normalizeCreatorIdentity(cmd.author);
        if ( author.empty() ) {
            m_ctx.lastActionMessage = "未设置默认 Creator，无法添加批注";
            return;
        }
        if ( m_ctx.currentBeatmap->m_annotations.size() >=
             ::MMM::MAX_BEATMAP_ANNOTATION_COUNT ) {
            m_ctx.lastActionMessage = "当前谱面批注数量已达到上限";
            return;
        }

        ::MMM::BeatmapAnnotation annotation;
        annotation.m_id         = makeNoteCollaborationId();
        annotation.m_targetKind = cmd.targetKind;
        annotation.m_author     = author;
        annotation.m_content    = cmd.content;

        if ( cmd.targetKind == ::MMM::BeatmapAnnotationTargetKind::TIMESTAMP ) {
            // 自由时间批注只需要有限非负秒时间，持久化时换算毫秒。
            if ( !std::isfinite(cmd.timestamp) || cmd.timestamp < 0.0 ) {
                m_ctx.lastActionMessage = "批注时间戳无效";
                return;
            }
            annotation.m_timestamp = cmd.timestamp * 1000.0;
        } else if ( cmd.targetKind ==
                    ::MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE ) {
            // Sample 目标必须来自 Sample Registry，不能用同值 Note
            // 实体句柄代替。
            if ( cmd.objectKind != ChartObjectKind::AudioSample ||
                 cmd.entity == entt::null ||
                 !m_ctx.sampleRegistry.valid(cmd.entity) ||
                 !m_ctx.sampleRegistry.all_of<SampleComponent>(cmd.entity) ) {
                m_ctx.lastActionMessage = "自动采样批注目标已经失效";
                return;
            }
            auto& sample =
                m_ctx.sampleRegistry.get<SampleComponent>(cmd.entity);
            ensureSampleCollaborationIdentity(sample);
            // targetId 引用稳定身份，时间快照用于排序和目标缺失时展示。
            annotation.m_targetId    = sample.m_collaborationId;
            annotation.m_timestamp   = sample.effectiveTime() * 1000.0;
            m_ctx.m_needsSamplesSync = true;
        } else if ( cmd.targetKind ==
                    ::MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT ) {
            // Player Object 目标可以是根 Note 或 Polyline 子投影实体。
            if ( cmd.objectKind != ChartObjectKind::PlayerNote ||
                 cmd.entity == entt::null ||
                 !m_ctx.noteRegistry.valid(cmd.entity) ||
                 !m_ctx.noteRegistry.all_of<NoteComponent>(cmd.entity) ) {
                m_ctx.lastActionMessage = "玩家物件批注目标已经失效";
                return;
            }

            entt::entity rootEntity = cmd.entity;
            std::int32_t subIndex   = cmd.subIndex;
            const auto&  requested =
                m_ctx.noteRegistry.get<const NoteComponent>(cmd.entity);
            if ( requested.m_isSubNote ) {
                // 子投影解析回父实体及持久化数组索引。
                rootEntity = requested.m_parentPolyline;
                if ( subIndex < 0 ) subIndex = requested.m_subIndex;
            }
            if ( rootEntity == entt::null ||
                 !m_ctx.noteRegistry.valid(rootEntity) ||
                 !m_ctx.noteRegistry.all_of<NoteComponent>(rootEntity) ) {
                m_ctx.lastActionMessage = "玩家物件批注目标已经失效";
                return;
            }
            auto& root = m_ctx.noteRegistry.get<NoteComponent>(rootEntity);
            ensureNoteCollaborationIdentity(root);
            // 根和所有子节点的稳定 ID 由统一助手按结构确保存在。
            if ( subIndex >= 0 ) {
                // 子目标引用内嵌 SubNote 的协作 ID 和节点时间。
                const auto index = static_cast<std::size_t>(subIndex);
                if ( root.m_type != ::MMM::NoteType::POLYLINE ||
                     index >= root.m_subNotes.size() ) {
                    m_ctx.lastActionMessage = "折线子物件批注目标已经失效";
                    return;
                }
                const auto& sub        = root.m_subNotes[index];
                annotation.m_targetId  = sub.collaborationId;
                annotation.m_timestamp = sub.timestamp * 1000.0;
            } else {
                // 根目标直接使用根 ID 和根锚点时间。
                annotation.m_targetId  = root.m_collaborationId;
                annotation.m_timestamp = root.m_timestamp * 1000.0;
            }
            m_ctx.m_needsNotesSync = true;
        } else {
            m_ctx.lastActionMessage = "批注目标类型无效";
            return;
        }
        after = std::move(annotation);
        // 新增用空 before 与完整 after 表达，撤销时按 ID 删除。
    }

    auto action = std::make_unique<BeatmapAnnotationAction>(
        std::move(before), std::move(after), "Edit Beatmap Annotation");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage = "已保存谱面批注";
}

/// @brief 按稳定 ID 删除一条谱面级批注。
/// @param cmd 待删除批注 ID。
/// @note 删除动作保存完整旧值，撤销时可恢复原目标和作者。
/// @details
/// 找不到目标时向状态栏报告过期命令，不按 vector 位置删除其他项。成功动作以
/// before 有值、after 为空表达，BeatmapAnnotationAction 负责刷新渲染缓存。
void ActionController::handleCommand(const CmdRemoveBeatmapAnnotation& cmd)
{
    // 空会话或空 ID 是无操作，不创建错误反馈动作。
    if ( !m_ctx.currentBeatmap || cmd.annotationId.empty() ) return;
    const auto found =
        std::find_if(m_ctx.currentBeatmap->m_annotations.begin(),
                     m_ctx.currentBeatmap->m_annotations.end(),
                     [&](const auto& annotation) {
                         return annotation.m_id == cmd.annotationId;
                     });
    if ( found == m_ctx.currentBeatmap->m_annotations.end() ) {
        m_ctx.lastActionMessage = "要删除的批注已经不存在";
        return;
    }
    auto action = std::make_unique<BeatmapAnnotationAction>(
        *found, std::nullopt, "Remove Beatmap Annotation");
    m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    m_ctx.lastActionMessage = "已删除谱面批注";
}

/// @brief 按选择的数据域用来源谱面替换当前会话内容。
/// @param cmd 来源 BeatMap、替换类别和协作权威同步信息。
/// @details
/// 函数在 SessionRegistry 互斥区内读取和重建 ECS，避免 UI 元数据窗口同时持有
/// EnTT View。空来源或没有选择任何类别时保持无操作。
///
/// 已准备对象编码基线的纯权威对象差量走增量路径：只更新指定稳定 ID，对象容器
/// 随后与来源 BeatMap 交换，并把旧容器交给线程池释放。该路径不创建撤销动作，
/// 但会把 ActionStack 标为未保存。
///
/// 其他替换先按类别收集 before/after 值快照。元数据替换保留当前项目文件和资源
/// 语义，并预计算玩家改键导致的 Sample 绝对轨道迁移；任一轨道溢出会在提交前
/// 拒绝整次替换。
///
/// 本地替换进入 ActionStack，可正常撤销。权威远端替换直接 execute；若还包含
/// Timing、元数据或 Sample 等无法安全重放本地历史的数据域，则清空撤销栈。
/// 纯 Note/批注权威更新保留可按稳定 ID 变换的本地历史。
///
/// 数据域之间存在以下依赖：
///
/// - Objects 替换重建正式 Note，但保留本地 Draft；Polyline 子投影随父重建。
/// - Timelines 替换需要同步来源首选 BPM，即使没有整体替换 Metadata。
/// - Metadata 替换可以改变玩家轨道数，因此可能附带 AudioSamples 轨道迁移。
/// - AudioSamples 整体替换直接重建 Sample Registry，不复用旧交互实体。
/// - Annotations 指谱面级批注列表，不等同于 NoteComponent 的内嵌注释字段。
///
/// before/after 快照只为启用的数据域填充。ReplaceBeatmapDataAction
/// 仍接收完整参数， 但 apply
/// 受对应布尔开关保护；这让一次组合替换拥有统一执行顺序，而不需要为
/// 每种类别组合定义不同动作类型。
///
/// 元数据来源不能接管目标谱面的 map_path、主音频、背景或 BGM 轨道数。这些字段
/// 绑定当前项目资源环境；若直接复制，远端或导入谱面的机器路径会破坏目标会话。
/// OSU 私有属性中重复表达的资源字段也必须同步校正。
///
/// Sample 轨道迁移以 beforeTrackCount 恢复相对 BGM lane，再加 afterTrackCount。
/// 中间值使用 uint64 检测上溢，验证完成前不修改任何组件。迁移表保存实体及双向
/// 轨道值，动作撤销不依赖重新计算或当时的轨道数。
///
/// 纯对象差量已经由后台编码基线证明只涉及给定稳定身份，因此可以增量维护缓存。
/// 完整对象替换则采用确定排序的值快照，并由 replaceNoteComponents 统一处理实体
/// 复用与缓存失效。两条路径最终都让 currentBeatmap 持有最新领域对象。
///
/// 权威同步直接执行后调用 markDirty，而不是 markSaved：服务端状态已经成为编辑
/// 基线，但本地项目文件尚未写盘。是否自动保存由上层 Session 命令聚合策略决定。
/// @warning 低频整体同步路径：可能复制并重建所选数据域，不得每帧调用。
void ActionController::handleCommand(const CmdReplaceBeatmapData& cmd)
{
    // 元数据编辑窗口仍会在 UI 线程同步读取 ECS；整体替换必须与该读取共用
    // SessionRegistry 锁，避免远端提交清空 Registry 时使 EnTT View 失效。
    std::lock_guard<std::recursive_mutex> sessionLock(
        EditorEngine::instance().getSessionMutex());
    if ( !m_ctx.currentBeatmap || !cmd.sourceBeatmap ) {
        // 当前和来源必须同时存在，避免构造只含一侧的不可撤销快照。
        return;
    }
    if ( !cmd.replaceObjects && !cmd.replaceTimelines && !cmd.replaceMetadata &&
         !cmd.replaceAudioSamples && !cmd.replaceAnnotations ) {
        // 空类别命令不改变保存点或状态栏。
        return;
    }

    const bool appliesPreparedObjectDelta =
        // 增量优化只适用于纯对象权威同步且调用方已经建立编码基线和身份集合。
        cmd.authoritativeRemote && cmd.replaceObjects &&
        !cmd.replaceTimelines && !cmd.replaceMetadata &&
        !cmd.replaceAudioSamples && !cmd.replaceAnnotations &&
        cmd.objectEncodingBaselinePrepared &&
        cmd.objectDeltaIdentities.has_value();
    if ( appliesPreparedObjectDelta ) {
        // 先更新 ECS 可见组件，再接管后台已物化的领域对象容器。
        applyIncrementalNoteComponents(
            m_ctx, *cmd.sourceBeatmap, *cmd.objectDeltaIdentities);
        if ( m_ctx.currentBeatmap.get() != cmd.sourceBeatmap.get() ) {
            // 两个 shared_ptr 指向不同对象时交换重容器，当前 BeatMap
            // 保持外层身份。
            std::swap(m_ctx.currentBeatmap->m_noteData,
                      cmd.sourceBeatmap->m_noteData);
            std::swap(m_ctx.currentBeatmap->m_allNotes,
                      cmd.sourceBeatmap->m_allNotes);
            retireBeatmapObjectStorage(cmd.sourceBeatmap);
            // sourceBeatmap 此时持有旧容器，异步释放不再阻塞逻辑线程。
        }
        m_ctx.m_needsNotesSync = false;
        // 领域容器已经来自权威来源，不需要再从刚更新的 ECS 反向同步一次。
        m_ctx.actionStack.markDirty();
        m_ctx.lastActionMessage = fmt::format(
            "{} {}", TR("ui.status.category.action"), "联机物件增量更新");
        return;
    }

    std::vector<NoteComponent> beforeNotes;
    std::vector<NoteComponent> afterNotes;
    if ( cmd.replaceObjects ) {
        // 正式根 Note 和 Polyline 子数组形成对象域的双向快照。
        beforeNotes = collectEditableNoteComponents(m_ctx);
        afterNotes  = makeNoteComponentsFromBeatMap(*cmd.sourceBeatmap);
    }

    std::vector<TimelineComponent> beforeTimelines;
    std::vector<TimelineComponent> afterTimelines;
    if ( cmd.replaceTimelines ) {
        // Timeline 快照均规范化排序，避免 Registry 迭代顺序进入动作语义。
        beforeTimelines = collectSortedTimelineComponents(m_ctx);
        afterTimelines  = makeTimelineComponentsFromBeatMap(*cmd.sourceBeatmap);
    }

    std::vector<SampleComponent> beforeSamples;
    std::vector<SampleComponent> afterSamples;
    if ( cmd.replaceAudioSamples ) {
        // Sample 使用独立 Registry 和领域数组，按值完整收集。
        beforeSamples = collectSampleComponents(m_ctx);
        afterSamples  = makeSampleComponentsFromBeatMap(*cmd.sourceBeatmap);
    }

    std::vector<::MMM::BeatmapAnnotation> beforeAnnotations;
    std::vector<::MMM::BeatmapAnnotation> afterAnnotations;
    if ( cmd.replaceAnnotations ) {
        // 谱面级批注直接位于 BeatMap，不与 Note 内嵌注释合并。
        beforeAnnotations = m_ctx.currentBeatmap->m_annotations;
        afterAnnotations  = cmd.sourceBeatmap->m_annotations;
    }

    BeatmapMetadataSnapshot beforeMetadata =
        // 元数据快照即使未选择替换也用于构造统一动作参数，但 apply 不访问。
        makeMetadataSnapshot(*m_ctx.currentBeatmap);
    BeatmapMetadataSnapshot afterMetadata = makeReplacementMetadataSnapshot(
        *m_ctx.currentBeatmap, *cmd.sourceBeatmap);
    const std::int32_t beforeTrackCount = std::max(1, m_ctx.trackCount);
    const std::int32_t afterTrackCount =
        cmd.replaceMetadata
            ? std::max(1, cmd.sourceBeatmap->m_baseMapMetadata.track_count)
            : beforeTrackCount;
    const std::int32_t persistentBgmTrackCount =
        // 来源谱面不能覆盖目标项目已经持久化的 BGM 轨道数量。
        std::max(0, m_ctx.bgmTrackCount);
    beforeMetadata.baseMeta.track_count     = beforeTrackCount;
    beforeMetadata.baseMeta.bgm_track_count = persistentBgmTrackCount;
    afterMetadata.baseMeta.track_count      = afterTrackCount;
    afterMetadata.baseMeta.bgm_track_count  = persistentBgmTrackCount;

    std::vector<TrackCountAction::SampleTrackChange> sampleTrackChanges;
    if ( cmd.replaceMetadata && beforeTrackCount != afterTrackCount ) {
        // 玩家轨道数变化前预计算每个现有 Sample 保持相对 BGM lane
        // 的新绝对轨道。
        const auto sampleView =
            m_ctx.sampleRegistry.view<const SampleComponent>();
        sampleTrackChanges.reserve(sampleView.size());
        for ( const auto entity : sampleView ) {
            const auto& sample = sampleView.get<const SampleComponent>(entity);
            const std::uint32_t bgmLane =
                sample.m_track >= static_cast<std::uint32_t>(beforeTrackCount)
                    ? sample.m_track -
                          static_cast<std::uint32_t>(beforeTrackCount)
                    : 0U;
            // 旧异常 Sample 落在玩家区时保守归入首个 BGM lane，避免无符号下溢。
            const std::uint64_t afterTrack =
                static_cast<std::uint64_t>(afterTrackCount) + bgmLane;
            if ( afterTrack > static_cast<std::uint64_t>(
                                  std::numeric_limits<std::uint32_t>::max()) ) {
                m_ctx.lastActionMessage =
                    "替换谱面元数据会导致自动采样轨道索引溢出";
                return;
                // 任何一项溢出都会拒绝整批，前面只收集值快照尚未修改 ECS。
            }
            sampleTrackChanges.push_back({
                .entity      = entity,
                .beforeTrack = sample.m_track,
                .afterTrack  = static_cast<std::uint32_t>(afterTrack),
            });
        }
    }

    const double beforePreferenceBpm = ::MMM::normalizeBpmValue(
        m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm);
    const double afterPreferenceBpm = ::MMM::normalizeBpmValue(
        // 两侧首选 BPM 都经过统一边界语义，供 Timeline 替换的撤销元数据使用。
        cmd.sourceBeatmap->m_baseMapMetadata.preference_bpm);

    auto action = std::make_unique<ReplaceBeatmapDataAction>(
        cmd.replaceObjects,
        cmd.replaceTimelines,
        cmd.replaceMetadata,
        cmd.replaceAudioSamples,
        cmd.replaceAnnotations,
        cmd.authoritativeRemote,
        std::move(beforeNotes),
        std::move(afterNotes),
        std::move(beforeTimelines),
        std::move(afterTimelines),
        std::move(beforeSamples),
        std::move(afterSamples),
        std::move(beforeAnnotations),
        std::move(afterAnnotations),
        std::move(beforeMetadata),
        std::move(afterMetadata),
        std::move(sampleTrackChanges),
        beforePreferenceBpm,
        afterPreferenceBpm);
    if ( cmd.authoritativeRemote ) {
        // 权威状态不进入本地撤销栈，否则 Undo 会尝试恢复服务端已淘汰状态。
        const bool preservesNoteHistory =
            (cmd.replaceObjects || cmd.replaceAnnotations) &&
            !cmd.replaceTimelines && !cmd.replaceMetadata &&
            !cmd.replaceAudioSamples;
        if ( !preservesNoteHistory ) {
            // 跨域替换会改变本地动作解释基准，只能丢弃历史保证后续安全。
            m_ctx.actionStack.clear();
        }
        action->execute(m_ctx);
        // 直接执行后仍标记未保存，使本地持久化状态反映权威内容变化。
        m_ctx.actionStack.markDirty();
    } else {
        // 用户主动的数据来源替换是普通可撤销编辑。
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
    }

    m_ctx.lastActionMessage =
        fmt::format("{} {}", TR("ui.status.category.action"), "数据来源替换");
}

/// @brief 将已选 Note 及完整 Polyline 结构吸附到皮肤常用分拍。
/// @param cmd 无附加参数的对齐命令。
/// @details
/// BPM 事件先按时间排序，每个目标时间在所属 BPM 段内枚举皮肤配置的分拍除数。
/// 误差乘以 divisor 进行加权，避免更密分拍仅因步长更小总是胜出；候选不会越过
/// 下一 BPM 红线。是否允许首红线之前对齐由视觉配置决定。
///
/// 选择闭包必须包含整个 Polyline：选中父时加入全部子实体，选中任一子时加入父
/// 及所有兄弟。父组件内嵌子数组是持久化权威，即使某些投影子实体缺失也独立
/// 对齐；随后按新时间稳定排序并更新实际存在子实体的 subIndex。
///
/// 全部 before/after 组件进入一个 BatchNoteAction，普通 Note、Hold、Flick、
/// Polyline 父和子可在一次撤销中恢复原时间与顺序。
///
/// BPM 分段定位使用半开区间 `[current, next)`。首红线之前若允许显示前置分拍，
/// 则使用首个 BPM 向前外推；最后红线之后使用末尾 BPM。候选吸附点最大只能等于
/// 下一红线，不能用当前 BPM 跨过节奏变化边界。
///
/// 每个 divisor 产生 `beatDuration / divisor`
/// 的步长。选择标准不是纯秒误差，而是 `error *
/// divisor`，从而按相对分拍粒度比较候选。非法非正除数被跳过；若皮肤
/// 没有合法除数，时间保持原值。
///
/// Hold 和子 Hold 的起止时间分别吸附，duration 由吸附后的差重新计算并夹到零。
/// 不能只吸附起点后保留旧秒时长，因为跨越 BPM 变化时旧时长不再对应同一拍数。
///
/// 选择闭包采用迭代到稳定集合，而非假设父实体先于子实体出现。这样无论用户直接
/// 选中父、点击某个子节点，还是 Registry 迭代顺序变化，都能得到相同完整折线。
///
/// 对齐后节点可能交换时间顺序。父 m_subNotes 依新时间排序，同时间使用原下标
/// 保持稳定；实际存在的子实体同步新的 m_subIndex。父锚点取排序后首节点，确保
/// 范围查询、渲染和下一次编辑使用同一结构顺序。
/// @warning 用户显式低频路径：收集 BPM 并闭包扩展选择，不得每帧调用。
void ActionController::handleCommand(const CmdAlignSelectedToCommonBeats& cmd)
{
    // 空谱面没有首选 BPM 和持久化目标，保持无操作。
    if ( !m_ctx.currentBeatmap ) return;

    // 常用分拍来自当前皮肤，允许不同主题提供自己的创作粒度集合。
    auto getCommonDivisorsFromSkin = []() -> std::vector<int> {
        return MMM::Config::SkinManager::instance().getCommonDivisors();
    };

    std::vector<int> commonDivisors = getCommonDivisorsFromSkin();

    // 收集 BPM 指针并排序；函数期间 Registry 不发生修改，指针保持有效。
    auto tlView = m_ctx.timelineRegistry.view<const TimelineComponent>();
    std::vector<const TimelineComponent*> bpmEvents;
    for ( auto entity : tlView ) {
        const auto& tc = tlView.get<const TimelineComponent>(entity);
        if ( tc.m_effect == ::MMM::TimingEffect::BPM ) {
            bpmEvents.push_back(&tc);
        }
    }
    std::stable_sort(
        bpmEvents.begin(), bpmEvents.end(), [](const auto* a, const auto* b) {
            return a->m_timestamp < b->m_timestamp;
        });

    auto getAlignedTime = [&](double rawTime) -> double {
        // 没有 BPM 时无法定义分拍长度，保留原秒时间。
        if ( bpmEvents.empty() ) return rawTime;

        /// @brief 首个 BPM 前是否允许按绘制出的前置分拍线对齐。
        const bool allowBeforeFirstTiming =
            m_ctx.lastConfig.visual.drawBeatLinesBeforeFirstTiming;
        if ( rawTime < bpmEvents[0]->m_timestamp && !allowBeforeFirstTiming )
            // 配置禁止绘制首红线前分拍时，对齐命令也保持相同可见语义。
            return rawTime;

        double bestSnappedTime  = rawTime;
        double minWeightedError = std::numeric_limits<double>::max();

        // 定位包含 rawTime 的 BPM 分段及下一段边界。
        const TimelineComponent* currentBPM = nullptr;
        double                   bpmTime    = 0.0;
        double                   bpmVal     = 120.0;
        double nextBpmTime = std::numeric_limits<double>::infinity();

        if ( rawTime < bpmEvents.front()->m_timestamp ) {
            // 允许首红线前对齐时，使用首个 BPM 向前线性外推。
            currentBPM  = bpmEvents.front();
            bpmTime     = currentBPM->m_timestamp;
            bpmVal      = currentBPM->m_value;
            nextBpmTime = bpmEvents.size() > 1
                              ? bpmEvents[1]->m_timestamp
                              : std::numeric_limits<double>::infinity();
        } else {
            // 正常区域采用半开区间 `[current, next)` 选择当前 BPM。
            for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
                double tBpm  = bpmEvents[i]->m_timestamp;
                double tNext = (i + 1 < bpmEvents.size())
                                   ? bpmEvents[i + 1]->m_timestamp
                                   : std::numeric_limits<double>::infinity();
                if ( rawTime >= tBpm && rawTime < tNext ) {
                    currentBPM  = bpmEvents[i];
                    bpmTime     = tBpm;
                    bpmVal      = currentBPM->m_value;
                    nextBpmTime = tNext;
                    break;
                }
            }
        }

        if ( !currentBPM ) {
            // 时间位于最后一段之后时显式回退末尾 BPM。
            currentBPM  = bpmEvents.back();
            bpmTime     = currentBPM->m_timestamp;
            bpmVal      = currentBPM->m_value;
            nextBpmTime = std::numeric_limits<double>::infinity();
        }

        const double fallbackBpm =
            m_ctx.currentBeatmap
                ? m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm
                : ::MMM::DEFAULT_NORMALIZED_BPM;
        const double bVal = ::MMM::normalizeBpmValue(bpmVal, fallbackBpm);
        // 异常 BPM 按领域边界规范，避免零除或无限步长。

        double beatDuration = 60.0 / bVal;

        for ( int divisor : commonDivisors ) {
            // 非正除数没有分拍定义，跳过皮肤中的异常配置。
            if ( divisor <= 0 ) continue;
            double stepDuration    = beatDuration / divisor;
            double relativeTime    = rawTime - bpmTime;
            double stepCount       = std::round(relativeTime / stepDuration);
            double nearestStepTime = bpmTime + stepCount * stepDuration;

            if ( nearestStepTime > nextBpmTime ) nearestStepTime = nextBpmTime;
            // 吸附点不能越过下一 BPM 边界进入使用另一速率的分段。

            double error         = std::abs(rawTime - nearestStepTime);
            double weightedError = error * static_cast<double>(divisor);
            // 加权让不同密度比较更接近“拍分数误差”，避免偏爱高除数。

            if ( weightedError < minWeightedError ) {
                minWeightedError = weightedError;
                bestSnappedTime  = nearestStepTime;
            }
        }

        return bestSnappedTime;
    };

    const auto alignTimeRange = [&](double& timestamp, double& duration) {
        // 起止点分别吸附，才能正确处理跨 BPM 的 Hold；结果时长最小为零。
        const double alignedStart = getAlignedTime(timestamp);
        const double alignedEnd   = getAlignedTime(timestamp + duration);
        timestamp                 = alignedStart;
        duration                  = std::max(0.0, alignedEnd - alignedStart);
    };

    std::unordered_set<entt::entity> toAlign;
    // 初始集合只包含当前配置下可编辑的已选实体。
    auto noteView =
        m_ctx.noteRegistry.view<InteractionComponent, NoteComponent>();
    for ( auto entity : noteView ) {
        const auto& ic   = noteView.get<InteractionComponent>(entity);
        const auto& note = noteView.get<NoteComponent>(entity);
        if ( ic.isSelected &&
             SessionUtils::isNoteEditable(note, m_ctx.lastConfig.settings) ) {
            toAlign.insert(entity);
        }
    }

    // 闭包扩展保证父、任一子和全部兄弟作为一个完整 Polyline 同时参与。
    bool expanded = true;
    while ( expanded ) {
        // 每轮只遍历当前快照，新增实体在下一轮继续扩展直到集合稳定。
        expanded = false;
        std::vector<entt::entity> currentToAlign(toAlign.begin(),
                                                 toAlign.end());
        for ( auto entity : currentToAlign ) {
            if ( !m_ctx.noteRegistry.valid(entity) ||
                 !m_ctx.noteRegistry.all_of<NoteComponent>(entity) )
                continue;

            const auto& nc = m_ctx.noteRegistry.get<NoteComponent>(entity);
            if ( nc.m_type == ::MMM::NoteType::POLYLINE ) {
                // 父实体加入全部实际存在的投影子实体。
                for ( auto subEnt : m_ctx.noteRegistry.view<NoteComponent>() ) {
                    const auto& subNC =
                        m_ctx.noteRegistry.get<NoteComponent>(subEnt);
                    if ( subNC.m_isSubNote &&
                         subNC.m_parentPolyline == entity ) {
                        if ( toAlign.insert(subEnt).second ) {
                            expanded = true;
                        }
                    }
                }
            } else if ( nc.m_isSubNote && nc.m_parentPolyline != entt::null ) {
                // 子实体先加入父；下一轮再由父加入其余兄弟。
                if ( toAlign.insert(nc.m_parentPolyline).second ) {
                    expanded = true;
                }
            }
        }
    }

    if ( toAlign.empty() ) return;

    std::vector<BatchNoteAction::Entry>             entries;
    std::unordered_map<entt::entity, NoteComponent> originalNotes;
    // 所有动作前值按实体保存，过滤闭包中已经失效的句柄。
    for ( auto entity : toAlign ) {
        if ( m_ctx.noteRegistry.valid(entity) &&
             m_ctx.noteRegistry.all_of<NoteComponent>(entity) ) {
            originalNotes[entity] =
                m_ctx.noteRegistry.get<NoteComponent>(entity);
        }
    }

    std::unordered_map<entt::entity, NoteComponent> newNotes = originalNotes;

    // 先对齐普通根 Note 和实际存在的 Polyline 子投影实体。
    for ( auto& [entity, newNote] : newNotes ) {
        if ( newNote.m_type != ::MMM::NoteType::POLYLINE ) {
            (void)entity;
            alignTimeRange(newNote.m_timestamp, newNote.m_duration);
        }
    }

    // 父 Polyline 持有完整子数组；缺失投影实体的节点仍必须从父值独立对齐。
    for ( auto& [entity, newNote] : newNotes ) {
        if ( newNote.m_type == ::MMM::NoteType::POLYLINE ) {
            struct AlignedSubNote {
                /// @brief 对齐后的持久化子组件。
                NoteComponent::SubNote note;
                /// @brief 存在时对应的 ECS 投影子实体。
                entt::entity childEntity{ entt::null };
                /// @brief 同时间稳定排序使用的原始数组位置。
                std::size_t originalSubIndex{ 0U };
            };

            std::vector<entt::entity> childEntities(
                newNote.m_subNotes.size(),
                static_cast<entt::entity>(entt::null));
            for ( const auto& [otherEnt, otherNote] : newNotes ) {
                // 按 parentPolyline 和旧 subIndex 建立父数组到投影实体的映射。
                if ( otherNote.m_isSubNote &&
                     otherNote.m_parentPolyline == entity &&
                     otherNote.m_subIndex >= 0 ) {
                    const auto subIndex =
                        static_cast<std::size_t>(otherNote.m_subIndex);
                    if ( subIndex < childEntities.size() &&
                         childEntities[subIndex] == entt::null ) {
                        childEntities[subIndex] = otherEnt;
                    }
                }
            }

            std::vector<AlignedSubNote> alignedSubNotes;
            alignedSubNotes.reserve(newNote.m_subNotes.size());
            for ( std::size_t index = 0U; index < newNote.m_subNotes.size();
                  ++index ) {
                auto       alignedSubNote = newNote.m_subNotes[index];
                const auto childEntity    = childEntities[index];
                if ( childEntity != entt::null ) {
                    // 已存在投影实体使用前一阶段对齐后的时间值。
                    const auto& childNote    = newNotes.at(childEntity);
                    alignedSubNote.timestamp = childNote.m_timestamp;
                    alignedSubNote.duration  = childNote.m_duration;
                } else {
                    // 缺失投影实体直接对齐父数组中的持久化子值。
                    alignTimeRange(alignedSubNote.timestamp,
                                   alignedSubNote.duration);
                }
                alignedSubNotes.push_back(
                    { std::move(alignedSubNote), childEntity, index });
            }

            std::stable_sort(
                alignedSubNotes.begin(),
                alignedSubNotes.end(),
                [](const AlignedSubNote& lhs, const AlignedSubNote& rhs) {
                    // 同时间节点保持原顺序，其他节点按新时间重新排列。
                    if ( std::abs(lhs.note.timestamp - rhs.note.timestamp) <
                         1e-9 ) {
                        return lhs.originalSubIndex < rhs.originalSubIndex;
                    }
                    return lhs.note.timestamp < rhs.note.timestamp;
                });

            std::vector<NoteComponent::SubNote> newSubNotesList;
            newSubNotesList.reserve(newNote.m_subNotes.size());
            for ( std::size_t index = 0U; index < alignedSubNotes.size();
                  ++index ) {
                auto& alignedSubNote = alignedSubNotes[index];
                newSubNotesList.push_back(alignedSubNote.note);
                if ( alignedSubNote.childEntity != entt::null ) {
                    // 投影子实体同步新时间和排序后的数组索引。
                    auto& childNote = newNotes.at(alignedSubNote.childEntity);
                    childNote.m_timestamp = alignedSubNote.note.timestamp;
                    childNote.m_duration  = alignedSubNote.note.duration;
                    childNote.m_subIndex  = static_cast<int>(index);
                }
            }

            newNote.m_subNotes = std::move(newSubNotesList);

            if ( !newNote.m_subNotes.empty() ) {
                // 父锚点重新绑定排序后的首节点，供投影和范围查询使用。
                newNote.m_timestamp  = newNote.m_subNotes.front().timestamp;
                newNote.m_trackIndex = newNote.m_subNotes.front().trackIndex;
            }
        }
    }

    // 以相同实体键配对 before/after，生成一次完整批量动作。
    entries.reserve(originalNotes.size());
    for ( const auto& [entity, originalNote] : originalNotes ) {
        entries.push_back({ entity, originalNote, newNotes.at(entity) });
    }

    if ( !entries.empty() ) {
        size_t count  = entries.size();
        auto   action = std::make_unique<BatchNoteAction>(std::move(entries),
                                                        "Align Selected");
        m_ctx.actionStack.pushAndExecute(std::move(action), m_ctx);
        XINFO("Aligned {} selected items to nearest common beat divisors",
              count);

        m_ctx.lastActionMessage = fmt::format("{} {} {} {}",
                                              TR("ui.status.category.action"),
                                              TR("ui.tools.align_beats"),
                                              count,
                                              TR("ui.status.info.items"));
    }
}

}  // namespace MMM::Logic
