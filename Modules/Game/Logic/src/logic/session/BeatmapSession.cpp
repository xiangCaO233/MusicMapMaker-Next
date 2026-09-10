#include "logic/BeatmapSession.h"
#include "audio/AudioManager.h"
#include "common/LogicCommandMutationClassification.h"
#include "config/EditorSettings.h"
#include "config/Utf8Path.h"
#include "event/core/EventBus.h"
#include "event/logic/BeatmapSaveResultEvent.h"
#include "event/project/ProjectEvents.h"
#include "log/colorful-log.h"
#include "logic/BeatmapBackupService.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectDraftLaneService.h"
#include "logic/UnlimitedIdleUpdateGate.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"

#include "logic/session/ActionController.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/InteractionController.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include <algorithm>
#include <chrono>
#include <cmath>

/// @brief 时间线组件增删改后使滚速映射缓存失效。
/// @param reg 发出组件信号的时间线注册表。
/// @note 实体参数由信号接口传入，此处只关心整份映射已过期。
/// @warning 组件信号回调只设置脏标记，不在修改组件期间重建缓存。
/// @note 回调不使用实体本身，因此删除信号触发时不要求组件仍可读取。
static void markScrollCacheDirty(entt::registry& reg, entt::entity)
{
    // 缓存可能随会话生命周期拆除，回调不为缺失缓存重新创建上下文数据。
    if ( auto* cache = reg.ctx().find<MMM::Logic::System::ScrollCache>() ) {
        cache->isDirty = true;
    }
}

namespace MMM::Logic
{

namespace
{
/// @brief Note 编辑后延迟同步 BeatMap 的空闲等待时间（秒）。
/// @note 只推迟完整模型同步，不阻塞线程或延迟 ECS 中的本地编辑反馈。
constexpr double DEFERRED_BEATMAP_SYNC_IDLE_SECONDS = 1.0;

/// @brief 元数据连续编辑停止后执行自动保存的空闲等待时间（秒）。
/// @note 连续输入更新最后修改时间，结束输入后再合并为一次保存请求。
constexpr double METADATA_AUTO_SAVE_IDLE_SECONDS = 0.75;

/// @brief 非忙碌状态下 Session 逻辑轻量轮询的最小间隔。
constexpr double IDLE_UPDATE_MIN_INTERVAL_SECONDS =
    std::chrono::duration<double>(UNLIMITED_IDLE_SESSION_POLL_INTERVAL).count();

/// @brief 视觉动画目标值的吸附阈值。
/// @note 达到阈值后直接采用目标值，防止长期保留无意义的微小动画尾差。
constexpr double VISUAL_ANIMATION_EPSILON = 0.0001;

/// @brief 限制单帧视觉动画步长，避免后台恢复时跨越过大。
constexpr double VISUAL_ANIMATION_MAX_DT = 0.05;

/// @brief 指数平滑系数，约等于在配置时长内完成 99.75% 的位移。
constexpr double VISUAL_ANIMATION_RESPONSE = 6.0;

/// @brief 物件绑定音效后台加载推进间隔。
/// @note 这是预读调度间隔，不是主逻辑循环必须等待的固定时长。
constexpr double BOUND_SOUND_PREFETCH_INTERVAL_SECONDS = 0.01;

/// @brief 物件绑定音效相对当前播放位置的预读窗口。
/// @note 窗口单位为谱面秒数，不是等待资源加载完成的超时时长。
constexpr double BOUND_SOUND_PREFETCH_WINDOW_SECONDS = 5.0;

/// @brief 单次预读最多检查的打击事件数量。
/// @note 预算包括未绑定音效的事件，限制的是扫描成本而非成功入队数量。
constexpr std::size_t MAX_BOUND_SOUND_PREFETCH_EVENTS_PER_TICK = 256U;

/// @brief 判断画笔位置将编辑普通物件还是 BGM 自动采样。
/// @param ctx 当前谱面会话。
/// @param cameraId 命令所属画布 ID。
/// @param mouseX 鼠标在画布内的横坐标。
/// @return BGM 轨道返回 AudioSamples，其余位置返回 Objects。
/// @note 返回的是协作权限类别，不表示此处已经生成物件或修改谱面。
/// @warning 命令权限检查热路径：只读取相机和配置缓存并执行常量级投影计算。
[[nodiscard]] ::MMM::BeatmapMutationFlags brushMutationFlagsAt(
    const SessionContext& ctx, const std::string& cameraId, float mouseX)
{
    if ( cameraId == "Preview" || cameraId == "PreviewCanvas" ) {
        // 预览画布的画笔操作按普通物件分类，不用主画布的 BGM 分区判断。
        return ::MMM::BeatmapMutationFlags::Objects;
    }

    const auto camera = ctx.cameras.find(cameraId);
    if ( camera == ctx.cameras.end() ) {
        // 无法投影时仍要求普通物件编辑权限，不把未知画布当成无修改命令。
        return ::MMM::BeatmapMutationFlags::Objects;
    }
    // 权限判断复用实际绘制的轨道布局，包含横向偏移、专业模式与草稿区。
    const auto lanes =
        calculateCanvasLaneProjection(camera->second.viewportWidth,
                                      ctx.trackCount,
                                      ctx.bgmTrackCount,
                                      ctx.lastConfig.visual.trackLayout,
                                      camera->second.horizontalOffsetX,
                                      true,
                                      ctx.lastConfig.settings.enableBmsEditing,
                                      ctx.lastConfig.settings.professionalMode,
                                      ctx.draftTrackCount,
                                      true);
    const auto lane = lanes.laneAt(mouseX);
    // 按投影后的横坐标命中轨道，不能把鼠标像素直接转换为绝对轨号。
    // 只有明确命中 BGM 轨道才归类为样本，其他位置保留普通物件权限要求。
    return lane && lane->kind == CanvasLaneKind::Bgm
               ? ::MMM::BeatmapMutationFlags::AudioSamples
               : ::MMM::BeatmapMutationFlags::Objects;
}

/// @brief 返回当前悬停物件对应的协作权限类别。
/// @param ctx 当前谱面会话。
/// @return 有效悬停物件的数据类别；没有有效目标时返回 None。
/// @note 草稿音符仍归入 Objects 权限，不单独增加一个草稿权限位。
/// @warning 命令权限检查热路径：仅在两个独立 Registry 中执行常量级实体查询。
[[nodiscard]] ::MMM::BeatmapMutationFlags hoveredMutationFlags(
    const SessionContext& ctx)
{
    if ( ctx.hoveredEntity == entt::null ) {
        // 悬停标记为空时不能从上一次物件领域推测编辑目标。
        return ::MMM::BeatmapMutationFlags::None;
    }
    if ( ctx.hoveredObjectKind == ChartObjectKind::AudioSample ) {
        // 样本与音符分属不同注册表，必须按领域验证实体和对应组件。
        return ctx.sampleRegistry.valid(ctx.hoveredEntity) &&
                       ctx.sampleRegistry.all_of<SampleComponent>(
                           ctx.hoveredEntity)
                   ? ::MMM::BeatmapMutationFlags::AudioSamples
                   : ::MMM::BeatmapMutationFlags::None;
    }
    return ctx.noteRegistry.valid(ctx.hoveredEntity) &&
                   ctx.noteRegistry.all_of<NoteComponent>(ctx.hoveredEntity)
               ? ::MMM::BeatmapMutationFlags::Objects
               : ::MMM::BeatmapMutationFlags::None;
}

/// @brief 将当前时间窗口内的物件绑定音效增量加入后台加载队列。
/// @param ctx 当前谱面会话。
/// @pre hitEvents 保持时间顺序，nextBoundSoundPrefetchIndex
/// 与当前索引版本对应。
/// @note 本函数不自行读取时钟节流，调用方负责按预读间隔决定是否调用。
/// @warning 逻辑低频预读路径：由系统时间节流，只线性推进尚未检查的事件，
/// 不得访问文件系统或回扫完整事件表。
void prefetchBoundNoteSounds(SessionContext& ctx)
{
    // 预读沿有序事件表单向推进；跳转后的游标重置由命中索引同步流程负责。
    auto&        audioManager = Audio::AudioManager::instance();
    const double prefetchEnd =
        ctx.animateTime + BOUND_SOUND_PREFETCH_WINDOW_SECONDS;
    std::size_t examinedCount = 0U;

    while ( ctx.nextBoundSoundPrefetchIndex < ctx.hitEvents.size() &&
            examinedCount < MAX_BOUND_SOUND_PREFETCH_EVENTS_PER_TICK ) {
        // 同时限制时间窗口与检查数量，密集谱面也不会在一次轮询扫完整个窗口。
        const auto& event = ctx.hitEvents[ctx.nextBoundSoundPrefetchIndex];
        if ( event.timestamp > prefetchEnd ) break;
        // 遇到窗口之后的首项就停止，保留游标等待播放位置继续推进。
        if ( event.sampleBinding &&
             !event.sampleBinding->m_audioResourceId.empty() ) {
            // 这里只提交资源身份，实际资源加载由音频管理器的队列推进。
            audioManager.queueBoundNoteSoundEffectLoad(
                event.sampleBinding->m_audioResourceId);
        }
        ++ctx.nextBoundSoundPrefetchIndex;
        // 没有绑定音效的事件也计入检查预算，避免稀疏绑定导致无界扫描。
        ++examinedCount;
    }

    // 即使本轮没有新增请求，也继续处理先前排队的加载工作。
    audioManager.updateQueuedSoundEffectLoads();
}

/// @brief 规范化时间线缩放倍率，避免无效配置进入视觉动画。
/// @param zoom 输入缩放倍率。
/// @return 可用于坐标映射的正缩放倍率。
/// @note 回退只返回本次计算值，不在这里改写持久化配置。
/// @warning 逻辑热路径：每个 Session update 执行；只做常量级数值检查。
double sanitizeTimelineZoom(double zoom)
{
    if ( !std::isfinite(zoom) || zoom <= VISUAL_ANIMATION_EPSILON ) {
        // 拒绝非有限、负值及过小尺度，统一退回单位缩放而非传播退化坐标。
        return 1.0;
    }
    return zoom;
}

/// @brief 判断当前配置是否启用了指定自动保存事件。
/// @param config 软件全局自动保存配置。
/// @param trigger 待检查的事件。
/// @return 事件模式已启用且对应事件开关打开时返回 true。
[[nodiscard]] bool isAutoSaveTriggerEnabled(
    const Config::AutoSaveConfig& config, AutoSaveTrigger trigger)
{
    // 定时模式不能顺带响应事件触发，事件开关只在对应模式下生效。
    if ( config.mode != Config::AutoSaveMode::EventTriggered ) return false;

    switch ( trigger ) {
    case AutoSaveTrigger::ObjectModified: return config.onObjectModified;
    case AutoSaveTrigger::BeatmapSwitch: return config.onBeatmapSwitch;
    case AutoSaveTrigger::ImGuiWindowFocusLost:
        return config.onImGuiWindowFocusLost;
    case AutoSaveTrigger::NativeWindowFocusLost:
        return config.onNativeWindowFocusLost;
    }
    return false;
}

/// @brief 判断自动备份配置是否允许指定编辑器事件。
/// @param config 项目覆盖后的有效自动备份配置。
/// @param trigger 待检查的事件。
/// @return 事件模式已启用且对应事件开关打开时返回 true。
[[nodiscard]] bool isAutoBackupTriggerEnabled(
    const Config::AutoBackupConfig& config, AutoSaveTrigger trigger)
{
    // 保存与备份各自检查有效配置，开启一种行为不隐式开启另一种。
    if ( config.mode != Config::AutoSaveMode::EventTriggered ) return false;

    switch ( trigger ) {
    case AutoSaveTrigger::ObjectModified: return config.onObjectModified;
    case AutoSaveTrigger::BeatmapSwitch: return config.onBeatmapSwitch;
    case AutoSaveTrigger::ImGuiWindowFocusLost:
        return config.onImGuiWindowFocusLost;
    case AutoSaveTrigger::NativeWindowFocusLost:
        return config.onNativeWindowFocusLost;
    }
    return false;
}
}  // namespace

/// @brief 建立会话上下文、领域控制器和时间线缓存失效连接。
/// @note 控制器借用同一上下文，必须先构造上下文再创建控制器。
/// @note 初始化只建立运行期状态，谱面内容通过后续加载命令进入会话。
BeatmapSession::BeatmapSession()
{
    // 会话持有所有权，各领域控制器共享数据但不各自复制会话状态。
    m_ctx         = std::make_unique<SessionContext>();
    m_playback    = std::make_unique<PlaybackController>(*m_ctx);
    m_interaction = std::make_unique<InteractionController>(*m_ctx);
    m_actions     = std::make_unique<ActionController>(*m_ctx);

    // 先放入缓存再连接信号，组件变化只需标脏，缓存消费时再按需更新。
    m_ctx->timelineRegistry.ctx().emplace<System::ScrollCache>();
    m_ctx->timelineRegistry.on_construct<TimelineComponent>()
        .connect<&markScrollCacheDirty>();
    m_ctx->timelineRegistry.on_update<TimelineComponent>()
        .connect<&markScrollCacheDirty>();
    // 删除事件同样改变滚速映射，不能只监听创建和属性更新。
    m_ctx->timelineRegistry.on_destroy<TimelineComponent>()
        .connect<&markScrollCacheDirty>();
}

/// @brief 在实现文件中销毁会话私有状态及控制器。
/// @note 此处各私有类型定义完整，支持公开头中的前向声明与独占所有权。
/// @pre 调用方已停止对该会话提交命令和更新，不与销毁并发访问控制器。
BeatmapSession::~BeatmapSession() = default;

/// @brief 通过协作编辑限制检查后将命令移入会话队列。
/// @param cmd 待处理命令，入队成功后其所有权转交队列。
/// @note 此处只做提交前检查，具体状态修改由会话消费命令时完成。
/// @note 拦截分支不移动 cmd；只有通过两种门控后才把内容移入队列。
/// @note 排队期间权限仍可能变化，提交前检查不能代替消费时的状态检查。
void BeatmapSession::pushCommand(LogicCommand&& cmd)
{
    // 离线编辑限制与权限限制任一不满足，都不让命令进入待处理队列。
    if ( blockCollaborationOfflineEdit(cmd) ||
         blockCollaborationUnauthorizedEdit(cmd) ) {
        return;
    }
    // 不在提交线程直接执行命令，维持会话状态更新的统一入口。
    m_commandQueue.enqueue(std::move(cmd));
}

/// @brief 根据命令实际影响的数据领域拦截无权限编辑。
/// @param cmd 待检查的命令。
/// @param inspectSessionState 是否结合当前手势、选择和历史栈细化类别。
/// @return 权限不足并已发布拦截事件时返回 true。
/// @note 提交前可以仅依据命令类型判断，消费阶段再结合会话状态细化目标领域。
/// @warning 读取会话状态的分支须由会话状态所属线程调用，不得并发遍历 ECS。
/// @warning
/// 权限由协作控制方原子发布，提交及消费命令时读取，避免跨线程门控竞争。
bool BeatmapSession::blockCollaborationUnauthorizedEdit(
    const LogicCommand& cmd, bool inspectSessionState)
{
    auto required = requiredBeatmapMutationFlags(cmd);
    // 命令静态类别无法表达所有副作用，执行前可根据当前数据补足权限要求。
    if ( inspectSessionState ) {
        // 动态分类只补充当前命令所需信息，不通过扫描整个谱面推断权限。
        if ( const auto* metadata =
                 std::get_if<CmdUpdateBeatmapMetadata>(&cmd) ) {
            const auto samples =
                m_ctx->sampleRegistry.view<const SampleComponent>();
            // 只检查是否存在采样；没有采样时不额外要求采样迁移权限。
            if ( m_ctx->currentBeatmap && !samples.empty() ) {
                // 玩家宽度与主音轨绑定变化可能迁移样本，不只是元数据编辑。
                const auto& current = m_ctx->currentBeatmap->m_baseMapMetadata;
                if ( metadata->baseMeta.track_count != m_ctx->trackCount ||
                     metadata->baseMeta.main_audio_path !=
                         current.main_audio_path ||
                     metadata->baseMeta.song_file_hint !=
                         current.song_file_hint ) {
                    required |= ::MMM::BeatmapMutationFlags::AudioSamples;
                }
            }
        } else if ( std::holds_alternative<CmdUpdateTrackCount>(cmd) ) {
            // 没有样本时只保留元数据权限，避免无条件要求无关领域的编辑权。
            if ( !m_ctx->sampleRegistry.view<const SampleComponent>()
                      .empty() ) {
                required |= ::MMM::BeatmapMutationFlags::AudioSamples;
            }
        } else if ( const auto* startBrush =
                        std::get_if<CmdStartBrush>(&cmd) ) {
            required = brushMutationFlagsAt(
                *m_ctx, startBrush->cameraId, startBrush->mouseX);
        } else if ( const auto* updateBrush =
                        std::get_if<CmdUpdateBrush>(&cmd) ) {
            // 移动中的画笔可能跨入另一个数据区域，不能沿用开始时的静态类别。
            required = brushMutationFlagsAt(
                *m_ctx, updateBrush->cameraId, updateBrush->mouseX);
        } else if ( std::holds_alternative<CmdEndBrush>(cmd) ) {
            // 结束手势以已建立笔刷的物件类型为准，不再依赖鼠标当前所在轨道。
            required = m_ctx->brushState.isActive
                           ? (m_ctx->brushState.createsAudioSample
                                  ? ::MMM::BeatmapMutationFlags::AudioSamples
                                  : ::MMM::BeatmapMutationFlags::Objects)
                           : ::MMM::BeatmapMutationFlags::None;
        } else if ( std::holds_alternative<CmdStartErase>(cmd) ||
                    std::holds_alternative<CmdUpdateErase>(cmd) ) {
            required = hoveredMutationFlags(*m_ctx);
        } else if ( std::holds_alternative<CmdEndErase>(cmd) ) {
            // 提交擦除检查累积目标集合；空手势没有需要授权的数据变化。
            required = m_ctx->eraserState.isActive &&
                               !m_ctx->eraserState.targetEntities.empty()
                           ? (m_ctx->eraserState.targetObjectKind ==
                                      ChartObjectKind::AudioSample
                                  ? ::MMM::BeatmapMutationFlags::AudioSamples
                                  : ::MMM::BeatmapMutationFlags::Objects)
                           : ::MMM::BeatmapMutationFlags::None;
        } else if ( std::holds_alternative<CmdUndo>(cmd) ) {
            // 撤销权限取实际栈顶动作，不能将所有历史操作一概视作音符修改。
            required = m_ctx->actionStack.undoMutationFlags();
        } else if ( std::holds_alternative<CmdRedo>(cmd) ) {
            required = m_ctx->actionStack.redoMutationFlags();
        } else if ( std::holds_alternative<CmdPaste>(cmd) ) {
            // 混合剪贴板要求各类权限的并集，不能只依据首个非空领域放行。
            if ( !m_ctx->clipboard.empty() ) {
                required |= ::MMM::BeatmapMutationFlags::Objects;
            }
            if ( !m_ctx->sampleClipboard.empty() ) {
                required |= ::MMM::BeatmapMutationFlags::AudioSamples;
            }
            if ( required == ::MMM::BeatmapMutationFlags::None ) {
                // 进程外剪贴板尚未解析时无法证明具体类别，必须由完整编辑权限
                // 接受，避免伪造剪贴板内容绕过本地门闩。
                required = ::MMM::BeatmapMutationFlags::All;
            }
        } else if ( std::holds_alternative<CmdCut>(cmd) ||
                    std::holds_alternative<CmdDeleteSelected>(cmd) ||
                    std::holds_alternative<CmdUpdateSelectedObjectSampleVolume>(
                        cmd) ) {
            // 选择集可能同时包含音符和样本，两个独立索引都参与分类。
            if ( !m_ctx->selectedNoteEntities.empty() ) {
                required |= ::MMM::BeatmapMutationFlags::Objects;
            }
            if ( !m_ctx->selectedSampleEntities.empty() ) {
                required |= ::MMM::BeatmapMutationFlags::AudioSamples;
            }
        }
    }
    if ( required == ::MMM::BeatmapMutationFlags::None ) return false;
    // 只读/无修改命令不需要权限位；编辑命令必须具备全部所需位。

    const auto allowed = static_cast<::MMM::BeatmapMutationFlags>(
        m_collaborationAllowedMutationFlags.load(std::memory_order_acquire));
    const auto requiredBits = static_cast<std::uint8_t>(required);
    const auto allowedBits  = static_cast<std::uint8_t>(allowed);
    // 要求所需位全部包含在许可集合中，交集非空不足以允许混合领域编辑。
    if ( (requiredBits & allowedBits) == requiredBits ) return false;

    Event::EventBus::instance().publish(
        Event::CollaborationPermissionEditBlockedEvent{});
    return true;
}

/// @brief 离线只读状态下阻止编辑命令，并合并重复提示。
/// @param cmd 待检查命令；非编辑命令仍可通过。
/// @return 本条命令是否被离线策略拦截。
/// @note 去重只影响提示，不影响拦截结果；重复的编辑命令仍返回 true。
/// @warning
/// 输入提交方读取只读状态并交换提示标记，协作状态切换方负责发布和复位。
bool BeatmapSession::blockCollaborationOfflineEdit(const LogicCommand& cmd)
{
    if ( !m_collaborationOfflineReadOnly.load(std::memory_order_acquire) ||
         !isBeatmapEditingCommand(cmd) ) {
        return false;
    }
    if ( !m_offlineEditBlockedNotificationSent.exchange(
             true, std::memory_order_acq_rel) ) {
        // 多个输入来源可能同时遭到拦截，交换标记保证一次离线阶段只提示一次。
        Event::EventBus::instance().publish(
            Event::CollaborationOfflineEditBlockedEvent{});
    }
    return true;
}

/// @brief 发布离线只读状态并请求逻辑线程更新相关交互状态。
/// @param readOnly 是否进入离线只读模式。
/// @note 即时门控使用原子状态，具体手势清理由排队命令完成。
/// @note 状态发布与命令处理不是同一步，调用返回时不能假定逻辑线程已清理手势。
void BeatmapSession::setCollaborationOfflineReadOnly(bool readOnly)
{
    const bool previous = m_collaborationOfflineReadOnly.exchange(
        readOnly, std::memory_order_acq_rel);
    if ( !readOnly ) {
        // 恢复在线后允许下一次离线阶段重新提示，不沿用旧的提示去重状态。
        m_offlineEditBlockedNotificationSent.store(false,
                                                   std::memory_order_release);
    }
    if ( previous == readOnly ) return;
    // 状态未变化不重复排队，避免连续状态广播反复取消交互。
    m_commandQueue.enqueue(
        LogicCommand(CmdSetCollaborationOfflineReadOnly{ readOnly }));
    // 内部状态命令直接入队，不经过用户编辑的提交前拦截。
}

/// @brief 读取供跨线程命令提交使用的离线只读门控。
/// @return 最近发布的只读状态，不表示相关排队命令已经执行。
/// @warning 原子读取用于跨线程门控，不允许据此直接访问逻辑线程的手势数据。
bool BeatmapSession::isCollaborationOfflineReadOnly() const
{
    return m_collaborationOfflineReadOnly.load(std::memory_order_acquire);
}

/// @brief 切换剪贴板隔离策略，并为新的隔离阶段分配作用域身份。
/// @param isolated 是否限制剪贴板在当前隔离作用域内使用。
/// @note 仅在开关发生变化时创建新阶段，重复设置不刷新已有作用域身份。
void BeatmapSession::setCollaborationClipboardIsolated(bool isolated)
{
    // 进程内各会话共用编号源；计数器仅保证编号唯一，不承载其他数据发布。
    static std::atomic_uint64_t nextScopeId{ 1 };
    const bool previous = m_collaborationClipboardIsolated.exchange(
        isolated, std::memory_order_acq_rel);
    if ( previous == isolated ) return;

    const auto scopeId =
        isolated ? nextScopeId.fetch_add(1, std::memory_order_relaxed) : 0U;
    // 编号只承担身份区分，不用作不同会话之间编辑动作的时序证明。
    // 零表示非隔离作用域，每次重新进入隔离使用新身份，旧内容不能自动复用。
    m_collaborationClipboardScopeId.store(scopeId, std::memory_order_release);
    m_commandQueue.enqueue(LogicCommand(
        CmdSetCollaborationClipboardIsolation{ isolated, scopeId }));
}

/// @brief 查询最近发布的剪贴板隔离开关。
/// @return 开关状态；逻辑线程的剪贴板清理由对应命令异步处理。
/// @note 开关与作用域 ID 分别发布，本查询不提供二者的一致性快照。
bool BeatmapSession::isCollaborationClipboardIsolated() const
{
    return m_collaborationClipboardIsolated.load(std::memory_order_acquire);
}

/// @brief 发布允许修改的数据类别，并在受限状态下取消未提交的交互。
/// @param allowedFlags 协作允许的类别组合，未知位会被剔除。
/// @note 恢复全部权限只更新门控；此函数不负责恢复此前取消的手势。
void BeatmapSession::setCollaborationAllowedMutationFlags(
    ::MMM::BeatmapMutationFlags allowedFlags)
{
    // 只保留已定义类别，避免外部协议中的未知位进入本地授权比较。
    const auto sanitized =
        static_cast<std::uint8_t>(allowedFlags) &
        static_cast<std::uint8_t>(::MMM::BeatmapMutationFlags::All);
    const auto previous = m_collaborationAllowedMutationFlags.exchange(
        sanitized, std::memory_order_acq_rel);
    if ( previous == sanitized ) return;

    if ( sanitized !=
         static_cast<std::uint8_t>(::MMM::BeatmapMutationFlags::All) ) {
        // 权限收紧时复用逻辑线程的交互取消流程，避免正在拖拽或绘制的草稿
        // 在旧权限下继续提交。
        m_commandQueue.enqueue(
            LogicCommand(CmdSetCollaborationOfflineReadOnly{ true }));
    }
}

/// @brief 获取跨线程发布的当前编辑权限集合。
/// @return 已过滤为本地支持类别的权限位。
/// @warning
/// 原子读取与协作控制方发布配对，返回值只描述当前门控，不锁定后续编辑权限。
::MMM::BeatmapMutationFlags
BeatmapSession::collaborationAllowedMutationFlags() const
{
    return static_cast<::MMM::BeatmapMutationFlags>(
        m_collaborationAllowedMutationFlags.load(std::memory_order_acquire));
}

/// @brief 替换变更观察者，并可请求逻辑线程发布当前全量快照。
/// @param observer 新观察者；空指针解除观察。
/// @param publishCurrentSnapshot 是否为非空观察者安排初始快照。
/// @note 此处不读取 ECS 或同步谱面，快照工作推迟到逻辑线程。
/// @note 观察者替换不承诺立即完成首次回调；调用方不能以返回时刻判断同步就绪。
void BeatmapSession::setMutationObserver(
    std::shared_ptr<::MMM::IBeatmapMutationObserver> observer,
    bool                                             publishCurrentSnapshot)
{
    const bool requestSnapshot = observer != nullptr && publishCurrentSnapshot;
    // 更换观察者后旧的已接受序号不再适用，新的同步关系从零开始。
    m_latestAcceptedLocalObjectMutationSequence.store(
        0, std::memory_order_release);
    m_mutationObserver.store(std::move(observer), std::memory_order_release);
    m_mutationSnapshotRequested.store(requestSnapshot,
                                      std::memory_order_relaxed);
    // 请求位只表达待办，观察者本身通过独立的共享指针发布与获取维持生命周期。
}

/// @brief 消费一次初始快照请求并将同步后的完整谱面交给观察者。
/// @warning
/// 逻辑线程低频请求路径，可能同步完整谱面；共享观察者保证回调期间存活。
void BeatmapSession::publishRequestedMutationSnapshot()
{
    // 先消费请求，避免一次初始订阅在后续普通 update 中重复发布完整谱面。
    if ( !m_mutationSnapshotRequested.exchange(false,
                                               std::memory_order_relaxed) ) {
        return;
    }
    auto observer = m_mutationObserver.load(std::memory_order_acquire);
    // 回调使用局部共享所有权，外部同时解除观察不会使当前调用目标悬空。
    // 请求消费后观察者可能已解除，或会话尚无谱面，此时不发布空快照。
    if ( !observer || !m_ctx->currentBeatmap ) return;
    // 已消费但未发布的请求不自动重置；重新发布须由后续订阅或显式请求安排。

    SessionUtils::syncBeatmap(*m_ctx);
    // 先把 ECS 编辑回写到模型，再发布 All，防止初始快照落后于画面状态。
    static_cast<void>(observer->onBeatmapMutated(
        *m_ctx->currentBeatmap, ::MMM::BeatmapMutationFlags::All));
}

/// @brief 判断会话是否存在等待逻辑线程消费的指令。
/// @return 延迟文件命令存在或队列近似长度非零时返回 true。
/// @note 这是调度提示，不是保证队列为空的同步屏障。
bool BeatmapSession::hasPendingCommands() const
{
    // 延迟文件命令已离开队列但仍待执行，不能仅凭队列近似长度判断空闲。
    return m_deferredFileCommand.has_value() ||
           m_commandQueue.size_approx() > 0;
}

/// @brief 判断会话是否需要跳过后台限频并立即更新。
/// @return 命令、播放、编辑手势或视觉动画任一仍活跃时返回 true。
/// @warning 读取会话运行期状态，须在逻辑调度方已有的同步约束下调用。
bool BeatmapSession::needsRealtimeUpdate() const
{
    // 手势与视觉动画即使没有新命令也需推进，后台限频不能冻结这些状态。
    return hasPendingCommands() || m_ctx->isPlaying ||
           m_ctx->isAudioTimelineSyncFollower || m_ctx->isDragging ||
           m_ctx->isSelecting || m_ctx->brushState.isActive ||
           m_ctx->eraserState.isActive || m_ctx->animateTimeAnimationActive ||
           m_ctx->animatedTimelineZoomAnimationActive ||
           std::abs(m_ctx->previewEdgeScrollVelocity) > 0.0001;
}

/// @brief 判断会话是否需要在 Unlimited 模式下逐逻辑轮次推进。
/// @return 自身播放或作为音轨同步跟随者时返回 true。
/// @note 与 needsRealtimeUpdate
/// 的广义忙碌条件不同，不把所有视觉动画视为音频轮询。
bool BeatmapSession::needsUnlimitedPolling() const
{
    // 无限帧率模式只为播放及音轨跟随保持持续轮询，静止会话可交给空闲门控。
    return m_ctx->isPlaying || m_ctx->isAudioTimelineSyncFollower;
}

/// @brief 跨线程请求一次由指定编辑器事件触发的自动保存与自动备份。
/// @param trigger 要合并到待处理位集合中的编辑器事件。
/// @note 不保证请求一定产生文件，消费方仍检查模式、脏数据与忙碌状态。
/// @warning
/// 多个事件来源写入位集合，逻辑线程交换消费；原子只承载事件位，不发布谱面数据。
void BeatmapSession::requestAutoSave(AutoSaveTrigger trigger)
{
    // 位并集保留不同来源的事件，同类重复请求合并；此处不执行磁盘写入。
    const auto triggerBit = autoSaveTriggerBit(trigger);
    // 两个消费者使用独立原子位集合，先处理保存不会吞掉备份所需的同一事件。
    m_requestedAutoSaveTriggers.fetch_or(triggerBit, std::memory_order_relaxed);
    // 保存与备份各自消费请求位，后续分别按有效配置判断是否触发。
    m_requestedAutoBackupTriggers.fetch_or(triggerBit,
                                           std::memory_order_relaxed);
}

/// @brief 判断后台会话是否仍需轮询自动保存或自动备份。
/// @param saveConfig 全局保存调度策略。
/// @param backupConfig 项目覆盖后的备份调度策略。
/// @return 存在待处理请求或满足定时轮询条件时返回 true。
/// @warning 调度热路径只读取请求位和会话状态；请求位由事件来源跨线程写入。
bool BeatmapSession::needsAutoSavePolling(
    const Config::AutoSaveConfig&   saveConfig,
    const Config::AutoBackupConfig& backupConfig) const
{
    // 已收到的事件和已进入待处理状态的请求都需要继续推进，不受定时模式限制。
    if ( m_requestedAutoSaveTriggers.load(std::memory_order_relaxed) != 0U ||
         m_requestedAutoBackupTriggers.load(std::memory_order_relaxed) != 0U ||
         m_triggeredAutoSavePending || m_triggeredAutoBackupPending ) {
        return true;
    }
    const bool needsTimedSave =
        // 定时任务只为实际有待保存数据的会话保留轮询，避免空白页无效唤醒。
        saveConfig.mode == Config::AutoSaveMode::Timed &&
        m_ctx->currentBeatmap && m_ctx->actionStack.isDirty();
    const bool needsTimedBackup =
        // 备份使用独立脏状态，普通保存是否清理撤销栈不能替代备份判断。
        backupConfig.mode == Config::AutoSaveMode::Timed &&
        m_ctx->currentBeatmap && m_autoBackupDirty;
    // 保存与备份任一需要推进就保持轮询，不能因另一项关闭而让任务饿死。
    return needsTimedSave || needsTimedBackup;
}

/// @brief 在用户停止 note 编辑一段时间后同步 BeatMap 数据。
/// @param currentSysTime 当前单调系统时间，单位秒。
/// @param processed 本轮是否处理过命令。
/// @param isBusy 是否仍有需要立即响应的会话活动。
/// @note 此入口只处理普通音符的延迟模型同步，不能代替所有数据域的保存入口。
/// @warning 逻辑热路径：每个 Session update
/// 调用；普通路径只做常量级状态判断，只有空闲超时脏分支允许全量同步 BeatMap。
void BeatmapSession::flushDeferredBeatmapSync(double currentSysTime,
                                              bool processed, bool isBusy)
{
    if ( !m_ctx->m_needsNotesSync ) {
        // 数据已被其他保存或同步入口刷新时，旧的空闲计时不再有意义。
        m_hasDeferredBeatmapSyncTimer = false;
        return;
    }

    if ( processed || isBusy ) {
        // 忙碌期间只后移截止基准，不阻塞线程，也不延迟已发生的 ECS 编辑。
        m_lastDeferredBeatmapSyncTime = currentSysTime;
        m_hasDeferredBeatmapSyncTimer = true;
        return;
    }

    if ( !m_hasDeferredBeatmapSyncTimer ) {
        // 首次发现脏数据从本轮开始计时，不能使用未初始化或旧会话的时间戳。
        m_lastDeferredBeatmapSyncTime = currentSysTime;
        m_hasDeferredBeatmapSyncTimer = true;
        return;
    }

    if ( currentSysTime - m_lastDeferredBeatmapSyncTime <
         DEFERRED_BEATMAP_SYNC_IDLE_SECONDS ) {
        // 未到期时立即返回，延迟状态由后续 update 再检查，不占用等待线程。
        return;
    }

    SessionUtils::syncBeatmap(*m_ctx);
    // 依据同步后的脏标记决定是否继续计时，不假定本次调用一定清除了全部请求。
    // 若仍需同步，从本轮重新计时，不在下一轮立即重复完整同步。
    m_hasDeferredBeatmapSyncTimer = m_ctx->m_needsNotesSync;
    m_lastDeferredBeatmapSyncTime = currentSysTime;
}

/// @brief 在元数据停止变化且会话空闲后执行一次尾随自动保存。
/// @param currentSysTime 当前单调系统时间，单位秒。
/// @param isEditingBusy 是否仍在进行编辑手势。
/// @note 请求计时重置优先于超时判断，刚发生的元数据编辑不能沿用旧截止点保存。
/// @warning 逻辑热路径：普通帧只做常量级状态判断；仅空闲超时分支允许
/// 调用同步文件保存流程。
void BeatmapSession::flushDeferredMetadataAutoSave(double currentSysTime,
                                                   bool   isEditingBusy)
{
    if ( !m_metadataAutoSavePending ) return;

    if ( m_metadataAutoSaveTimerNeedsReset ) {
        // 元数据命令只请求重置，由逻辑轮询使用一致的系统时钟建立基准。
        m_lastMetadataUpdateTime          = currentSysTime;
        m_metadataAutoSaveTimerNeedsReset = false;
        return;
    }

    if ( isEditingBusy || currentSysTime - m_lastMetadataUpdateTime <
                              METADATA_AUTO_SAVE_IDLE_SECONDS ) {
        // 连续输入期间保留待保存状态，等待时仍正常返回主逻辑循环。
        return;
    }

    (void)flushPendingMetadataAutoSave();
    // 保存结果由实际保存入口维护，此处不根据调用发生就擅自清除待保存状态。
}

/// @brief 消费全局配置允许的事件请求并推进定时/事件自动保存。
/// @param currentSysTime 当前单调系统时间，供截止时间比较。
/// @param isEditingBusy 是否需要暂缓文件保存以避免打断编辑。
/// @param config 当前有效的全局自动保存配置。
/// @note 事件位按种类合并，不保存同类事件次数，不能据此生成逐事件保存历史。
/// @warning 周期检查只更新状态；到期且非编辑忙碌时可进入同步文件保存流程。
void BeatmapSession::flushConfiguredAutoSave(
    double currentSysTime, bool isEditingBusy,
    const Config::AutoSaveConfig& config)
{
    // 一次交换取得此前累积的事件；交换后到达的请求留给下一轮处理。
    const std::uint8_t requestedTriggers =
        m_requestedAutoSaveTriggers.exchange(0U, std::memory_order_relaxed);
    constexpr AutoSaveTrigger EXTERNAL_TRIGGERS[]{
        // 这里只消费切换/失焦等外部事件，物件编辑的待保存状态由编辑流程安排。
        AutoSaveTrigger::BeatmapSwitch,
        AutoSaveTrigger::ImGuiWindowFocusLost,
        AutoSaveTrigger::NativeWindowFocusLost,
    };
    for ( const AutoSaveTrigger trigger : EXTERNAL_TRIGGERS ) {
        if ( (requestedTriggers & autoSaveTriggerBit(trigger)) != 0U &&
             isAutoSaveTriggerEnabled(config, trigger) ) {
            m_triggeredAutoSavePending = true;
        }
    }

    const bool hasUnsavedChanges =
        // 元数据可能不在撤销栈中，因此不能只检查 actionStack 的保存深度。
        m_ctx->currentBeatmap &&
        (m_ctx->actionStack.isDirty() || m_metadataAutoSavePending);

    if ( config.mode == Config::AutoSaveMode::Timed ) {
        // 定时和事件模式互斥，切换模式时不继承旧事件形成的待保存标记。
        m_triggeredAutoSavePending   = false;
        const double intervalSeconds = config.intervalSeconds();
        if ( m_timedAutoSaveDeadline <= 0.0 ||
             m_timedAutoSaveIntervalSeconds != intervalSeconds ) {
            // 初次启用或用户修改间隔时从现在重新计时，不追补旧周期。
            m_timedAutoSaveIntervalSeconds = intervalSeconds;
            m_timedAutoSaveDeadline        = currentSysTime + intervalSeconds;
            return;
        }
        if ( currentSysTime < m_timedAutoSaveDeadline || isEditingBusy ) {
            return;
        }

        m_timedAutoSaveDeadline = currentSysTime + intervalSeconds;
        // 下一周期以实际处理时刻为基准，忙碌造成的延迟不会引发补偿式连环保存。
        // 无脏数据也推进下一截止点，避免到期后每轮重复执行到期分支。
        if ( hasUnsavedChanges ) {
            handleCommand(CmdSaveBeatmap{
                .kind = BeatmapSaveKind::TimedAutoSave,
            });
        }
        return;
    }

    m_timedAutoSaveDeadline = 0.0;
    // 离开定时模式后清空旧调度参数，下次启用重新建立截止时间。
    m_timedAutoSaveIntervalSeconds = 0.0;
    // 关闭自动保存不删除谱面脏状态，只清除该调度器不再适用的请求。
    if ( config.mode != Config::AutoSaveMode::EventTriggered ) {
        m_triggeredAutoSavePending = false;
        return;
    }
    if ( !m_triggeredAutoSavePending ) return;

    if ( isEditingBusy ) return;

    m_triggeredAutoSavePending = false;
    if ( hasUnsavedChanges ) {
        // 触发事件是一次请求，不因保存失败自动恢复同一个事件位。
        // 先消费触发状态，再进入保存命令；无修改时不生成额外写盘。
        handleCommand(CmdSaveBeatmap{
            .kind = BeatmapSaveKind::TriggeredAutoSave,
        });
    }
}

/// @brief 消费有效项目配置允许的事件请求并推进谱面自动备份。
/// @param currentSysTime 当前单调系统时间。
/// @param isEditingBusy 是否存在尚未提交完成的编辑活动。
/// @param config 项目覆盖后得到的有效备份策略。
/// @note 备份与保存有各自的事件交换和截止时间，不能共用其中一方已消费的请求位。
/// @warning 到期备份会同步完整模型并访问文件系统，仅在非编辑忙碌分支执行。
void BeatmapSession::flushConfiguredAutoBackup(
    double currentSysTime, bool isEditingBusy,
    const Config::AutoBackupConfig& config)
{
    const std::uint8_t requestedTriggers =
        m_requestedAutoBackupTriggers.exchange(0U, std::memory_order_relaxed);
    constexpr AutoSaveTrigger EXTERNAL_TRIGGERS[]{
        AutoSaveTrigger::BeatmapSwitch,
        AutoSaveTrigger::ImGuiWindowFocusLost,
        AutoSaveTrigger::NativeWindowFocusLost,
    };
    for ( const AutoSaveTrigger trigger : EXTERNAL_TRIGGERS ) {
        if ( (requestedTriggers & autoSaveTriggerBit(trigger)) != 0U &&
             isAutoBackupTriggerEnabled(config, trigger) ) {
            m_triggeredAutoBackupPending = true;
        }
    }

    bool shouldBackup = false;
    // 调度分支只决定本轮是否进入备份，尚未表示备份文件创建成功。
    // 分开记录是否需要备份与反馈类型，定时和事件分支共用实际备份流程。
    bool timedBackup = false;
    if ( config.mode == Config::AutoSaveMode::Timed ) {
        m_triggeredAutoBackupPending = false;
        const double intervalSeconds = config.intervalSeconds();
        if ( m_timedAutoBackupDeadline <= 0.0 ||
             m_timedAutoBackupIntervalSeconds != intervalSeconds ) {
            m_timedAutoBackupIntervalSeconds = intervalSeconds;
            m_timedAutoBackupDeadline        = currentSysTime + intervalSeconds;
            return;
        }
        if ( currentSysTime < m_timedAutoBackupDeadline || isEditingBusy ) {
            return;
        }
        m_timedAutoBackupDeadline = currentSysTime + intervalSeconds;
        shouldBackup              = m_autoBackupDirty;
        // 备份脏状态独立于主文件保存，普通保存不代表已经生成备份副本。
        timedBackup = true;
    } else {
        m_timedAutoBackupDeadline        = 0.0;
        m_timedAutoBackupIntervalSeconds = 0.0;
        if ( config.mode != Config::AutoSaveMode::EventTriggered ) {
            m_triggeredAutoBackupPending = false;
            return;
        }
        if ( !m_triggeredAutoBackupPending || isEditingBusy ) return;
        m_triggeredAutoBackupPending = false;
        shouldBackup                 = m_autoBackupDirty;
    }

    if ( !shouldBackup || !m_ctx->currentBeatmap ) return;
    // 触发已消费但数据仍脏；缺少项目上下文时等待后续有效触发，不制造无归属副本。
    // 备份依赖正式项目根目录，无项目或无根路径时不能推导备份归属位置。
    auto* project = EditorEngine::instance().getCurrentProject();
    if ( !project || project->m_projectRoot.empty() ) return;

    m_ctx->m_needsTimingsSync = true;
    // 强制把时间线和物件当前状态回写到模型，不能备份仍滞后的缓存快照。
    m_ctx->m_needsNotesSync = true;
    SessionUtils::syncBeatmap(*m_ctx);
    const auto sourcePath = m_ctx->currentBeatmap->m_baseMapMetadata.map_path;
    // 源路径用于确定谱面归属，实际输出和轮转由备份服务统一管理。
    const auto result =
        BeatmapBackupService::createBackup(*m_ctx->currentBeatmap,
                                           project->m_projectRoot,
                                           sourcePath,
                                           config.maxBackupCount);
    const auto presentation =
        // 反馈类型只区分调度来源，不改变备份文件格式和轮转规则。
        timedBackup ? Event::BeatmapSavePresentation::TimedAutoBackupStatus
                    : Event::BeatmapSavePresentation::TriggeredAutoBackupStatus;
    if ( !result.m_success ) {
        // 失败保持备份脏标记，后续有效触发仍可重试；本次按失败类型反馈。
        XERROR("Beatmap auto-backup failed for {}: {}",
               Config::pathToUtf8(sourcePath),
               result.m_errorMessage);
        Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
            .path         = Config::pathToUtf8(sourcePath),
            .success      = false,
            .isExport     = false,
            .errorMessage = result.m_errorMessage,
            .presentation = presentation,
        });
        return;
    }

    m_autoBackupDirty = false;
    // 副本成功不标记主谱面已保存，两份持久化状态不能相互抵扣。
    // 副本已成功创建才清脏，旧副本轮转不完整不等同于本次备份失败。
    if ( !result.m_errorMessage.empty() ) {
        XWARN("Beatmap backup created but rotation was incomplete: {}",
              result.m_errorMessage);
    }
    Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
        .path = Config::pathToUtf8(result.m_backupPath),
        // 成功反馈指向实际副本路径，便于与主谱面保存结果区分。
        .success      = true,
        .isExport     = false,
        .errorMessage = result.m_errorMessage,
        .presentation = presentation,
    });
}

/// @brief 立即落盘尚在等待空闲期的元数据自动保存。
/// @return 无待处理元数据或保存后所有修改已清除时返回 true。
/// @note 此强制刷新入口允许覆盖外部修改，不能当成无副作用的状态查询。
/// @warning 低频阻塞路径：仅允许逻辑线程在打包、项目关闭或尾随自动保存
/// 超时时调用；可能同步谱面数据、访问文件系统并保存项目配置。
bool BeatmapSession::flushPendingMetadataAutoSave()
{
    if ( !m_metadataAutoSavePending ) return true;
    // 无待处理元数据仅表示本入口无需工作，不承诺其他数据域已经全部落盘。
    // 尚未关联谱面时不能完成待处理保存，保留请求供调用方处理失败。
    if ( !m_ctx->currentBeatmap ) return false;

    handleCommand(CmdSaveBeatmap{
        .allowExternallyModifiedOverwrite = true,
        .kind                             = BeatmapSaveKind::TriggeredAutoSave,
    });
    // 保存命令通过更新脏状态表达结果，不能仅因命令返回就认定落盘成功。
    return !m_metadataAutoSavePending && !m_ctx->actionStack.isDirty();
}

/// @brief 为打包流程立即保存当前会话中的全部未落盘修改。
/// @return 无谱面、无修改或保存成功返回 true，否则阻止使用旧文件继续打包。
/// @note 这里只准备当前会话的源文件，不创建谱包，也不保存其他会话。
/// @warning
/// 低频阻塞路径：仅允许逻辑线程在打包前调用；会同步完整谱面并访问文件系统。
bool BeatmapSession::saveDirtyBeatmapForPackaging()
{
    if ( !m_ctx->currentBeatmap ) return true;
    // 没有关联谱面时此会话不提供打包输入，不在这里创建空谱面文件。
    if ( !m_metadataAutoSavePending && !m_ctx->actionStack.isDirty() ) {
        // 文件已是最新状态时跳过写入，避免打包入口产生无关保存副作用。
        return true;
    }

    handleCommand(CmdSaveBeatmap{
        .allowExternallyModifiedOverwrite = true,
        .kind                             = BeatmapSaveKind::Internal,
    });
    // 使用内部保存类型但仍检查全部未保存状态，保证打包读取的是已落盘版本。
    return !m_metadataAutoSavePending && !m_ctx->actionStack.isDirty();
}

/// @brief 判断本轮是否需要生成并发布渲染快照。
/// @param currentSysTime 当前单调系统时间。
/// @param forceImmediate 是否由实际编辑、跳转或脏数据要求立即发布。
/// @param config 当前有效配置，供自适应发布间隔计算。
/// @return 首次发布、强制更新或达到发布间隔时返回 true。
/// @note 返回允许发布不更新计时器，只有调用方真正生成快照后才记录发布时间。
/// @note currentSysTime 必须与上次发布时间使用同一单调时间基准。
/// @warning 逻辑热路径：只做常量时间背压判断，禁止访问 ECS 或同步缓冲区。
bool BeatmapSession::shouldUpdateRenderSnapshot(
    double currentSysTime, bool forceImmediate,
    const Config::EditorConfig& config) const
{
    if ( forceImmediate ) {
        // 用户操作和数据失效不受普通连续动画的快照限频影响。
        return true;
    }
    if ( m_lastRenderSnapshotTime <= 0.0 ) {
        // 尚无已发布快照时先提供首帧，不等待一个空的发布周期。
        return true;
    }
    const double minInterval =
        // 使用引擎统一的背压策略，不在每个会话中独立维护固定帧率。
        EditorEngine::instance().adaptiveRenderSnapshotMinInterval(config,
                                                                   false);
    return currentSysTime - m_lastRenderSnapshotTime >= minInterval;
}

/// @brief 根据逻辑时间刷新动画渲染时间。
/// @param dt 本轮时间增量，过大的值会限制为允许的最大动画步长。
/// @pre dt 为有限秒数；暂停或时间步长为零时也允许维持未完成的动画状态。
/// @param config 当前视觉偏移与滚动动画时长配置。
/// @param forceImmediate 是否直接跟随播放时钟而不进行平滑。
/// @note 动画只更新显示时间，不改变逻辑定位、音频播放位置或谱面事件时间。
/// @warning 逻辑热路径：每个 Session update
/// 执行；只做常量级指数平滑计算，不访问文件系统或 ECS。
void BeatmapSession::updateAnimateTime(double                      dt,
                                       const Config::EditorConfig& config,
                                       bool forceImmediate)
{
    const double targetAnimateTime =
        // 逻辑定位保留原时间，显示偏移只作用于渲染目标，不能写回传输位置。
        m_ctx->currentTime + config.visual.getEffectiveVisualOffset();
    m_ctx->animateTimeTarget = targetAnimateTime;

    const double duration = std::max(
        0.0, static_cast<double>(config.visual.scrollAnimationDuration));
    // duration 是响应参数，不代表等待这段时间之后才显示目标位置。
    if ( forceImmediate || duration <= VISUAL_ANIMATION_EPSILON ||
         !std::isfinite(targetAnimateTime) ||
         !std::isfinite(m_ctx->animateTime) ) {
        // 播放跟随和禁用动画使用直接赋值，非有限状态也不进入指数运算。
        m_ctx->animateTime                = targetAnimateTime;
        m_ctx->animateTimeAnimationActive = false;
        return;
    }

    const double diff = targetAnimateTime - m_ctx->animateTime;
    // 小残差直接吸附，避免浮点尾差让空闲会话永久保持动画活跃状态。
    if ( std::abs(diff) <= VISUAL_ANIMATION_EPSILON ) {
        m_ctx->animateTime                = targetAnimateTime;
        m_ctx->animateTimeAnimationActive = false;
        return;
    }

    const double clampedDt = std::clamp(dt, 0.0, VISUAL_ANIMATION_MAX_DT);
    // 负步长规整为零，不反向推进动画；这不改变目标时间本身。
    // 指数响应随实际 dt 调整；长帧限步避免后台恢复时一帧跨过整个过渡。
    const double alpha = std::clamp(
        1.0 - std::exp(-VISUAL_ANIMATION_RESPONSE * clampedDt / duration),
        0.0,
        1.0);

    // alpha 位于零到一之间，正常有限输入下只向目标靠近，不越过目标。
    m_ctx->animateTime += diff * alpha;
    // 步进后再次判定收敛，最后一帧即可解除动画标记而非多等一轮。
    if ( std::abs(targetAnimateTime - m_ctx->animateTime) <=
         VISUAL_ANIMATION_EPSILON ) {
        m_ctx->animateTime                = targetAnimateTime;
        m_ctx->animateTimeAnimationActive = false;
        return;
    }

    m_ctx->animateTimeAnimationActive = true;
}

/// @brief 刷新渲染使用的动画时间线缩放倍率。
/// @param dt 本轮动画步长，内部限制到非负且不过大的范围。
/// @param config 目标缩放与滚动动画时长配置。
/// @pre dt 为有限秒数；输入配置可含退化缩放，由 sanitizeTimelineZoom 单独回退。
/// @note 平滑值仅供绘制使用，目标配置本身不被动画过程覆盖。
/// @note 缩放和时间动画分别保存活跃标记，其中一个收敛不应停止另一个。
/// @warning 逻辑热路径：每个 Session update
/// 执行；只做常量级指数平滑计算，不访问文件系统或 ECS。
void BeatmapSession::updateAnimatedTimelineZoom(
    double dt, const Config::EditorConfig& config)
{
    const double targetZoom = sanitizeTimelineZoom(config.visual.timelineZoom);
    // 目标值与历史动画值都须为有效正尺度，避免插值从退化投影开始。
    m_ctx->animatedTimelineZoomTarget = static_cast<float>(targetZoom);

    double animatedZoom   = sanitizeTimelineZoom(m_ctx->animatedTimelineZoom);
    const double duration = std::max(
        0.0, static_cast<double>(config.visual.scrollAnimationDuration));
    if ( duration <= VISUAL_ANIMATION_EPSILON ) {
        // 关闭动画即提交目标尺度，并撤销前一轮遗留的动画活跃标志。
        m_ctx->animatedTimelineZoom = static_cast<float>(targetZoom);
        m_ctx->animatedTimelineZoomAnimationActive = false;
        return;
    }

    const double diff = targetZoom - animatedZoom;
    // 缩放使用与时间动画相同的收敛阈值，防止极小误差持续触发更新。
    if ( std::abs(diff) <= VISUAL_ANIMATION_EPSILON ) {
        m_ctx->animatedTimelineZoom = static_cast<float>(targetZoom);
        m_ctx->animatedTimelineZoomAnimationActive = false;
        return;
    }

    const double clampedDt = std::clamp(dt, 0.0, VISUAL_ANIMATION_MAX_DT);
    const double alpha     = std::clamp(
        1.0 - std::exp(-VISUAL_ANIMATION_RESPONSE * clampedDt / duration),
        0.0,
        1.0);
    animatedZoom += diff * alpha;
    // 插值以倍率的线性差值进行，不在对数空间中平滑，也不改变配置里的目标倍率。
    // 中间计算保留 double 精度，仅在发布到会话渲染字段时转换为 float。

    if ( std::abs(targetZoom - animatedZoom) <= VISUAL_ANIMATION_EPSILON ) {
        m_ctx->animatedTimelineZoom = static_cast<float>(targetZoom);
        m_ctx->animatedTimelineZoomAnimationActive = false;
        return;
    }

    m_ctx->animatedTimelineZoom = static_cast<float>(animatedZoom);
    // 未收敛即保留活跃标记，静止会话仍需继续产生后续动画步进。
    m_ctx->animatedTimelineZoomAnimationActive = true;
}

/// @brief 会话逻辑每帧更新。
/// @param dt 逻辑轮次的经过时间，单位秒。
/// @param config 已应用项目覆盖的有效编辑器配置。
/// @param isActiveSession 是否拥有当前全局音频控制权。
/// @pre 调用方串行更新本会话，dt 与配置由同一逻辑调度轮次提供。
/// @note 命令消费先于本轮布局和播放处理，避免渲染继续使用已失效的会话参数。
/// @warning 逻辑热路径：由 EditorEngine::loop 按 UPS
/// 调用；普通路径禁止文件系统访问、完整排序、try/catch 和可避免的 shared_ptr
/// 拷贝；仅配置到期的自动保存/备份低频分支允许持久化。
void BeatmapSession::update(double dt, const Config::EditorConfig& config,
                            bool isActiveSession)
{
    m_ctx->lastConfig      = config;
    m_ctx->isActiveSession = isActiveSession;
    // 保存有效配置副本供本轮控制器读取，避免持有调用方配置对象的可变引用。
    // 先接收同谱面草稿的新版本，再处理当前会话命令，避免从旧基线编辑。
    ProjectDraftLaneService::refreshIfChanged(*m_ctx);
    if ( !isActiveSession && m_ctx->isPlaying ) {
        // 后台会话不再作为播放源，但同音轨跟随由独立状态维护。
        m_ctx->isPlaying = false;
    }
    bool processed = processCommands();
    // processed 参与后续强制快照条件，不能只看消费后队列是否为空。
    // 初始观察快照应包含本轮已处理的命令，不能抢在它们之前发布旧模型。
    publishRequestedMutationSnapshot();
    // 配置命令携带软件级快照；本轮调度仍必须恢复项目覆盖后的有效配置。
    m_ctx->lastConfig = config;
    m_ctx->lastConfig.visual.applyKeyCountLayout(m_ctx->trackCount);
    // 命令可能改变轨道数量，重新选择布局后本轮投影才与新元数据一致。
    // effectiveConfig 借用会话内副本，本轮后续流程均使用这份一致的布局配置。
    const auto& effectiveConfig = m_ctx->lastConfig;

    if ( m_ctx->isAudioTimelineDescriptorDirty ) {
        // 描述符重建仅由脏状态触发，协作会话优先使用自己的项目资源视图。
        const auto* project =
            m_ctx->collaborationProject
                ? m_ctx->collaborationProject.get()
                : EditorEngine::instance().getCurrentProject();
        SessionUtils::rebuildAudioTimelineDescriptor(*m_ctx, project);
    }
    if ( m_ctx->isAudioTimelineFingerprintPublishPending ) {
        // 指纹发布和音频激活是不同待办，先让跨会话匹配看到新资源身份。
        // 指纹改变后更新引擎的跨会话匹配信息，再尝试激活新的音频时间线。
        EditorEngine::instance().refreshAudioTimelineFingerprints();
        m_ctx->isAudioTimelineFingerprintPublishPending = false;
    }
    if ( isActiveSession && m_ctx->isAudioTimelineActivationPending ) {
        // 后台会话可刷新描述符，但不能借此替换全局正在加载的音频。
        if ( !SessionUtils::activateAudioTimeline(*m_ctx, m_ctx->isPlaying) ) {
            m_ctx->isPlaying = false;
            // 激活失败停止本地播放声明，避免渲染按未就绪的传输继续推进。
        }
    }

    double currentSysTime =
        // 节流与自动保存共享单调时间，不使用可被用户跳转的谱面时间。
        std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();

    const bool shouldPrefetchBoundSounds = isActiveSession ||
                                           m_ctx->isPlaying ||
                                           m_ctx->isAudioTimelineSyncFollower;
    // 同音轨跟随者也需要准备绑定音效资源，但后续声音调度仍只由播放源执行。
    // 活动画布暂停时也预读附近音效，后台静止会话不重复推进加载队列。
    if ( shouldPrefetchBoundSounds &&
         currentSysTime >= m_ctx->nextBoundSoundPrefetchSystemTime ) {
        prefetchBoundNoteSounds(*m_ctx);
        m_ctx->nextBoundSoundPrefetchSystemTime =
            // 跳过错过的周期，不因一次长帧连续补跑多轮预读。
            currentSysTime + BOUND_SOUND_PREFETCH_INTERVAL_SECONDS;
    }

    bool isInteracting = m_ctx->isDragging || m_ctx->isSelecting ||
                         // 未提交的笔刷和擦除同样是交互，不只看通用拖拽标志。
                         m_ctx->brushState.isActive ||
                         m_ctx->eraserState.isActive;
    const bool isVisualAnimationActive =
        m_ctx->animateTimeAnimationActive ||
        m_ctx->animatedTimelineZoomAnimationActive;
    const bool isEdgeScrollActive =
        std::abs(m_ctx->previewEdgeScrollVelocity) > 0.0001;
    bool isBusy = isInteracting || m_ctx->isPlaying ||
                  // 播放、动画、边缘滚动和剩余命令都要求继续推进逻辑。
                  m_ctx->isAudioTimelineSyncFollower ||
                  isVisualAnimationActive || isEdgeScrollActive ||
                  hasPendingCommands();

    if ( effectiveConfig.settings.frameLimit !=
             Config::FrameLimitPreference::VSync &&
         !isBusy && !processed ) {
        // 空闲节流发生在命令和资源状态处理之后，不能延迟输入的本地反馈。
        if ( currentSysTime - m_ctx->lastSnapshotTime <
             IDLE_UPDATE_MIN_INTERVAL_SECONDS ) {
            // 这里跳过空闲尾部工作，不丢弃此前已经消费的命令或资源状态变化。
            return;
        }
    }
    flushDeferredBeatmapSync(currentSysTime, processed, isBusy);
    // 模型空闲同步与文件保存的忙碌口径不同，不能直接共用 isBusy 延迟一切保存。
    const bool isMetadataEditingBusy =
        // 文件持久化仅避开编辑与命令处理，单纯播放不被算作编辑忙碌。
        isInteracting || processed || hasPendingCommands();
    flushConfiguredAutoSave(currentSysTime,
                            isMetadataEditingBusy,
                            effectiveConfig.settings.autoSave);
    flushConfiguredAutoBackup(currentSysTime,
                              isMetadataEditingBusy,
                              effectiveConfig.settings.autoBackup);
    flushDeferredMetadataAutoSave(currentSysTime, isMetadataEditingBusy);
    // lastSnapshotTime 在此是轻量轮询节流基准，实际渲染发布另有独立时间字段。
    m_ctx->lastSnapshotTime = currentSysTime;

    // 边缘速度按实际经过时间积分，使拖动自动滚动不依赖逻辑帧率。
    if ( std::abs(m_ctx->previewEdgeScrollVelocity) > 0.0001 ) {
        double delta     = m_ctx->previewEdgeScrollVelocity * dt;
        double totalTime = SessionUtils::getEffectiveTotalTimeSeconds(*m_ctx);
        m_ctx->currentTime =
            // 自动滚动限制在有效曲长内，不允许持续速度将位置推到范围外。
            std::clamp(m_ctx->currentTime + delta, 0.0, totalTime);

        if ( m_ctx->isPlaying && m_ctx->isActiveSession ) {
            // 只有播放源同步全局音频定位，后台画布不能抢占传输位置。
            Audio::AudioManager::instance().seek(m_ctx->currentTime);
        }
        SessionUtils::syncHitIndex(*m_ctx);
        // 自动滚动同样改变时间，命中游标必须跟随新位置。
    }

    double previousAnimateTime = m_ctx->animateTime;
    // 保留更新前视觉时间，用于识别本轮越过的事件与不连续跳转。
    updateAnimatedTimelineZoom(dt, effectiveConfig);

    // 播放源和同音轨跟随者都读取传输时钟，但只有播放源负责调度声音。
    bool isPlaybackClockActive =
        m_ctx->isPlaying || m_ctx->isAudioTimelineSyncFollower;
    bool playbackJumped = false;
    // 是否发生跳转要保留到发布阶段，保证新位置不受普通快照限频拖延。
    if ( isPlaybackClockActive ) {
        // 命中表可被本轮命令修改，在推进两个事件游标前确保缓存有效。
        SessionUtils::ensureHitEvents(*m_ctx);

        auto&      audio              = Audio::AudioManager::instance();
        const auto audioClockSnapshot = audio.getAudioTimelineClockSnapshot();
        // 一次读取成值快照，再用于本轮校准，避免不同字段取自不同音频更新时刻。
        // 时钟快照与当前单调时间一起交给连续时钟校准，不逐帧累计音频 dt。
        const double playbackClockNow =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        isPlaybackClockActive =
            // 资源身份不匹配或传输结束可在此取消活跃状态，后续分支使用返回值。
            SessionUtils::applyAudioTimelineTransportSnapshot(
                *m_ctx,
                audio.getLoadedAudioTimelineFingerprint(),
                audioClockSnapshot,
                playbackClockNow,
                effectiveConfig.settings.syncConfig);

        updateAnimateTime(dt, effectiveConfig, isPlaybackClockActive);
        // 有效播放时直接跟随时钟，避免滚动平滑叠加音画延迟。

        std::vector<System::HitFXSystem::HitEvent> triggeredEvents;
        // 本轮视觉事件与提前预约音频不是同一列表，预播放不能提前点亮命中特效。

        bool isJump =
            // 回退、较大跨越、首轮播放或时钟失效都不视作正常连续命中区间。
            !isPlaybackClockActive ||
            (std::abs(m_ctx->animateTime - previousAnimateTime) > 0.2) ||
            (m_ctx->animateTime < previousAnimateTime) || !m_wasPlaying;
        playbackJumped = isJump;

        if ( isJump ) {
            // 清除旧状态后重建当前位置，不能把 Seek
            // 跨越的全部音符当作连续击中。
            // 跳转不补播跨过区间的事件，清除旧特效后从新位置重新建立状态。
            if ( m_ctx->isPlaying ) {
                // 全局预约音效只由播放源清理，跟随画布不能取消源画布的声音。
                Audio::AudioManager::instance().clearAllScheduledSoundEffects();
            }
            m_ctx->hitFXSystem.clearActiveEffects();
            SessionUtils::syncHitIndex(*m_ctx);
            m_ctx->hitFXSystem.restoreActiveHoldEffects(
                // 长条头可能在跳转点之前，持续效果需要独立于头部命中恢复。
                m_ctx->animateTime,
                m_ctx->hitEvents,
                effectiveConfig);

            if ( m_ctx->isPlaying ) {
                // 跳转后重建前方音效窗口，不能只等待下一轮新增事件进入窗口。
                // 预测窗口只用于预约，不通过阻塞等待窗口结束来同步音画。
                double predictWindow = 0.2;
                while (
                    m_ctx->nextPredictHitIndex < m_ctx->hitEvents.size() &&
                    m_ctx->hitEvents[m_ctx->nextPredictHitIndex].timestamp <=
                        (m_ctx->animateTime + predictWindow) ) {
                    const auto& ev =
                        m_ctx->hitEvents[m_ctx->nextPredictHitIndex];
                    // 过滤当前播放点之前的事件，不补播已经跨过的音符。
                    if ( ev.timestamp >= m_ctx->animateTime ) {
                        m_ctx->hitFXSystem.triggerAudio(ev,
                                                        m_ctx->trackCount,
                                                        m_ctx->draftTrackCount,
                                                        effectiveConfig);
                    }
                    m_ctx->nextPredictHitIndex++;
                }
            }
        } else {
            // 连续播放只处理窗口新增长的尾部，避免重复预约已经覆盖的音效。
            if ( m_ctx->isPlaying ) {
                double predictWindow = 0.2;
                while (
                    m_ctx->nextPredictHitIndex < m_ctx->hitEvents.size() &&
                    m_ctx->hitEvents[m_ctx->nextPredictHitIndex].timestamp <=
                        (m_ctx->animateTime + predictWindow) ) {
                    const auto& ev =
                        m_ctx->hitEvents[m_ctx->nextPredictHitIndex];
                    if ( ev.timestamp >
                         (previousAnimateTime + predictWindow) ) {
                        // 左边界取上一轮预测窗口末端，不是上一轮实际播放位置。
                        m_ctx->hitFXSystem.triggerAudio(ev,
                                                        m_ctx->trackCount,
                                                        m_ctx->draftTrackCount,
                                                        effectiveConfig);
                    }
                    m_ctx->nextPredictHitIndex++;
                }
            }

            while ( m_ctx->nextHitIndex < m_ctx->hitEvents.size() &&
                    m_ctx->hitEvents[m_ctx->nextHitIndex].timestamp <=
                        m_ctx->animateTime ) {
                // 右端点包含当前时间、左端点排除上一时刻，避免同一边界事件重复触发。
                // 视觉命中只取实际跨越区间，与提前调度的音效游标分开推进。
                const auto& ev = m_ctx->hitEvents[m_ctx->nextHitIndex];
                if ( ev.timestamp > previousAnimateTime ) {
                    triggeredEvents.push_back(ev);
                }
                m_ctx->nextHitIndex++;
            }
        }
        m_ctx->hitFXSystem.update(
            m_ctx->animateTime,
            // 跳转时列表为空，但已恢复的持续效果仍需更新。
            triggeredEvents,
            m_ctx->trackCount,
            effectiveConfig);
    } else {
        updateAnimateTime(dt, effectiveConfig, false);
        // 暂停态仍可执行滚动动画，但不按动画经过的音符调度打击声音。

        if ( std::abs(m_ctx->animateTime - previousAnimateTime) > 0.0001 ) {
            SessionUtils::syncHitIndex(*m_ctx);
        }
    }

    m_wasPlaying = isPlaybackClockActive;
    // 这里记录的是有效传输时钟状态，包含跟随者，不仅是本地 isPlaying 标志。
    // 保存校准后的实际状态，下一次重新取得时钟会进入首轮跳转处理。
    // 传输失效后即使本轮没有声音，也必须更新该状态以免下轮误判连续播放。

    const auto* scrollCache =
        m_ctx->timelineRegistry.ctx().find<System::ScrollCache>();
    const bool isTimelineCacheDirty = scrollCache && scrollCache->isDirty;
    const bool hasRenderDirtyState =
        // 数据变化与普通动画区分：前者需立即更新，后者可以接受快照背压。
        m_ctx->isNoteOrderDirty || m_ctx->isNotePruneDirty ||
        m_ctx->isNoteStatsDirty || m_ctx->isTransformDirty ||
        m_ctx->isBpmEventsDirty || m_ctx->isMarqueeSelectionDirty ||
        isTimelineCacheDirty;
    // 连续视野动画必须遵守自适应快照间隔，避免逻辑线程空转时无限重建快照。
    const bool forceRenderSnapshot =
        processed || playbackJumped || hasRenderDirtyState;
    // 视觉动画活跃本身不强制发布，否则会绕过专门为连续动画设置的背压。
    if ( shouldUpdateRenderSnapshot(
             currentSysTime, forceRenderSnapshot, effectiveConfig) ) {
        // 背压只控制快照生成，前面的命令、时钟和命中游标已经在本轮完成更新。
        updateECSAndRender(effectiveConfig, isActiveSession);
        // 只有实际生成快照才推进发布时刻，跳过轮次不延后下一次可发布时间。
        m_lastRenderSnapshotTime = currentSysTime;
    }
}

}  // namespace MMM::Logic
