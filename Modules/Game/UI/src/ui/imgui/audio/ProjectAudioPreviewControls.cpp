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

/// @brief 使用当前子窗口裁剪域检测绝对定位控件。
/// @param minimum 控件左上角屏幕坐标。
/// @param extent 控件屏幕尺寸。
/// @param acceptExplicitPointerHit 是否由调用方保证当前控件位于最上层对象。
/// @return 鼠标位于当前可交互窗口中的控件矩形时返回 true。
/// @warning 每个可见试听按钮每帧调用，不得引入分配或阻塞操作。
bool isPreviewControlHovered(ImVec2 minimum, ImVec2 extent,
                             bool acceptExplicitPointerHit)
{
    if ( !acceptExplicitPointerHit &&
         !ImGui::IsWindowHovered(
             ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ) {
        return false;
    }
    return ImGui::IsMouseHoveringRect(
        minimum, { minimum.x + extent.x, minimum.y + extent.y }, true);
}

/// @brief 保留主题色相并确保方块内控件具有足够的不透明度。
/// @param color 当前主题颜色。
/// @param minimumAlpha 最低不透明度。
/// @return 可用于叠加在音频方块上的高对比度颜色。
ImVec4 ensureControlAlpha(ImVec4 color, float minimumAlpha)
{
    color.w = std::max(color.w, minimumAlpha);
    return color;
}

/// @brief 绘制一个采用全局圆角、边框和文字对齐的方形按钮外观。
/// @param minimum 按钮左上角屏幕坐标。
/// @param extent 按钮屏幕尺寸。
/// @param icon 按钮图标。
/// @param hovered 当前是否悬浮。
/// @param active 当前是否按下。
/// @warning UI 热路径：只向当前 ImGui DrawList 追加固定数量图元。
void drawPreviewButton(ImVec2 minimum, ImVec2 extent, const char* icon,
                       bool hovered, bool active)
{
    const auto& style = ImGui::GetStyle();
    ImVec4 fill = ImGui::GetStyleColorVec4(active    ? ImGuiCol_ButtonActive
                                           : hovered ? ImGuiCol_ButtonHovered
                                                     : ImGuiCol_Button);
    fill        = ensureControlAlpha(fill, 0.88F);
    const ImVec4 border =
        ensureControlAlpha(ImGui::GetStyleColorVec4(ImGuiCol_Border), 0.72F);
    const ImVec2 maximum{ minimum.x + extent.x, minimum.y + extent.y };

    auto* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        minimum, maximum, ImGui::GetColorU32(fill), style.FrameRounding);
    drawList->AddRect(minimum,
                      maximum,
                      ImGui::GetColorU32(border),
                      style.FrameRounding,
                      0,
                      std::max(1.0F, style.FrameBorderSize));

    const ImVec2 iconSize = ImGui::CalcTextSize(icon);
    const ImVec2 iconPosition{
        minimum.x +
            std::max(0.0F, extent.x - iconSize.x) * style.ButtonTextAlign.x,
        minimum.y +
            std::max(0.0F, extent.y - iconSize.y) * style.ButtonTextAlign.y,
    };
    drawList->AddText(iconPosition, ImGui::GetColorU32(ImGuiCol_Text), icon);
}

/// @brief 绘制方块内的试听进度条。
/// @param minimum 进度条左上角屏幕坐标。
/// @param extent 进度条屏幕尺寸。
/// @param progress 已裁切到 `[0, 1]` 的播放进度。
/// @warning UI 热路径：只向当前 ImGui DrawList 追加固定数量图元。
void drawPreviewProgress(ImVec2 minimum, ImVec2 extent, float progress)
{
    const auto&  style = ImGui::GetStyle();
    const ImVec2 maximum{ minimum.x + extent.x, minimum.y + extent.y };
    const ImVec2 filledMaximum{
        minimum.x + extent.x * std::clamp(progress, 0.0F, 1.0F),
        maximum.y,
    };
    const ImVec4 background =
        ensureControlAlpha(ImGui::GetStyleColorVec4(ImGuiCol_FrameBg), 0.82F);
    const ImVec4 filled = ensureControlAlpha(
        ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogram), 0.95F);
    const ImVec4 border =
        ensureControlAlpha(ImGui::GetStyleColorVec4(ImGuiCol_Border), 0.72F);

    auto* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        minimum, maximum, ImGui::GetColorU32(background), style.FrameRounding);
    if ( filledMaximum.x > minimum.x ) {
        drawList->AddRectFilled(minimum,
                                filledMaximum,
                                ImGui::GetColorU32(filled),
                                style.FrameRounding);
    }
    drawList->AddRect(minimum,
                      maximum,
                      ImGui::GetColorU32(border),
                      style.FrameRounding,
                      0,
                      std::max(1.0F, style.FrameBorderSize));
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
    const ProjectAudioPreviewControlsLayout& layout,
    bool                                     acceptExplicitPointerHit)
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

    ImGui::PushID(idScope);
    const ImVec2 progressExtent{ layout.width, layout.progressHeight };
    ImGui::SetCursorScreenPos(layout.topLeft);
    ImGui::InvisibleButton("##ProjectAudioPreviewProgress", progressExtent);
    accumulateLastItemState(result);
    const bool progressHovered =
        ImGui::IsItemHovered() ||
        isPreviewControlHovered(
            layout.topLeft, progressExtent, acceptExplicitPointerHit);
    result.hovered = result.hovered || progressHovered;
    drawPreviewProgress(layout.topLeft, progressExtent, progress);
    if ( progressHovered ) {
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

    const auto renderButton = [&](const char*               icon,
                                  const char*               hiddenId,
                                  const char*               tooltip,
                                  ProjectAudioPreviewAction action,
                                  std::size_t               index,
                                  bool triggerPreview = true) {
        const ImVec2 buttonPosition{
            buttonStartX + static_cast<float>(index) *
                               (layout.buttonSize + layout.buttonSpacing),
            buttonY,
        };
        ImGui::SetCursorScreenPos(buttonPosition);
        const std::string label = std::string(icon) + hiddenId;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
        const bool feedbackClicked =
            FeedbackButton(label.c_str(), buttonExtent);
        const bool manualHovered = isPreviewControlHovered(
            buttonPosition, buttonExtent, acceptExplicitPointerHit);
        const bool clicked =
            feedbackClicked ||
            (manualHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left));
        if ( clicked && triggerPreview ) {
            if ( !previewPoolKey.empty() ) {
                result.activated = controlProjectAudioPreview(project,
                                                              audioResourceId,
                                                              previewPoolKey,
                                                              action,
                                                              volumeFactor) ||
                                   result.activated;
            }
        }
        ImGui::PopStyleColor(4);
        const bool hovered = ImGui::IsItemHovered() || manualHovered;
        const bool active =
            ImGui::IsItemActive() ||
            (manualHovered && ImGui::IsMouseDown(ImGuiMouseButton_Left));
        accumulateLastItemState(result);
        result.hovered = result.hovered || hovered || active;
        drawPreviewButton(buttonPosition, buttonExtent, icon, hovered, active);
        if ( hovered ) {
            ImGui::SetTooltip("%s", tooltip);
        }
        return clicked;
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
        if ( renderButton(ICON_MMM_VOLUME_HIGH,
                          "##ProjectAudioPreviewVolume",
                          TR("ui.edit.sample_properties.volume").data(),
                          ProjectAudioPreviewAction::Stop,
                          3U,
                          false) ) {
            ImGui::OpenPopup("##ProjectAudioPreviewVolumePopup");
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
