#include "audio/AudioManager.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/audio/AudioSpectrumView.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/imgui/audio/AudioWaveformView.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIThemeUtils.h"

#include <fmt/core.h>

namespace MMM::UI
{

CLayHBox& AudioTrackControllerUI::getRow(size_t index)
{
    while ( m_rows.size() <= index ) m_rows.emplace_back();
    auto& row = m_rows[index];
    row.clear();
    return row;
}

CLayVBox& AudioTrackControllerUI::getSection(size_t index)
{
    while ( m_sections.size() <= index ) m_sections.emplace_back();
    auto& sec = m_sections[index];
    sec.clear();
    return sec;
}

float AudioTrackControllerUI::measureLabelWidth(const char* label)
{
    auto&   skinMgr = Config::SkinManager::instance();
    ImFont* font    = skinMgr.getFont("content");
    if ( !font ) font = ImGui::GetFont();
    float  fontSize = font->LegacySize * font->Scale;
    ImVec2 sz       = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
    return sz.x;
}

void AudioTrackControllerUI::addSettingItem(CLayVBox& parent, size_t& rowIndex,
                                            const char* label, float labelWidth,
                                            CLayBox::DrawFunc widget)
{
    auto& row = getRow(rowIndex++);
    row.setPadding(8, 8, 0, 0).setSpacing(8).setAlignment(Alignment::Center());

    std::string labelId = "AT_R" + std::to_string(rowIndex) + "_L_" + label;
    row.addElement(labelId + "_lbl",
                   Sizing::Fixed(labelWidth),
                   Sizing::Grow(),
                   [label](Clay_BoundingBox r, bool) {
                       float textH  = ImGui::CalcTextSize(label).y;
                       float offset = (r.height - textH) * 0.5f;
                       ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                       ImGui::Text("%s", label);
                   });

    row.addElement(labelId + "_wgt",
                   Sizing::Grow(),
                   Sizing::Grow(),
                   [widget](Clay_BoundingBox r, bool h) { widget(r, h); });

    float rowH = ImGui::GetFrameHeight() + 8.0f;
    parent.addLayout(
        (labelId + "_row").c_str(), row, Sizing::Grow(), Sizing::Fixed(rowH));
}

void AudioTrackControllerUI::buildVolumeSection(CLayVBox& parent,
                                                size_t&   rowIndex,
                                                float labelWidth, float& volume,
                                                bool& muted, bool& changed)
{
    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.volume").data(),
        labelWidth,
        [this, &volume, &muted, &changed](Clay_BoundingBox r, bool) {
            auto& audio = Audio::AudioManager::instance();

            float widgetH = ImGui::GetFrameHeight();
            float offset  = (r.height - widgetH) * 0.5f;
            ImGui::SetCursorScreenPos({ r.x, r.y + offset });

            const char* icon = ICON_MMM_VOLUME_MUTE;
            if ( !muted ) {
                if ( volume <= 0.33f )
                    icon = ICON_MMM_VOLUME_OFF;
                else if ( volume <= 0.66f )
                    icon = ICON_MMM_VOLUME_LOW;
                else
                    icon = ICON_MMM_VOLUME_HIGH;
            }

            bool pushedTextColor = false;
            if ( muted ) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      Utils::UIThemeUtils::getDangerColor());
                pushedTextColor = true;
            }

            float btnWidth = 30.0f;
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            if ( ImGui::Button(icon, ImVec2(btnWidth, 0)) ) {
                muted   = !muted;
                changed = true;
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();

            if ( pushedTextColor ) {
                ImGui::PopStyleColor();
            }

            if ( ImGui::IsItemHovered() ) {
                ImGui::SetTooltip("%s",
                                  muted ? TR("ui.audio_manager.unmute").data()
                                        : TR("ui.audio_manager.mute").data());
            }

            ImGui::SameLine();

            float lrWidth =
                (m_type == TrackType::Main) ? (22.0f * 2 + 4.0f) : 0.0f;
            float sliderWidth = r.width - btnWidth - lrWidth - 16.0f;
            ImGui::SetNextItemWidth(sliderWidth);
            if ( ImGui::SliderFloat("##Volume", &volume, 0.0f, 1.0f, "%.2f") ) {
                changed = true;
                if ( muted && volume > 0.0f ) {
                    muted = false;
                }
            }

            if ( m_type == TrackType::Main ) {
                bool muteL = audio.isMainMixerLeftMuted();
                bool muteR = audio.isMainMixerRightMuted();

                ImGui::SameLine();
                if ( muteL ) {
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, Utils::UIThemeUtils::getDangerColor());
                }
                if ( ImGui::Button("L", ImVec2(22, 0)) ) {
                    audio.setMainMixerLeftMute(!muteL);
                }
                if ( muteL ) {
                    ImGui::PopStyleColor();
                }
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("%s",
                                      TR("ui.audio_manager.mute_l").data());
                }

                ImGui::SameLine(0, 2);

                if ( muteR ) {
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, Utils::UIThemeUtils::getDangerColor());
                }
                if ( ImGui::Button("R", ImVec2(22, 0)) ) {
                    audio.setMainMixerRightMute(!muteR);
                }
                if ( muteR ) {
                    ImGui::PopStyleColor();
                }
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("%s",
                                      TR("ui.audio_manager.mute_r").data());
                }
            }
        });
}

void AudioTrackControllerUI::buildSpeedAndPitchSection(
    CLayVBox& parent, size_t& rowIndex, float labelWidth, float& speed,
    float& pitch, bool& changed)
{
    auto& audio = Audio::AudioManager::instance();

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.speed_control").data(),
        labelWidth,
        [&speed, &audio](Clay_BoundingBox r, bool) {
            float widgetH = ImGui::GetFrameHeight();
            float offset  = (r.height - widgetH) * 0.5f;
            ImGui::SetCursorScreenPos({ r.x, r.y + offset });

            float actualSpeed = (float)audio.getActualPlaybackSpeed();
            ImGui::AlignTextToFramePadding();
            ImGui::Text(
                TR("ui.audio_manager.speed_info").data(), speed, actualSpeed);
        });

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.speed_presets").data(),
        labelWidth,
        [&speed, &changed](Clay_BoundingBox r, bool) {
            float widgetH = ImGui::GetFrameHeight();
            float offset  = (r.height - widgetH) * 0.5f;
            ImGui::SetCursorScreenPos({ r.x, r.y + offset });
            if ( ImGui::Button(TR("ui.audio_manager.speed_025x").data()) ) {
                speed   = 0.25f;
                changed = true;
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.audio_manager.speed_050x").data()) ) {
                speed   = 0.5f;
                changed = true;
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.audio_manager.speed_075x").data()) ) {
                speed   = 0.75f;
                changed = true;
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.audio_manager.speed_100x").data()) ) {
                speed   = 1.0f;
                changed = true;
            }
        });

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.speed_value").data(),
        labelWidth,
        [&speed, &changed](Clay_BoundingBox r, bool) {
            ImGui::SetNextItemWidth(r.width);
            if ( ImGui::SliderFloat(
                     "##SpeedSlider", &speed, 0.25f, 2.0f, "%.4fx") ) {
                changed = true;
            }
        });

    addSettingItem(parent,
                   rowIndex,
                   TR_CACHE("ui.audio_manager.stretch_quality").data(),
                   labelWidth,
                   [&changed, &audio](Clay_BoundingBox r, bool) {
                       const char* qualityNames[] = {
                           TR("ui.audio_manager.quality_fast").data(),
                           TR("ui.audio_manager.quality_balanced").data(),
                           TR("ui.audio_manager.quality_finer").data(),
                           TR("ui.audio_manager.quality_best").data()
                       };

                       int currentQuality =
                           static_cast<int>(audio.getPlaybackQuality());
                       ImGui::SetNextItemWidth(r.width);
                       if ( ImGui::Combo("##StretchQuality",
                                         &currentQuality,
                                         qualityNames,
                                         IM_ARRAYSIZE(qualityNames)) ) {
                           audio.setPlaybackQuality(
                               static_cast<Audio::AudioManager::StretchQuality>(
                                   currentQuality));
                           changed = true;
                       }
                   });

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.pitch_presets").data(),
        labelWidth,
        [&pitch, &changed](Clay_BoundingBox r, bool) {
            ImGui::SetCursorScreenPos({ r.x, r.y });
            if ( ImGui::Button(TR("ui.audio_manager.pitch_n24").data()) ) {
                pitch   = -24.0f;
                changed = true;
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.audio_manager.pitch_n12").data()) ) {
                pitch   = -12.0f;
                changed = true;
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.audio_manager.pitch_n5").data()) ) {
                pitch   = -5.0f;
                changed = true;
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.audio_manager.pitch_0").data()) ) {
                pitch   = 0.0f;
                changed = true;
            }
        });

    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.pitch_value").data(),
        labelWidth,
        [&pitch, &changed](Clay_BoundingBox r, bool) {
            ImGui::SetNextItemWidth(r.width);
            if ( ImGui::SliderFloat(
                     "##PitchSlider", &pitch, -24.0f, 24.0f, "%.1f st") ) {
                changed = true;
            }
        });
}

void AudioTrackControllerUI::buildEffectPreviewSection(CLayVBox& parent,
                                                       size_t&   rowIndex,
                                                       float     labelWidth)
{
    addSettingItem(
        parent,
        rowIndex,
        TR_CACHE("ui.audio_manager.play_preview").data(),
        labelWidth,
        [this](Clay_BoundingBox r, bool) {
            auto& audio = Audio::AudioManager::instance();

            ImGui::SetCursorScreenPos({ r.x, r.y });

            bool        isPaused = audio.isSFXPaused(m_trackId);
            const char* playText =
                isPaused ? TR("ui.audio_manager.resume_preview").data()
                         : TR("ui.audio_manager.play_preview").data();

            if ( ImGui::Button(playText, ImVec2(80, 0)) ) {
                if ( isPaused ) {
                    audio.resumeSoundEffect(m_trackId);
                } else {
                    audio.playSoundEffect(m_trackId);
                }
            }

            ImGui::SameLine();

            if ( ImGui::Button(TR("ui.audio_manager.pause_preview").data(),
                               ImVec2(80, 0)) ) {
                audio.pauseSoundEffect(m_trackId);
            }

            ImGui::SameLine();

            float duration     = (float)audio.getSFXDuration(m_trackId);
            float playbackTime = (float)audio.getSFXPlaybackTime(m_trackId);
            float progress =
                (duration > 0.0f) ? (playbackTime / duration) : 0.0f;
            std::string progressText =
                fmt::format("{:.2f}s / {:.2f}s", playbackTime, duration);

            float remaining = r.width - 80.0f - 80.0f - 16.0f;
            ImGui::ProgressBar(
                progress, ImVec2(remaining, 0), progressText.c_str());
        });
}

void AudioTrackControllerUI::buildAnalysisButtons(CLayVBox&  parent,
                                                  size_t&    rowIndex,
                                                  UIManager* sourceManager)
{
    auto& row = getRow(rowIndex++);
    row.setPadding(8, 8, 4, 4).setSpacing(8).setAlignment(Alignment::Center());

    std::string rowId = "AT_Analysis_R" + std::to_string(rowIndex);
    row.addElement(
        rowId + "_btns",
        Sizing::Grow(),
        Sizing::Grow(),
        [this, sourceManager](Clay_BoundingBox r, bool) {
            ImGui::SetCursorScreenPos({ r.x, r.y });
            float btnW = (r.width - 8.0f) * 0.5f;
            if ( ImGui::Button(TR("ui.audio_manager.open_waveform").data(),
                               ImVec2(btnW, 0)) ) {
                std::string viewName = "AudioWaveform";
                if ( !sourceManager->getView<AudioWaveformView>(viewName) ) {
                    sourceManager->registerView(
                        viewName,
                        std::make_unique<AudioWaveformView>(
                            TR("ui.audio_manager.waveform_title").data()));
                }
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.audio_manager.open_spectrum").data(),
                               ImVec2(btnW, 0)) ) {
                std::string viewName = "AudioSpectrum";
                if ( !sourceManager->getView<AudioSpectrumView>(viewName) ) {
                    sourceManager->registerView(
                        viewName,
                        std::make_unique<AudioSpectrumView>(
                            TR("ui.audio_manager.spectrum_title").data()));
                }
            }
        });

    float rowH = ImGui::GetFrameHeight() + 8.0f;
    parent.addLayout(
        (rowId + "_row").c_str(), row, Sizing::Grow(), Sizing::Fixed(rowH));
}

}  // namespace MMM::UI
