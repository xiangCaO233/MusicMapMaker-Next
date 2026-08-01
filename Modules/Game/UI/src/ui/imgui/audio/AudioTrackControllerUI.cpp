#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include "ui/UIManager.h"
#include "ui/utils/UIWidgetUtils.h"
#include <cfloat>
#include <cmath>

namespace MMM::UI
{

std::string AudioTrackControllerUI::makeViewName(const std::string& trackId)
{
    return "TrackController_" + trackId;
}

const char* AudioTrackControllerUI::trackTypeToWorkspaceName(TrackType type)
{
    return type == TrackType::Effect ? "Effect" : "Main";
}

AudioTrackControllerUI::TrackType
AudioTrackControllerUI::workspaceNameToTrackType(const std::string& name)
{
    return name == "Effect" ? TrackType::Effect : TrackType::Main;
}

AudioTrackControllerUI::AudioTrackControllerUI(const std::string& trackId,
                                               const std::string& trackName,
                                               TrackType          type)
    : IUIView(trackName)
    , m_trackId(trackId)
    , m_trackName(trackName)
    , m_type(type)
{
}

/// @brief 请求下一次显示时停靠到指定 Dock 节点。
/// @param dockId 目标 ImGui Dock 节点 ID，0 表示不改变停靠位置。
void AudioTrackControllerUI::requestDockTo(ImGuiID dockId)
{
    m_pendingDockId = dockId;
}

/// @brief 请求下一次更新时将音轨控制器窗口聚焦到前台。
void AudioTrackControllerUI::requestFocus()
{
    m_shouldFocusNextFrame = true;
}

void AudioTrackControllerUI::update(UIManager* sourceManager)
{
    if ( !m_isOpen ) {
        if ( m_type == TrackType::Effect ) {
            Audio::AudioManager::instance().pauseSoundEffect(m_trackId);
        }
        return;
    }

    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    const auto& layoutMetrics = getLayoutMetrics(dpiScale);

    if ( m_pendingDockId != 0 ) {
        ImGui::SetNextWindowDockID(m_pendingDockId, ImGuiCond_Always);
    }
    if ( m_shouldFocusNextFrame ) {
        ImGui::SetNextWindowFocus();
        m_shouldFocusNextFrame = false;
    }
    ImGui::SetNextWindowSizeConstraints(getMinWindowSize(dpiScale),
                                        ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_FirstUseEver);
    std::string windowTitle =
        m_trackName + "###" + AudioTrackControllerUI::makeViewName(m_trackId);
    const bool wasOpenBeforeBegin = m_isOpen;
    const bool opened = ImGui::Begin(windowTitle.c_str(), &m_isOpen);
    FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin, &m_isOpen);
    if ( opened ) {
        if ( sourceManager && sourceManager->isProjectTransitionInProgress() ) {
            Utils::renderProjectTransitionPlaceholder();
            ImGui::End();
            return;
        }

        auto& audio   = Audio::AudioManager::instance();
        auto& engine  = Logic::EditorEngine::instance();
        auto* project = engine.getCurrentProject();

        if ( m_pendingDockId != 0 && ImGui::IsWindowDocked() ) {
            m_pendingDockId = 0;
        }
        CLayWrapperCore::instance().makeCurrent(m_layoutCtx.context);
        float volume = 0.5f;
        float speed  = 1.0f;
        float pitch  = 0.0f;
        bool  muted  = false;

        AudioTrackConfig  editedConfig;
        AudioTrackConfig* config = nullptr;
        if ( project ) {
            for ( const auto& res : project->m_audioResources ) {
                if ( res.m_id == m_trackId ) {
                    editedConfig = res.m_config;
                    config       = &editedConfig;
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
                m_currentPreset = Audio::EQPreset::None;
            } else {
                volume = audio.getSFXPoolVolume(m_trackId);
                muted  = audio.getSFXPoolMute(m_trackId);
            }
        }

        bool changed = false;

        // --- Clay 布局构建 ---
        m_contentVBox.clear();
        m_contentVBox
            .setSpacing(
                static_cast<uint16_t>(std::ceil(layoutMetrics.contentSpacing)))
            .setPadding(
                static_cast<uint16_t>(std::ceil(layoutMetrics.contentPadding)),
                static_cast<uint16_t>(std::ceil(layoutMetrics.contentPadding)),
                static_cast<uint16_t>(std::ceil(layoutMetrics.contentPadding)),
                static_cast<uint16_t>(std::ceil(layoutMetrics.contentPadding)));
        size_t rowIndex = 0;

        float maxLabelW = layoutMetrics.labelWidth;

        buildVolumeSection(
            m_contentVBox, rowIndex, maxLabelW, volume, muted, changed);

        if ( m_type == TrackType::Main ) {
            float availWidgetW = ImGui::GetContentRegionAvail().x - maxLabelW -
                                 layoutMetrics.contentPadding * 2.0f -
                                 layoutMetrics.rowPaddingX * 2.0f -
                                 layoutMetrics.rowSpacing;
            buildSpeedAndPitchSection(m_contentVBox,
                                      rowIndex,
                                      maxLabelW,
                                      availWidgetW,
                                      speed,
                                      pitch,
                                      changed);
            buildAnalysisButtons(m_contentVBox, rowIndex, sourceManager);
        }

        if ( m_type == TrackType::Effect ) {
            buildEffectPreviewSection(m_contentVBox, rowIndex, maxLabelW);
        }

        // --- 执行 Clay 布局渲染 ---
        // 预留微量顶部空间，防止某些布局下盖住 Tab
        ImGui::Dummy(ImVec2(0, 2 * dpiScale));

        ImVec2 startPos = ImGui::GetCursorScreenPos();
        ImVec2 sz       = m_contentVBox.renderInCurrent(
            startPos, { ImGui::GetContentRegionAvail().x, 0 });
        ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

        // --- EQ 区域使用传统 ImGui 渲染（ImPlot 不适合 Clay） ---
        if ( m_type == TrackType::Main ) {
            AudioTrackConfig unavailableConfig;
            if ( !config ) ImGui::BeginDisabled();
            renderEQSection(config ? *config : unavailableConfig, changed);
            if ( !config ) ImGui::EndDisabled();
        }

        // --- 应用更改并持久化 ---
        if ( changed ) {
            if ( config ) {
                config->volume        = volume;
                config->muted         = muted;
                config->playbackSpeed = speed;
                config->playbackPitch = pitch;
            }

            if ( m_type == TrackType::Effect && !config ) {
                bool  isPermanent = true;
                auto& skinData    = Config::SkinManager::instance().getData();
                if ( skinData.audioPaths.count(m_trackId) == 0 ) {
                    isPermanent = false;
                }
                audio.setSFXPoolVolume(m_trackId, volume, isPermanent);
                audio.setSFXPoolMute(m_trackId, muted, isPermanent);
            }

            if ( config ) {
                engine.pushCommand(Logic::CmdUpdateAudioResourceConfig{
                    .id     = m_trackId,
                    .config = *config,
                });
            }
        }
    }
    ImGui::End();
}

}  // namespace MMM::UI
