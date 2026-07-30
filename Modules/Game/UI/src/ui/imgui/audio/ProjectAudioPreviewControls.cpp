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

bool controlProjectAudioPreview(const Project&            project,
                                std::string_view          audioResourceId,
                                ProjectAudioPreviewAction action,
                                float                     volumeFactor)
{
    if ( audioResourceId.empty() ) return false;

    const auto* resource = findAudioResource(project, audioResourceId);
    if ( !resource ) return false;

    auto&             audio = Audio::AudioManager::instance();
    const std::string resourceId(audioResourceId);
    if ( action == ProjectAudioPreviewAction::Pause ) {
        audio.pauseSoundEffect(resourceId);
        return true;
    }
    if ( action == ProjectAudioPreviewAction::Stop ) {
        audio.stopSoundEffect(resourceId);
        return true;
    }

    const auto absolutePath =
        project.m_projectRoot / Config::utf8ToPath(resource->m_path);
    audio.registerSoundEffect(
        resourceId, Config::pathToUtf8(absolutePath), resource->m_config);
    if ( audio.isSFXPaused(resourceId) ) {
        audio.resumeSoundEffect(resourceId);
        return true;
    }
    if ( !audio.ensureSoundEffectLoaded(resourceId) ) return false;

    audio.stopSoundEffect(resourceId);
    audio.playSoundEffect(
        resourceId,
        std::isfinite(volumeFactor) ? std::max(0.0F, volumeFactor) : 1.0F);
    return true;
}

ProjectAudioPreviewControlsResult renderProjectAudioPreviewControls(
    const char* idScope, const Project& project,
    std::string_view audioResourceId, float volumeFactor, ImVec2 topLeft,
    float buttonSize, float spacing)
{
    ProjectAudioPreviewControlsResult result;
    if ( !idScope || audioResourceId.empty() || buttonSize <= 0.0F ) {
        return result;
    }

    const ImVec2 previousCursor = ImGui::GetCursorScreenPos();
    const ImVec2 buttonExtent{ buttonSize, buttonSize };
    ImGui::PushID(idScope);

    const auto renderButton = [&](const char*               icon,
                                  const char*               hiddenId,
                                  const char*               tooltip,
                                  ProjectAudioPreviewAction action,
                                  std::size_t               index) {
        ImGui::SetCursorScreenPos(
            { topLeft.x + static_cast<float>(index) * (buttonSize + spacing),
              topLeft.y });
        const std::string label = std::string(icon) + hiddenId;
        if ( FeedbackButton(label.c_str(), buttonExtent) ) {
            result.activated =
                controlProjectAudioPreview(
                    project, audioResourceId, action, volumeFactor) ||
                result.activated;
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

    ImGui::PopID();
    ImGui::SetCursorScreenPos(previousCursor);
    return result;
}

}  // namespace MMM::UI
