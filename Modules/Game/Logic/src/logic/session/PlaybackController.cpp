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
    double totalTime       = Audio::AudioManager::instance().getTotalTime();
    m_ctx.currentTime      = std::clamp(cmd.time, 0.0, totalTime);
    m_ctx.lastAudioPos     = 0.0;
    m_ctx.lastAudioSysTime = 0.0;
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
    Audio::AudioManager::instance().setPlaybackSpeed(cmd.speed);
}

void PlaybackController::handleCommand(const CmdScroll& cmd)
{
    float wheel = cmd.wheel;
    if ( m_ctx.lastConfig.settings.reverseScroll ) {
        wheel = -wheel;
    }

    if ( m_ctx.isPlaying && m_ctx.lastConfig.settings.stopPlaybackOnScroll ) {
        m_ctx.isPlaying = false;
        Audio::AudioManager::instance().pause();
        m_ctx.currentTime = Audio::AudioManager::instance().getCurrentTime();
        // 如果停止了播放，需要同步一下渲染状态 (虽然 seek 也会做，但这里明确一下更好)
    }

    bool isShiftAccelerated = cmd.isShiftDown;
    if ( isShiftAccelerated && m_ctx.brushState.isActive &&
         m_ctx.lastConfig.settings.disableScrollAccelerationWhileDrawing ) {
        isShiftAccelerated = false;
    }

    double targetTime   = m_ctx.currentTime;
    double visualOffset = m_ctx.lastConfig.visual.visualOffset;

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

    double totalTime       = Audio::AudioManager::instance().getTotalTime();
    m_ctx.currentTime      = std::clamp(targetTime, 0.0, totalTime);
    m_ctx.lastAudioPos     = 0.0;
    m_ctx.lastAudioSysTime = 0.0;
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
