#include "logic/session/PlaybackController.h"
#include "audio/AudioManager.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include <algorithm>
#include <chrono>

namespace MMM::Logic
{

void PlaybackController::handleCommand(const CmdSetPlayState& cmd)
{
    m_ctx.isPlaying = cmd.isPlaying;
    if ( m_ctx.isPlaying ) {
        m_ctx.syncTimer             = 0.0;
        m_ctx.lastAudioPos          = 0.0;
        m_ctx.lastAudioSysTime      = 0.0;
        m_ctx.hasInitialAudioOffset = false;
        // 初始化壁钟基准，用于后续无抖动的 visualTime 计算
        m_ctx.playStartSysTime =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        m_ctx.playStartVisualTime = m_ctx.currentTime;
        Audio::AudioManager::instance().play();
        m_ctx.syncClock.reset(m_ctx.currentTime);
        SessionUtils::syncHitIndex(m_ctx);
        m_ctx.hitFXSystem.clearActiveEffects();
    } else {
        Audio::AudioManager::instance().pause();
        m_ctx.currentTime = Audio::AudioManager::instance().getCurrentTime();
    }
}

void PlaybackController::handleCommand(const CmdSeek& cmd)
{
    if ( m_ctx.isPlaying && m_ctx.lastConfig.settings.stopPlaybackOnScroll ) {
        m_ctx.isPlaying = false;
        Audio::AudioManager::instance().pause();
        m_ctx.currentTime = Audio::AudioManager::instance().getCurrentTime();
    }

    double totalTime = Audio::AudioManager::instance().getTotalTime();
    double minTime   = -m_ctx.lastConfig.visual.getEffectiveVisualOffset();

    // 核心修复：确保 std::clamp 的上限不小于下限。
    // 如果由于配置（如负的 visualOffset）导致 minTime > totalTime，
    // 我们将 minTime 限制为 totalTime，防止触发 std::clamp 的断言失败。
    if ( minTime > totalTime ) {
        minTime = totalTime;
    }

    m_ctx.currentTime           = std::clamp(cmd.time, minTime, totalTime);
    m_ctx.lastAudioPos          = 0.0;
    m_ctx.lastAudioSysTime      = 0.0;
    m_ctx.hasInitialAudioOffset = false;
    // 重置壁钟基准
    m_ctx.playStartSysTime =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    m_ctx.playStartVisualTime = m_ctx.currentTime;
    m_ctx.syncClock.reset(m_ctx.currentTime);
    Audio::AudioManager::instance().seek(m_ctx.currentTime);
    SessionUtils::syncHitIndex(m_ctx);
    m_ctx.hitFXSystem.clearActiveEffects();
}

void PlaybackController::handleCommand(const CmdSetPlaybackSpeed& cmd)
{
    float oldSpeed =
        static_cast<float>(Audio::AudioManager::instance().getPlaybackSpeed());
    if ( std::abs(static_cast<float>(cmd.speed) - oldSpeed) < 1e-6f ) {
        return;
    }

    if ( m_ctx.isPlaying ) {
        // 获取当前系统时间
        double currentSysTime =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();

        // 1. 在切换速度前，以旧速度计算出当前的精确逻辑时间
        m_ctx.currentTime =
            m_ctx.playStartVisualTime +
            (currentSysTime - m_ctx.playStartSysTime) * oldSpeed;

        // 2. 以当前逻辑时间作为新速度的起点，重置系统时钟基准
        m_ctx.playStartVisualTime = m_ctx.currentTime;
        m_ctx.playStartSysTime    = currentSysTime;

        // 3. 强制重置音频同步系统，使其在变速后立即重新对齐硬件时钟
        m_ctx.hasInitialAudioOffset = false;
        // 将计时器设为间隔值，确保在下一次 BeatmapSession::update
        // 中立即触发同步块
        m_ctx.syncTimer = m_ctx.lastConfig.settings.syncConfig.syncInterval;

        m_ctx.syncClock.reset(m_ctx.currentTime);
        SessionUtils::syncHitIndex(m_ctx);
    }

    Audio::AudioManager::instance().setPlaybackSpeed(cmd.speed);
}

void PlaybackController::handleCommand(const CmdScroll& cmd)
{
    float wheel = cmd.wheel;
    if ( m_ctx.lastConfig.settings.reverseScroll &&
         (SessionUtils::isMainCanvasCameraId(cmd.cameraId) ||
          cmd.cameraId == "Timeline") ) {
        wheel = -wheel;
    }

    if ( m_ctx.isPlaying && m_ctx.lastConfig.settings.stopPlaybackOnScroll ) {
        m_ctx.isPlaying = false;
        Audio::AudioManager::instance().pause();
        m_ctx.currentTime = Audio::AudioManager::instance().getCurrentTime();
        // 如果停止了播放，需要同步一下渲染状态 (虽然 seek
        // 也会做，但这里明确一下更好)
    }

    bool isShiftAccelerated = cmd.isShiftDown;
    if ( isShiftAccelerated && m_ctx.brushState.isActive &&
         m_ctx.lastConfig.settings.disableScrollAccelerationWhileDrawing ) {
        isShiftAccelerated = false;
    }

    double targetTime   = m_ctx.currentTime;
    double visualOffset = m_ctx.lastConfig.visual.getEffectiveVisualOffset();

    if ( m_ctx.lastConfig.settings.scrollSnap ) {
        int beatDivisor = m_ctx.lastConfig.settings.beatDivisor;
        if ( beatDivisor <= 0 ) beatDivisor = 4;

        std::vector<const TimelineComponent*> bpmEvents;
        auto tlView = m_ctx.timelineRegistry.view<const TimelineComponent>();
        for ( auto entity : tlView ) {
            const auto& tl = tlView.get<const TimelineComponent>(entity);
            if ( tl.m_effect == ::MMM::TimingEffect::BPM ) {
                bpmEvents.push_back(&tl);
            }
        }

        if ( !bpmEvents.empty() ) {
            std::sort(bpmEvents.begin(),
                      bpmEvents.end(),
                      [](const auto* a, const auto* b) {
                          return a->m_timestamp < b->m_timestamp;
                      });

            double visualCurrentTime = m_ctx.currentTime + visualOffset;
            size_t currentIdx        = 0;
            for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
                if ( visualCurrentTime >= bpmEvents[i]->m_timestamp ) {
                    currentIdx = i;
                } else {
                    break;
                }
            }

            const auto* currentBPM = bpmEvents[currentIdx];
            double      bpmVal     = currentBPM->m_value;
            double      bVal       = bpmVal;
            if ( bVal <= 0.0 ) {
                bVal = 120.0;
                if ( m_ctx.currentBeatmap &&
                     m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm >
                         0.0 ) {
                    bVal =
                        m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm;
                }
            }
            double beatDuration = 60.0 / bVal;
            double stepDuration = isShiftAccelerated
                                      ? beatDuration
                                      : (beatDuration / beatDivisor);

            double relativeVisualTime =
                visualCurrentTime - currentBPM->m_timestamp;
            double stepCount = relativeVisualTime / stepDuration;
            double jump = std::max(1.0, static_cast<double>(std::abs(wheel)));

            double targetVisualTime = visualCurrentTime;
            if ( wheel > 0 ) {
                targetVisualTime =
                    currentBPM->m_timestamp +
                    std::floor(stepCount - 0.001 - (jump - 1.0)) * stepDuration;
            } else {
                targetVisualTime =
                    currentBPM->m_timestamp +
                    std::ceil(stepCount + 0.001 + (jump - 1.0)) * stepDuration;
            }
            targetTime = targetVisualTime - visualOffset;
        } else {
            double step = 0.25;
            if ( isShiftAccelerated )
                step *= m_ctx.lastConfig.settings.scrollSpeedMultiplier;
            targetTime = m_ctx.currentTime - static_cast<double>(wheel) * step;
        }
    } else {
        double step = 0.25;
        if ( isShiftAccelerated )
            step *= m_ctx.lastConfig.settings.scrollSpeedMultiplier;
        targetTime = m_ctx.currentTime - static_cast<double>(wheel) * step;
    }

    double totalTime = Audio::AudioManager::instance().getTotalTime();
    double minTime   = -m_ctx.lastConfig.visual.getEffectiveVisualOffset();

    if ( minTime > totalTime ) {
        minTime = totalTime;
    }

    m_ctx.currentTime           = std::clamp(targetTime, minTime, totalTime);
    m_ctx.lastAudioPos          = 0.0;
    m_ctx.lastAudioSysTime      = 0.0;
    m_ctx.hasInitialAudioOffset = false;
    // 重置壁钟基准
    m_ctx.playStartSysTime =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    m_ctx.playStartVisualTime = m_ctx.currentTime;
    m_ctx.syncClock.reset(m_ctx.currentTime);
    Audio::AudioManager::instance().seek(m_ctx.currentTime);
    SessionUtils::syncHitIndex(m_ctx);
    m_ctx.hitFXSystem.clearActiveEffects();
}


}  // namespace MMM::Logic
