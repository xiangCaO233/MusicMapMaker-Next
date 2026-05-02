#include "audio/AudioManager.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "ui/Icons.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/utils/UIThemeUtils.h"

// Needed for fmt::format
#include <fmt/core.h>

namespace MMM::UI
{

void AudioTrackControllerUI::renderVolumeSection(float& volume, bool& muted,
                                                 bool& changed)
{
    // --- 1. 静音按钮与音量图标 ---
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
        ImVec4 dangerCol = Utils::UIThemeUtils::getDangerColor();
        ImGui::PushStyleColor(ImGuiCol_Text, dangerCol);
        pushedTextColor = true;
    }
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    if ( ImGui::Button(icon, ImVec2(30, 0)) ) {
        muted   = !muted;
        changed = true;
    }
    ImGui::PopStyleVar();    // Pop ImGuiStyleVar_FramePadding
    ImGui::PopStyleColor();  // Pop ImGuiCol_Button
    if ( pushedTextColor ) {
        ImGui::PopStyleColor();  // Pop ImGuiCol_Text
    }
    if ( ImGui::IsItemHovered() ) {
        ImGui::SetTooltip("%s",
                          muted ? TR("ui.audio_manager.unmute").data()
                                : TR("ui.audio_manager.mute").data());
    }

    ImGui::SameLine();

    ImGui::AlignTextToFramePadding();

    // --- 2. 音量拉条 ---
    float sliderWidth = (m_type == TrackType::Main) ? -100.0f : -50.0f;
    ImGui::SetNextItemWidth(sliderWidth);
    if ( ImGui::SliderFloat("##Volume", &volume, 0.0f, 1.0f, "%.2f") ) {
        changed = true;
        // 如果拉动了进度条，自动解除静音（可选，但通常用户期望这样）
        if ( muted && volume > 0.0f ) {
            muted = false;
        }
    }

    // --- 3. L/R 静音按钮 (仅限主音轨) ---
    if ( m_type == TrackType::Main ) {
        auto& audio = Audio::AudioManager::instance();
        bool  muteL = audio.isMainMixerLeftMuted();
        bool  muteR = audio.isMainMixerRightMuted();

        ImGui::SameLine();
        if ( muteL ) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  Utils::UIThemeUtils::getDangerColor());
        }
        if ( ImGui::Button("L", ImVec2(22, 0)) ) {
            audio.setMainMixerLeftMute(!muteL);
        }
        if ( muteL ) {
            ImGui::PopStyleColor();
        }
        if ( ImGui::IsItemHovered() ) {
            ImGui::SetTooltip("%s", TR("ui.audio_manager.mute_l").data());
        }

        ImGui::SameLine(0, 2);

        if ( muteR ) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  Utils::UIThemeUtils::getDangerColor());
        }
        if ( ImGui::Button("R", ImVec2(22, 0)) ) {
            audio.setMainMixerRightMute(!muteR);
        }
        if ( muteR ) {
            ImGui::PopStyleColor();
        }
        if ( ImGui::IsItemHovered() ) {
            ImGui::SetTooltip("%s", TR("ui.audio_manager.mute_r").data());
        }
    }
}

void AudioTrackControllerUI::renderSpeedAndPitchSection(float& speed,
                                                        float& pitch,
                                                        bool&  changed)
{
    auto& audio = Audio::AudioManager::instance();

    ImGui::Separator();
    ImGui::Text("%s", TR("ui.audio_manager.speed_control").data());

    // 显示期望和实际倍率
    float actualSpeed = (float)audio.getActualPlaybackSpeed();
    ImGui::Text(TR("ui.audio_manager.speed_info").data(), speed, actualSpeed);

    // 预设按钮
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

    // 滑块
    if ( ImGui::SliderFloat("##SpeedSlider", &speed, 0.25f, 2.0f, "%.4fx") ) {
        changed = true;
    }

    // 拉伸质量选择
    ImGui::Text("%s", TR("ui.audio_manager.stretch_quality").data());
    const char* qualityNames[] = {
        TR("ui.audio_manager.quality_fast").data(),
        TR("ui.audio_manager.quality_balanced").data(),
        TR("ui.audio_manager.quality_finer").data(),
        TR("ui.audio_manager.quality_best").data()
    };

    int currentQuality = static_cast<int>(audio.getPlaybackQuality());
    ImGui::SetNextItemWidth(-1);
    if ( ImGui::Combo("##StretchQuality",
                      &currentQuality,
                      qualityNames,
                      IM_ARRAYSIZE(qualityNames)) ) {
        audio.setPlaybackQuality(
            static_cast<Audio::AudioManager::StretchQuality>(currentQuality));
        changed = true;
    }

    // --- 3.5 音高控制 ---
    ImGui::Separator();
    ImGui::Text("%s", TR("ui.audio_manager.pitch_control").data());

    // 预设按钮
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

    // 滑块
    if ( ImGui::SliderFloat(
             "##PitchSlider", &pitch, -24.0f, 24.0f, "%.1f st") ) {
        changed = true;
    }
}

void AudioTrackControllerUI::renderEffectPreviewSection()
{
    auto& audio = Audio::AudioManager::instance();

    ImGui::Separator();
    if ( ImGui::Button(TR("ui.audio_manager.play_preview").data()) ) {
        audio.playSoundEffect(m_trackId);
    }

    ImGui::SameLine();
    float       duration     = (float)audio.getSFXDuration(m_trackId);
    float       playbackTime = (float)audio.getSFXPlaybackTime(m_trackId);
    float       progress = (duration > 0.0f) ? (playbackTime / duration) : 0.0f;
    std::string progressText =
        fmt::format("{:.2f}s / {:.2f}s", playbackTime, duration);

    ImGui::ProgressBar(progress, ImVec2(-1, 0), progressText.c_str());
}

}  // namespace MMM::UI
