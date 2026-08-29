#include "ui/imgui/manager/SettingsView.h"

#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "network/collaboration/CollaborationRoom.h"
#include "ui/UIManager.h"
#include "ui/utils/UIWidgetUtils.h"

#include "imgui.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 返回协作目录连接状态对应的本地化文本。
/// @param state 当前目录连接状态。
/// @return 可直接绘制的本地化文本。
const char* directoryStateText(
    Network::Collaboration::CollaborationDirectoryState state)
{
    using State = Network::Collaboration::CollaborationDirectoryState;
    switch ( state ) {
    case State::Connecting:
        return TR("ui.collaboration.directory.connecting").data();
    case State::Connected:
        return TR("ui.collaboration.directory.connected").data();
    case State::Error: return TR("ui.collaboration.directory.error").data();
    case State::Idle:
    default: return TR("ui.collaboration.directory.idle").data();
    }
}
}  // namespace

/// @brief 绘制多人协作设置页。
/// @warning UI 热路径：设置窗口打开且当前页为协作页时每帧执行；仅在
/// 用户确认应用时修改目录连接并持久化配置。
void SettingsView::drawCollaborationSettings()
{
    auto& appConfig = Config::AppConfig::instance();
    auto* room =
        m_sourceManager ? m_sourceManager->getCollaborationRoom() : nullptr;
    const bool roomActive = room && room->isActive();

    ImGui::TextUnformatted(TR("ui.settings.collaboration.server").data());
    ImGui::TextWrapped(
        "%s", TR("ui.settings.collaboration.server_description").data());
    ImGui::Spacing();

    m_contentVBox.clear();
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    auto& section = getSection(0);
    section.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
    std::size_t rowIndex = 0;
    const float maxLabelWidth =
        getCurrentTabLabelWidth(appConfig.getWindowContentScale());

    addSettingItem(section,
                   rowIndex,
                   TR("ui.collaboration.server_address").data(),
                   maxLabelWidth,
                   [this, roomActive](Clay_BoundingBox rect, bool) {
                       ImGui::BeginDisabled(roomActive);
                       ImGui::SetNextItemWidth(rect.width);
                       ImGui::InputTextWithHint(
                           "##SettingsCollaborationServerAddress",
                           TR("ui.collaboration.server_address_hint").data(),
                           m_collaborationServerAddressInputBuffer.data(),
                           m_collaborationServerAddressInputBuffer.size());
                       ImGui::EndDisabled();
                   });

    addSettingItem(section,
                   rowIndex,
                   TR("ui.collaboration.signaling_port").data(),
                   maxLabelWidth,
                   [this, roomActive](Clay_BoundingBox rect, bool) {
                       ImGui::BeginDisabled(roomActive);
                       ImGui::SetNextItemWidth(rect.width);
                       ImGui::InputInt("##SettingsCollaborationSignalingPort",
                                       &m_collaborationSignalingPortInput,
                                       0,
                                       0);
                       m_collaborationSignalingPortInput = std::clamp(
                           m_collaborationSignalingPortInput, 1, 65535);
                       ImGui::EndDisabled();
                   });

    addSettingItem(section,
                   rowIndex,
                   TR("ui.collaboration.use_tls").data(),
                   maxLabelWidth,
                   [this, roomActive](Clay_BoundingBox, bool) {
                       ImGui::BeginDisabled(roomActive);
                       FeedbackCheckbox("##SettingsCollaborationUseTls",
                                        &m_collaborationUseTlsInput);
                       ImGui::EndDisabled();
                   });

    addSettingItem(section,
                   rowIndex,
                   TR("ui.collaboration.directory.status").data(),
                   maxLabelWidth,
                   [room](Clay_BoundingBox, bool) {
                       ImGui::TextUnformatted(
                           room ? directoryStateText(room->directoryState())
                                : TR("ui.collaboration.unavailable").data());
                   });

    m_contentVBox.addLayout("CollaborationServerSettingsSection",
                            section,
                            Sizing::Grow(),
                            Sizing::Fit());
    const ImVec2 startPosition = ImGui::GetCursorScreenPos();
    const ImVec2 size          = m_contentVBox.renderInCurrent(
        startPosition, { ImGui::GetContentRegionAvail().x, 0.0F });
    ImGui::SetCursorScreenPos(
        { startPosition.x,
          startPosition.y + size.y + ImGui::GetStyle().ItemSpacing.y });

    ImGui::BeginDisabled(!room || roomActive);
    if ( FeedbackButton(TR("ui.collaboration.apply_server").data(),
                        ImVec2(-1.0F, 0.0F)) ) {
        Network::Collaboration::CollaborationServerEndpoint endpoint;
        endpoint.address = m_collaborationServerAddressInputBuffer.data();
        endpoint.signalingPort =
            static_cast<std::uint16_t>(m_collaborationSignalingPortInput);
        endpoint.useTls = m_collaborationUseTlsInput;

        auto&      settings         = appConfig.getEditorSettings();
        const auto previousSettings = settings.collaborationServer;
        const bool endpointApplied  = room->setServerEndpoint(endpoint);
        if ( endpointApplied ) {
            settings.collaborationServer.address       = endpoint.address;
            settings.collaborationServer.signalingPort = endpoint.signalingPort;
            settings.collaborationServer.useTls        = endpoint.useTls;
        }
        if ( endpointApplied && appConfig.save() ) {
            m_collaborationServerApplyState =
                CollaborationServerApplyState::Succeeded;
        } else {
            settings.collaborationServer = previousSettings;
            if ( endpointApplied ) {
                Network::Collaboration::CollaborationServerEndpoint previous;
                previous.address       = previousSettings.address;
                previous.signalingPort = previousSettings.signalingPort;
                previous.useTls        = previousSettings.useTls;
                static_cast<void>(room->setServerEndpoint(std::move(previous)));
            }
            m_collaborationServerApplyState =
                CollaborationServerApplyState::Failed;
        }
    }
    ImGui::EndDisabled();

    if ( roomActive ) {
        ImGui::TextWrapped(
            "%s", TR("ui.settings.collaboration.active_warning").data());
    } else if ( m_collaborationServerApplyState ==
                CollaborationServerApplyState::Succeeded ) {
        ImGui::TextColored(
            ImGui::GetStyleColorVec4(ImGuiCol_TextSelectedBg),
            "%s",
            TR("ui.settings.collaboration.apply_success").data());
    } else if ( m_collaborationServerApplyState ==
                CollaborationServerApplyState::Failed ) {
        ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                           "%s",
                           TR("ui.settings.collaboration.apply_failed").data());
    }

    if ( room && !room->directoryError().empty() ) {
        ImGui::TextWrapped("%s", room->directoryError().c_str());
    }
}

}  // namespace MMM::UI
