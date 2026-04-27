#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "audio/AudioManager.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include "ui/UIManager.h"
#include "ui/imgui/audio/AudioWaveformView.h"
#include "ui/imgui/audio/AudioSpectrumView.h"

namespace MMM::UI
{

AudioTrackControllerUI::AudioTrackControllerUI(const std::string& trackId,
                                               const std::string& trackName,
                                               TrackType          type)
    : IUIView(trackName)
    , m_trackId(trackId)
    , m_trackName(trackName)
    , m_type(type)
{
}

void AudioTrackControllerUI::update(UIManager* sourceManager)
{
    if ( !m_isOpen ) {
        return;
    }

    auto& audio   = Audio::AudioManager::instance();
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();

    ImGui::SetNextWindowSize(ImVec2(350, 450), ImGuiCond_FirstUseEver);
    if ( ImGui::Begin(m_trackName.c_str(), &m_isOpen) ) {
        float volume = 0.5f;
        float speed  = 1.0f;
        float pitch  = 0.0f;
        bool  muted  = false;

        AudioTrackConfig* config = nullptr;
        if ( project ) {
            for ( auto& res : project->m_audioResources ) {
                if ( res.m_id == m_trackId ) {
                    config = &res.m_config;
                    break;
                }
            }
        }

        if ( config ) {
            volume = config->volume;
            speed  = config->playbackSpeed;
            pitch  = config->playbackPitch;
            muted  = config->muted;
            if ( m_type == TrackType::Main ) {
                m_currentPreset =
                    config->eqEnabled
                        ? static_cast<Audio::EQPreset>(config->eqPreset)
                        : Audio::EQPreset::None;
            }
        } else {
            // 回退到内存状态
            if ( m_type == TrackType::Main ) {
                volume = audio.getMainTrackVolume();
                speed  = (float)audio.getPlaybackSpeed();
                pitch  = (float)audio.getPlaybackPitch();
                muted  = audio.isMainTrackMuted();
            } else {
                volume = audio.getSFXPoolVolume(m_trackId);
                muted  = audio.getSFXPoolMute(m_trackId);
            }
        }

        bool changed = false;

        renderVolumeSection(volume, muted, changed);

        if ( m_type == TrackType::Main ) {
            renderSpeedAndPitchSection(speed, pitch, changed);
            renderEQSection(changed);

            ImGui::Separator();
            if ( ImGui::Button(TR("ui.audio_manager.open_waveform").data(),
                               ImVec2(-1, 0)) ) {
                std::string viewName = "AudioWaveform";
                if ( !sourceManager->getView<AudioWaveformView>(viewName) ) {
                    sourceManager->registerView(
                        viewName,
                        std::make_unique<AudioWaveformView>(
                            TR("ui.audio_manager.waveform_title").data()));
                }
            }
            if ( ImGui::Button(TR("ui.audio_manager.open_spectrum").data(),
                               ImVec2(-1, 0)) ) {
                std::string viewName = "AudioSpectrum";
                if ( !sourceManager->getView<AudioSpectrumView>(viewName) ) {
                    sourceManager->registerView(
                        viewName,
                        std::make_unique<AudioSpectrumView>(
                            TR("ui.audio_manager.spectrum_title").data()));
                }
            }
        }

        if ( m_type == TrackType::Effect ) {
            renderEffectPreviewSection();
        }

        // --- 5. 应用更改并持久化 ---
        if ( changed ) {
            if ( config ) {
                config->volume        = volume;
                config->muted         = muted;
                config->playbackSpeed = speed;
                config->playbackPitch = pitch;
                if ( m_type == TrackType::Main ) {
                    config->eqEnabled = audio.isMainTrackEQEnabled();
                    config->eqPreset =
                        static_cast<int>(audio.getMainTrackEQPreset());
                    config->eqBandGains.clear();
                    config->eqBandQs.clear();

                    const size_t bandCount = audio.getMainTrackEQBandCount();
                    config->eqBandGains.reserve(bandCount);
                    config->eqBandQs.reserve(bandCount);
                    for ( size_t i = 0; i < bandCount; ++i ) {
                        config->eqBandGains.push_back(
                            audio.getMainTrackEQBandGain(i));
                        config->eqBandQs.push_back(
                            audio.getMainTrackEQBandQ(i));
                    }
                }
            }

            if ( m_type == TrackType::Main ) {
                audio.setMainTrackVolume(volume);
                audio.setMainTrackMute(muted);
                audio.setPlaybackSpeed(speed);
                audio.setPlaybackPitch(pitch);
            } else {
                bool  isPermanent = true;
                auto& skinData    = Config::SkinManager::instance().getData();
                if ( skinData.audioPaths.count(m_trackId) == 0 ) {
                    isPermanent = false;
                }
                audio.setSFXPoolVolume(m_trackId, volume, isPermanent);
                audio.setSFXPoolMute(m_trackId, muted, isPermanent);
            }

            if ( project ) {
                engine.saveProject();
            }
        }
    }
    ImGui::End();
}

}  // namespace MMM::UI
