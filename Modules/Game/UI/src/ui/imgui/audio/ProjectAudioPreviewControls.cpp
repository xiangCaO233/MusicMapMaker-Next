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
/// @warning UI 热路径：线性扫描当前项目音频资源，不得在此访问文件系统。
const AudioResource* findAudioResource(const Project&   project,
                                       std::string_view audioResourceId)
{
    // ranges 投影直接比较稳定 ID，避免复制 AudioResource 或临时名称。
    const auto resource = std::ranges::find(
        project.m_audioResources, audioResourceId, &AudioResource::m_id);
    return resource == project.m_audioResources.end() ? nullptr : &*resource;
}

/// @brief 累积最后一个 ImGui 控件的悬浮和激活状态。
/// @param result 待更新的按钮组结果。
/// @warning UI 热路径：只读取上一控件的 ImGui 状态。
void accumulateLastItemState(ProjectAudioPreviewControlsResult& result)
{
    // 激活态也视为悬浮组，保证拖动指针离开按钮时上层仍保留交互。
    result.hovered =
        result.hovered || ImGui::IsItemHovered() || ImGui::IsItemActive();
}

/// @brief 使用当前子窗口裁剪域检测绝对定位控件。
/// @param minimum 控件左上角屏幕坐标。
/// @param extent 控件屏幕尺寸。
/// @param acceptExplicitPointerHit 是否由调用方保证当前控件位于最上层对象。
/// @return 鼠标位于当前可交互窗口中的控件矩形时返回 true。
///
/// 绝对定位按钮可能与 ImGui 常规布局的命中结果不同，因此先做矩形检测，再由
/// 调用方声明的最上层保证或当前窗口悬浮状态确认交互归属。
/// @warning 每个可见试听按钮每帧调用，不得引入分配或阻塞操作。
bool isPreviewControlHovered(ImVec2 minimum, ImVec2 extent,
                             bool acceptExplicitPointerHit)
{
    // 坐标均为屏幕空间，与 layout.topLeft 和 DrawList 几何保持一致。
    const ImVec2 mousePosition = ImGui::GetIO().MousePos;
    const bool   inside        = mousePosition.x >= minimum.x &&
                                 mousePosition.x <= minimum.x + extent.x &&
                                 mousePosition.y >= minimum.y &&
                                 mousePosition.y <= minimum.y + extent.y;
    if ( !inside ) {
        // 早退避免为明显不相关控件查询窗口遮挡状态。
        return false;
    }
    // 显式命中只用于调用方已完成层级判定的场景。
    return acceptExplicitPointerHit ||
           ImGui::IsWindowHovered(
               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
}

/// @brief 保留主题色相并确保方块内控件具有足够的不透明度。
/// @param color 当前主题颜色。
/// @param minimumAlpha 最低不透明度。
/// @return 可用于叠加在音频方块上的高对比度颜色。
/// @warning UI 热路径：只修改颜色副本的 alpha 分量。
ImVec4 ensureControlAlpha(ImVec4 color, float minimumAlpha)
{
    // 保留主题 RGB 和更高的原始 alpha，只补足最低可读性。
    color.w = std::max(color.w, minimumAlpha);
    return color;
}

/// @brief 绘制一个采用全局圆角、边框和文字对齐的方形按钮外观。
/// @param minimum 按钮左上角屏幕坐标。
/// @param extent 按钮屏幕尺寸。
/// @param icon 按钮图标。
/// @param hovered 当前是否悬浮。
/// @param active 当前是否按下。
///
/// 交互由透明 FeedbackButton 提供，本函数只复现主题按钮的填充、边框和图标，
/// 使绝对定位控件仍能获得统一反馈而不依赖 ImGui 默认游标布局。
/// @warning UI 热路径：只向当前 ImGui DrawList 追加固定数量图元。
void drawPreviewButton(ImVec2 minimum, ImVec2 extent, const char* icon,
                       bool hovered, bool active)
{
    // 颜色优先级与普通按钮一致：按下、悬浮、默认。
    const auto& style = ImGui::GetStyle();
    ImVec4 fill = ImGui::GetStyleColorVec4(active    ? ImGuiCol_ButtonActive
                                           : hovered ? ImGuiCol_ButtonHovered
                                                     : ImGuiCol_Button);
    fill        = ensureControlAlpha(fill, 0.88F);
    // 边框提升最低 alpha，避免叠在波形或封面上时丢失轮廓。
    const ImVec4 border =
        ensureControlAlpha(ImGui::GetStyleColorVec4(ImGuiCol_Border), 0.72F);
    const ImVec2 maximum{ minimum.x + extent.x, minimum.y + extent.y };

    // 绘制到当前子窗口列表，自动继承其裁剪矩形。
    auto* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        minimum, maximum, ImGui::GetColorU32(fill), style.FrameRounding);
    drawList->AddRect(minimum,
                      maximum,
                      ImGui::GetColorU32(border),
                      style.FrameRounding,
                      0,
                      std::max(1.0F, style.FrameBorderSize));

    // 图标位置沿用主题 ButtonTextAlign，而不是假定几何中心。
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
///
/// 背景、填充和边框分别使用主题 FrameBg、PlotHistogram 和 Border 色，并提升
/// 最低 alpha 以覆盖音频方块背景。填充宽度仍在函数内防御性钳制。
/// @warning UI 热路径：只向当前 ImGui DrawList 追加固定数量图元。
void drawPreviewProgress(ImVec2 minimum, ImVec2 extent, float progress)
{
    // maximum 和 filledMaximum 都使用屏幕坐标，直接供 DrawList 绘制。
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
    // 零进度不提交退化填充矩形，只保留背景和边框。
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

/// @brief 为独立试听实例构造不会与业务音效冲突的池键。
/// @param previewInstanceId 调用方分配的稳定试听实例标识。
/// @return 带项目音频试听保留前缀的 AudioManager 池键。
std::string makeProjectAudioPreviewPoolKey(std::string_view previewInstanceId)
{
    // 保留前缀把工具试听与皮肤音效、谱面音效的 ID 空间隔离。
    std::string key{ "__mmm_project_audio_preview__/" };
    key.append(previewInstanceId);
    return key;
}

/// @brief 执行项目音频试听的播放、暂停或停止操作。
/// @param project 当前项目，用于解析资源路径和配置。
/// @param audioResourceId 目标音频资源稳定 ID。
/// @param previewPoolKey 当前试听实例的独立音效池键。
/// @param action 本次试听动作。
/// @param volumeFactor 播放时叠加的非负音量倍率。
/// @return 找到资源且 AudioManager 接受操作时返回 true。
///
/// 暂停和停止只作用于既有池；播放会以项目绝对路径注册资源，优先恢复暂停实例，
/// 否则确保解码资源可用后从头播放。资源配置随注册传入音频层。
/// @warning 用户触发的低频路径：ensureSoundEffectLoaded 可能触发资源准备；不得
/// 从每帧无条件调用。
bool controlProjectAudioPreview(const Project&            project,
                                std::string_view          audioResourceId,
                                const std::string&        previewPoolKey,
                                ProjectAudioPreviewAction action,
                                float                     volumeFactor)
{
    // 空资源 ID 或池键无法建立稳定的试听目标。
    if ( audioResourceId.empty() || previewPoolKey.empty() ) return false;

    const auto* resource = findAudioResource(project, audioResourceId);
    // 资源可能在 UI 绘制后被删除，找不到时不操作旧池。
    if ( !resource ) return false;

    auto& audio = Audio::AudioManager::instance();
    if ( action == ProjectAudioPreviewAction::Pause ) {
        // Pause 保留当前位置，下一次 Play 可以通过 resume 恢复。
        audio.pauseSoundEffect(previewPoolKey);
        return true;
    }
    if ( action == ProjectAudioPreviewAction::Stop ) {
        // Stop 重置本试听池的播放状态但不移除注册资源。
        audio.stopSoundEffect(previewPoolKey);
        return true;
    }

    // 项目记录相对路径，注册前以项目根目录解析为本机绝对目标。
    const auto absolutePath =
        project.m_projectRoot / Config::utf8ToPath(resource->m_path);
    audio.registerSoundEffect(
        previewPoolKey, Config::pathToUtf8(absolutePath), resource->m_config);
    if ( audio.isSFXPaused(previewPoolKey) ) {
        // 已暂停实例不重新加载或从头开始，保持用户预期的继续播放。
        audio.resumeSoundEffect(previewPoolKey);
        return true;
    }
    // 首次播放只有在音频层确认资源可用后才继续。
    if ( !audio.ensureSoundEffectLoaded(previewPoolKey) ) return false;

    // 非暂停播放总是先停止旧实例，保证从时间零开始试听。
    audio.stopSoundEffect(previewPoolKey);
    audio.playSoundEffect(
        previewPoolKey,
        std::isfinite(volumeFactor) ? std::max(0.0F, volumeFactor) : 1.0F);
    return true;
}

/// @brief 在给定屏幕矩形内绘制项目音频试听按钮、进度和可选音量编辑器。
/// @param idScope 调用方提供的 ImGui ID 作用域。
/// @param project 当前项目。
/// @param audioResourceId 目标音频资源 ID。
/// @param previewPoolKey 独立试听池键。
/// @param volumeFactor 当前播放音量倍率。
/// @param editableVolume 可选可编辑音量；为空时不显示音量按钮。
/// @param layout 绝对定位控件的尺寸与间距。
/// @param acceptExplicitPointerHit 调用方是否已保证控件位于最上层。
/// @return 本帧激活、悬浮和音量编辑状态。
///
/// 控件使用透明 FeedbackButton 获取统一交互反馈，再通过 DrawList 自绘外观。
/// 结果对象让上层决定拖拽互斥和配置持久化，本函数不保存 UI 状态引用。
///
/// 布局契约：
/// - topLeft、width、buttonSize 和各间距均使用屏幕像素；
/// - 进度条占据第一行完整宽度，不响应跳转；
/// - 按钮行在宽度内居中，音量按钮按 editableVolume 是否存在决定；
/// - 图标内边距根据最大图标和按钮尺寸自适应；
/// - Popup 使用当前 idScope，多个资源的音量编辑器互不冲突。
///
/// 返回状态契约：
/// - activated 表示播放控制被音频层接受；
/// - hovered 同时覆盖进度、按钮激活态和音量 Popup；
/// - volumeEditorOpen 表示当前资源的 Popup 正在显示；
/// - volumeChanged 只在步进点击或文本编辑提交后置位；
/// - 函数不把 editableVolume 的改变写回 Project。
/// - 播放进度来自 AudioManager 当前试听池的时间与时长；
/// - 池未加载或时长非正时进度稳定显示为零；
/// - volumeFactor 只在新播放动作发生时传给音频层；
/// - 暂停和停止动作不解释或修改 editableVolume；
/// - 所有颜色均来自当前 ImGui 主题并保持最低可见 alpha。
/// @warning UI 热路径：每个可见音频方块每帧调用；只允许固定数量控件和查询。
ProjectAudioPreviewControlsResult renderProjectAudioPreviewControls(
    const char* idScope, const Project& project,
    std::string_view audioResourceId, const std::string& previewPoolKey,
    float volumeFactor, float* editableVolume,
    const ProjectAudioPreviewControlsLayout& layout,
    bool                                     acceptExplicitPointerHit)
{
    ProjectAudioPreviewControlsResult result;
    // 无效 ID、资源或非正布局尺寸不创建 ImGui 控件。
    if ( !idScope || audioResourceId.empty() || layout.width <= 0.0F ||
         layout.buttonSize <= 0.0F || layout.progressHeight <= 0.0F ) {
        return result;
    }

    // 播放状态只从独立试听池读取，不触碰项目主音轨播放状态。
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

    // 以四种可能图标的最大尺寸计算内边距，切换按钮内容时不会跳动。
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
    // 小按钮空间不足时把内边距压到零，但不产生负值。
    const ImVec2 adaptivePadding{
        std::min(style.FramePadding.x,
                 std::max(0.0F, (layout.buttonSize - widestIcon) * 0.5F)),
        std::min(style.FramePadding.y,
                 std::max(0.0F, (layout.buttonSize - tallestIcon) * 0.5F)),
    };

    // 所有隐藏标签都在调用方作用域内，允许同一窗口渲染多个资源方块。
    ImGui::PushID(idScope);
    const ImVec2 progressExtent{ layout.width, layout.progressHeight };
    ImGui::SetCursorScreenPos(layout.topLeft);
    ImGui::InvisibleButton("##ProjectAudioPreviewProgress", progressExtent);
    // 进度条本身不改变播放位置，只提供 Tooltip 和组悬浮状态。
    accumulateLastItemState(result);
    const bool progressHovered =
        ImGui::IsItemHovered() ||
        isPreviewControlHovered(
            layout.topLeft, progressExtent, acceptExplicitPointerHit);
    result.hovered = result.hovered || progressHovered;
    drawPreviewProgress(layout.topLeft, progressExtent, progress);
    if ( progressHovered ) {
        // 时间提示保留两位小数，避免常驻文字占用方块空间。
        ImGui::SetTooltip("%.2f / %.2f s", playbackTime, duration);
    }

    // 音量可编辑时追加第四按钮，否则播放控制行保持三个按钮居中。
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

    /// 绘制单个绝对定位试听按钮并可选触发音频动作。
    /// 返回本帧点击状态，供音量按钮打开独立 Popup。
    const auto renderButton = [&](const char*               icon,
                                  const char*               hiddenId,
                                  const char*               tooltip,
                                  ProjectAudioPreviewAction action,
                                  std::size_t               index,
                                  bool triggerPreview = true) {
        // Lambda 交互约定：
        // - FeedbackButton 提供键鼠行为和统一反馈；
        // - manualHovered 补足绝对定位或上层显式命中的情况；
        // - 鼠标释放才构成手工点击，按住状态只用于自绘 active；
        // - triggerPreview 为 false 时只向调用方返回点击；
        // - 音频操作失败不会置 activated，但按钮点击仍按 true 返回；
        // - Tooltip 只在最终合并的 hovered 状态下显示。
        const ImVec2 buttonPosition{
            buttonStartX + static_cast<float>(index) *
                               (layout.buttonSize + layout.buttonSpacing),
            buttonY,
        };
        ImGui::SetCursorScreenPos(buttonPosition);
        // 图标参与可见标签，hiddenId 保证同作用域内动作 ID 唯一。
        const std::string label = std::string(icon) + hiddenId;
        // 隐藏 ImGui 默认外观，但保留 FeedbackButton 的行为与反馈声效。
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
            // 空池键仍允许按钮呈现，但不会向 AudioManager 提交动作。
            if ( !previewPoolKey.empty() ) {
                result.activated = controlProjectAudioPreview(project,
                                                              audioResourceId,
                                                              previewPoolKey,
                                                              action,
                                                              volumeFactor) ||
                                   result.activated;
            }
        }
        // 在自绘前恢复主题色，使 drawPreviewButton 读取真实按钮配色。
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
    // 暂停与停止保持独立按钮，不根据当前播放状态替换图标或 ID。
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
        // 音量按钮只打开编辑 Popup，不把 Stop 枚举实际提交给音频层。
        if ( renderButton(ICON_MMM_VOLUME_HIGH,
                          "##ProjectAudioPreviewVolume",
                          TR("ui.edit.sample_properties.volume").data(),
                          ProjectAudioPreviewAction::Stop,
                          3U,
                          false) ) {
            ImGui::OpenPopup("##ProjectAudioPreviewVolumePopup");
        }

        if ( ImGui::BeginPopup("##ProjectAudioPreviewVolumePopup") ) {
            // Popup 可见时向上层报告占用，防止父方块误启动拖拽。
            result.volumeEditorOpen = true;
            result.hovered = result.hovered || ImGui::IsWindowHovered();

            ImGui::TextUnformatted(
                TR("ui.edit.sample_properties.volume").data());
            if ( FeedbackButton("-25%##ProjectAudioPreviewVolumeDecrease") ) {
                // 音量允许超过 1.0，仅下限钳制为静音。
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
                    // 手工输入保留任意非负倍率，负值统一归零。
                    *editableVolume = std::max(0.0F, *editableVolume);
                } else {
                    // NaN 或无穷回退到单位倍率，避免传播到音频播放接口。
                    *editableVolume = 1.0F;
                }
            }
            if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                // 文本编辑完成才请求持久化，输入过程不重复写项目配置。
                result.volumeChanged = true;
            }
            ImGui::SameLine();
            if ( FeedbackButton("+25%##ProjectAudioPreviewVolumeIncrease") ) {
                // 步进按钮立即构成一次完整编辑，直接置变更标志。
                *editableVolume      = std::max(0.0F, *editableVolume + 0.25F);
                result.volumeChanged = true;
            }
            ImGui::EndPopup();
        }
    }

    // 恢复 ID 和 FramePadding 栈，避免影响同一方块的后续内容。
    ImGui::PopID();
    ImGui::PopStyleVar();
    return result;
}

}  // namespace MMM::UI
