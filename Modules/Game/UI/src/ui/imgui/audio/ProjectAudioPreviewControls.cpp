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

/// @brief 构造不会与项目资源、皮肤音效或 HitEffect 冲突的试听池标识。
/// @param previewInstanceId 独立试听实例 ID。
/// @return AudioManager 专用试听池标识。
std::string makePreviewPoolKey(std::string_view previewInstanceId)
{
    std::string key{ "__mmm_project_audio_preview__/" };
    key.append(previewInstanceId);
    return key;
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
                                std::string_view          previewInstanceId,
                                ProjectAudioPreviewAction action,
                                float                     volumeFactor)
{
    if ( audioResourceId.empty() || previewInstanceId.empty() ) return false;

    const auto* resource = findAudioResource(project, audioResourceId);
    if ( !resource ) return false;

    auto&             audio          = Audio::AudioManager::instance();
    const std::string previewPoolKey = makePreviewPoolKey(previewInstanceId);
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
    std::string_view audioResourceId, std::string_view previewInstanceId,
    float volumeFactor, ImVec2 topLeft, float buttonSize, float spacing)
{
    ProjectAudioPreviewControlsResult result;
    if ( !idScope || audioResourceId.empty() || previewInstanceId.empty() ||
         buttonSize <= 0.0F ) {
        return result;
    }

    const auto& style      = ImGui::GetStyle();
    const float widestIcon = std::max({ ImGui::CalcTextSize(ICON_MMM_PLAY).x,
                                        ImGui::CalcTextSize(ICON_MMM_PAUSE).x,
                                        ImGui::CalcTextSize(ICON_MMM_STOP).x });
    const float tallestIcon =
        std::max({ ImGui::CalcTextSize(ICON_MMM_PLAY).y,
                   ImGui::CalcTextSize(ICON_MMM_PAUSE).y,
                   ImGui::CalcTextSize(ICON_MMM_STOP).y });
    const ImVec2 adaptivePadding{
        std::min(style.FramePadding.x,
                 std::max(0.0F, (buttonSize - widestIcon) * 0.5F)),
        std::min(style.FramePadding.y,
                 std::max(0.0F, (buttonSize - tallestIcon) * 0.5F)),
    };

    const ImVec2 buttonExtent{ buttonSize, buttonSize };
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, adaptivePadding);
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
            result.activated = controlProjectAudioPreview(project,
                                                          audioResourceId,
                                                          previewInstanceId,
                                                          action,
                                                          volumeFactor) ||
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
    ImGui::PopStyleVar();
    return result;
}

}  // namespace MMM::UI
