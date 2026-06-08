#include "logic/BeatmapSession.h"
#include "audio/AudioManager.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"

#include "logic/session/ActionController.h"
#include "logic/session/InteractionController.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
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
           m_ctx->eraserState.isActive ||
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

/// @brief 会话逻辑每帧更新。
/// @warning 逻辑热路径：由 EditorEngine::loop 按 UPS
/// 调用；禁止文件系统访问、完整排序、try/catch 和可避免的 shared_ptr 拷贝。
void BeatmapSession::update(double dt, const Config::EditorConfig& config,
                            bool isActiveSession)
{
    m_ctx->lastConfig = config;
    bool processed    = processCommands();

    // --- 性能节流 (Performance Throttling) ---
    // 逻辑：如果 VSync 关闭，逻辑线程会极其频繁地执行。
    // 我们限制渲染快照和 ECS 变换的最高频率（约为 2000Hz），
    // 除非有关键状态变化（如指令输入、正在播放或正在交互）。
    double currentSysTime =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();

    bool isInteracting = m_ctx->isDragging || m_ctx->isSelecting ||
                         m_ctx->brushState.isActive ||
                         m_ctx->eraserState.isActive;
    bool isBusy = isInteracting || m_ctx->isPlaying ||
                  m_ctx->isMainAudioSyncFollower || hasPendingCommands();

    if ( config.settings.frameLimit != Config::FrameLimitPreference::VSync &&
         !m_ctx->isPlaying && !isInteracting && !processed ) {
        // 阈值设为 0.5ms (2000Hz)。这对于非播放状态下的 UI 响应已经绰绰有余。
        if ( currentSysTime - m_ctx->lastSnapshotTime < 0.0005 ) {
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

    double prevVisualTime = m_ctx->visualTime;

    // --- Playback 更新 ---
    const bool isVisualPlaybackActive =
        m_ctx->isPlaying || m_ctx->isMainAudioSyncFollower;
    if ( isVisualPlaybackActive ) {
        SessionUtils::ensureHitEvents(*m_ctx);

        float  speed = Audio::AudioManager::instance().getPlaybackSpeed();
        double currentSysTime =
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

        // visualTime = currentTime + 视觉偏移
        m_ctx->visualTime =
            m_ctx->currentTime + config.visual.getEffectiveVisualOffset();

        std::vector<System::HitFXSystem::HitEvent> triggeredEvents;

        bool isJump = (std::abs(m_ctx->visualTime - prevVisualTime) > 0.2) ||
                      (m_ctx->visualTime < prevVisualTime) || !m_wasPlaying;

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
                        (m_ctx->visualTime + predictWindow) ) {
                    const auto& ev =
                        m_ctx->hitEvents[m_ctx->nextPredictHitIndex];
                    // 只要物件在当前播放点之后，都触发
                    if ( ev.timestamp >= m_ctx->visualTime ) {
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
                        (m_ctx->visualTime + predictWindow) ) {
                    const auto& ev =
                        m_ctx->hitEvents[m_ctx->nextPredictHitIndex];
                    if ( ev.timestamp > (prevVisualTime + predictWindow) ) {
                        m_ctx->hitFXSystem.triggerAudio(ev, config);
                    }
                    m_ctx->nextPredictHitIndex++;
                }
            }

            while ( m_ctx->nextHitIndex < m_ctx->hitEvents.size() &&
                    m_ctx->hitEvents[m_ctx->nextHitIndex].timestamp <=
                        m_ctx->visualTime ) {
                const auto& ev = m_ctx->hitEvents[m_ctx->nextHitIndex];
                if ( ev.timestamp > prevVisualTime ) {
                    triggeredEvents.push_back(ev);
                }
                m_ctx->nextHitIndex++;
            }
        }
        m_ctx->hitFXSystem.update(m_ctx->visualTime, triggeredEvents, config);
    } else {
        m_ctx->visualTime =
            m_ctx->currentTime + config.visual.getEffectiveVisualOffset();
        m_ctx->syncTimer = 0.0;

        if ( std::abs(m_ctx->visualTime - prevVisualTime) > 0.0001 ) {
            SessionUtils::syncHitIndex(*m_ctx);
        }
    }

    m_wasPlaying = isVisualPlaybackActive;

    updateECSAndRender(config, isActiveSession);
}

}  // namespace MMM::Logic
