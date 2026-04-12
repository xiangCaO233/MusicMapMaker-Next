#include "audio/AudioManager.h"
#include "logic/BeatmapSession.h"
#include "logic/ecs/components/TimelineComponent.h"
#include <algorithm>

namespace MMM::Logic
{

void BeatmapSession::handleCommand(const CmdSetPlayState& cmd)
{
    m_isPlaying = cmd.isPlaying;
    if ( m_isPlaying ) {
        m_syncTimer        = 0.0;
        m_lastAudioPos     = 0.0;
        m_lastAudioSysTime = 0.0;
        Audio::AudioManager::instance().play();
        m_syncClock.reset(m_currentTime);
        syncHitIndex();
        m_hitFXSystem.clearActiveEffects();
    } else {
        Audio::AudioManager::instance().pause();
        m_currentTime = Audio::AudioManager::instance().getCurrentTime();
    }
}

void BeatmapSession::handleCommand(const CmdSeek& cmd)
{
    double totalTime   = Audio::AudioManager::instance().getTotalTime();
    m_currentTime      = std::clamp(cmd.time, 0.0, totalTime);
    m_lastAudioPos     = 0.0;
    m_lastAudioSysTime = 0.0;
    m_syncClock.reset(m_currentTime);
    Audio::AudioManager::instance().seek(m_currentTime);
    syncHitIndex();
    m_hitFXSystem.clearActiveEffects();
}

void BeatmapSession::handleCommand(const CmdSetPlaybackSpeed& cmd)
{
    Audio::AudioManager::instance().setPlaybackSpeed(cmd.speed);
}

void BeatmapSession::handleCommand(const CmdScroll& cmd)
{
    float wheel = cmd.wheel;
    if ( m_lastConfig.settings.reverseScroll ) {
        wheel = -wheel;
    }

    double targetTime   = m_currentTime;
    double visualOffset = m_lastConfig.visual.visualOffset;

    if ( m_lastConfig.settings.scrollSnap ) {
        int beatDivisor = m_lastConfig.settings.beatDivisor;
        if ( beatDivisor <= 0 ) beatDivisor = 4;

        std::vector<const TimelineComponent*> bpmEvents;
        auto tlView = m_timelineRegistry.view<const TimelineComponent>();
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

            double visualCurrentTime = m_currentTime + visualOffset;
            size_t currentIdx        = 0;
            for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
                if ( visualCurrentTime >= bpmEvents[i]->m_timestamp ) {
                    currentIdx = i;
                } else {
                    break;
                }
            }

            const auto* currentBPM   = bpmEvents[currentIdx];
            double      bpmVal       = currentBPM->m_value;
            double      beatDuration = 60.0 / (bpmVal > 0 ? bpmVal : 120.0);
            double      stepDuration = cmd.isShiftDown
                                           ? beatDuration
                                           : (beatDuration / beatDivisor);

            double relativeVisualTime =
                visualCurrentTime - currentBPM->m_timestamp;
            double stepCount = relativeVisualTime / stepDuration;
            double jump      = std::max(1.0, static_cast<double>(std::abs(wheel)));

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
            if ( cmd.isShiftDown )
                step *= m_lastConfig.settings.scrollSpeedMultiplier;
            targetTime = m_currentTime - static_cast<double>(wheel) * step;
        }
    } else {
        double step = 0.25;
        if ( cmd.isShiftDown )
            step *= m_lastConfig.settings.scrollSpeedMultiplier;
        targetTime = m_currentTime - static_cast<double>(wheel) * step;
    }

    double totalTime   = Audio::AudioManager::instance().getTotalTime();
    m_currentTime      = std::clamp(targetTime, 0.0, totalTime);
    m_lastAudioPos     = 0.0;
    m_lastAudioSysTime = 0.0;
    m_syncClock.reset(m_currentTime);
    Audio::AudioManager::instance().seek(m_currentTime);
    syncHitIndex();
    m_hitFXSystem.clearActiveEffects();
}

} // namespace MMM::Logic
