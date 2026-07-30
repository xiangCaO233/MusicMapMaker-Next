#include "ui/imgui/audio/ProjectAudioPreviewControls.h"

#include "audio/AudioManager.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "mmm/project/Project.h"
#include "ui/Icons.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <string>

namespace MMM::UI
{
namespace
{

/// @brief 查找项目音频资源。
/// @param project 当前项目。
/// @param audioResourceId 音频资源 ID。
/// @return 找到时返回非拥有指针。
const AudioResource* findAudioResource(const Project&   project,
                                       std::string_view audioResourceId)
{
    const auto resource = std::ranges::find(
        project.m_audioResources, audioResourceId, &AudioResource::m_id);
    return resource == project.m_audioResources.end() ? nullptr : &*resource;
}

/// @brief 累积最后一个 ImGui 控件的悬浮和激活状态。
/// @param result 待更新的按钮组结果。
void accumulateLastItemState(ProjectAudioPreviewControlsResult& result)
{
    result.hovered =
        result.hovered || ImGui::IsItemHovered() || ImGui::IsItemActive();
}

}  // namespace

std::string makeProjectAudioPreviewPoolKey(std::string_view previewInstanceId)
{
    std::string key{ "__mmm_project_audio_preview__/" };
    key.append(previewInstanceId);
    return key;
}

bool controlProjectAudioPreview(const Project&            project,
                                std::string_view          audioResourceId,
                                const std::string&        previewPoolKey,
                                ProjectAudioPreviewAction action,
                                float                     volumeFactor)
{
    if ( audioResourceId.empty() || previewPoolKey.empty() ) return false;

    const auto* resource = findAudioResource(project, audioResourceId);
    if ( !resource ) return false;

    auto& audio = Audio::AudioManager::instance();
    if ( action == ProjectAudioPreviewAction::Pause ) {
        audio.pauseSoundEffect(previewPoolKey);
        return true;
    }
    if ( action == ProjectAudioPreviewAction::Stop ) {
        audio.stopSoundEffect(previewPoolKey);
        return true;
    }

    const auto absolutePath =
        project.m_projectRoot / Config::utf8ToPath(resource->m_path);
    audio.registerSoundEffect(
        previewPoolKey, Config::pathToUtf8(absolutePath), resource->m_config);
    if ( audio.isSFXPaused(previewPoolKey) ) {
        audio.resumeSoundEffect(previewPoolKey);
        return true;
    }
    if ( !audio.ensureSoundEffectLoaded(previewPoolKey) ) return false;

    audio.stopSoundEffect(previewPoolKey);
    audio.playSoundEffect(
        previewPoolKey,
        std::isfinite(volumeFactor) ? std::max(0.0F, volumeFactor) : 1.0F);
    return true;
}

ProjectAudioPreviewControlsResult renderProjectAudioPreviewControls(
    const char* idScope, const Project& project,
    std::string_view audioResourceId, const std::string& previewPoolKey,
    float volumeFactor, float* editableVolume,
    const ProjectAudioPreviewControlsLayout& layout)
{
    ProjectAudioPreviewControlsResult result;
    if ( !idScope || audioResourceId.empty() || layout.width <= 0.0F ||
         layout.buttonSize <= 0.0F || layout.progressHeight <= 0.0F ) {
        return result;
    }

    auto&      audio = Audio::AudioManager::instance();
    const bool loaded =
        !previewPoolKey.empty() && audio.isSoundEffectLoaded(previewPoolKey);
    const double duration = loaded ? audio.getSFXDuration(previewPoolKey) : 0.0;
    const double playbackTime =
        loaded ? audio.getSFXPlaybackTime(previewPoolKey) : 0.0;
    const float progress =
        duration > 0.0
            ? std::clamp(
                  static_cast<float>(playbackTime / duration), 0.0F, 1.0F)
            : 0.0F;

    const auto& style = ImGui::GetStyle();
    const float widestIcon =
        std::max({ ImGui::CalcTextSize(ICON_MMM_PLAY).x,
                   ImGui::CalcTextSize(ICON_MMM_PAUSE).x,
                   ImGui::CalcTextSize(ICON_MMM_STOP).x,
                   ImGui::CalcTextSize(ICON_MMM_VOLUME_HIGH).x });
    const float tallestIcon =
        std::max({ ImGui::CalcTextSize(ICON_MMM_PLAY).y,
                   ImGui::CalcTextSize(ICON_MMM_PAUSE).y,
                   ImGui::CalcTextSize(ICON_MMM_STOP).y,
                   ImGui::CalcTextSize(ICON_MMM_VOLUME_HIGH).y });
    const ImVec2 adaptivePadding{
        std::min(style.FramePadding.x,
                 std::max(0.0F, (layout.buttonSize - widestIcon) * 0.5F)),
        std::min(style.FramePadding.y,
                 std::max(0.0F, (layout.buttonSize - tallestIcon) * 0.5F)),
    };

    ImGui::SetCursorScreenPos(layout.topLeft);
    ImGui::ProgressBar(progress, { layout.width, layout.progressHeight }, "");
    accumulateLastItemState(result);
    if ( ImGui::IsItemHovered() ) {
        ImGui::SetTooltip("%.2f / %.2f s", playbackTime, duration);
    }

    const std::size_t buttonCount = editableVolume ? 4U : 3U;
    const float       buttonRowWidth =
        layout.buttonSize * static_cast<float>(buttonCount) +
        layout.buttonSpacing * static_cast<float>(buttonCount - 1U);
    const float buttonStartX =
        layout.topLeft.x +
        std::max(0.0F, (layout.width - buttonRowWidth) * 0.5F);
    const float buttonY =
        layout.topLeft.y + layout.progressHeight + layout.progressSpacing;
    const ImVec2 buttonExtent{ layout.buttonSize, layout.buttonSize };
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, adaptivePadding);
    ImGui::PushID(idScope);

    const auto renderButton = [&](const char*               icon,
                                  const char*               hiddenId,
                                  const char*               tooltip,
                                  ProjectAudioPreviewAction action,
                                  std::size_t               index) {
        ImGui::SetCursorScreenPos(
            { buttonStartX + static_cast<float>(index) *
                                 (layout.buttonSize + layout.buttonSpacing),
              buttonY });
        const std::string label = std::string(icon) + hiddenId;
        if ( FeedbackButton(label.c_str(), buttonExtent) ) {
            if ( !previewPoolKey.empty() ) {
                result.activated = controlProjectAudioPreview(project,
                                                              audioResourceId,
                                                              previewPoolKey,
                                                              action,
                                                              volumeFactor) ||
                                   result.activated;
            }
        }
        accumulateLastItemState(result);
        if ( ImGui::IsItemHovered() ) {
            ImGui::SetTooltip("%s", tooltip);
        }
    };

    renderButton(ICON_MMM_PLAY,
                 "##ProjectAudioPreviewPlay",
                 TR("ui.tools.bpm_measure.play").data(),
                 ProjectAudioPreviewAction::Play,
                 0U);
    renderButton(ICON_MMM_PAUSE,
                 "##ProjectAudioPreviewPause",
                 TR("ui.tools.bpm_measure.pause").data(),
                 ProjectAudioPreviewAction::Pause,
                 1U);
    renderButton(ICON_MMM_STOP,
                 "##ProjectAudioPreviewStop",
                 TR("ui.tools.bpm_measure.stop").data(),
                 ProjectAudioPreviewAction::Stop,
                 2U);

    if ( editableVolume ) {
        ImGui::SetCursorScreenPos(
            { buttonStartX + 3.0F * (layout.buttonSize + layout.buttonSpacing),
              buttonY });
        if ( FeedbackButton((std::string(ICON_MMM_VOLUME_HIGH) +
                             "##ProjectAudioPreviewVolume")
                                .c_str(),
                            buttonExtent) ) {
            ImGui::OpenPopup("##ProjectAudioPreviewVolumePopup");
        }
        accumulateLastItemState(result);
        if ( ImGui::IsItemHovered() ) {
            ImGui::SetTooltip("%s",
                              TR("ui.edit.sample_properties.volume").data());
        }

        if ( ImGui::BeginPopup("##ProjectAudioPreviewVolumePopup") ) {
            result.volumeEditorOpen = true;
            result.hovered = result.hovered || ImGui::IsWindowHovered();

            ImGui::TextUnformatted(
                TR("ui.edit.sample_properties.volume").data());
            if ( FeedbackButton("-25%##ProjectAudioPreviewVolumeDecrease") ) {
                *editableVolume      = std::max(0.0F, *editableVolume - 0.25F);
                result.volumeChanged = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(
                std::max(72.0F, ImGui::GetFontSize() * 5.5F));
            if ( ImGui::InputFloat("##ProjectAudioPreviewVolumeValue",
                                   editableVolume,
                                   0.0F,
                                   0.0F,
                                   "%.2f") ) {
                if ( std::isfinite(*editableVolume) ) {
                    *editableVolume = std::max(0.0F, *editableVolume);
                } else {
                    *editableVolume = 1.0F;
                }
            }
            if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                result.volumeChanged = true;
            }
            ImGui::SameLine();
            if ( FeedbackButton("+25%##ProjectAudioPreviewVolumeIncrease") ) {
                *editableVolume      = std::max(0.0F, *editableVolume + 0.25F);
                result.volumeChanged = true;
            }
            ImGui::EndPopup();
        }
    }

    ImGui::PopID();
    ImGui::PopStyleVar();
    return result;
}

}  // namespace MMM::UI
