#include "logic/BeatmapSession.h"
#include "audio/AudioManager.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"

#include "logic/session/ActionController.h"
#include "logic/session/InteractionController.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
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
constexpr double IDLE_UPDATE_MIN_INTERVAL_SECONDS = 0.0005;

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
    if ( blockCollaborationOfflineEdit(cmd) ) return;
    m_commandQueue.enqueue(std::move(cmd));
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

void BeatmapSession::setMutationObserver(
    std::shared_ptr<::MMM::IBeatmapMutationObserver> observer,
    bool                                             publishCurrentSnapshot)
{
    const bool requestSnapshot = observer != nullptr && publishCurrentSnapshot;
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
    observer->onBeatmapMutated(*m_ctx->currentBeatmap,
                               ::MMM::BeatmapMutationFlags::All);
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

/// @brief 立即落盘尚在等待空闲期的元数据自动保存。
/// @warning 低频阻塞路径：仅允许逻辑线程在打包、项目关闭或尾随自动保存
/// 超时时调用；可能同步谱面数据、访问文件系统并保存项目配置。
bool BeatmapSession::flushPendingMetadataAutoSave()
{
    if ( !m_metadataAutoSavePending ) return true;
    if ( !m_ctx->currentBeatmap ) return false;

    handleCommand(CmdSaveBeatmap{ .allowExternallyModifiedOverwrite = true });
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

    handleCommand(CmdSaveBeatmap{ .allowExternallyModifiedOverwrite = true });
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
/// 调用；禁止文件系统访问、完整排序、try/catch 和可避免的 shared_ptr 拷贝。
void BeatmapSession::update(double dt, const Config::EditorConfig& config,
                            bool isActiveSession)
{
    m_ctx->lastConfig      = config;
    m_ctx->isActiveSession = isActiveSession;
    if ( !isActiveSession && m_ctx->isPlaying ) {
        m_ctx->isPlaying = false;
    }
    bool processed = processCommands();
    publishRequestedMutationSnapshot();
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

    bool       isInteracting = m_ctx->isDragging || m_ctx->isSelecting ||
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
    const bool isMetadataEditingBusy = isInteracting || hasPendingCommands();
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
    const bool isVisualAnimationStillActive =
        m_ctx->animateTimeAnimationActive ||
        m_ctx->animatedTimelineZoomAnimationActive;
    const bool forceRenderSnapshot = processed || isEdgeScrollActive ||
                                     isVisualAnimationStillActive ||
                                     playbackJumped || hasRenderDirtyState;
    if ( shouldUpdateRenderSnapshot(
             currentSysTime, forceRenderSnapshot, effectiveConfig) ) {
        updateECSAndRender(effectiveConfig, isActiveSession);
        m_lastRenderSnapshotTime = currentSysTime;
    }
}

}  // namespace MMM::Logic
