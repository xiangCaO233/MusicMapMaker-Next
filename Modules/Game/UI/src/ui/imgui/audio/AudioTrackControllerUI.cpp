#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "audio/AudioManager.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include "ui/UIManager.h"
#include "ui/imgui/audio/AudioSpectrumView.h"
#include "ui/imgui/audio/AudioWaveformView.h"

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

    ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_FirstUseEver);
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

        // --- Clay 布局构建 ---
        m_contentVBox.clear();
        m_contentVBox.setSpacing(4).setPadding(4, 4, 4, 4);
        size_t rowIndex = 0;

        const char* allLabels[] = {
            TR_CACHE("ui.audio_manager.volume").data(),
            TR_CACHE("ui.audio_manager.speed_control").data(),
            TR_CACHE("ui.audio_manager.speed_presets").data(),
            TR_CACHE("ui.audio_manager.speed_value").data(),
            TR_CACHE("ui.audio_manager.stretch_quality").data(),
            TR_CACHE("ui.audio_manager.pitch_presets").data(),
            TR_CACHE("ui.audio_manager.pitch_value").data(),
            TR_CACHE("ui.audio_manager.play_preview").data(),
        };
        float maxLabelW = 0;
        for ( auto* l : allLabels )
            maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
        maxLabelW += 8.0f;

        buildVolumeSection(
            m_contentVBox, rowIndex, maxLabelW, volume, muted, changed);

        if ( m_type == TrackType::Main ) {
            buildSpeedAndPitchSection(
                m_contentVBox, rowIndex, maxLabelW, speed, pitch, changed);
            buildAnalysisButtons(m_contentVBox, rowIndex, sourceManager);
        }

        if ( m_type == TrackType::Effect ) {
            buildEffectPreviewSection(m_contentVBox, rowIndex, maxLabelW);
        }

        // --- 执行 Clay 布局渲染 ---
        ImVec2 startPos = ImGui::GetCursorScreenPos();
        ImVec2 sz       = m_contentVBox.renderInCurrent(
            startPos, { ImGui::GetContentRegionAvail().x, 0 });
        ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

        // --- EQ 区域使用传统 ImGui 渲染（ImPlot 不适合 Clay） ---
        if ( m_type == TrackType::Main ) {
            renderEQSection(changed);
        }

        // --- 应用更改并持久化 ---
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
                engine.pushCommand(
                    Logic::CmdSetPlaybackSpeed{ static_cast<double>(speed) });
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
