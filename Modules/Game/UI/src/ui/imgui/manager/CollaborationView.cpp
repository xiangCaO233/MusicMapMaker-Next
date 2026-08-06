#include "ui/imgui/manager/CollaborationView.h"

#include "config/AppConfig.h"
#include "config/CreatorIdentity.h"
#include "config/skin/translation/Translation.h"
#include "event/ui/UISettingsTabEvent.h"
#include "imgui.h"
#include "network/collaboration/CollaborationRoom.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/CollaborationLogWindow.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace MMM::UI
{
namespace
{
/// @brief 将固定字符串写入零结尾输入缓冲区。
template<std::size_t Size>
void setInputBuffer(std::array<char, Size>& buffer, std::string_view value)
{
    buffer.fill('\0');
    const auto copyLength = std::min(value.size(), Size - 1);
    std::copy_n(value.begin(), copyLength, buffer.begin());
}

/// @brief 返回房间状态对应的本地化文本。
const char* roomStateText(Network::Collaboration::CollaborationRoomState state)
{
    switch ( state ) {
    case Network::Collaboration::CollaborationRoomState::Hosting:
        return TR("ui.collaboration.state.hosting").data();
    case Network::Collaboration::CollaborationRoomState::Joining:
        return TR("ui.collaboration.state.joining").data();
    case Network::Collaboration::CollaborationRoomState::Connected:
        return TR("ui.collaboration.state.connected").data();
    case Network::Collaboration::CollaborationRoomState::Error:
        return TR("ui.collaboration.state.error").data();
    case Network::Collaboration::CollaborationRoomState::Idle:
    default: return TR("ui.collaboration.state.idle").data();
    }
}
}  // namespace

CollaborationView::CollaborationView(
    const std::string&                                         subViewName,
    std::shared_ptr<Network::Collaboration::CollaborationRoom> room)
    : ISubView(subViewName), m_room(std::move(room))
{
    setInputBuffer(m_hostAddress, "127.0.0.1");
}

void CollaborationView::onUpdate(LayoutContext&, UIManager* sourceManager)
{
    if ( !m_room ) {
        ImGui::TextDisabled("%s", TR("ui.collaboration.unavailable").data());
        return;
    }

    if ( m_room->isActive() ) {
        drawActiveRoom(sourceManager);
        return;
    }

    const bool creatorValid = drawIdentitySection(sourceManager);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    drawOfflineFlow(sourceManager, creatorValid);
}

ImVec2 CollaborationView::getMinContentSize(float dpiScale) const
{
    const float scale = std::max(1.0f, dpiScale);
    return ImVec2(std::ceil(340.0f * scale), std::ceil(420.0f * scale));
}

bool CollaborationView::drawIdentitySection(UIManager* sourceManager)
{
    const auto creator = Config::normalizeCreatorIdentity(
        Config::AppConfig::instance().getEditorSettings().defaultCreator);
    ImGui::TextUnformatted(TR("ui.collaboration.identity").data());
    ImGui::SameLine();
    if ( creator.empty() ) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                           "%s",
                           TR("ui.collaboration.creator_missing").data());
        ImGui::TextWrapped("%s",
                           TR("ui.collaboration.creator_required").data());
        if ( FeedbackButton(
                 TR("ui.collaboration.open_creator_settings").data()) &&
             sourceManager ) {
            sourceManager->openSettingsWindow(Event::SettingsTab::Software);
        }
        return false;
    }

    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextSelectedBg),
                       "%s",
                       creator.c_str());
    return true;
}

void CollaborationView::drawOfflineFlow(UIManager* sourceManager,
                                        bool       creatorValid)
{
    const bool hasProject =
        sourceManager && sourceManager->hasActiveProjectUiState();
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float spacing        = ImGui::GetStyle().ItemSpacing.x;
    const float buttonWidth    = (availableWidth - spacing) * 0.5f;

    if ( m_entryMode == EntryMode::Host ) {
        const ImVec4 active = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Button, active);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    }
    if ( FeedbackButton(TR("ui.collaboration.host_room").data(),
                        ImVec2(buttonWidth, 0.0f)) ) {
        m_entryMode = EntryMode::Host;
    }
    if ( m_entryMode == EntryMode::Host ) ImGui::PopStyleColor(3);

    ImGui::SameLine();
    if ( m_entryMode == EntryMode::Join ) {
        const ImVec4 active = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Button, active);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    }
    if ( FeedbackButton(TR("ui.collaboration.join_room").data(),
                        ImVec2(buttonWidth, 0.0f)) ) {
        m_entryMode = EntryMode::Join;
    }
    if ( m_entryMode == EntryMode::Join ) ImGui::PopStyleColor(3);

    ImGui::Spacing();
    if ( m_entryMode == EntryMode::Host ) {
        ImGui::TextWrapped("%s", TR("ui.collaboration.host_desc").data());
        if ( !hasProject ) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.25f, 1.0f),
                               "%s",
                               TR("ui.collaboration.project_required").data());
        }

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputInt(TR("ui.collaboration.port").data(), &m_port);
        m_port = std::clamp(m_port, 0, 65535);

        ImGui::BeginDisabled(!creatorValid || !hasProject);
        if ( FeedbackButton(TR("ui.collaboration.start_room").data(),
                            ImVec2(-1.0f, 0.0f)) ) {
            Network::Collaboration::CollaborationHostRoomConfig config;
            config.creator = Config::AppConfig::instance()
                                 .getEditorSettings()
                                 .defaultCreator;
            config.port    = static_cast<std::uint16_t>(m_port);
            if ( m_room->startHost(std::move(config)) ) {
                m_port = m_room->port();
            }
            showLogWindow(sourceManager);
        }
        ImGui::EndDisabled();
    } else {
        ImGui::TextWrapped("%s", TR("ui.collaboration.join_desc").data());
        if ( !hasProject ) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.25f, 1.0f),
                               "%s",
                               TR("ui.collaboration.project_required").data());
        }

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint(
            "##CollaborationHostAddress",
            TR("ui.collaboration.host_address_hint").data(),
            m_hostAddress.data(),
            m_hostAddress.size());
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputInt(TR("ui.collaboration.port").data(), &m_port);
        m_port = std::clamp(m_port, 1, 65535);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##CollaborationRoomCode",
                                 TR("ui.collaboration.room_code_hint").data(),
                                 m_roomCode.data(),
                                 m_roomCode.size());

        const bool inputValid =
            m_hostAddress[0] != '\0' && m_roomCode[0] != '\0' && m_port > 0;
        ImGui::BeginDisabled(!creatorValid || !hasProject || !inputValid);
        if ( FeedbackButton(TR("ui.collaboration.connect").data(),
                            ImVec2(-1.0f, 0.0f)) ) {
            Network::Collaboration::CollaborationJoinRoomConfig config;
            config.creator  = Config::AppConfig::instance()
                                  .getEditorSettings()
                                  .defaultCreator;
            config.host     = m_hostAddress.data();
            config.port     = static_cast<std::uint16_t>(m_port);
            config.roomCode = m_roomCode.data();
            static_cast<void>(m_room->join(std::move(config)));
            showLogWindow(sourceManager);
        }
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted(TR("ui.collaboration.how_to_connect").data());
    ImGui::BulletText("%s", TR("ui.collaboration.step.host").data());
    ImGui::BulletText("%s", TR("ui.collaboration.step.local").data());
    ImGui::BulletText("%s", TR("ui.collaboration.step.lan").data());
    ImGui::TextWrapped("%s", TR("ui.collaboration.internet_notice").data());
}

void CollaborationView::drawActiveRoom(UIManager* sourceManager)
{
    ImGui::TextUnformatted(TR("ui.collaboration.status").data());
    ImGui::SameLine();
    ImGui::TextUnformatted(roomStateText(m_room->state()));

    ImGui::Text("%s: %s",
                TR("ui.collaboration.role").data(),
                m_room->isHost() ? TR("ui.collaboration.role.host").data()
                                 : TR("ui.collaboration.role.guest").data());
    ImGui::Text("%s: %llu",
                TR("ui.collaboration.peer_id").data(),
                static_cast<unsigned long long>(m_room->localPeerId()));
    ImGui::Text("%s: %s",
                TR("ui.collaboration.room_code").data(),
                m_room->roomCode().c_str());
    ImGui::Text("%s: %u",
                TR("ui.collaboration.port").data(),
                static_cast<unsigned int>(m_room->port()));

    if ( !m_room->lastError().empty() ) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
                           "%s: %s",
                           TR("ui.collaboration.error").data(),
                           m_room->lastError().c_str());
    }

    const bool hostRoom = m_room->isHost();
    if ( hostRoom ) {
        const std::string localConnection =
            "127.0.0.1:" + std::to_string(m_room->port()) + "  " +
            m_room->roomCode();
        ImGui::TextWrapped("%s", TR("ui.collaboration.share_hint").data());
        ImGui::TextWrapped("%s", localConnection.c_str());
        if ( FeedbackButton(TR("ui.collaboration.copy_local_info").data()) ) {
            ImGui::SetClipboardText(localConnection.c_str());
        }
    }

    if ( hostRoom ) ImGui::SameLine();
    if ( FeedbackButton(TR("ui.collaboration.show_log").data()) ) {
        showLogWindow(sourceManager);
    }
    if ( FeedbackButton(TR("ui.collaboration.disconnect").data()) ) {
        m_room->disconnect();
        return;
    }

    ImGui::Spacing();
    ImGui::SeparatorText(TR("ui.collaboration.participants").data());
    for ( const auto& [peerId, creator] : m_room->participants() ) {
        ImGui::BulletText("%s  (#%llu)%s",
                          creator.c_str(),
                          static_cast<unsigned long long>(peerId),
                          peerId == m_room->localPeerId()
                              ? TR("ui.collaboration.you_suffix").data()
                              : "");
    }
}

void CollaborationView::showLogWindow(UIManager* sourceManager) const
{
    if ( !sourceManager ) return;
    if ( auto* logWindow = sourceManager->getView<CollaborationLogWindow>(
             "CollaborationLogWindow") ) {
        logWindow->show();
    }
}
}  // namespace MMM::UI
