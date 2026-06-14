#include "logic/BeatmapSession.h"
#include "audio/AudioManager.h"
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

/// @brief 非忙碌状态下 Session 逻辑轻量轮询的最小间隔。
constexpr double IDLE_UPDATE_MIN_INTERVAL_SECONDS = 0.0005;

/// @brief 播放/跟随状态下主动生成渲染快照的最小间隔。
/// @warning 逻辑热路径常量：可见音符查询使用 AbsY 桶索引后允许提高快照频率；
/// UI 亚帧补间仍负责快照间视觉连续性。
constexpr double ACTIVE_RENDER_SNAPSHOT_MIN_INTERVAL_SECONDS = 1.0 / 480.0;

/// @brief 视觉动画目标值的吸附阈值。
constexpr double VISUAL_ANIMATION_EPSILON = 0.0001;

/// @brief 限制单帧视觉动画步长，避免后台恢复时跨越过大。
constexpr double VISUAL_ANIMATION_MAX_DT = 0.05;

/// @brief 指数平滑系数，约等于在配置时长内完成 99.75% 的位移。
constexpr double VISUAL_ANIMATION_RESPONSE = 6.0;

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

    m_ctx->audioFinishedToken =
        MMM::Event::EventBus::instance()
            .subscribe<MMM::Event::AudioFinishedEvent>(
                [this](const MMM::Event::AudioFinishedEvent& e) {
                    if ( !e.isLooping ) {
                        m_ctx->isPlaying               = false;
                        m_ctx->isMainAudioSyncFollower = false;
                        Audio::AudioManager::instance().pause();
                    }
                });

    m_ctx->audioPositionToken =
        MMM::Event::EventBus::instance()
            .subscribe<MMM::Event::AudioPositionEvent>(
                [this](const MMM::Event::AudioPositionEvent& e) {
                    if ( m_ctx->isPlaying ) {
                        m_ctx->lastAudioPos     = e.positionSeconds;
                        m_ctx->lastAudioSysTime = e.systemTimeSeconds;

                        float speed =
                            Audio::AudioManager::instance().getPlaybackSpeed();
                        double currentOffset =
                            e.systemTimeSeconds - (e.positionSeconds / speed);

                        if ( !m_ctx->hasInitialAudioOffset ) {
                            m_ctx->smoothedAudioOffset   = currentOffset;
                            m_ctx->hasInitialAudioOffset = true;
                        } else {
                            // 低通滤波器平滑处理音频时间与系统时间的误差，消除
                            // 50Hz 音频块回调抖动 alpha
                            // 值越小越平滑，但响应音频偏移(如缓冲卡顿)的速度越慢
                            double alpha = 0.05;
                            m_ctx->smoothedAudioOffset =
                                m_ctx->smoothedAudioOffset * (1.0 - alpha) +
                                currentOffset * alpha;
                        }
                    }
                });
}

BeatmapSession::~BeatmapSession() = default;

void BeatmapSession::pushCommand(LogicCommand&& cmd)
{
    m_commandQueue.enqueue(std::move(cmd));
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
           m_ctx->isMainAudioSyncFollower || m_ctx->isDragging ||
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

/// @brief 判断本轮是否需要生成并发布渲染快照。
/// @warning 逻辑热路径：只做常量时间节流判断，禁止访问 ECS 或同步缓冲区。
bool BeatmapSession::shouldUpdateRenderSnapshot(double currentSysTime,
                                                bool   forceImmediate) const
{
    if ( forceImmediate ) {
        return true;
    }
    if ( m_lastRenderSnapshotTime <= 0.0 ) {
        return true;
    }
    return currentSysTime - m_lastRenderSnapshotTime >=
           ACTIVE_RENDER_SNAPSHOT_MIN_INTERVAL_SECONDS;
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
    m_ctx->lastConfig = config;
    bool processed    = processCommands();

    double currentSysTime =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();

    bool       isInteracting = m_ctx->isDragging || m_ctx->isSelecting ||
                               m_ctx->brushState.isActive ||
                               m_ctx->eraserState.isActive;
    const bool isVisualAnimationActive =
        m_ctx->animateTimeAnimationActive ||
        m_ctx->animatedTimelineZoomAnimationActive;
    const bool isEdgeScrollActive =
        std::abs(m_ctx->previewEdgeScrollVelocity) > 0.0001;
    bool isBusy = isInteracting || m_ctx->isPlaying ||
                  m_ctx->isMainAudioSyncFollower || isVisualAnimationActive ||
                  isEdgeScrollActive || hasPendingCommands();

    if ( config.settings.frameLimit != Config::FrameLimitPreference::VSync &&
         !isBusy && !processed ) {
        if ( currentSysTime - m_ctx->lastSnapshotTime <
             IDLE_UPDATE_MIN_INTERVAL_SECONDS ) {
            return;
        }
    }
    flushDeferredBeatmapSync(currentSysTime, processed, isBusy);
    m_ctx->lastSnapshotTime = currentSysTime;

    // --- 边缘自动滚动处理 ---
    if ( std::abs(m_ctx->previewEdgeScrollVelocity) > 0.0001 ) {
        double delta     = m_ctx->previewEdgeScrollVelocity * dt;
        double totalTime = SessionUtils::getEffectiveTotalTimeSeconds(*m_ctx);
        m_ctx->currentTime =
            std::clamp(m_ctx->currentTime + delta, 0.0, totalTime);

        m_ctx->syncClock.reset(m_ctx->currentTime);
        // 重置壁钟基准
        m_ctx->playStartSysTime =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        m_ctx->playStartVisualTime = m_ctx->currentTime;
        if ( m_ctx->isPlaying ) {
            Audio::AudioManager::instance().seek(m_ctx->currentTime);
        }
        SessionUtils::syncHitIndex(*m_ctx);
    }

    double previousAnimateTime = m_ctx->animateTime;
    updateAnimatedTimelineZoom(dt, config);

    // --- Playback 更新 ---
    const bool isPlaybackClockActive =
        m_ctx->isPlaying || m_ctx->isMainAudioSyncFollower;
    bool playbackJumped = false;
    if ( isPlaybackClockActive ) {
        SessionUtils::ensureHitEvents(*m_ctx);

        float speed = Audio::AudioManager::instance().getPlaybackSpeed();
        currentSysTime =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();

        // 核心修复：直接用系统时钟计算 currentTime，而非累加逻辑线程的 dt。
        // 后台同主音轨跟随谱面也使用该基准推进视觉特效，但不参与音频校准。
        m_ctx->currentTime = m_ctx->playStartVisualTime +
                             (currentSysTime - m_ctx->playStartSysTime) * speed;

        // --- 周期性音频同步校准 ---
        if ( m_ctx->isPlaying ) {
            m_ctx->syncTimer += dt;
        } else {
            m_ctx->syncTimer = 0.0;
        }
        if ( m_ctx->isPlaying &&
             m_ctx->syncTimer >= config.settings.syncConfig.syncInterval ) {
            double audioTime = 0.0;
            if ( m_ctx->hasInitialAudioOffset ) {
                audioTime =
                    (currentSysTime - m_ctx->smoothedAudioOffset) * speed;
            } else {
                audioTime = Audio::AudioManager::instance().getCurrentTime();
            }

            double error = audioTime - m_ctx->currentTime;
            // 只有误差超过死区 (2ms) 才进行校准
            if ( std::abs(error) > 0.002 ) {
                if ( std::abs(error) > 0.5 ) {
                    // 大幅度跳转 (Seek)，直接重置基准
                    m_ctx->playStartVisualTime = audioTime;
                    m_ctx->playStartSysTime    = currentSysTime;
                } else {
                    // 微小偏移：将基准做渐进修正，避免跳跃
                    double correction =
                        error * static_cast<double>(
                                    config.settings.syncConfig.integralFactor);
                    m_ctx->playStartVisualTime += correction;
                }
                // 重新计算 currentTime 以应用修正
                m_ctx->currentTime =
                    m_ctx->playStartVisualTime +
                    (currentSysTime - m_ctx->playStartSysTime) * speed;
            }
            m_ctx->syncTimer = 0.0;
        }

        updateAnimateTime(dt, config, true);

        std::vector<System::HitFXSystem::HitEvent> triggeredEvents;

        bool isJump =
            (std::abs(m_ctx->animateTime - previousAnimateTime) > 0.2) ||
            (m_ctx->animateTime < previousAnimateTime) || !m_wasPlaying;
        playbackJumped = isJump;

        if ( isJump ) {
            if ( m_ctx->isPlaying ) {
                Audio::AudioManager::instance().clearAllScheduledSoundEffects();
            }
            m_ctx->hitFXSystem.clearActiveEffects();
            SessionUtils::syncHitIndex(*m_ctx);

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
                        m_ctx->hitFXSystem.triggerAudio(ev, config);
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
                        m_ctx->hitFXSystem.triggerAudio(ev, config);
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
        m_ctx->hitFXSystem.update(m_ctx->animateTime, triggeredEvents, config);
    } else {
        updateAnimateTime(dt, config, false);
        m_ctx->syncTimer = 0.0;

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
    const bool forceRenderSnapshot =
        processed || isInteracting || isEdgeScrollActive ||
        isVisualAnimationStillActive || playbackJumped || hasRenderDirtyState;
    if ( shouldUpdateRenderSnapshot(currentSysTime, forceRenderSnapshot) ) {
        updateECSAndRender(config, isActiveSession);
        m_lastRenderSnapshotTime = currentSysTime;
    }
}

}  // namespace MMM::Logic
