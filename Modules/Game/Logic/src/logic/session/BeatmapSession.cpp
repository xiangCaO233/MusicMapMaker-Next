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

static void markScrollCacheDirty(entt::registry& reg, entt::entity)
{
    if ( auto* cache = reg.ctx().find<MMM::Logic::System::ScrollCache>() ) {
        cache->isDirty = true;
    }
}

namespace MMM::Logic
{

namespace
{
/// @brief Note 编辑后延迟同步 BeatMap 的空闲等待时间（秒）。
constexpr double DEFERRED_BEATMAP_SYNC_IDLE_SECONDS = 1.0;

/// @brief 元数据连续编辑停止后执行自动保存的空闲等待时间（秒）。
constexpr double METADATA_AUTO_SAVE_IDLE_SECONDS = 0.75;

/// @brief 非忙碌状态下 Session 逻辑轻量轮询的最小间隔。
constexpr double IDLE_UPDATE_MIN_INTERVAL_SECONDS =
    std::chrono::duration<double>(UNLIMITED_IDLE_SESSION_POLL_INTERVAL).count();

/// @brief 视觉动画目标值的吸附阈值。
constexpr double VISUAL_ANIMATION_EPSILON = 0.0001;

/// @brief 限制单帧视觉动画步长，避免后台恢复时跨越过大。
constexpr double VISUAL_ANIMATION_MAX_DT = 0.05;

/// @brief 指数平滑系数，约等于在配置时长内完成 99.75% 的位移。
constexpr double VISUAL_ANIMATION_RESPONSE = 6.0;

/// @brief 物件绑定音效后台加载推进间隔。
constexpr double BOUND_SOUND_PREFETCH_INTERVAL_SECONDS = 0.01;

/// @brief 物件绑定音效相对当前播放位置的预读窗口。
constexpr double BOUND_SOUND_PREFETCH_WINDOW_SECONDS = 5.0;

/// @brief 单次预读最多检查的打击事件数量。
constexpr std::size_t MAX_BOUND_SOUND_PREFETCH_EVENTS_PER_TICK = 256U;

/// @brief 判断画笔位置将编辑普通物件还是 BGM 自动采样。
/// @param ctx 当前谱面会话。
/// @param cameraId 命令所属画布 ID。
/// @param mouseX 鼠标在画布内的横坐标。
/// @return BGM 轨道返回 AudioSamples，其余位置返回 Objects。
/// @warning 命令权限检查热路径：只读取相机和配置缓存并执行常量级投影计算。
[[nodiscard]] ::MMM::BeatmapMutationFlags brushMutationFlagsAt(
    const SessionContext& ctx, const std::string& cameraId, float mouseX)
{
    if ( cameraId == "Preview" || cameraId == "PreviewCanvas" ) {
        return ::MMM::BeatmapMutationFlags::Objects;
    }

    const auto camera = ctx.cameras.find(cameraId);
    if ( camera == ctx.cameras.end() ) {
        return ::MMM::BeatmapMutationFlags::Objects;
    }
    const auto lanes =
        calculateCanvasLaneProjection(camera->second.viewportWidth,
                                      ctx.trackCount,
                                      ctx.bgmTrackCount,
                                      ctx.lastConfig.visual.trackLayout.left,
                                      ctx.lastConfig.visual.trackLayout.right,
                                      camera->second.horizontalOffsetX,
                                      true,
                                      ctx.lastConfig.settings.enableBmsEditing,
                                      ctx.lastConfig.settings.enableDraftLanes);
    const auto lane = lanes.laneAt(mouseX);
    return lane && lane->kind == CanvasLaneKind::Bgm
               ? ::MMM::BeatmapMutationFlags::AudioSamples
               : ::MMM::BeatmapMutationFlags::Objects;
}

/// @brief 返回当前悬停物件对应的协作权限类别。
/// @param ctx 当前谱面会话。
/// @return 有效悬停物件的数据类别；没有有效目标时返回 None。
/// @warning 命令权限检查热路径：仅在两个独立 Registry 中执行常量级实体查询。
[[nodiscard]] ::MMM::BeatmapMutationFlags hoveredMutationFlags(
    const SessionContext& ctx)
{
    if ( ctx.hoveredEntity == entt::null ) {
        return ::MMM::BeatmapMutationFlags::None;
    }
    if ( ctx.hoveredObjectKind == ChartObjectKind::AudioSample ) {
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
/// @warning 逻辑低频预读路径：由系统时间节流，只线性推进尚未检查的事件，
/// 不得访问文件系统或回扫完整事件表。
void prefetchBoundNoteSounds(SessionContext& ctx)
{
    auto&        audioManager = Audio::AudioManager::instance();
    const double prefetchEnd =
        ctx.animateTime + BOUND_SOUND_PREFETCH_WINDOW_SECONDS;
    std::size_t examinedCount = 0U;

    while ( ctx.nextBoundSoundPrefetchIndex < ctx.hitEvents.size() &&
            examinedCount < MAX_BOUND_SOUND_PREFETCH_EVENTS_PER_TICK ) {
        const auto& event = ctx.hitEvents[ctx.nextBoundSoundPrefetchIndex];
        if ( event.timestamp > prefetchEnd ) break;
        if ( event.sampleBinding &&
             !event.sampleBinding->m_audioResourceId.empty() ) {
            audioManager.queueBoundNoteSoundEffectLoad(
                event.sampleBinding->m_audioResourceId);
        }
        ++ctx.nextBoundSoundPrefetchIndex;
        ++examinedCount;
    }

    audioManager.updateQueuedSoundEffectLoads();
}

/// @brief 规范化时间线缩放倍率，避免无效配置进入视觉动画。
/// @param zoom 输入缩放倍率。
/// @return 可用于坐标映射的正缩放倍率。
/// @warning 逻辑热路径：每个 Session update 执行；只做常量级数值检查。
double sanitizeTimelineZoom(double zoom)
{
    if ( !std::isfinite(zoom) || zoom <= VISUAL_ANIMATION_EPSILON ) {
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

BeatmapSession::BeatmapSession()
{
    m_ctx         = std::make_unique<SessionContext>();
    m_playback    = std::make_unique<PlaybackController>(*m_ctx);
    m_interaction = std::make_unique<InteractionController>(*m_ctx);
    m_actions     = std::make_unique<ActionController>(*m_ctx);

    m_ctx->timelineRegistry.ctx().emplace<System::ScrollCache>();
    m_ctx->timelineRegistry.on_construct<TimelineComponent>()
        .connect<&markScrollCacheDirty>();
    m_ctx->timelineRegistry.on_update<TimelineComponent>()
        .connect<&markScrollCacheDirty>();
    m_ctx->timelineRegistry.on_destroy<TimelineComponent>()
        .connect<&markScrollCacheDirty>();
}

BeatmapSession::~BeatmapSession() = default;

void BeatmapSession::pushCommand(LogicCommand&& cmd)
{
    if ( blockCollaborationOfflineEdit(cmd) ||
         blockCollaborationUnauthorizedEdit(cmd) ) {
        return;
    }
    m_commandQueue.enqueue(std::move(cmd));
}

bool BeatmapSession::blockCollaborationUnauthorizedEdit(
    const LogicCommand& cmd, bool inspectSessionState)
{
    auto required = requiredBeatmapMutationFlags(cmd);
    if ( inspectSessionState ) {
        if ( const auto* metadata =
                 std::get_if<CmdUpdateBeatmapMetadata>(&cmd) ) {
            const auto samples =
                m_ctx->sampleRegistry.view<const SampleComponent>();
            if ( m_ctx->currentBeatmap && !samples.empty() ) {
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
            required = brushMutationFlagsAt(
                *m_ctx, updateBrush->cameraId, updateBrush->mouseX);
        } else if ( std::holds_alternative<CmdEndBrush>(cmd) ) {
            required = m_ctx->brushState.isActive
                           ? (m_ctx->brushState.createsAudioSample
                                  ? ::MMM::BeatmapMutationFlags::AudioSamples
                                  : ::MMM::BeatmapMutationFlags::Objects)
                           : ::MMM::BeatmapMutationFlags::None;
        } else if ( std::holds_alternative<CmdStartErase>(cmd) ||
                    std::holds_alternative<CmdUpdateErase>(cmd) ) {
            required = hoveredMutationFlags(*m_ctx);
        } else if ( std::holds_alternative<CmdEndErase>(cmd) ) {
            required = m_ctx->eraserState.isActive &&
                               !m_ctx->eraserState.targetEntities.empty()
                           ? (m_ctx->eraserState.targetObjectKind ==
                                      ChartObjectKind::AudioSample
                                  ? ::MMM::BeatmapMutationFlags::AudioSamples
                                  : ::MMM::BeatmapMutationFlags::Objects)
                           : ::MMM::BeatmapMutationFlags::None;
        } else if ( std::holds_alternative<CmdUndo>(cmd) ) {
            required = m_ctx->actionStack.undoMutationFlags();
        } else if ( std::holds_alternative<CmdRedo>(cmd) ) {
            required = m_ctx->actionStack.redoMutationFlags();
        } else if ( std::holds_alternative<CmdPaste>(cmd) ) {
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
            if ( !m_ctx->selectedNoteEntities.empty() ) {
                required |= ::MMM::BeatmapMutationFlags::Objects;
            }
            if ( !m_ctx->selectedSampleEntities.empty() ) {
                required |= ::MMM::BeatmapMutationFlags::AudioSamples;
            }
        }
    }
    if ( required == ::MMM::BeatmapMutationFlags::None ) return false;

    const auto allowed = static_cast<::MMM::BeatmapMutationFlags>(
        m_collaborationAllowedMutationFlags.load(std::memory_order_acquire));
    const auto requiredBits = static_cast<std::uint8_t>(required);
    const auto allowedBits  = static_cast<std::uint8_t>(allowed);
    if ( (requiredBits & allowedBits) == requiredBits ) return false;

    Event::EventBus::instance().publish(
        Event::CollaborationPermissionEditBlockedEvent{});
    return true;
}

bool BeatmapSession::blockCollaborationOfflineEdit(const LogicCommand& cmd)
{
    if ( !m_collaborationOfflineReadOnly.load(std::memory_order_acquire) ||
         !isBeatmapEditingCommand(cmd) ) {
        return false;
    }
    if ( !m_offlineEditBlockedNotificationSent.exchange(
             true, std::memory_order_acq_rel) ) {
        Event::EventBus::instance().publish(
            Event::CollaborationOfflineEditBlockedEvent{});
    }
    return true;
}

void BeatmapSession::setCollaborationOfflineReadOnly(bool readOnly)
{
    const bool previous = m_collaborationOfflineReadOnly.exchange(
        readOnly, std::memory_order_acq_rel);
    if ( !readOnly ) {
        m_offlineEditBlockedNotificationSent.store(false,
                                                   std::memory_order_release);
    }
    if ( previous == readOnly ) return;
    m_commandQueue.enqueue(
        LogicCommand(CmdSetCollaborationOfflineReadOnly{ readOnly }));
}

bool BeatmapSession::isCollaborationOfflineReadOnly() const
{
    return m_collaborationOfflineReadOnly.load(std::memory_order_acquire);
}

void BeatmapSession::setCollaborationClipboardIsolated(bool isolated)
{
    static std::atomic_uint64_t nextScopeId{ 1 };
    const bool previous = m_collaborationClipboardIsolated.exchange(
        isolated, std::memory_order_acq_rel);
    if ( previous == isolated ) return;

    const auto scopeId =
        isolated ? nextScopeId.fetch_add(1, std::memory_order_relaxed) : 0U;
    m_collaborationClipboardScopeId.store(scopeId, std::memory_order_release);
    m_commandQueue.enqueue(LogicCommand(
        CmdSetCollaborationClipboardIsolation{ isolated, scopeId }));
}

bool BeatmapSession::isCollaborationClipboardIsolated() const
{
    return m_collaborationClipboardIsolated.load(std::memory_order_acquire);
}

void BeatmapSession::setCollaborationAllowedMutationFlags(
    ::MMM::BeatmapMutationFlags allowedFlags)
{
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

::MMM::BeatmapMutationFlags
BeatmapSession::collaborationAllowedMutationFlags() const
{
    return static_cast<::MMM::BeatmapMutationFlags>(
        m_collaborationAllowedMutationFlags.load(std::memory_order_acquire));
}

void BeatmapSession::setMutationObserver(
    std::shared_ptr<::MMM::IBeatmapMutationObserver> observer,
    bool                                             publishCurrentSnapshot)
{
    const bool requestSnapshot = observer != nullptr && publishCurrentSnapshot;
    m_latestAcceptedLocalObjectMutationSequence.store(
        0, std::memory_order_release);
    std::atomic_store_explicit(
        &m_mutationObserver, std::move(observer), std::memory_order_release);
    m_mutationSnapshotRequested.store(requestSnapshot,
                                      std::memory_order_relaxed);
}

void BeatmapSession::publishRequestedMutationSnapshot()
{
    if ( !m_mutationSnapshotRequested.exchange(false,
                                               std::memory_order_relaxed) ) {
        return;
    }
    auto observer = std::atomic_load_explicit(&m_mutationObserver,
                                              std::memory_order_acquire);
    if ( !observer || !m_ctx->currentBeatmap ) return;

    SessionUtils::syncBeatmap(*m_ctx);
    static_cast<void>(observer->onBeatmapMutated(
        *m_ctx->currentBeatmap, ::MMM::BeatmapMutationFlags::All));
}

/// @brief 判断会话是否存在等待逻辑线程消费的指令。
bool BeatmapSession::hasPendingCommands() const
{
    return m_commandQueue.size_approx() > 0;
}

/// @brief 判断会话是否需要跳过后台限频并立即更新。
bool BeatmapSession::needsRealtimeUpdate() const
{
    return hasPendingCommands() || m_ctx->isPlaying ||
           m_ctx->isAudioTimelineSyncFollower || m_ctx->isDragging ||
           m_ctx->isSelecting || m_ctx->brushState.isActive ||
           m_ctx->eraserState.isActive || m_ctx->animateTimeAnimationActive ||
           m_ctx->animatedTimelineZoomAnimationActive ||
           std::abs(m_ctx->previewEdgeScrollVelocity) > 0.0001;
}

/// @brief 判断会话是否需要在 Unlimited 模式下逐逻辑轮次推进。
bool BeatmapSession::needsUnlimitedPolling() const
{
    return m_ctx->isPlaying || m_ctx->isAudioTimelineSyncFollower;
}

/// @brief 跨线程请求一次由指定编辑器事件触发的自动保存与自动备份。
void BeatmapSession::requestAutoSave(AutoSaveTrigger trigger)
{
    const auto triggerBit = autoSaveTriggerBit(trigger);
    m_requestedAutoSaveTriggers.fetch_or(triggerBit, std::memory_order_relaxed);
    m_requestedAutoBackupTriggers.fetch_or(triggerBit,
                                           std::memory_order_relaxed);
}

/// @brief 判断后台会话是否仍需轮询自动保存或自动备份。
bool BeatmapSession::needsAutoSavePolling(
    const Config::AutoSaveConfig&   saveConfig,
    const Config::AutoBackupConfig& backupConfig) const
{
    if ( m_requestedAutoSaveTriggers.load(std::memory_order_relaxed) != 0U ||
         m_requestedAutoBackupTriggers.load(std::memory_order_relaxed) != 0U ||
         m_triggeredAutoSavePending || m_triggeredAutoBackupPending ) {
        return true;
    }
    const bool needsTimedSave =
        saveConfig.mode == Config::AutoSaveMode::Timed &&
        m_ctx->currentBeatmap && m_ctx->actionStack.isDirty();
    const bool needsTimedBackup =
        backupConfig.mode == Config::AutoSaveMode::Timed &&
        m_ctx->currentBeatmap && m_autoBackupDirty;
    return needsTimedSave || needsTimedBackup;
}

/// @brief 在用户停止 note 编辑一段时间后同步 BeatMap 数据。
/// @warning 逻辑热路径：每个 Session update
/// 调用；普通路径只做常量级状态判断，只有空闲超时脏分支允许全量同步 BeatMap。
void BeatmapSession::flushDeferredBeatmapSync(double currentSysTime,
                                              bool processed, bool isBusy)
{
    if ( !m_ctx->m_needsNotesSync ) {
        m_hasDeferredBeatmapSyncTimer = false;
        return;
    }

    if ( processed || isBusy ) {
        m_lastDeferredBeatmapSyncTime = currentSysTime;
        m_hasDeferredBeatmapSyncTimer = true;
        return;
    }

    if ( !m_hasDeferredBeatmapSyncTimer ) {
        m_lastDeferredBeatmapSyncTime = currentSysTime;
        m_hasDeferredBeatmapSyncTimer = true;
        return;
    }

    if ( currentSysTime - m_lastDeferredBeatmapSyncTime <
         DEFERRED_BEATMAP_SYNC_IDLE_SECONDS ) {
        return;
    }

    SessionUtils::syncBeatmap(*m_ctx);
    m_hasDeferredBeatmapSyncTimer = m_ctx->m_needsNotesSync;
    m_lastDeferredBeatmapSyncTime = currentSysTime;
}

/// @brief 在元数据停止变化且会话空闲后执行一次尾随自动保存。
/// @warning 逻辑热路径：普通帧只做常量级状态判断；仅空闲超时分支允许
/// 调用同步文件保存流程。
void BeatmapSession::flushDeferredMetadataAutoSave(double currentSysTime,
                                                   bool   isEditingBusy)
{
    if ( !m_metadataAutoSavePending ) return;

    if ( m_metadataAutoSaveTimerNeedsReset ) {
        m_lastMetadataUpdateTime          = currentSysTime;
        m_metadataAutoSaveTimerNeedsReset = false;
        return;
    }

    if ( isEditingBusy || currentSysTime - m_lastMetadataUpdateTime <
                              METADATA_AUTO_SAVE_IDLE_SECONDS ) {
        return;
    }

    (void)flushPendingMetadataAutoSave();
}

/// @brief 消费全局配置允许的事件请求并推进定时/事件自动保存。
void BeatmapSession::flushConfiguredAutoSave(
    double currentSysTime, bool isEditingBusy,
    const Config::AutoSaveConfig& config)
{
    const std::uint8_t requestedTriggers =
        m_requestedAutoSaveTriggers.exchange(0U, std::memory_order_relaxed);
    constexpr AutoSaveTrigger EXTERNAL_TRIGGERS[]{
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
        m_ctx->currentBeatmap &&
        (m_ctx->actionStack.isDirty() || m_metadataAutoSavePending);

    if ( config.mode == Config::AutoSaveMode::Timed ) {
        m_triggeredAutoSavePending   = false;
        const double intervalSeconds = config.intervalSeconds();
        if ( m_timedAutoSaveDeadline <= 0.0 ||
             m_timedAutoSaveIntervalSeconds != intervalSeconds ) {
            m_timedAutoSaveIntervalSeconds = intervalSeconds;
            m_timedAutoSaveDeadline        = currentSysTime + intervalSeconds;
            return;
        }
        if ( currentSysTime < m_timedAutoSaveDeadline || isEditingBusy ) {
            return;
        }

        m_timedAutoSaveDeadline = currentSysTime + intervalSeconds;
        if ( hasUnsavedChanges ) {
            handleCommand(CmdSaveBeatmap{
                .kind = BeatmapSaveKind::TimedAutoSave,
            });
        }
        return;
    }

    m_timedAutoSaveDeadline        = 0.0;
    m_timedAutoSaveIntervalSeconds = 0.0;
    if ( config.mode != Config::AutoSaveMode::EventTriggered ) {
        m_triggeredAutoSavePending = false;
        return;
    }
    if ( !m_triggeredAutoSavePending ) return;

    if ( isEditingBusy ) return;

    m_triggeredAutoSavePending = false;
    if ( hasUnsavedChanges ) {
        handleCommand(CmdSaveBeatmap{
            .kind = BeatmapSaveKind::TriggeredAutoSave,
        });
    }
}

/// @brief 消费有效项目配置允许的事件请求并推进谱面自动备份。
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
    bool timedBackup  = false;
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
        timedBackup               = true;
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
    auto* project = EditorEngine::instance().getCurrentProject();
    if ( !project || project->m_projectRoot.empty() ) return;

    m_ctx->m_needsTimingsSync = true;
    m_ctx->m_needsNotesSync   = true;
    SessionUtils::syncBeatmap(*m_ctx);
    const auto sourcePath = m_ctx->currentBeatmap->m_baseMapMetadata.map_path;
    const auto result =
        BeatmapBackupService::createBackup(*m_ctx->currentBeatmap,
                                           project->m_projectRoot,
                                           sourcePath,
                                           config.maxBackupCount);
    const auto presentation =
        timedBackup ? Event::BeatmapSavePresentation::TimedAutoBackupStatus
                    : Event::BeatmapSavePresentation::TriggeredAutoBackupStatus;
    if ( !result.m_success ) {
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
    if ( !result.m_errorMessage.empty() ) {
        XWARN("Beatmap backup created but rotation was incomplete: {}",
              result.m_errorMessage);
    }
    Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
        .path         = Config::pathToUtf8(result.m_backupPath),
        .success      = true,
        .isExport     = false,
        .errorMessage = result.m_errorMessage,
        .presentation = presentation,
    });
}

/// @brief 立即落盘尚在等待空闲期的元数据自动保存。
/// @warning 低频阻塞路径：仅允许逻辑线程在打包、项目关闭或尾随自动保存
/// 超时时调用；可能同步谱面数据、访问文件系统并保存项目配置。
bool BeatmapSession::flushPendingMetadataAutoSave()
{
    if ( !m_metadataAutoSavePending ) return true;
    if ( !m_ctx->currentBeatmap ) return false;

    handleCommand(CmdSaveBeatmap{
        .allowExternallyModifiedOverwrite = true,
        .kind                             = BeatmapSaveKind::TriggeredAutoSave,
    });
    return !m_metadataAutoSavePending && !m_ctx->actionStack.isDirty();
}

/// @brief 为打包流程立即保存当前会话中的全部未落盘修改。
/// @warning
/// 低频阻塞路径：仅允许逻辑线程在打包前调用；会同步完整谱面并访问文件系统。
bool BeatmapSession::saveDirtyBeatmapForPackaging()
{
    if ( !m_ctx->currentBeatmap ) return true;
    if ( !m_metadataAutoSavePending && !m_ctx->actionStack.isDirty() ) {
        return true;
    }

    handleCommand(CmdSaveBeatmap{
        .allowExternallyModifiedOverwrite = true,
        .kind                             = BeatmapSaveKind::Internal,
    });
    return !m_metadataAutoSavePending && !m_ctx->actionStack.isDirty();
}

/// @brief 判断本轮是否需要生成并发布渲染快照。
/// @warning 逻辑热路径：只做常量时间背压判断，禁止访问 ECS 或同步缓冲区。
bool BeatmapSession::shouldUpdateRenderSnapshot(
    double currentSysTime, bool forceImmediate,
    const Config::EditorConfig& config) const
{
    if ( forceImmediate ) {
        return true;
    }
    if ( m_lastRenderSnapshotTime <= 0.0 ) {
        return true;
    }
    const double minInterval =
        EditorEngine::instance().adaptiveRenderSnapshotMinInterval(config,
                                                                   false);
    return currentSysTime - m_lastRenderSnapshotTime >= minInterval;
}

/// @brief 根据逻辑时间刷新动画渲染时间。
/// @warning 逻辑热路径：每个 Session update
/// 执行；只做常量级指数平滑计算，不访问文件系统或 ECS。
void BeatmapSession::updateAnimateTime(double                      dt,
                                       const Config::EditorConfig& config,
                                       bool forceImmediate)
{
    const double targetAnimateTime =
        m_ctx->currentTime + config.visual.getEffectiveVisualOffset();
    m_ctx->animateTimeTarget = targetAnimateTime;

    const double duration = std::max(
        0.0, static_cast<double>(config.visual.scrollAnimationDuration));
    if ( forceImmediate || duration <= VISUAL_ANIMATION_EPSILON ||
         !std::isfinite(targetAnimateTime) ||
         !std::isfinite(m_ctx->animateTime) ) {
        m_ctx->animateTime                = targetAnimateTime;
        m_ctx->animateTimeAnimationActive = false;
        return;
    }

    const double diff = targetAnimateTime - m_ctx->animateTime;
    if ( std::abs(diff) <= VISUAL_ANIMATION_EPSILON ) {
        m_ctx->animateTime                = targetAnimateTime;
        m_ctx->animateTimeAnimationActive = false;
        return;
    }

    const double clampedDt = std::clamp(dt, 0.0, VISUAL_ANIMATION_MAX_DT);
    const double alpha     = std::clamp(
        1.0 - std::exp(-VISUAL_ANIMATION_RESPONSE * clampedDt / duration),
        0.0,
        1.0);

    m_ctx->animateTime += diff * alpha;
    if ( std::abs(targetAnimateTime - m_ctx->animateTime) <=
         VISUAL_ANIMATION_EPSILON ) {
        m_ctx->animateTime                = targetAnimateTime;
        m_ctx->animateTimeAnimationActive = false;
        return;
    }

    m_ctx->animateTimeAnimationActive = true;
}

/// @brief 刷新渲染使用的动画时间线缩放倍率。
/// @warning 逻辑热路径：每个 Session update
/// 执行；只做常量级指数平滑计算，不访问文件系统或 ECS。
void BeatmapSession::updateAnimatedTimelineZoom(
    double dt, const Config::EditorConfig& config)
{
    const double targetZoom = sanitizeTimelineZoom(config.visual.timelineZoom);
    m_ctx->animatedTimelineZoomTarget = static_cast<float>(targetZoom);

    double animatedZoom   = sanitizeTimelineZoom(m_ctx->animatedTimelineZoom);
    const double duration = std::max(
        0.0, static_cast<double>(config.visual.scrollAnimationDuration));
    if ( duration <= VISUAL_ANIMATION_EPSILON ) {
        m_ctx->animatedTimelineZoom = static_cast<float>(targetZoom);
        m_ctx->animatedTimelineZoomAnimationActive = false;
        return;
    }

    const double diff = targetZoom - animatedZoom;
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

    if ( std::abs(targetZoom - animatedZoom) <= VISUAL_ANIMATION_EPSILON ) {
        m_ctx->animatedTimelineZoom = static_cast<float>(targetZoom);
        m_ctx->animatedTimelineZoomAnimationActive = false;
        return;
    }

    m_ctx->animatedTimelineZoom = static_cast<float>(animatedZoom);
    m_ctx->animatedTimelineZoomAnimationActive = true;
}

/// @brief 会话逻辑每帧更新。
/// @warning 逻辑热路径：由 EditorEngine::loop 按 UPS
/// 调用；普通路径禁止文件系统访问、完整排序、try/catch 和可避免的 shared_ptr
/// 拷贝；仅配置到期的自动保存/备份低频分支允许持久化。
void BeatmapSession::update(double dt, const Config::EditorConfig& config,
                            bool isActiveSession)
{
    m_ctx->lastConfig      = config;
    m_ctx->isActiveSession = isActiveSession;
    ProjectDraftLaneService::refreshIfChanged(*m_ctx);
    if ( !isActiveSession && m_ctx->isPlaying ) {
        m_ctx->isPlaying = false;
    }
    bool processed = processCommands();
    publishRequestedMutationSnapshot();
    // 配置命令携带软件级快照；本轮调度仍必须恢复项目覆盖后的有效配置。
    m_ctx->lastConfig = config;
    m_ctx->lastConfig.visual.applyKeyCountLayout(m_ctx->trackCount);
    const auto& effectiveConfig = m_ctx->lastConfig;

    if ( m_ctx->isAudioTimelineDescriptorDirty ) {
        const auto* project =
            m_ctx->collaborationProject
                ? m_ctx->collaborationProject.get()
                : EditorEngine::instance().getCurrentProject();
        SessionUtils::rebuildAudioTimelineDescriptor(*m_ctx, project);
    }
    if ( m_ctx->isAudioTimelineFingerprintPublishPending ) {
        EditorEngine::instance().refreshAudioTimelineFingerprints();
        m_ctx->isAudioTimelineFingerprintPublishPending = false;
    }
    if ( isActiveSession && m_ctx->isAudioTimelineActivationPending ) {
        if ( !SessionUtils::activateAudioTimeline(*m_ctx, m_ctx->isPlaying) ) {
            m_ctx->isPlaying = false;
        }
    }

    double currentSysTime =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();

    const bool shouldPrefetchBoundSounds = isActiveSession ||
                                           m_ctx->isPlaying ||
                                           m_ctx->isAudioTimelineSyncFollower;
    if ( shouldPrefetchBoundSounds &&
         currentSysTime >= m_ctx->nextBoundSoundPrefetchSystemTime ) {
        prefetchBoundNoteSounds(*m_ctx);
        m_ctx->nextBoundSoundPrefetchSystemTime =
            currentSysTime + BOUND_SOUND_PREFETCH_INTERVAL_SECONDS;
    }

    bool isInteracting = m_ctx->isDragging || m_ctx->isSelecting ||
                         m_ctx->brushState.isActive ||
                         m_ctx->eraserState.isActive;
    const bool isVisualAnimationActive =
        m_ctx->animateTimeAnimationActive ||
        m_ctx->animatedTimelineZoomAnimationActive;
    const bool isEdgeScrollActive =
        std::abs(m_ctx->previewEdgeScrollVelocity) > 0.0001;
    bool isBusy = isInteracting || m_ctx->isPlaying ||
                  m_ctx->isAudioTimelineSyncFollower ||
                  isVisualAnimationActive || isEdgeScrollActive ||
                  hasPendingCommands();

    if ( effectiveConfig.settings.frameLimit !=
             Config::FrameLimitPreference::VSync &&
         !isBusy && !processed ) {
        if ( currentSysTime - m_ctx->lastSnapshotTime <
             IDLE_UPDATE_MIN_INTERVAL_SECONDS ) {
            return;
        }
    }
    flushDeferredBeatmapSync(currentSysTime, processed, isBusy);
    const bool isMetadataEditingBusy =
        isInteracting || processed || hasPendingCommands();
    flushConfiguredAutoSave(currentSysTime,
                            isMetadataEditingBusy,
                            effectiveConfig.settings.autoSave);
    flushConfiguredAutoBackup(currentSysTime,
                              isMetadataEditingBusy,
                              effectiveConfig.settings.autoBackup);
    flushDeferredMetadataAutoSave(currentSysTime, isMetadataEditingBusy);
    m_ctx->lastSnapshotTime = currentSysTime;

    // --- 边缘自动滚动处理 ---
    if ( std::abs(m_ctx->previewEdgeScrollVelocity) > 0.0001 ) {
        double delta     = m_ctx->previewEdgeScrollVelocity * dt;
        double totalTime = SessionUtils::getEffectiveTotalTimeSeconds(*m_ctx);
        m_ctx->currentTime =
            std::clamp(m_ctx->currentTime + delta, 0.0, totalTime);

        if ( m_ctx->isPlaying && m_ctx->isActiveSession ) {
            Audio::AudioManager::instance().seek(m_ctx->currentTime);
        }
        SessionUtils::syncHitIndex(*m_ctx);
    }

    double previousAnimateTime = m_ctx->animateTime;
    updateAnimatedTimelineZoom(dt, effectiveConfig);

    // --- Playback 更新 ---
    bool isPlaybackClockActive =
        m_ctx->isPlaying || m_ctx->isAudioTimelineSyncFollower;
    bool playbackJumped = false;
    if ( isPlaybackClockActive ) {
        SessionUtils::ensureHitEvents(*m_ctx);

        auto&        audio              = Audio::AudioManager::instance();
        const auto   audioClockSnapshot = audio.getAudioTimelineClockSnapshot();
        const double playbackClockNow =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        isPlaybackClockActive =
            SessionUtils::applyAudioTimelineTransportSnapshot(
                *m_ctx,
                audio.getLoadedAudioTimelineFingerprint(),
                audioClockSnapshot,
                playbackClockNow,
                effectiveConfig.settings.syncConfig);

        updateAnimateTime(dt, effectiveConfig, isPlaybackClockActive);

        std::vector<System::HitFXSystem::HitEvent> triggeredEvents;

        bool isJump =
            !isPlaybackClockActive ||
            (std::abs(m_ctx->animateTime - previousAnimateTime) > 0.2) ||
            (m_ctx->animateTime < previousAnimateTime) || !m_wasPlaying;
        playbackJumped = isJump;

        if ( isJump ) {
            if ( m_ctx->isPlaying ) {
                Audio::AudioManager::instance().clearAllScheduledSoundEffects();
            }
            m_ctx->hitFXSystem.clearActiveEffects();
            SessionUtils::syncHitIndex(*m_ctx);
            m_ctx->hitFXSystem.restoreActiveHoldEffects(
                m_ctx->animateTime, m_ctx->hitEvents, effectiveConfig);

            if ( m_ctx->isPlaying ) {
                // 核心修复：Jump/Start 后立即预测播放窗口内的所有音效
                double predictWindow = 0.2;
                while (
                    m_ctx->nextPredictHitIndex < m_ctx->hitEvents.size() &&
                    m_ctx->hitEvents[m_ctx->nextPredictHitIndex].timestamp <=
                        (m_ctx->animateTime + predictWindow) ) {
                    const auto& ev =
                        m_ctx->hitEvents[m_ctx->nextPredictHitIndex];
                    // 只要物件在当前播放点之后，都触发
                    if ( ev.timestamp >= m_ctx->animateTime ) {
                        m_ctx->hitFXSystem.triggerAudio(
                            ev, m_ctx->trackCount, effectiveConfig);
                    }
                    m_ctx->nextPredictHitIndex++;
                }
            }
        } else {
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
                        m_ctx->hitFXSystem.triggerAudio(
                            ev, m_ctx->trackCount, effectiveConfig);
                    }
                    m_ctx->nextPredictHitIndex++;
                }
            }

            while ( m_ctx->nextHitIndex < m_ctx->hitEvents.size() &&
                    m_ctx->hitEvents[m_ctx->nextHitIndex].timestamp <=
                        m_ctx->animateTime ) {
                const auto& ev = m_ctx->hitEvents[m_ctx->nextHitIndex];
                if ( ev.timestamp > previousAnimateTime ) {
                    triggeredEvents.push_back(ev);
                }
                m_ctx->nextHitIndex++;
            }
        }
        m_ctx->hitFXSystem.update(m_ctx->animateTime,
                                  triggeredEvents,
                                  m_ctx->trackCount,
                                  effectiveConfig);
    } else {
        updateAnimateTime(dt, effectiveConfig, false);

        if ( std::abs(m_ctx->animateTime - previousAnimateTime) > 0.0001 ) {
            SessionUtils::syncHitIndex(*m_ctx);
        }
    }

    m_wasPlaying = isPlaybackClockActive;

    const auto* scrollCache =
        m_ctx->timelineRegistry.ctx().find<System::ScrollCache>();
    const bool isTimelineCacheDirty = scrollCache && scrollCache->isDirty;
    const bool hasRenderDirtyState =
        m_ctx->isNoteOrderDirty || m_ctx->isNotePruneDirty ||
        m_ctx->isNoteStatsDirty || m_ctx->isTransformDirty ||
        m_ctx->isBpmEventsDirty || m_ctx->isMarqueeSelectionDirty ||
        isTimelineCacheDirty;
    // 连续视野动画必须遵守自适应快照间隔，避免逻辑线程空转时无限重建快照。
    const bool forceRenderSnapshot =
        processed || playbackJumped || hasRenderDirtyState;
    if ( shouldUpdateRenderSnapshot(
             currentSysTime, forceRenderSnapshot, effectiveConfig) ) {
        updateECSAndRender(effectiveConfig, isActiveSession);
        m_lastRenderSnapshotTime = currentSysTime;
    }
}

}  // namespace MMM::Logic
