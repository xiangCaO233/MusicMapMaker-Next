#include "logic/BeatmapSession.h"
#include "audio/AudioManager.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"

#include "logic/session/ActionController.h"
#include "logic/session/InteractionController.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include <chrono>

static void markScrollCacheDirty(entt::registry& reg, entt::entity)
{
    if ( auto* cache = reg.ctx().find<MMM::Logic::System::ScrollCache>() ) {
        cache->isDirty = true;
    }
}

namespace MMM::Logic
{

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
                        m_ctx->isPlaying = false;
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

void BeatmapSession::update(double dt, const Config::EditorConfig& config)
{
    m_ctx->lastConfig = config;

    processCommands();

    // --- 边缘自动滚动处理 ---
    if ( std::abs(m_ctx->previewEdgeScrollVelocity) > 0.0001 ) {
        double delta     = m_ctx->previewEdgeScrollVelocity * dt;
        double totalTime = Audio::AudioManager::instance().getTotalTime();
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
    if ( m_ctx->isPlaying ) {
        float  speed = Audio::AudioManager::instance().getPlaybackSpeed();
        double currentSysTime =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();

        // 核心修复：直接用系统时钟计算 currentTime，而非累加逻辑线程的 dt
        // 这样无论逻辑线程的帧间隔如何波动，currentTime
        // 都是系统时钟的严格线性函数 彻底消除因 dt 采样精度差异导致的周期性抖动
        m_ctx->currentTime = m_ctx->playStartVisualTime +
                             (currentSysTime - m_ctx->playStartSysTime) * speed;

        // --- 周期性音频同步校准 ---
        m_ctx->syncTimer += dt;
        if ( m_ctx->syncTimer >= config.settings.syncConfig.syncInterval ) {
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
        m_ctx->visualTime = m_ctx->currentTime + config.visual.visualOffset;

        std::vector<System::HitFXSystem::HitEvent> triggeredEvents;

        bool isJump = (std::abs(m_ctx->visualTime - prevVisualTime) > 0.2) ||
                      (m_ctx->visualTime < prevVisualTime) || !m_wasPlaying;

        if ( isJump ) {
            m_ctx->hitFXSystem.clearActiveEffects();
            SessionUtils::syncHitIndex(*m_ctx);

            // 核心修复：Jump/Start 后立即预测播放窗口内的所有音效
            double predictWindow = 0.2;
            while ( m_ctx->nextPredictHitIndex < m_ctx->hitEvents.size() &&
                    m_ctx->hitEvents[m_ctx->nextPredictHitIndex].timestamp <=
                        (m_ctx->visualTime + predictWindow) ) {
                const auto& ev = m_ctx->hitEvents[m_ctx->nextPredictHitIndex];
                // 只要物件在当前播放点之后，都触发
                if ( ev.timestamp >= m_ctx->visualTime ) {
                    m_ctx->hitFXSystem.triggerAudio(ev, config);
                }
                m_ctx->nextPredictHitIndex++;
            }
        } else {
            double predictWindow = 0.2;
            while ( m_ctx->nextPredictHitIndex < m_ctx->hitEvents.size() &&
                    m_ctx->hitEvents[m_ctx->nextPredictHitIndex].timestamp <=
                        (m_ctx->visualTime + predictWindow) ) {
                const auto& ev = m_ctx->hitEvents[m_ctx->nextPredictHitIndex];
                if ( ev.timestamp > (prevVisualTime + predictWindow) ) {
                    m_ctx->hitFXSystem.triggerAudio(ev, config);
                }
                m_ctx->nextPredictHitIndex++;
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
        m_ctx->visualTime = m_ctx->currentTime + config.visual.visualOffset;
        m_ctx->syncTimer  = 0.0;

        if ( std::abs(m_ctx->visualTime - prevVisualTime) > 0.0001 ) {
            SessionUtils::syncHitIndex(*m_ctx);
        }
    }

    m_wasPlaying = m_ctx->isPlaying;

    updateECSAndRender(config);
}

}  // namespace MMM::Logic
