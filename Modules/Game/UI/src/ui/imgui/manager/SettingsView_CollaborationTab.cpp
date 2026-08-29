#include "ui/imgui/manager/SettingsView.h"

#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "network/collaboration/CollaborationRoom.h"
#include "ui/UIManager.h"
#include "ui/utils/UIWidgetUtils.h"

#include "imgui.h"
#include "imgui_internal.h"

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

    m_contentVBox.clear();
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    std::size_t rowIndex     = 0;
    std::size_t sectionIndex = 0;
    const float maxLabelWidth =
        getCurrentTabLabelWidth(appConfig.getWindowContentScale());

    const char* sectionLabel =
        TR_CACHE("ui.settings.collaboration.server").data();
    std::string   headerId = "COLLAB_S" + std::to_string(sectionIndex) + "_R" +
                             std::to_string(rowIndex) + "_H_" + sectionLabel;
    const ImGuiID headerStorageId = ImGui::GetID(headerId.c_str());
    const bool    sectionOpen =
        ImGui::GetStateStorage()->GetInt(headerStorageId, 1) != 0;

    auto& headerRow = getRow(rowIndex++);
    headerRow.setPadding(0, 0, 0, 0).setSpacing(0);
    const float headerHeight = ImGui::GetFrameHeight();
    headerRow.addElement(
        (headerId + "_el").c_str(),
        Sizing::Grow(),
        Sizing::Fixed(headerHeight),
        [sectionLabel, headerStorageId](Clay_BoundingBox rect, bool) {
            ImGui::SetCursorScreenPos({ rect.x, rect.y });
            const ImVec4 headerColor =
                ImGui::GetStyle().Colors[ImGuiCol_Header];
            ImGui::PushStyleColor(ImGuiCol_Header, headerColor);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                  { headerColor.x + 0.05F,
                                    headerColor.y + 0.05F,
                                    headerColor.z + 0.05F,
                                    headerColor.w + 0.1F });
            ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                  { headerColor.x + 0.1F,
                                    headerColor.y + 0.1F,
                                    headerColor.z + 0.1F,
                                    headerColor.w + 0.15F });

            ImGuiWindow* window    = ImGui::GetCurrentWindow();
            const float  savedMaxX = window->WorkRect.Max.x;
            window->WorkRect.Max.x = rect.x + rect.width;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                ImVec2(0.0F, 0.0F));
            const bool nowOpen = ImGui::TreeNodeEx(
                reinterpret_cast<void*>(
                    static_cast<std::intptr_t>(headerStorageId)),
                ImGuiTreeNodeFlags_CollapsingHeader |
                    ImGuiTreeNodeFlags_DefaultOpen,
                "%s",
                sectionLabel);
            ImGui::PopStyleVar();
            window->WorkRect.Max.x = savedMaxX;
            ImGui::GetStateStorage()->SetInt(headerStorageId, nowOpen ? 1 : 0);
            ImGui::PopStyleColor(3);
        });
    m_contentVBox.addLayout((headerId + "_layout").c_str(),
                            headerRow,
                            Sizing::Grow(),
                            Sizing::Fixed(headerHeight));

    if ( sectionOpen ) {
        auto& section = getSection(sectionIndex++);
        section.setDecorated(true).setSpacing(6).setPadding(8, 8, 8, 8);

        auto& descriptionRow = getRow(rowIndex++);
        descriptionRow.setPadding(8, 8, 4, 4);
        const float descriptionHeight =
            ImGui::GetTextLineHeightWithSpacing() + 8.0F;
        descriptionRow.addElement(
            "CollaborationServerDescription",
            Sizing::Grow(),
            Sizing::Fixed(descriptionHeight),
            [](Clay_BoundingBox rect, bool) {
                ImGui::SetCursorScreenPos({ rect.x, rect.y });
                ImGui::PushTextWrapPos(rect.x + rect.width);
                ImGui::TextUnformatted(
                    TR("ui.settings.collaboration.server_description").data());
                ImGui::PopTextWrapPos();
            });
        section.addLayout("CollaborationServerDescriptionRow",
                          descriptionRow,
                          Sizing::Grow(),
                          Sizing::Fit());

        addSettingItem(
            section,
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

        addSettingItem(
            section,
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
                m_collaborationSignalingPortInput =
                    std::clamp(m_collaborationSignalingPortInput, 1, 65535);
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
                               room
                                   ? directoryStateText(room->directoryState())
                                   : TR("ui.collaboration.unavailable").data());
                       });

        m_contentVBox.addLayout("CollaborationServerSettingsSection",
                                section,
                                Sizing::Grow(),
                                Sizing::Fit());
    }

    const ImVec2 startPosition = ImGui::GetCursorScreenPos();
    const ImVec2 contentSize   = m_contentVBox.renderInCurrent(
        startPosition, { ImGui::GetContentRegionAvail().x, 0.0F });
    ImGui::SetCursorScreenPos(
        { startPosition.x, startPosition.y + contentSize.y + 4.0F });

    if ( sectionOpen ) {
        const float actionInset = 8.0F;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + actionInset);
        ImGui::BeginDisabled(!room || roomActive);
        if ( FeedbackButton(TR("ui.collaboration.apply_server").data(),
                            ImVec2(-actionInset, 0.0F)) ) {
            Network::Collaboration::CollaborationServerEndpoint endpoint;
            endpoint.address = m_collaborationServerAddressInputBuffer.data();
            endpoint.signalingPort =
                static_cast<std::uint16_t>(m_collaborationSignalingPortInput);
            endpoint.useTls = m_collaborationUseTlsInput;

            auto&      settings         = appConfig.getEditorSettings();
            const auto previousSettings = settings.collaborationServer;
            const bool endpointApplied  = room->setServerEndpoint(endpoint);
            if ( endpointApplied ) {
                settings.collaborationServer.address = endpoint.address;
                settings.collaborationServer.signalingPort =
                    endpoint.signalingPort;
                settings.collaborationServer.useTls = endpoint.useTls;
            }
            if ( endpointApplied && appConfig.save() ) {
                m_collaborationServerApplyState =
                    CollaborationServerApplyState::Succeeded;
            } else {
                settings.collaborationServer = previousSettings;
                if ( endpointApplied ) {
                    Network::Collaboration::CollaborationServerEndpoint
                        previous;
                    previous.address       = previousSettings.address;
                    previous.signalingPort = previousSettings.signalingPort;
                    previous.useTls        = previousSettings.useTls;
                    static_cast<void>(
                        room->setServerEndpoint(std::move(previous)));
                }
                m_collaborationServerApplyState =
                    CollaborationServerApplyState::Failed;
            }
        }
        ImGui::EndDisabled();

        const char* feedbackText = nullptr;
        ImVec4 feedbackColor = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        if ( roomActive ) {
            feedbackText =
                TR("ui.settings.collaboration.active_warning").data();
        } else if ( m_collaborationServerApplyState ==
                    CollaborationServerApplyState::Succeeded ) {
            feedbackText = TR("ui.settings.collaboration.apply_success").data();
            feedbackColor = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        } else if ( m_collaborationServerApplyState ==
                    CollaborationServerApplyState::Failed ) {
            feedbackText  = TR("ui.settings.collaboration.apply_failed").data();
            feedbackColor = ImVec4(1.0F, 0.45F, 0.35F, 1.0F);
        }

        const std::string* directoryError =
            room && !room->directoryError().empty() ? &room->directoryError()
                                                    : nullptr;
        if ( feedbackText || directoryError ) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + actionInset);
            const ImVec2 cardMin   = ImGui::GetCursorScreenPos();
            const float  cardWidth = ImGui::GetContentRegionAvail().x;
            const char*  displayText =
                directoryError ? directoryError->c_str() : feedbackText;
            const float cardHeight =
                ImGui::GetTextLineHeightWithSpacing() * 2.0F + 16.0F;
            ImVec4 cardColor = feedbackColor;
            cardColor.w      = 0.12F;
            ImGui::GetWindowDrawList()->AddRectFilled(
                cardMin,
                { cardMin.x + cardWidth, cardMin.y + cardHeight },
                ImGui::ColorConvertFloat4ToU32(cardColor),
                ImGui::GetStyle().FrameRounding);
            ImGui::GetWindowDrawList()->AddRect(
                cardMin,
                { cardMin.x + cardWidth, cardMin.y + cardHeight },
                ImGui::ColorConvertFloat4ToU32(feedbackColor),
                ImGui::GetStyle().FrameRounding);
            ImGui::SetCursorScreenPos({ cardMin.x + 12.0F, cardMin.y + 8.0F });
            ImGui::PushTextWrapPos(cardMin.x + cardWidth - 12.0F);
            ImGui::TextColored(feedbackColor, "%s", displayText);
            ImGui::PopTextWrapPos();
            ImGui::SetCursorScreenPos({ cardMin.x, cardMin.y + cardHeight });
        }
    }
}

}  // namespace MMM::UI
