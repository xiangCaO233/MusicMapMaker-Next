#include "audio/AudioManager.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "ui/Icons.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"

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
        auto dangerCol = Config::SkinManager::instance().getColor("ui.danger");
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            ImVec4(dangerCol.r, dangerCol.g, dangerCol.b, dangerCol.a));
        pushedTextColor = true;
    }
    auto btnTransCol =
        Config::SkinManager::instance().getColor("ui.button.transparent");
    ImGui::PushStyleColor(
        ImGuiCol_Button,
        ImVec4(btnTransCol.r, btnTransCol.g, btnTransCol.b, btnTransCol.a));
    if ( ImGui::Button(icon, ImVec2(30, 0)) ) {
        muted   = !muted;
        changed = true;
    }
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
    ImGui::SetNextItemWidth(-50);
    if ( ImGui::SliderFloat("##Volume", &volume, 0.0f, 1.0f, "%.2f") ) {
        changed = true;
        // 如果拉动了进度条，自动解除静音（可选，但通常用户期望这样）
        if ( muted && volume > 0.0f ) {
            muted = false;
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
