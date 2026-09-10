#include "common/LogicCommands.h"
#include "config/EditorConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/BeatmapSaveProgressEvent.h"
#include "event/project/ProjectEvents.h"
#include "logic/BeatmapSession.h"
#include "logic/ProjectController.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/beatmap/BeatmapMutationObserver.h"

#include "log/colorful-log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <latch>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
/// @brief 记录谱面观察者收到的通知次数与最后一次变化类别。
/// @note 同步记录会话回调，不模拟网络发送、排队或远端确认。
/// @note 计数器不做原子同步；本文件只在会话 update 所在线程访问该观察者。
/// @note 本地变化序号与权威修订号属于不同序列，不能互相替代。
class CountingMutationObserver final : public MMM::IBeatmapMutationObserver
{
public:
    /// @brief 模拟接收一次本地变化并分配单调序号。
    /// @param flags 本轮命令报告的变化类别。
    /// @return 新分配的非零本地变化序号。
    /// @note 不检查 BeatMap 内容，数据正确性由会话断言独立覆盖。
    /// @copydoc MMM::IBeatmapMutationObserver::onBeatmapMutated
    std::uint64_t onBeatmapMutated(const MMM::BeatMap&,
                                   MMM::BeatmapMutationFlags flags) override
    {
        // 每次本地变更回调同时记录次数和类别，便于识别重复通知。
        // 序号从一递增，零保留给尚无本地变化的权威状态。
        ++m_notificationCount;
        m_lastFlags = flags;
        return m_nextMutationSequence++;
    }

    /// @brief 记录一次完整权威基线同步。
    /// @note 仅计数，不主动回调会话，避免测试观察者制造同步回声。
    /// @copydoc MMM::IBeatmapMutationObserver::onBeatmapSynchronized
    void onBeatmapSynchronized(const MMM::BeatMap&) override
    {
        // 完整权威同步与本地编辑分开计数，避免把同步回声误认为编辑。
        // 该回调不分配新的本地变化序号。
        ++m_synchronizationCount;
    }

    /// @brief 记录后台准备好的权威基线已安装。
    /// @param revision 当前安装的远端修订号。
    /// @param includedLocalMutationSequence 该状态已包含的本地编辑序号。
    /// @note 两个序号由调用方提供，本观察者不推断二者大小关系。
    /// @copydoc MMM::IBeatmapMutationObserver::onAuthoritativeBeatmapApplied
    void onAuthoritativeBeatmapApplied(
        std::uint64_t revision,
        std::uint64_t includedLocalMutationSequence) override
    {
        // 已准备的后台基线通过这一轻量确认入口报告。
        // 修订号与所含本地序号分别保存，不触发完整同步计数。
        ++m_authoritativeApplyCount;
        m_lastAuthoritativeRevision         = revision;
        m_lastIncludedLocalMutationSequence = includedLocalMutationSequence;
    }

    /// @brief 返回已经收到的通知次数。
    /// @note 无重置接口，测试按连续阶段的累计值验证每次增量。
    [[nodiscard]] int notificationCount() const { return m_notificationCount; }

    /// @brief 返回最后一次通知的变化类别。
    /// @note 只保留最后一个值；历史次数由 notificationCount 单独记录。
    [[nodiscard]] MMM::BeatmapMutationFlags lastFlags() const
    {
        // 此值表达观察者最后收到的类别，不重新从谱面内容推断。
        // 因而可以检测数据已更新但通知类别错误的接线问题。
        return m_lastFlags;
    }

    /// @brief 返回已经收到的权威基线同步次数。
    /// @note 不包含 authoritativeApplyCount 记录的专用准备基线确认。
    [[nodiscard]] int synchronizationCount() const
    {
        // 读取计数不触发同步，也不消费已收到的确认。
        // 同一阶段可以多次断言，而不会改变后续累计结果。
        return m_synchronizationCount;
    }

    /// @brief 返回已经收到的后台权威基线安装确认次数。
    /// @note 与完整同步计数分离，可检查是否误走了全量通知入口。
    [[nodiscard]] int authoritativeApplyCount() const
    {
        // 专用确认路径独立计数，使测试能区分准备好的差量安装。
        // 不把它叠加到完整同步数中，否则两条路径无法分别验证。
        return m_authoritativeApplyCount;
    }

    /// @brief 返回最后确认的权威修订号。
    /// @note 用于确认准备基线的版本未在会话安装过程中被替换。
    [[nodiscard]] std::uint64_t lastAuthoritativeRevision() const
    {
        return m_lastAuthoritativeRevision;
    }

private:
    /// @brief 已经收到的通知次数。
    /// @note 每个本地回调增加一次，远端同步回调保持该值不变。
    int m_notificationCount{ 0 };
    /// @brief 最后一次通知的变化类别。
    /// @note 初始 None 使未收到回调时不会误报完整变化。
    MMM::BeatmapMutationFlags m_lastFlags{ MMM::BeatmapMutationFlags::None };
    /// @brief 已经收到的权威基线同步次数。
    int m_synchronizationCount{ 0 };
    /// @brief 已经收到的后台权威基线安装确认次数。
    /// @note 只由专用安装回调更新，不根据修订号是否变化推断次数。
    int m_authoritativeApplyCount{ 0 };
    /// @brief 最后确认的权威修订号。
    /// @note 保存原始输入，即使连续确认相同修订也不自动去重。
    std::uint64_t m_lastAuthoritativeRevision{ 0 };
    /// @brief 最后确认状态包含的本地变化序号。
    /// @note 记录接口输入以模拟确认状态，当前用例不单独读取此字段。
    std::uint64_t m_lastIncludedLocalMutationSequence{ 0 };
    /// @brief 模拟协作层接受本地变化后分配的严格递增序号。
    /// @note 只随本地通知递增，完整同步与已准备基线确认不得消耗它。
    std::uint64_t m_nextMutationSequence{ 1 };
};

/// @brief 创建可载入会话的最小谱面。
/// @return 最小谱面共享对象。
/// @note 每次返回独立谱面，避免前一个用例的编辑状态泄漏。
/// @note 只提供会话加载所需的名称和轨数，具体物件由用例添加。
/// @note 载入命令持有共享所有权，局部变量移动后谱面仍能被会话使用。
[[nodiscard]] std::shared_ptr<MMM::BeatMap> makeBeatmap()
{
    auto beatmap                           = std::make_shared<MMM::BeatMap>();
    beatmap->m_baseMapMetadata.name        = "Collaboration Snapshot";
    beatmap->m_baseMapMetadata.track_count = 4;
    // 不在助手内调用 sync，新增物件的用例负责填完源数据后同步。
    // 空模型加载案例则直接交给会话执行其正常初始化。
    return beatmap;
}

/// @brief 判断会话中是否存在指定时间与轨道的非折线根 Note。
/// @param session 待检查会话。
/// @param timestamp 时间戳，单位秒。
/// @param track 玩家轨道索引。
/// @return 找到匹配物件时返回 true。
/// @note 扫描只用于低频测试断言，不得把该完整遍历方式搬入生产热路径。
/// @note 测试时间参数为秒，与 ECS 一致；持久化模型的毫秒需在加载时转换。
/// @note 这里验证普通点击的存在性，不区分同位置的多个稳定身份。
[[nodiscard]] bool hasRootNote(const MMM::Logic::BeatmapSession& session,
                               double timestamp, int track)
{
    const auto& context = session.getContext();
    const auto  notes =
        context.noteRegistry.view<const MMM::Logic::NoteComponent>();
    // 查询 ECS 而不是源模型，确保断言反映命令已经安装的会话状态。
    // 根 Note 条件排除折线子实体，防止仅有子节点也算普通点击存在。
    for ( const auto entity : notes ) {
        const auto& note = notes.get<const MMM::Logic::NoteComponent>(entity);
        if ( !note.m_isSubNote && note.m_type == MMM::NoteType::NOTE &&
             std::abs(note.m_timestamp - timestamp) < 1e-9 &&
             note.m_trackIndex == track ) {
            return true;
        }
    }
    return false;
}

/// @brief 按稳定协作标识查找非子物件实体。
/// @param session 待检查会话。
/// @param identity 根物件稳定协作标识。
/// @return 找到时返回实体，否则返回 entt::null。
/// @note 协作标识跨权威替换保持稳定，实体号是否保留由用例另行比较。
/// @note 过滤折线子实体，避免父子共享关联信息时误选子项。
/// @note 返回值仅供当前会话 Registry 使用，不能跨会话访问。
/// @note 同一测试模型中的协作 ID 应唯一；助手遇到首个匹配即返回。
/// @note 不负责修复重复身份或验证全模型一致性。
[[nodiscard]] entt::entity findRootNoteByIdentity(
    const MMM::Logic::BeatmapSession& session, std::string_view identity)
{
    const auto& context = session.getContext();
    const auto  notes =
        context.noteRegistry.view<const MMM::Logic::NoteComponent>();
    // 稳定字符串用于跨替换定位，再由调用方比较本地实体是否变化。
    // 不要用时间轨道代替身份，已修改物件的位置本来就会改变。
    for ( const auto entity : notes ) {
        const auto& note = notes.get<const MMM::Logic::NoteComponent>(entity);
        if ( !note.m_isSubNote && note.m_collaborationId == identity ) {
            return entity;
        }
    }
    return entt::null;
}

/// @brief 判断会话中是否存在已经提交的折线根物件。
/// @param session 待检查会话。
/// @return 找到至少包含两个子段的折线时返回 true。
/// @note 画笔状态中的临时段不在此检查范围，必须进入正式 Note Registry。
/// @note 至少两段用于区分有效折线与尚未形成连接的占位根。
/// @note 只验证存在性，不检查每段的精确坐标或音效绑定。
/// @note 完整遍历仅用于断言，不承诺热路径性能。
/// @note 结果为 true 仅证明至少一个满足条件的根对象存在，不证明唯一性。
[[nodiscard]] bool hasCommittedPolyline(
    const MMM::Logic::BeatmapSession& session)
{
    const auto& context = session.getContext();
    const auto  notes =
        context.noteRegistry.view<const MMM::Logic::NoteComponent>();
    // 只有正式父折线且包含至少两个子项才视作提交完成。
    // 活动画笔段数非零不代表对象已经写入可撤销的实体状态。
    for ( const auto entity : notes ) {
        const auto& note = notes.get<const MMM::Logic::NoteComponent>(entity);
        if ( !note.m_isSubNote && note.m_type == MMM::NoteType::POLYLINE &&
             note.m_subNotes.size() >= 2U ) {
            return true;
        }
    }
    return false;
}

/// @brief 验证访客绑定观察者时不会把房主快照回传为本地编辑。
/// @return 禁止初始快照且显式请求仍能发布完整快照时返回 true。
/// @note 先载入再绑定观察者，隔离加载与绑定本身产生的通知。
/// @note 访客关闭初始推送可避免把收到的房主状态回传为新编辑。
/// @note 重新绑定同一观察者并开启初始推送，应恰好发布一次 All。
/// @note 不比较快照编码内容，只验证绑定时是否发布以及类别是否完整。
/// @note 观察者由会话共享持有，本用例的局部指针保留到所有计数断言结束。
[[nodiscard]] bool testOptionalInitialSnapshot()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = makeBeatmap() },
    });
    session.update(0.0, config, false);
    auto observer = std::make_shared<CountingMutationObserver>();
    // 关闭初始快照后再推进一次 update，检查延后的通知也没有出现。
    // 不能只在 setter 返回后立即读计数，因为发布可能由下一轮消费。
    session.setMutationObserver(observer, false);
    session.update(0.0, config, false);
    // 零计数同时排除立即通知和下次 update 才发出的延后通知。
    // 这里只检查本地变化回调，不对加载内部的其他事件计数。
    if ( observer->notificationCount() != 0 ) {
        XERROR("Guest observer echoed the received host snapshot");
        return false;
    }

    // 复用同一观察者切换初始发布策略，计数仍从前一阶段的零开始。
    // 一次 All 通知证明显式请求有效，而不是观察者整体失效。
    session.setMutationObserver(observer, true);
    session.update(0.0, config, false);
    // 次数一排除重复绑定产生重复快照，All 则排除只发布某个子类别。
    // 同一观察者的累计值没有清零，前一阶段的异常也会反映在这里。
    if ( observer->notificationCount() != 1 ||
         observer->lastFlags() != MMM::BeatmapMutationFlags::All ) {
        XERROR("Requested mutation snapshot was not published exactly once");
        return false;
    }
    return true;
}

/// @brief 验证 Timeline 窗口使用的单个和批量 Timing 命令都会发布协作变化。
/// @return 两次编辑分别以 Timelines 类型通知观察者时返回 true。
/// @note 单条、批量、撤销和重做都经过会话命令队列。
/// @note 通知类别应始终为 Timelines，不能扩成完整谱面变更。
/// @note 批量入口仅放一项，重点验证入口接线而非批次合并算法。
/// @note 每次显式 update 消费命令，不等待后台线程。
/// @note 未验证撤销后各 Timing 的精确值，本用例聚焦通知接线。
/// @note SCROLL 和 HS 都属于时间线域，不能因效果不同改变通知权限类别。
[[nodiscard]] bool testTimelineCommandsPublishMutations()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = makeBeatmap() },
    });
    session.update(0.0, config, false);

    auto observer = std::make_shared<CountingMutationObserver>();
    session.setMutationObserver(observer, false);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdCreateTimelineEvent{
            // 显式设置不同于默认 BPM 起点的滚动事件。
            // 该编辑不改变物件数据，通知不应被归入 Objects。
            .time  = 1.25,
            .type  = MMM::TimingEffect::SCROLL,
            .value = 1.5,
        },
    });
    session.update(0.0, config, false);
    // 单命令必须真正到达观察者，而非只在 Registry 内创建事件。
    // 分类检查约束协作层只发送需要更新的时间线数据。
    if ( observer->notificationCount() != 1 ||
         observer->lastFlags() != MMM::BeatmapMutationFlags::Timelines ) {
        XERROR("Single Timeline command did not publish a mutation");
        return false;
    }

    // 第二次编辑经过批量命令入口，不能仅依靠单条命令覆盖。
    // 使用另一种 Timing 效果，通知仍属于同一时间线类别。
    MMM::Logic::CmdCreateTimelineEvents batch;
    batch.events.push_back(MMM::Logic::CmdCreateTimelineEvents::Entry{
        .time  = 2.5,
        .type  = MMM::TimingEffect::HS,
        .value = 0.75,
    });
    session.pushCommand(MMM::Logic::LogicCommand{ std::move(batch) });
    session.update(0.0, config, false);
    // 累计次数二说明批量入口补上了独立通知。
    // 批次只有一条事件，因此本例不要求逐条或整批合并的复杂语义。
    if ( observer->notificationCount() != 2 ||
         observer->lastFlags() != MMM::BeatmapMutationFlags::Timelines ) {
        XERROR("Batch Timeline command did not publish a mutation");
        return false;
    }

    // 撤销和重做各应贡献一次变化通知，沿用原编辑的类别。
    // 每个命令分别 update，避免两次变更被同一轮合并掩盖。
    session.pushCommand(MMM::Logic::LogicCommand{ MMM::Logic::CmdUndo{} });
    session.update(0.0, config, false);
    session.pushCommand(MMM::Logic::LogicCommand{ MMM::Logic::CmdRedo{} });
    session.update(0.0, config, false);
    // 两次历史操作在原有两次编辑上再贡献两次通知。
    // 最终类别仍为 Timelines，撤销重做不能退化成无分类刷新。
    if ( observer->notificationCount() != 4 ||
         observer->lastFlags() != MMM::BeatmapMutationFlags::Timelines ) {
        XERROR("Timeline undo/redo did not publish mutations");
        return false;
    }
    return true;
}

/// @brief 验证多批注的 Creator 门禁、物件定位、时间分组与协作权限。
/// @return 无 Creator 拒绝新增，同时间戳聚合且撤销和权限类别正确时返回 true。
/// @note 同时覆盖对象、时间戳和自动采样三类批注目标。
/// @note 源模型使用毫秒，命令和渲染缓存使用秒，同一时间应归入一个标记。
/// @note 权限只允许批注时，不应要求额外 Objects 或 AudioSamples
/// 权限来添加目标批注。
/// @note 远端批注替换不进入本地撤销历史，撤销应只移除此前本地批注。
/// @note 作者空白裁剪与 Markdown 原文保留分别检查，不把文本格式化混入存储。
/// @note 时间聚合发生在会话更新内，断言不依赖 UI 读取过快照。
/// @note 对象和采样在整个批注流程中保持存活，本例不覆盖目标失效后的回退位置。
/// @note 远端替换后的撤销以条目身份为边界，不应回滚整个批注列表。
[[nodiscard]] bool testBeatmapAnnotationPermissionAndTimestampGrouping()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    auto                       beatmap = makeBeatmap();
    // 构造折线内 Hold 与 Flick，批注需要定位父实体内的具体子项。
    // 持久化物件时间为毫秒，加载后零号子项位于一秒。
    auto& hold       = beatmap->m_noteData.holds.emplace_back();
    hold.m_timestamp = 1000.0;
    hold.m_duration  = 500.0;
    hold.m_track     = 1;
    // 分类容器中的 Hold 被标为子物件，避免载入时再生成一个独立根。
    // 批注通过父折线加 subIndex 定位它，而不是依赖独立子实体。
    hold.m_isSubNote  = true;
    auto& flick       = beatmap->m_noteData.flicks.emplace_back();
    flick.m_timestamp = 1500.0;
    flick.m_track     = 1;
    flick.m_dtrack    = 1;
    // Flick 同样归属折线，源模型中保留分类索引供同步使用。
    // 两个子项的不同时间可区分根部批注与末端批注。
    flick.m_isSubNote = true;
    // 同时填入折线通用子项与分类容器，保持源模型的关联结构。
    // 调用 sync 后再载入，让稳定目标身份按正常模型流程建立。
    auto& polyline       = beatmap->m_noteData.polylines.emplace_back();
    polyline.m_timestamp = hold.m_timestamp;
    polyline.m_track     = hold.m_track;
    polyline.m_subNotes.emplace_back(hold);
    polyline.m_subNotes.emplace_back(flick);
    polyline.m_subHolds.emplace_back(hold);
    polyline.m_subFlicks.emplace_back(flick);
    // 采样同样放在一秒，但属于独立 Registry 和目标类别。
    // 后面验证它可以与物件、时间戳批注共用同一时间标记。
    auto& sample             = beatmap->m_audioSamples.emplace_back();
    sample.m_timestamp       = 1000.0;
    sample.m_track           = 4;
    sample.m_audioResourceId = "sample";
    beatmap->sync();
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = std::move(beatmap) },
    });
    session.update(0.0, config, false);

    // 从加载完成的 ECS 找父实体，不能用源模型中的引用作为命令目标。
    // 实体号是本会话局部身份，持久化批注随后应转换为稳定目标 ID。
    entt::entity polylineEntity = entt::null;
    const auto view = session.getContext()
                          .noteRegistry.view<const MMM::Logic::NoteComponent>();
    for ( const auto entity : view ) {
        const auto& note = view.get<const MMM::Logic::NoteComponent>(entity);
        if ( !note.m_isSubNote && note.m_type == MMM::NoteType::POLYLINE ) {
            polylineEntity = entity;
            break;
        }
    }
    // 没有父折线说明夹具载入未建立预期对象，应停止后续命令。
    // 不能用空实体提交批注，再把失败误认为 Creator 门禁生效。
    if ( polylineEntity == entt::null ) return false;
    const auto sampleView =
        session.getContext()
            .sampleRegistry.view<const MMM::Logic::SampleComponent>();
    // 确认夹具只有一个采样后才读取实体，避免空容器解引用。
    // 该实体与音符实体即使数值相同，也必须配合 AudioSample 种类使用。
    if ( sampleView.size() != 1U ) return false;
    const entt::entity sampleEntity = *sampleView.begin();

    auto observer = std::make_shared<CountingMutationObserver>();
    session.setMutationObserver(observer, false);
    // 只开放批注权限，目标是音符或采样并不意味着正在修改目标对象。
    // 观察者初始推送关闭，后面的次数仅来自显式批注命令。
    session.setCollaborationAllowedMutationFlags(
        MMM::BeatmapMutationFlags::Annotations);

    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpsertBeatmapAnnotation{
            .targetKind = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT,
            .objectKind = MMM::Logic::ChartObjectKind::PlayerNote,
            .entity     = polylineEntity,
            .subIndex   = 0,
            // 有合法目标但缺少作者，门禁仍应拒绝。
            // 失败既不能落盘到模型，也不能产生协作变化通知。
            .content = "无作者不应写入",
        },
    });
    session.update(0.0, config, false);
    // 同时验证数据不变、无通知和明确失败消息。
    // 只有其中一个条件不足以证明未授权新增被完整拒绝。
    if ( !session.getContext().currentBeatmap->m_annotations.empty() ||
         observer->notificationCount() != 0 ||
         session.getContext().lastActionMessage !=
             "未设置默认 Creator，无法添加批注" ) {
        XERROR("Annotation without Creator was not rejected");
        return false;
    }

    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpsertBeatmapAnnotation{
            .targetKind = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT,
            .objectKind = MMM::Logic::ChartObjectKind::PlayerNote,
            .entity     = polylineEntity,
            .subIndex   = 0,
            // 作者首尾空白用于验证输入规范化，内部内容不参与 Markdown 处理。
            // 明确作者后，同一目标应能够通过之前的 Creator 门禁。
            .author  = "  Creator One  ",
            .content = "# 折线起段\n- 检查节奏",
        },
    });
    session.update(0.0, config, false);
    // 检查作者去除两端空白，Markdown 内容仍逐字保存。
    // 非空稳定目标 ID 证明实体目标已解析为可同步身份。
    const auto& annotations =
        session.getContext().currentBeatmap->m_annotations;
    // 条目、身份、作者、正文与通知必须共同满足，避免只存下空壳。
    // 先检查数量再访问 front，异常空列表不会导致测试越界。
    if ( annotations.size() != 1U ||
         annotations.front().m_targetKind !=
             MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT ||
         annotations.front().m_targetId.empty() ||
         annotations.front().m_author != "Creator One" ||
         annotations.front().m_content != "# 折线起段\n- 检查节奏" ||
         observer->notificationCount() != 1 ||
         observer->lastFlags() != MMM::BeatmapMutationFlags::Annotations ) {
        XERROR("Object annotation did not preserve its target and Creator");
        return false;
    }

    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpsertBeatmapAnnotation{
            .targetKind = MMM::BeatmapAnnotationTargetKind::TIMESTAMP,
            // 命令使用秒，恰好等于源模型里一千毫秒的目标时间。
            // 这是跨单位分组验证，不应在命令里再乘一千。
            .timestamp = 1.0,
            .author    = "Creator Two",
            .content   = "> 同一时间戳",
        },
    });
    session.update(0.0, config, false);
    // 缓存按解析后的秒时间聚合，而不是按批注目标种类分组。
    // 两个原始批注仍独立存在，只共用一个可见标记。
    const auto& grouped = session.getContext().annotationRenderCache;
    // 原始条目数量与分组数量故意不同，一个标记可携带多条批注。
    // 分组时间也需精确为一秒，排除错误地按毫秒展示。
    if ( session.getContext().currentBeatmap->m_annotations.size() != 2U ||
         grouped.size() != 1U || grouped.front().items.size() != 2U ||
         std::abs(grouped.front().timestamp - 1.0) > 1e-9 ||
         observer->notificationCount() != 2 ||
         observer->lastFlags() != MMM::BeatmapMutationFlags::Annotations ) {
        XERROR("Annotations at the same timestamp did not share one marker");
        return false;
    }

    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpsertBeatmapAnnotation{
            .targetKind = MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE,
            .objectKind = MMM::Logic::ChartObjectKind::AudioSample,
            .entity     = sampleEntity,
            // 第三位作者属于采样目标，文本中的反引号仍作为原文保留。
            // 本例不解码 sample 音频资源，只验证对象身份与批注分组。
            .author  = "Creator Three",
            .content = "`sample` 批注",
        },
    });
    session.update(0.0, config, false);
    // 采样批注检查自身目标类型与稳定 ID，不误用玩家目标身份。
    // 第三个条目仍加入一秒分组，聚合不能丢失任一作者的内容。
    const auto& sampleAnnotations =
        session.getContext().currentBeatmap->m_annotations;
    const auto& sampleGrouped = session.getContext().annotationRenderCache;
    // 稳定采样目标与同时间分组都必须存在。
    // 新增一条通知不代表它已正确加入可见缓存，因此联合检查两侧。
    if ( sampleAnnotations.size() != 3U ||
         sampleAnnotations.back().m_targetKind !=
             MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE ||
         sampleAnnotations.back().m_targetId.empty() ||
         sampleGrouped.size() != 1U ||
         sampleGrouped.front().items.size() != 3U ||
         observer->notificationCount() != 3 ) {
        XERROR("Automatic sample annotation did not join the timestamp marker");
        return false;
    }

    // 权威来源先包含已有批注，再追加带明确 ID 的远端条目。
    // 替换只涉及批注，不提供物件数据以重建本地目标。
    auto remoteAnnotations           = std::make_shared<MMM::BeatMap>();
    remoteAnnotations->m_annotations = sampleAnnotations;
    remoteAnnotations->m_annotations.emplace_back(MMM::BeatmapAnnotation{
        // 远端 ID 明确不同于本地生成 ID，供撤销后的身份断言使用。
        // 相同时间不会使不同身份批注互相覆盖。
        .m_id         = "remote-annotation",
        .m_targetKind = MMM::BeatmapAnnotationTargetKind::TIMESTAMP,
        .m_timestamp  = 1000.0,
        .m_author     = "Remote Creator",
        .m_content    = "远程批注",
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdReplaceBeatmapData{
            .sourceBeatmap = std::move(remoteAnnotations),
            // 仅替换批注域，物件和采样继续作为原有定位目标。
            // 禁用本地变更通知，防止权威安装被回传成新的本地批注编辑。
            .replaceAnnotations     = true,
            .notifyMutationObserver = false,
            .authoritativeRemote    = true,
        },
    });
    session.update(0.0, config, false);
    // 先确认远端条目实际进入当前模型，才验证撤销是否保留它。
    // 否则撤销数量碰巧正确也无法证明跨权威替换的历史行为。
    if ( session.getContext().currentBeatmap->m_annotations.size() != 4U ) {
        XERROR("Remote annotation replacement did not apply");
        return false;
    }

    session.pushCommand(MMM::Logic::LogicCommand{ MMM::Logic::CmdUndo{} });
    session.update(0.0, config, false);
    // 远端安装不应占用本地撤销栈顶部，撤销仍针对最近本地采样批注。
    // 远端明确 ID 作为保留哨兵，避免只用数量判断丢错了哪一项。
    const auto& afterUndo = session.getContext().currentBeatmap->m_annotations;
    // 按稳定 ID 查询远端条目，不依赖替换后的容器顺序。
    // 本地撤销必须保留它，即使远端条目与本地批注时间相同。
    const bool keepsRemote =
        std::any_of(afterUndo.begin(), afterUndo.end(), [](const auto& item) {
            return item.m_id == "remote-annotation";
        });
    // 撤销掉最近本地采样批注后应余三条，远端条目仍在其中。
    // 回调类别继续是 Annotations，不因其原目标属于采样而变更。
    if ( afterUndo.size() != 3U || !keepsRemote ||
         observer->notificationCount() != 4 ||
         observer->lastFlags() != MMM::BeatmapMutationFlags::Annotations ) {
        XERROR("Annotation undo did not preserve mutation category");
        return false;
    }

    session.setCollaborationAllowedMutationFlags(
        MMM::BeatmapMutationFlags::Objects);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpsertBeatmapAnnotation{
            .targetKind = MMM::BeatmapAnnotationTargetKind::TIMESTAMP,
            .timestamp  = 2.0,
            // 此命令作者合法，因此失败只能来自当前批注权限关闭等执行条件。
            // 用合法内容避免再次落入此前缺 Creator 的门禁分支。
            .author  = "Creator Four",
            .content = "不应写入",
        },
    });
    session.update(0.0, config, false);
    // 切换为仅 Objects 权限后，新增纯时间批注应被拒绝。
    // 最终数量与通知计数都保持，证明没有静默写入或多发变化。
    return session.getContext().currentBeatmap->m_annotations.size() == 3U &&
           observer->notificationCount() == 4;
}

/// @brief 验证远端纯物件替换不会覆盖正在绘制的本地折线草稿。
/// @return 远端物件即时出现且松键后本地折线与远端物件同时存在时返回 true。
/// @note 远端只替换正式物件，本地未提交折线仍属于工具状态。
/// @note 松键后应在新基线上提交折线，不能恢复已被远端删除的旧 Note。
/// @note 一次远端同步和一次本地编辑通过不同回调计数。
/// @note 撤销本地折线后仍保留远端物件，并消费本轮统计脏标记。
/// @note 更新参数显式递增以推进手势，测试不建立网络连接。
/// @note 旧 Note 缺失也属于正确结果，不能只检查远端新增是否出现。
/// @note 统计脏标记清零只证明本轮刷新入口已执行，不直接验证每个连击数值。
[[nodiscard]] bool testRemoteSynchronizationPreservesActiveBrush()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    auto                       initial = makeBeatmap();
    // 初始物件作为稍后远端删除的哨兵，时间与远端新增物件不同。
    // 这样能同时验证旧内容清除和新内容即时出现。
    auto& initialNote       = initial->m_noteData.notes.emplace_back();
    initialNote.m_timestamp = 500.0;
    initialNote.m_track     = 2;
    // 源模型先同步派生数据，再通过正常加载命令生成 ECS。
    // 后面读取 ECS 的秒时间与这里的毫秒时间相对应。
    initial->sync();
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = std::move(initial) },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        // 画笔需要真实相机尺寸把鼠标坐标映射为时间和轨道。
        // 固定尺寸和主画布 ID，让手势生成可重复的折线草稿。
        MMM::Logic::CmdUpdateViewport{
            .cameraId = "Basic2DCanvas",
            .width    = 400.0F,
            .height   = 600.0F,
        },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdChangeTool{ .tool = MMM::Logic::EditTool::Draw },
    });
    session.update(0.0, config, false);

    auto observer = std::make_shared<CountingMutationObserver>();
    // 绑定不发初始快照，通知一应专指后面松键提交。
    // 远端同步回调另行累计，能识别不应出现的回声。
    session.setMutationObserver(observer, false);
    session.pushCommand(MMM::Logic::LogicCommand{
        // Shift 与 Ctrl 组合进入本例所需的折线手势。
        // 开始和移动都通过命令入口，避免直接伪造已提交实体。
        MMM::Logic::CmdStartBrush{
            .cameraId    = "Basic2DCanvas",
            .mouseX      = 150.0F,
            .mouseY      = 300.0F,
            .isShiftDown = true,
            .isCtrlDown  = true,
        },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        // 移动到另一时间和轨道，确保折线不只是单个初始点。
        // 活动状态与非空子段在远端替换后都要保留。
        MMM::Logic::CmdUpdateBrush{
            .cameraId    = "Basic2DCanvas",
            .mouseX      = 250.0F,
            .mouseY      = 150.0F,
            .isShiftDown = true,
            .isCtrlDown  = true,
        },
    });
    session.update(0.1, config, false);

    // 远端只保留一个不同位置的 Note，模拟权威物件集替换。
    // 本地活动折线不放入远端源数据，必须由工具状态自行保留。
    auto  remote           = makeBeatmap();
    auto& remoteNote       = remote->m_noteData.notes.emplace_back();
    remoteNote.m_timestamp = 1000.0;
    remoteNote.m_track     = 3;
    remote->sync();
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdReplaceBeatmapData{
            .sourceBeatmap = std::move(remote),
            // 权威替换范围限定为正式物件，不要求重置相机或工具。
            // 该测试不通过全会话重新加载来处理远端更新。
            .replaceObjects         = true,
            .notifyMutationObserver = false,
            .authoritativeRemote    = true,
        },
    });
    session.update(0.2, config, false);
    // 远端安装应通知同步，但不能结束本地画笔或清空临时子段。
    // 同时检查旧物件消失和新物件存在，防止为保留画笔而忽略远端更新。
    if ( observer->synchronizationCount() != 1 ||
         !session.getContext().brushState.isActive ||
         session.getContext().brushState.polylineSegments.empty() ||
         !hasRootNote(session, 1.0, 3) || hasRootNote(session, 0.5, 2) ) {
        XERROR(
            "Remote synchronization did not preserve the active Polyline "
            "draft: syncs={}, active={}, segments={}, added={}, deleted={}",
            observer->synchronizationCount(),
            session.getContext().brushState.isActive,
            session.getContext().brushState.polylineSegments.size(),
            hasRootNote(session, 1.0, 3),
            !hasRootNote(session, 0.5, 2));
        return false;
    }

    session.pushCommand(MMM::Logic::LogicCommand{
        // 松键才提交本地折线，提交所依赖的正式基线已经是远端新状态。
        // 不能把开始手势时的旧谱面整体写回。
        MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" },
    });
    // 一次更新处理松键提交及其后的索引、统计刷新。
    // 断言立即读取最终状态，不额外等待渲染线程补偿。
    session.update(0.3, config, false);
    // 一次提交只产生一次 Objects 通知，同步次数不因本地编辑增加。
    // 统计脏标记也应在当前 update 消费，避免后续视图保留旧计数。
    if ( observer->notificationCount() != 1 ||
         observer->lastFlags() != MMM::BeatmapMutationFlags::Objects ||
         observer->synchronizationCount() != 1 ||
         !hasRootNote(session, 1.0, 3) || hasRootNote(session, 0.5, 2) ||
         !hasCommittedPolyline(session) ||
         session.getContext().isNoteStatsDirty ) {
        XERROR(
            "Active Polyline did not commit on top of the remote baseline: "
            "mutations={}, syncs={}, added={}, deleted={}, polyline={}, "
            "statsDirty={}",
            observer->notificationCount(),
            observer->synchronizationCount(),
            hasRootNote(session, 1.0, 3),
            !hasRootNote(session, 0.5, 2),
            hasCommittedPolyline(session),
            session.getContext().isNoteStatsDirty);
        return false;
    }

    // 撤销只移除本地折线，远端 Note 仍是当前权威基线的一部分。
    // 检查统计刷新，确保撤销走完整的编辑后更新链。
    session.pushCommand(MMM::Logic::LogicCommand{ MMM::Logic::CmdUndo{} });
    session.update(0.4, config, false);
    // 撤销贡献第二次本地对象通知，远端新增 Note 仍应存在。
    // 折线消失且统计不脏，证明历史回退与派生缓存同步完成。
    if ( observer->notificationCount() != 2 ||
         observer->lastFlags() != MMM::BeatmapMutationFlags::Objects ||
         !hasRootNote(session, 1.0, 3) || hasCommittedPolyline(session) ||
         session.getContext().isNoteStatsDirty ) {
        XERROR(
            "Collaborative Polyline undo left render stats dirty: "
            "mutations={}, added={}, polyline={}, statsDirty={}",
            observer->notificationCount(),
            hasRootNote(session, 1.0, 3),
            hasCommittedPolyline(session),
            session.getContext().isNoteStatsDirty);
        return false;
    }
    return true;
}

/// @brief 验证活跃框选期间的旧权威状态不会在命令队列内自旋或覆盖本地编辑。
/// @return 旧状态留在队列外，且只有包含本地变化序号的新状态能够回灌时返回
/// true。
/// @note 本地删除分配序号一，不含该序号的权威快照不能覆盖删除。
/// @note 手势结束、收到回执和收到新权威状态是三个独立条件。
/// @note 延后状态须留在命令队列外，避免每次 update 重入队列自旋。
/// @note 测试用确定的更新步推进状态，不用 sleep 等待网络。
/// @note 不对 update 执行耗时设硬阈值，以免机器负载影响断言。
/// @note 队列排空和下一次更新正常完成用于覆盖延后状态的调度路径。
/// @note 所用权威状态都只替换物件，不能据此推断其他数据域也采用相同延后规则。
[[nodiscard]] bool testRemoteSynchronizationWaitsForLocalMutationReceipt()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    auto                       initial = makeBeatmap();
    auto& initialNote       = initial->m_noteData.notes.emplace_back();
    initialNote.m_timestamp = 500.0;
    initialNote.m_track     = 2;
    initial->sync();
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = std::move(initial) },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        // 框选工具配合全选命令，使随后删除具有明确目标。
        // 直接设置 isSelecting 只负责保持手势状态，不替代选择集合。
        MMM::Logic::CmdChangeTool{ .tool = MMM::Logic::EditTool::Marquee },
    });
    session.pushCommand(MMM::Logic::LogicCommand{ MMM::Logic::CmdSelectAll{
        .scope = MMM::Logic::SelectAllScope::AllTrackAreas,
    } });
    session.update(0.0, config, false);

    auto observer = std::make_shared<CountingMutationObserver>();
    session.setMutationObserver(observer, false);
    // 显式保持框选手势活跃，用来触发权威状态的非阻塞延后分支。
    // 本例不是鼠标命中测试，不通过坐标构造框选矩形。
    session.getContextMutable().isSelecting = true;

    auto&      loadedContext = session.getContext();
    const auto loadedNotes =
        loadedContext.noteRegistry.view<const MMM::Logic::NoteComponent>();
    // 源 Note 必须已进入 ECS，才能从中取得稳定协作身份。
    // 缺失时停止构造权威快照，避免使用不存在对象来验证回灌冲突。
    if ( loadedNotes.empty() ) return false;
    // 读取载入后分配的稳定身份，让旧权威状态包含同一个已删除对象。
    // 如果身份不同，替换结果无法证明是否复活了本地删除。
    const auto& loadedInitial =
        loadedNotes.get<const MMM::Logic::NoteComponent>(*loadedNotes.begin());

    // 旧快照保留本地将删除的物件，另加一个远端 Note。
    // 它所含本地序号为零，不能证明已纳入接下来产生的删除操作。
    auto  staleAuthority     = makeBeatmap();
    auto& staleInitial       = staleAuthority->m_noteData.notes.emplace_back();
    staleInitial.m_timestamp = 500.0;
    staleInitial.m_track     = 2;
    // 旧权威条目沿用当前对象身份，明确代表同一被删除物件。
    // 身份相同使复活检测不依赖位置碰巧一致。
    staleInitial.m_collaborationId = loadedInitial.m_collaborationId;
    auto& staleRemote       = staleAuthority->m_noteData.notes.emplace_back();
    staleRemote.m_timestamp = 1000.0;
    staleRemote.m_track     = 3;
    // 新增远端对象有独立稳定身份，后面的已确认基线继续使用它。
    // 同一远端对象不应因本地确认状态变化而被视为另一次新增。
    staleRemote.m_collaborationId = "remote-marquee-note";
    staleAuthority->sync();

    session.pushCommand(
        // 先排队本地删除，再排队旧权威替换，顺序是冲突发生的前提。
        // 同一次 update 应立即完成本地删除，不能阻塞等待远端确认。
        MMM::Logic::LogicCommand{ MMM::Logic::CmdDeleteSelected{} });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdReplaceBeatmapData{
            .sourceBeatmap          = std::move(staleAuthority),
            .replaceObjects         = true,
            .notifyMutationObserver = false,
            .authoritativeRemote    = true,
            // 零表示权威编码尚未包含本地删除产生的序号一。
            // 即便权威数据包含其他远端新增，也不能据此覆盖本地结果。
            .includedLocalMutationSequence = 0,
        },
    });
    session.update(0.1, config, false);
    // 命令队列应排空，但旧快照还不能安装。
    // 删除对象不复活、远端新对象也未出现，证明整份旧状态被暂存。
    if ( session.hasPendingCommands() || observer->notificationCount() != 1 ||
         observer->synchronizationCount() != 0 ||
         hasRootNote(session, 0.5, 2) || hasRootNote(session, 1.0, 3) ) {
        XERROR(
            "Deferred authority spun or overwrote a local delete: pending={}, "
            "mutations={}, syncs={}, deletedReturned={}, remoteVisible={}",
            session.hasPendingCommands(),
            observer->notificationCount(),
            observer->synchronizationCount(),
            hasRootNote(session, 0.5, 2),
            hasRootNote(session, 1.0, 3));
        return false;
    }

    // 没有新命令的下一轮仍须立即完成，旧权威状态不得重新入队循环。
    // 通过队列与同步计数检查延后状态的持有位置。
    session.update(0.2, config, false);
    if ( session.hasPendingCommands() ||
         observer->synchronizationCount() != 0 ) {
        XERROR(
            "Deferred authority re-entered the command queue while selecting");
        return false;
    }

    // 手势结束只移除交互条件，不等于远端已经包含本地修改。
    // 未确认删除仍须保留，不能仅因鼠标松开安装旧状态。
    session.getContextMutable().isSelecting = false;
    session.update(0.3, config, false);
    if ( session.hasPendingCommands() ||
         observer->synchronizationCount() != 0 ||
         hasRootNote(session, 0.5, 2) ) {
        XERROR(
            "Unconfirmed local delete was replaced after gesture completion");
        return false;
    }

    session.pushCommand(MMM::Logic::LogicCommand{
        // 回执确认本地序号一已被接收，但不改写旧快照所包含的序号。
        // 只有回执而没有新基线时，仍不能安装序号零的暂存内容。
        MMM::Logic::CmdAcknowledgeCollaborationMutation{ .sequence = 1 },
    });
    // 收到回执后立即验证旧快照仍未安装。
    // 不能把最高已确认序号直接赋给此前编码好的旧状态。
    session.update(0.35, config, false);
    if ( session.hasPendingCommands() ||
         observer->synchronizationCount() != 0 ||
         hasRootNote(session, 0.5, 2) || hasRootNote(session, 1.0, 3) ) {
        XERROR("Local receipt applied a stale deferred object snapshot");
        return false;
    }

    // 新权威快照去掉被本地删除的物件，仅保留远端新增物件。
    // 所含序号明确为一，才满足当前本地编辑已纳入基线的条件。
    auto  confirmedAuthority = makeBeatmap();
    auto& confirmedRemote = confirmedAuthority->m_noteData.notes.emplace_back();
    confirmedRemote.m_timestamp = 1000.0;
    confirmedRemote.m_track     = 3;
    // 沿用旧权威中远端新增物件的身份，而不是创建第二个远端对象。
    // 新基线的区别在于已经包含本地删除，并更新了包含序号。
    confirmedRemote.m_collaborationId = "remote-marquee-note";
    confirmedAuthority->sync();
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdReplaceBeatmapData{
            .sourceBeatmap          = std::move(confirmedAuthority),
            .replaceObjects         = true,
            .notifyMutationObserver = false,
            .authoritativeRemote    = true,
            // 新快照自身声明已包含本地删除，满足安装的内容版本条件。
            // 这与单独的接收回执不同，后者不证明快照已重新编码。
            .includedLocalMutationSequence = 1,
        },
    });
    session.update(0.4, config, false);
    // 最终恰好安装一次，并同时保留本地删除和远端新增结果。
    // 这区分正确延后与永久忽略所有权威状态。
    if ( observer->synchronizationCount() != 1 ||
         hasRootNote(session, 0.5, 2) || !hasRootNote(session, 1.0, 3) ) {
        XERROR(
            "Confirmed authority did not preserve local deletion: syncs={}, "
            "deletedReturned={}, remoteVisible={}",
            observer->synchronizationCount(),
            hasRootNote(session, 0.5, 2),
            hasRootNote(session, 1.0, 3));
        return false;
    }
    return true;
}

/// @brief 验证后台物件差量只更新目标实体并以常量时间确认编码基线。
/// @return 未变化实体身份保留、增删改正确且不触发完整基线扫描时返回 true。
/// @note 差量身份列表同时包含修改、删除与新增，不包含未变物件。
/// @note 验证未变与已修改根实体保持身份，新增实体存在、删除实体消失。
/// @note 已准备编码基线通过专用安装确认回调发布修订号。
/// @note 回调计数与同步脏标记验证所走路径，不构成运行时间复杂度测量。
/// @note 所用对象全为普通 Note，不覆盖折线子实体的增量重建。
/// @note 新增实体编号不固定，分配器可以复用已删除实体的槽位。
/// @note 通过稳定 ID 判断删除与新增，避免把槽位复用当作原物件仍存在。
[[nodiscard]] bool testPreparedRemoteObjectDeltaAppliesIncrementally()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    auto                       initial = makeBeatmap();
    // 助手直接构造带稳定身份的源模型物件，时间参数使用毫秒。
    // 不同类别由显式身份字符串区分，不依赖加载时实体分配顺序。
    const auto appendNote = [](MMM::BeatMap& beatmap,
                               double        timestamp,
                               std::uint32_t track,
                               std::string   identity) {
        auto& note             = beatmap.m_noteData.notes.emplace_back();
        note.m_timestamp       = timestamp;
        note.m_track           = track;
        note.m_collaborationId = std::move(identity);
    };
    // 三个身份分别充当不变、移动与删除案例，初始位置彼此不同。
    // 差量应用后按身份追踪，不靠数组下标推断对应关系。
    appendNote(*initial, 500.0, 0, "unchanged-note");
    appendNote(*initial, 750.0, 1, "changed-note");
    appendNote(*initial, 900.0, 2, "removed-note");
    initial->sync();
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = std::move(initial) },
    });
    session.update(0.0, config, false);

    // 在差量应用前保存实体号，之后才能验证原地更新和身份稳定。
    // 只比较最终位置不足以发现全量销毁重建。
    const auto unchangedEntity =
        findRootNoteByIdentity(session, "unchanged-note");
    const auto changedEntity = findRootNoteByIdentity(session, "changed-note");
    // 必须先取得真实实体，才能证明差量之后身份保留。
    // 空实体与空实体相等不能被当作保留成功。
    if ( unchangedEntity == entt::null || changedEntity == entt::null ) {
        return false;
    }

    auto observer = std::make_shared<CountingMutationObserver>();
    session.setMutationObserver(observer, false);
    // 新源数据包含完整结果，但差量身份列表只列受影响对象。
    // 保留未变化 Note 作为检测不必要实体重建的哨兵。
    auto updated = makeBeatmap();
    appendNote(*updated, 500.0, 0, "unchanged-note");
    // 同一 changed-note 身份移动到另一时间与轨道。
    // 新增身份使用不同位置，让两种结果都能独立检查。
    appendNote(*updated, 1250.0, 3, "changed-note");
    appendNote(*updated, 1500.0, 2, "added-note");
    updated->sync();
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdReplaceBeatmapData{
            .sourceBeatmap                 = std::move(updated),
            .replaceObjects                = true,
            .notifyMutationObserver        = false,
            .authoritativeRemote           = true,
            .includedLocalMutationSequence = 0,
            // 删除身份也要列入，即使新源中已没有对应对象。
            // 未变化身份故意不列，渲染 Registry 应保留原实体。
            .objectDeltaIdentities = std::vector<std::string>{ "changed-note",
                                                               "removed-note",
                                                               "added-note" },
            // 修订号选定固定哨兵值，确认回调必须原样报告它。
            // 准备标志说明编码基线已完成，不需要会话再次触发完整同步。
            .authoritativeRevision          = 42,
            .objectEncodingBaselinePrepared = true,
        },
    });
    session.update(0.1, config, false);

    // 分别取回已修改和新增实体，再检查删除身份消失。
    // 已修改对象应保留实体号，新增对象只要求存在而不指定编号。
    const auto currentChanged = findRootNoteByIdentity(session, "changed-note");
    const auto currentAdded   = findRootNoteByIdentity(session, "added-note");
    // 同时检查 ECS 与当前 BeatMap 的物件数量。
    // 仅更新 Registry 而留下待同步源模型会被 m_needsNotesSync 暴露。
    const auto& context = session.getContext();
    // 联合检查实体、位置、源模型数量和同步状态，防止只更新一侧数据。
    // 专用权威确认计数应为一，完整同步计数应保持零。
    if ( findRootNoteByIdentity(session, "unchanged-note") != unchangedEntity ||
         currentChanged != changedEntity || currentAdded == entt::null ||
         findRootNoteByIdentity(session, "removed-note") != entt::null ||
         !hasRootNote(session, 1.25, 3) || !hasRootNote(session, 1.5, 2) ||
         context.m_needsNotesSync ||
         context.currentBeatmap->m_noteData.notes.size() != 3U ||
         observer->synchronizationCount() != 0 ||
         observer->authoritativeApplyCount() != 1 ||
         observer->lastAuthoritativeRevision() != 42 ) {
        XERROR(
            "Prepared remote object delta did not stay incremental: "
            "unchanged={}, changed={}, added={}, removed={}, pendingSync={}, "
            "fullSyncs={}, preparedSyncs={}, revision={}",
            findRootNoteByIdentity(session, "unchanged-note") ==
                unchangedEntity,
            currentChanged == changedEntity,
            currentAdded != entt::null,
            findRootNoteByIdentity(session, "removed-note") == entt::null,
            context.m_needsNotesSync,
            observer->synchronizationCount(),
            observer->authoritativeApplyCount(),
            observer->lastAuthoritativeRevision());
        return false;
    }
    return true;
}

/// @brief 验证房间掉线后编辑命令被统一拦截且提示在一个离线周期只发布一次。
/// @return 只读状态保持谱面不变，解除只读后编辑恢复时返回 true。
/// @note 离线门闩应同时阻止已经排队和随后入队的编辑。
/// @note 同一离线周期提示一次，与细分类别权限的逐次提示策略不同。
/// @note 解除门闩后新命令应恢复执行，不重放此前被拒绝的命令。
/// @note 事件订阅在返回前移除，避免回调借用已销毁的局部计数器。
/// @note 事件计数不携带其他共享状态，不能把 relaxed 读写当作通用同步屏障。
/// @note 解除只读后检查一条新轨数命令，不覆盖重连协议或远端权限恢复。
[[nodiscard]] bool testOfflineCollaborationSessionIsReadOnly()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = makeBeatmap() },
    });
    session.update(0.0, config, false);

    // 事件回调只累计提示次数，主测试在命令处理后读取。
    // 这里不以计数器发布其他数据，relaxed 足以表达独立统计。
    std::atomic_int blockedEvents{ 0 };
    auto&           eventBus = MMM::Event::EventBus::instance();
    const auto      subscription =
        eventBus.subscribe<MMM::Event::CollaborationOfflineEditBlockedEvent>(
            [&blockedEvents](
                const MMM::Event::CollaborationOfflineEditBlockedEvent&) {
                blockedEvents.fetch_add(1, std::memory_order_relaxed);
            });

    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateTrackCount{ .trackCount = 6 },
    });
    // 第一条轨数编辑已入队但尚未执行，开启离线门闩后也应被拒绝。
    // 后续新命令与旧队列命令都在实际处理入口接受同一检查。
    session.setCollaborationOfflineReadOnly(true);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateTrackCount{ .trackCount = 7 },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        // 混入另一类别编辑，测试离线门闩不是只针对轨数命令。
        // 该命令与轨数命令同轮处理，提示仍应只发布一次。
        MMM::Logic::CmdCreateTimelineEvent{
            .time  = 1.0,
            .type  = MMM::TimingEffect::SCROLL,
            .value = 2.0,
        },
    });
    session.update(0.0, config, false);
    // 多次编辑尝试仅产生一次离线提示，轨数保持初始四轨。
    // 这同时约束状态未改和同一离线周期的提示去重。
    const bool stayedReadOnly =
        session.getContext().trackCount == 4 &&
        blockedEvents.load(std::memory_order_relaxed) == 1;

    // 解除离线状态后重新提交命令，验证门闩可逆。
    // 不要求恢复此前被拒绝的编辑，它们不应继续留在队列中。
    session.setCollaborationOfflineReadOnly(false);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateTrackCount{ .trackCount = 7 },
    });
    session.update(0.0, config, false);
    // 恢复后轨数变成七，证明合法新请求实际执行。
    // 只检查门闩布尔值解除不足以证明队列处理恢复。
    const bool resumedEditing = session.getContext().trackCount == 7;

    // 在所有可能返回的失败检查之前退订，释放回调对局部计数器的借用。
    // EventBus 是全局对象，不能让订阅越过本用例生命周期。
    eventBus.unsubscribe<MMM::Event::CollaborationOfflineEditBlockedEvent>(
        subscription);
    // 退订之后再汇总失败，确保任何失败分支都不会遗留回调。
    // 日志并列记录拒绝和恢复两个阶段，便于定位门闩方向错误。
    if ( !stayedReadOnly || !resumedEditing ) {
        XERROR(
            "Offline collaboration session edit gate failed: readOnly={}, "
            "resumed={}, events={}",
            stayedReadOnly,
            resumedEditing,
            blockedEvents.load(std::memory_order_relaxed));
        return false;
    }
    return true;
}

/// @brief 验证协作细分权限拦截未授权类别并逐次提示独立编辑尝试。
/// @return 权限门闩与重复拒绝提示均正确时返回 true。
/// @note 先允许 Timing，再切换到元数据和采样权限，验证类别过滤方向。
/// @note 同类拒绝在不同编辑尝试中都应提示，不按离线周期去重。
/// @note 允许的命令与被拒绝命令混排，不能因一项拒绝丢掉整批队列。
/// @note 结束前恢复 All 并退订事件，保持测试局部状态完整释放。
/// @note 权限测试针对本地命令入口，不测试服务端对恶意客户端的校验。
/// @note 提示次数反映本次订阅期间事件，不读取先前用例留下的全局累计值。
[[nodiscard]] bool testCollaborationMutationPermissionsAreLocalGate()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = makeBeatmap() },
    });
    session.update(0.0, config, false);
    // 加载可能自动生成默认 Timing，因此保存基线数量而非假设零。
    // 后面只比较本用例增加的一条事件。
    const auto initialTimelineCount =
        session.getContext()
            .timelineRegistry.view<const MMM::Logic::TimelineComponent>()
            .size();

    // 事件回调只累计提示次数，主测试在命令处理后读取。
    // 这里不以计数器发布其他数据，relaxed 足以表达独立统计。
    std::atomic_int blockedEvents{ 0 };
    auto&           eventBus = MMM::Event::EventBus::instance();
    const auto      subscription =
        eventBus.subscribe<MMM::Event::CollaborationPermissionEditBlockedEvent>(
            [&blockedEvents](
                const MMM::Event::CollaborationPermissionEditBlockedEvent&) {
                blockedEvents.fetch_add(1, std::memory_order_relaxed);
            });

    session.setCollaborationAllowedMutationFlags(
        // 第一阶段只允许时间线，轨数属于元数据所以应拒绝。
        // 随后把两种命令一起排队，验证门禁按命令类别逐项判断。
        MMM::BeatmapMutationFlags::Timelines);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateTrackCount{ .trackCount = 7 },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdCreateTimelineEvent{
            .time  = 1.0,
            .type  = MMM::TimingEffect::SCROLL,
            .value = 2.0,
        },
    });
    session.update(0.0, config, false);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateTrackCount{ .trackCount = 8 },
    });
    session.update(0.0, config, false);
    // 轨数两次修改都被拒绝，但允许的 Timing 应成功加入。
    // 拒绝提示按独立尝试计两次，不沿用离线周期的一次提示策略。
    const bool timingOnlyApplied =
        session.getContext().trackCount == 4 &&
        session.getContext()
                .timelineRegistry.view<const MMM::Logic::TimelineComponent>()
                .size() == initialTimelineCount + 1U &&
        blockedEvents.load(std::memory_order_relaxed) == 2;

    session.setCollaborationAllowedMutationFlags(
        // 第二阶段允许元数据和采样，但不含 Timelines。
        // 权限切换不会删除前阶段已创建的时间线事件。
        MMM::BeatmapMutationFlags::Metadata |
        MMM::BeatmapMutationFlags::AudioSamples);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdCreateTimelineEvent{
            .time  = 2.0,
            .type  = MMM::TimingEffect::SCROLL,
            .value = 3.0,
        },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateTrackCount{ .trackCount = 7 },
    });
    session.update(0.0, config, false);
    // 权限切换后轨数可以修改，新增 Timing 必须被拒绝。
    // 前阶段已有 Timing 保留，权限变更不回滚已接受的内容。
    const bool metadataOnlyApplied =
        session.getContext().trackCount == 7 &&
        session.getContext()
                .timelineRegistry.view<const MMM::Logic::TimelineComponent>()
                .size() == initialTimelineCount + 1U &&
        blockedEvents.load(std::memory_order_relaxed) == 3;

    session.setCollaborationAllowedMutationFlags(
        MMM::BeatmapMutationFlags::All);
    // 在所有可能返回的失败检查之前退订，释放回调对局部计数器的借用。
    // EventBus 是全局对象，不能让订阅越过本用例生命周期。
    eventBus.unsubscribe<MMM::Event::CollaborationPermissionEditBlockedEvent>(
        subscription);
    // 两阶段都必须满足允许项成功、禁止项不变和准确提示次数。
    // 仅有最终轨数正确无法证明中间未发生未经授权编辑。
    if ( !timingOnlyApplied || !metadataOnlyApplied ) {
        XERROR(
            "Collaboration mutation permission gate failed: timingOnly={}, "
            "metadataOnly={}, tracks={}, timelines={}, events={}",
            timingOnlyApplied,
            metadataOnlyApplied,
            session.getContext().trackCount,
            session.getContext()
                .timelineRegistry.view<const MMM::Logic::TimelineComponent>()
                .size(),
            blockedEvents.load(std::memory_order_relaxed));
        return false;
    }
    return true;
}

/// @brief 验证关闭 BGM 权限后会拦截轨道数、绘制、擦除及撤销入口。
/// @return BGM 编辑保持不变且普通物件仍可编辑时返回 true。
/// @note 关闭 AudioSamples 类别，其余类别继续允许。
/// @note BGM 轨数、画笔、擦除和撤销都应通过同一权限边界。
/// @note 普通玩家轨画笔作为允许对照，不能把整个会话锁死来满足拒绝断言。
/// @note 先允许生成一份 BGM 编辑历史，再关闭权限检查擦除与撤销。
/// @note 不对擦除预览的颜色或几何作断言，结果只检查正式数据是否改变。
/// @note BGM 轨数虽然保存在元数据中，编辑权限仍按采样类别处理。
[[nodiscard]] bool testCollaborationBgmPermissionIsLocalGate()
{
    MMM::Logic::BeatmapSession session;
    MMM::Config::EditorConfig  config;
    config.visual.trackLayout.left  = 0.1F;
    config.visual.trackLayout.right = 0.5F;
    config.visual.judgeline_pos     = 0.5F;
    // 先打开 BMS 编辑功能，使失败来自协作权限而非功能未启用。
    // 固定布局让 550 像素输入落到 BGM 区，150 像素落到玩家区。
    config.settings.enableBmsEditing = true;

    auto beatmap = makeBeatmap();
    // 从一条持久 BGM 轨开始，轨数编辑尝试把它扩为两条。
    // 初始值与目标值不同，便于辨认门禁是否真的生效。
    beatmap->m_baseMapMetadata.bgm_track_count = 1;
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdLoadBeatmap{ .beatmap = std::move(beatmap) },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateViewport{
            .cameraId = "Basic2DCanvas",
            .width    = 1000.0F,
            .height   = 600.0F,
        },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdChangeTool{ .tool = MMM::Logic::EditTool::Draw },
    });
    session.update(0.0, config, false);

    // 事件回调只累计提示次数，主测试在命令处理后读取。
    // 这里不以计数器发布其他数据，relaxed 足以表达独立统计。
    std::atomic_int blockedEvents{ 0 };
    auto&           eventBus = MMM::Event::EventBus::instance();
    const auto      subscription =
        eventBus.subscribe<MMM::Event::CollaborationPermissionEditBlockedEvent>(
            [&blockedEvents](
                const MMM::Event::CollaborationPermissionEditBlockedEvent&) {
                blockedEvents.fetch_add(1, std::memory_order_relaxed);
            });

    // 权限掩码只去掉 AudioSamples，其余编辑类别明确保留。
    // 这样普通 Note 的成功可以证明不是整个会话被设为只读。
    const auto withoutBgm = MMM::BeatmapMutationFlags::Objects |
                            MMM::BeatmapMutationFlags::Timelines |
                            MMM::BeatmapMutationFlags::Metadata |
                            MMM::BeatmapMutationFlags::Annotations;
    session.setCollaborationAllowedMutationFlags(withoutBgm);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateBgmTrackCount{ .bgmTrackCount = 2 },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdStartBrush{
            .cameraId = "Basic2DCanvas",
            .mouseX   = 550.0F,
            .mouseY   = 150.0F,
        },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" },
    });
    session.update(0.1, config, false);
    // BGM 轨数和采样创建都不得生效，即使画笔手势已正常结束。
    // 通过 Registry 为空验证没有隐藏的正式采样被创建。
    const bool directBgmEditsBlocked =
        session.getContext().bgmTrackCount == 1 &&
        session.getContext()
            .sampleRegistry.view<const MMM::Logic::SampleComponent>()
            .empty();

    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdStartBrush{
            .cameraId = "Basic2DCanvas",
            .mouseX   = 150.0F,
            .mouseY   = 150.0F,
        },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" },
    });
    session.update(0.2, config, false);
    // 玩家区域使用相同画笔入口，应仍生成一个普通 Note。
    // 它是权限分类的正向对照，避免过度拦截通过测试。
    const bool objectEditStillAllowed =
        session.getContext()
            .noteRegistry.view<const MMM::Logic::NoteComponent>()
            .size() == 1U;

    session.setCollaborationAllowedMutationFlags(
        MMM::BeatmapMutationFlags::All);
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdStartBrush{
            .cameraId = "Basic2DCanvas",
            .mouseX   = 550.0F,
            .mouseY   = 150.0F,
        },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdEndBrush{ .cameraId = "Basic2DCanvas" },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdUpdateBgmTrackCount{ .bgmTrackCount = 2 },
    });
    // 这一轮仍为全部权限，先建立一个采样和一次 BGM 轨数历史。
    // 下一阶段关闭权限后，撤销栈顶部就是受限制的 BGM 变更。
    session.update(0.3, config, false);

    // 临时开放权限后创建真实采样，才能测试后续擦除是否被阻止。
    // 取出实体后再收紧权限，不用无效目标制造虚假的删除失败。
    const auto samples =
        session.getContext()
            .sampleRegistry.view<const MMM::Logic::SampleComponent>();
    const auto sampleEntity = samples.empty() ? entt::null : *samples.begin();
    session.setCollaborationAllowedMutationFlags(withoutBgm);
    session.pushCommand(MMM::Logic::LogicCommand{
        // 显式指定采样类别与实体，擦除命令才能到达目标权限分支。
        // 不依赖 UI 光标拾取，测试范围保持为逻辑权限门禁。
        MMM::Logic::CmdSetHoveredEntity{
            .entity   = sampleEntity,
            .part     = 0,
            .subIndex = -1,
            .kind     = MMM::Logic::ChartObjectKind::AudioSample,
        },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdStartErase{ .cameraId = "Basic2DCanvas" },
    });
    session.pushCommand(MMM::Logic::LogicCommand{
        MMM::Logic::CmdEndErase{ .cameraId = "Basic2DCanvas" },
    });
    session.update(0.4, config, false);
    session.pushCommand(MMM::Logic::LogicCommand{ MMM::Logic::CmdUndo{} });
    // 撤销在单独一轮执行，避免与擦除手势结束混成一次状态更新。
    // 轨数保持两条证明历史入口也检查当前权限。
    session.update(0.5, config, false);

    // 采样仍有效证明擦除受阻，BGM 轨数仍为二证明撤销也受阻。
    // 仅检查其中一个结果无法覆盖两个不同编辑入口。
    const bool eraseAndUndoBlocked =
        sampleEntity != entt::null &&
        session.getContext().sampleRegistry.valid(sampleEntity) &&
        session.getContext().bgmTrackCount == 2;
    session.setCollaborationAllowedMutationFlags(
        MMM::BeatmapMutationFlags::All);
    // 在所有可能返回的失败检查之前退订，释放回调对局部计数器的借用。
    // EventBus 是全局对象，不能让订阅越过本用例生命周期。
    eventBus.unsubscribe<MMM::Event::CollaborationPermissionEditBlockedEvent>(
        subscription);

    if ( !directBgmEditsBlocked || !objectEditStillAllowed ||
         !eraseAndUndoBlocked ||
         // 拒绝次数只设下限，不约束复合手势内部每个命令的提示细节。
         // 核心要求是多个受限入口确实反馈拒绝，同时不影响允许的玩家编辑。
         blockedEvents.load(std::memory_order_relaxed) < 2 ) {
        XERROR(
            "Collaboration BGM permission gate failed: direct={}, object={}, "
            "eraseUndo={}, bgmTracks={}, samples={}, events={}",
            directBgmEditsBlocked,
            objectEditStillAllowed,
            eraseAndUndoBlocked,
            session.getContext().bgmTrackCount,
            session.getContext()
                .sampleRegistry.view<const MMM::Logic::SampleComponent>()
                .size(),
            blockedEvents.load(std::memory_order_relaxed));
        return false;
    }
    return true;
}

/// @brief 验证访客连接门闩会清空旧打开请求并拦截所有后续本机项目请求。
/// @return 门闩解除前没有项目动作进入逻辑线程时返回 true。
/// @note 本例操作全局项目控制器，开头和结束都清理挂起切换状态。
/// @note 路径只用于构造请求，不要求实际项目存在或读取文件。
/// @note 开启访客门闩后，既有请求应清除，新请求应被拦截。
/// @note 只验证请求是否进入待处理状态，不执行真实项目加载。
/// @note 未启动项目处理线程，待处理标志是该入口验证的权威状态。
/// @note 测试请求路径不会被当作真实输入交给加载器。
[[nodiscard]] bool testGuestConnectionBlocksLocalProjectOpening()
{
    auto& controller = MMM::Logic::ProjectController::instance();
    controller.setLocalProjectOpeningBlockedByCollaboration(false);
    // 先清理单例中可能遗留的请求，再建立一份可观察的待开项目。
    // 本例只检查排队，不调用实际文件加载逻辑。
    controller.cancelPendingProjectSwitch();
    controller.requestOpenProject("/tmp/mmm-collaboration-pending-project");
    // 门闩关闭时请求必须先成功进入队列，才能验证之后的清除。
    // 否则始终拒绝全部请求的错误实现也可能通过后半段测试。
    if ( !controller.hasPendingProjectAction() ) {
        XERROR("Local project request was not queued before collaboration");
        return false;
    }

    // 事件回调只累计提示次数，主测试在命令处理后读取。
    // 这里不以计数器发布其他数据，relaxed 足以表达独立统计。
    std::atomic_int blockedEvents{ 0 };
    auto&           eventBus = MMM::Event::EventBus::instance();
    const auto      subscription =
        eventBus.subscribe<MMM::Event::CollaborationProjectOpenBlockedEvent>(
            [&blockedEvents](
                const MMM::Event::CollaborationProjectOpenBlockedEvent&) {
                blockedEvents.fetch_add(1, std::memory_order_relaxed);
            });

    // 开启访客连接门闩应清空之前的请求。
    // 随后再请求另一路径，验证门闩也覆盖新到请求。
    controller.setLocalProjectOpeningBlockedByCollaboration(true);
    controller.requestOpenProject("/tmp/mmm-collaboration-blocked-project");
    // 同时检查门闩状态、队列为空和新请求拒绝提示。
    // 仅检查状态布尔值不能发现旧项目请求仍待逻辑线程消费。
    const bool blocked =
        controller.isLocalProjectOpeningBlockedByCollaboration() &&
        !controller.hasPendingProjectAction() &&
        blockedEvents.load(std::memory_order_relaxed) == 1;

    controller.setLocalProjectOpeningBlockedByCollaboration(false);
    controller.cancelPendingProjectSwitch();
    // 在所有可能返回的失败检查之前退订，释放回调对局部计数器的借用。
    // EventBus 是全局对象，不能让订阅越过本用例生命周期。
    eventBus.unsubscribe<MMM::Event::CollaborationProjectOpenBlockedEvent>(
        subscription);
    if ( !blocked ) {
        XERROR("Guest collaboration did not isolate local project requests");
        return false;
    }
    return true;
}
}  // namespace

/// @brief 运行协作谱面观察者绑定回归测试。
/// @return 全部断言通过时返回 0。
/// @note 文件门闩用例先运行，之后顺序验证观察者与协作权限。
/// @note 每个子测试同步驱动会话，退出码供 CTest 汇总结果。
int main()
{
    /// @brief 模拟 UI 帧占用门闩，验证逻辑不会等待、丢失或倒置文件指令。
    /// @return 持锁时延后且解锁后顺序处理两条文件命令时返回 true。
    /// @note 路径是哨兵参数，没有载入谱面，不把文件内容作为成功依据。
    /// @warning 仅在测试入口运行；latch 与 join 用于可证明的跨线程持锁顺序，
    /// 不得将此阻塞夹具复制到逻辑 update 或 UI 热路径。
    const auto testDeferredFileCommands = []() {
        MMM::Logic::BeatmapSession session;
        MMM::Config::EditorConfig  config;
        // 保存进度只记录 active 阶段名称，用顺序验证两个文件命令没有倒置。
        // 没有加载谱面，测试不要求成功生成 unused 路径对应的文件。
        std::vector<std::string> stages;
        auto                     subscription =
            MMM::Event::EventBus::instance()
                .subscribe<MMM::Event::BeatmapSaveProgressEvent>(
                    [&](const auto& event) {
                        // 只记录开始阶段，忽略结束或失败状态，避免影响命令启动顺序。
                        // 回调由本测试同步处理命令时触发，UI 夹具线程不写
                        // stages。
                        if ( event.active ) stages.push_back(event.stage);
                    });
        // 两只 latch 分别确认持锁已建立和允许模拟 UI 释放锁。
        // 同步依赖事件关系，不用固定毫秒等待推测线程是否已启动。
        std::latch   locked(1), release(1);
        std::jthread uiFrame([&]() {
            // 模拟 UI 帧占用文件操作门闩，主测试必须在它持锁期间调用 update。
            // 先通知 locked 再等待 release，保证测试实际覆盖竞争条件。
            std::lock_guard lock(MMM::Event::beatmapFileOperationGate());
            locked.count_down();
            release.wait();
        });
        // 主线程等到模拟 UI 确实持锁，才排队文件命令。
        // 该等待用于测试前置条件，不是生产逻辑等待文件锁的实现。
        // UI 线程只操作门闩和 latch，不读取会话或修改进度列表。
        // 因此观察者和进度字符串仍按单线程顺序访问。
        locked.wait();
        // 先保存再导出，两个不同进度阶段作为命令顺序哨兵。
        // 没有谱面时执行可以失败，但不能丢失其开始处理的次序。
        session.pushCommand(
            MMM::Logic::CmdSaveBeatmapAs{ .path = "unused.mmm" });
        session.pushCommand(
            MMM::Logic::CmdExportImdPackage{ .path = "unused.zip" });
        session.update(0.0, config, false);
        // 持锁期间命令仍待处理，且不能发出已开始保存的进度阶段。
        // update 必须返回才能到达释放 latch 的语句，因此也检验非阻塞处理。
        const bool deferred = session.hasPendingCommands() && stages.empty();
        // 先允许 UI 释放门闩，再 join 确认该线程退出。
        // 不能先 join，否则持锁线程仍等待本线程发送释放信号。
        release.count_down();
        // join 返回后锁已释放，下一次 update 无需再竞争模拟 UI。
        // 没有固定超时窗口，线程退出由 release 信号明确驱动。
        uiFrame.join();
        // 释放锁后只更新一次，证明不需要额外轮询才能开始处理队首文件命令。
        // 退订发生在命令消费后，所有预期阶段都已经记录完毕。
        session.update(0.0, config, false);
        MMM::Event::EventBus::instance()
            .unsubscribe<MMM::Event::BeatmapSaveProgressEvent>(subscription);
        // 解锁后的下一轮应按保存、导出顺序各启动一次并排空队列。
        // 数量和顺序一起检查，防止命令重复、丢失或颠倒。
        return deferred && !session.hasPendingCommands() &&
               stages.size() == 2 && stages[0] == "正在保存谱面…" &&
               stages[1] == "正在准备资源包…";
    };
    // 先覆盖持锁延后，再验证同步与权限，失败通过短路返回非零。
    // 各用例必须自行释放订阅和恢复单例状态，不能依赖后续用例清理。
    return testDeferredFileCommands() && testOptionalInitialSnapshot() &&
                   testTimelineCommandsPublishMutations() &&
                   testBeatmapAnnotationPermissionAndTimestampGrouping() &&
                   testRemoteSynchronizationPreservesActiveBrush() &&
                   testRemoteSynchronizationWaitsForLocalMutationReceipt() &&
                   testPreparedRemoteObjectDeltaAppliesIncrementally() &&
                   testOfflineCollaborationSessionIsReadOnly() &&
                   testCollaborationMutationPermissionsAreLocalGate() &&
                   testCollaborationBgmPermissionIsLocalGate() &&
                   testGuestConnectionBlocksLocalProjectOpening()
               ? 0
               : 1;
}
