#include "ui/imgui/manager/CollaborationView.h"

#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/CreatorIdentity.h"
#include "config/skin/translation/Translation.h"
#include "event/ui/UISettingsTabEvent.h"
#include "imgui.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "network/collaboration/CollaborationRoom.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/CollaborationEntryPolicy.h"
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

/// @brief 返回公网目录连接状态对应的本地化文本。
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

/// @brief 返回资源同步阶段对应的本地化文本。
const char* resourcePhaseText(
    Network::Collaboration::CollaborationResourceSyncPhase phase)
{
    using Phase = Network::Collaboration::CollaborationResourceSyncPhase;
    switch ( phase ) {
    case Phase::Preparing:
        return TR("ui.collaboration.resource.preparing").data();
    case Phase::WaitingManifest:
        return TR("ui.collaboration.resource.waiting_manifest").data();
    case Phase::ComparingCache:
        return TR("ui.collaboration.resource.comparing_cache").data();
    case Phase::Downloading:
        return TR("ui.collaboration.resource.downloading").data();
    case Phase::Verifying:
        return TR("ui.collaboration.resource.verifying").data();
    case Phase::Ready: return TR("ui.collaboration.resource.ready").data();
    case Phase::Error: return TR("ui.collaboration.resource.error").data();
    case Phase::Idle:
    default: return TR("ui.collaboration.resource.idle").data();
    }
}
}  // namespace

CollaborationView::CollaborationView(
    const std::string&                                         subViewName,
    std::shared_ptr<Network::Collaboration::CollaborationRoom> room)
    : ISubView(subViewName), m_room(std::move(room))
{
    if ( m_room ) {
        const auto& endpoint = m_room->serverEndpoint();
        setInputBuffer(m_serverAddress, endpoint.address);
        m_signalingPort = endpoint.signalingPort;
        m_useTls        = endpoint.useTls;
    } else {
        setInputBuffer(m_serverAddress, "xiang233.top");
    }
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
    auto&       engine        = Logic::EditorEngine::instance();
    auto        activeSession = engine.getActiveNonLogoSession();
    const auto* project       = engine.getCurrentProject();
    const bool  hasProject =
        sourceManager && sourceManager->hasActiveProjectUiState() && project;
    const bool hostReady = hasProject && activeSession &&
                           activeSession->getContext().currentBeatmap;
    if ( hostReady && !m_roomNameInitialized ) {
        const auto& metadata =
            activeSession->getContext().currentBeatmap->m_baseMapMetadata;
        const std::string_view roomName = !metadata.title.empty()
                                              ? std::string_view(metadata.title)
                                              : std::string_view(metadata.name);
        if ( !roomName.empty() ) setInputBuffer(m_roomName, roomName);
        m_roomNameInitialized = true;
    }

    ImGui::SeparatorText(TR("ui.collaboration.server").data());
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##CollaborationServerAddress",
                             TR("ui.collaboration.server_address_hint").data(),
                             m_serverAddress.data(),
                             m_serverAddress.size());
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputInt(TR("ui.collaboration.signaling_port").data(),
                    &m_signalingPort);
    m_signalingPort = std::clamp(m_signalingPort, 1, 65535);
    ImGui::Checkbox(TR("ui.collaboration.use_tls").data(), &m_useTls);
    if ( FeedbackButton(TR("ui.collaboration.apply_server").data(),
                        ImVec2(-1.0f, 0.0f)) ) {
        static_cast<void>(applyServerEndpoint());
    }
    ImGui::Text("%s: %s",
                TR("ui.collaboration.directory.status").data(),
                directoryStateText(m_room->directoryState()));
    if ( !m_room->directoryError().empty() ) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
                           "%s",
                           m_room->directoryError().c_str());
    }

    ImGui::Spacing();
    ImGui::SeparatorText(TR("ui.collaboration.host_room").data());
    ImGui::TextWrapped("%s", TR("ui.collaboration.host_desc").data());
    if ( !hasProject ) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.25f, 1.0f),
                           "%s",
                           TR("ui.collaboration.project_required").data());
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##CollaborationRoomName",
                             TR("ui.collaboration.room_name_hint").data(),
                             m_roomName.data(),
                             m_roomName.size());
    ImGui::BeginDisabled(
        !creatorValid || m_roomName[0] == '\0' ||
        !isCollaborationProjectRequirementSatisfied(true, hostReady));
    if ( FeedbackButton(TR("ui.collaboration.start_room").data(),
                        ImVec2(-1.0f, 0.0f)) ) {
        if ( applyServerEndpoint() ) {
            Network::Collaboration::CollaborationHostRoomConfig config;
            config.creator  = Config::AppConfig::instance()
                                  .getEditorSettings()
                                  .defaultCreator;
            config.roomName = m_roomName.data();
            config.endpoint = m_room->serverEndpoint();
            m_room->prepareHostResources(
                *project, *activeSession->getContext().currentBeatmap);
            static_cast<void>(m_room->startHost(std::move(config)));
            showLogWindow(sourceManager);
        }
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::SeparatorText(TR("ui.collaboration.online_rooms").data());
    ImGui::TextWrapped("%s", TR("ui.collaboration.join_desc").data());
    if ( FeedbackButton(TR("ui.collaboration.refresh_rooms").data()) ) {
        static_cast<void>(m_room->refreshDirectory());
    }
    const auto& rooms = m_room->directoryRooms();
    if ( rooms.empty() ) {
        ImGui::TextDisabled("%s", TR("ui.collaboration.no_rooms").data());
        return;
    }

    for ( const auto& room : rooms ) {
        ImGui::PushID(room.roomId.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("%s", room.roomName.c_str());
        ImGui::TextDisabled(TR("ui.collaboration.room_summary").data(),
                            room.hostCreator.c_str(),
                            static_cast<unsigned int>(room.participants),
                            static_cast<unsigned int>(room.capacity));
        const bool full = room.participants >= room.capacity;
        ImGui::BeginDisabled(
            !creatorValid || full ||
            !isCollaborationProjectRequirementSatisfied(false, hasProject));
        if ( FeedbackButton(full ? TR("ui.collaboration.room_full").data()
                                 : TR("ui.collaboration.join_now").data(),
                            ImVec2(-1.0f, 0.0f)) ) {
            if ( applyServerEndpoint() ) {
                Network::Collaboration::CollaborationJoinRoomConfig config;
                config.creator  = Config::AppConfig::instance()
                                      .getEditorSettings()
                                      .defaultCreator;
                config.roomId   = room.roomId;
                config.roomName = room.roomName;
                config.endpoint = m_room->serverEndpoint();
                config.resourceCacheRoot =
                    Config::AppPaths::configRootPath() / "collaboration-cache";
                static_cast<void>(m_room->join(std::move(config)));
                showLogWindow(sourceManager);
            }
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
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
                TR("ui.collaboration.room_name").data(),
                m_room->roomName().c_str());
    if ( !m_room->roomId().empty() ) {
        ImGui::Text("%s: %s",
                    TR("ui.collaboration.room_id").data(),
                    m_room->roomId().c_str());
    }
    const auto& endpoint = m_room->serverEndpoint();
    ImGui::Text("%s: %s:%u",
                TR("ui.collaboration.server").data(),
                endpoint.address.c_str(),
                static_cast<unsigned int>(endpoint.signalingPort));

    if ( !m_room->lastError().empty() ) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
                           "%s: %s",
                           TR("ui.collaboration.error").data(),
                           m_room->lastError().c_str());
    }

    const auto resource = m_room->resourceProgress();
    using ResourcePhase =
        Network::Collaboration::CollaborationResourceSyncPhase;
    const bool showProgress = resource.phase == ResourcePhase::Preparing ||
                              resource.phase == ResourcePhase::ComparingCache ||
                              resource.phase == ResourcePhase::Downloading ||
                              resource.phase == ResourcePhase::Verifying;
    if ( resource.phase != ResourcePhase::Idle ) {
        ImGui::Text("%s: %s",
                    TR("ui.collaboration.resource.status").data(),
                    resourcePhaseText(resource.phase));
        if ( showProgress ) {
            float fraction = 0.0F;
            if ( (resource.phase == ResourcePhase::Preparing ||
                  resource.phase == ResourcePhase::ComparingCache) &&
                 resource.totalFiles > 0 ) {
                const auto progressedFiles =
                    resource.phase == ResourcePhase::Preparing
                        ? resource.completedFiles
                        : resource.comparedFiles;
                fraction = static_cast<float>(progressedFiles) /
                           static_cast<float>(resource.totalFiles);
            } else if ( resource.totalBytes > 0 ) {
                fraction = static_cast<float>(resource.transferredBytes) /
                           static_cast<float>(resource.totalBytes);
            }
            fraction = std::clamp(fraction, 0.0F, 1.0F);
            const std::string overlay =
                std::to_string(resource.completedFiles) + "/" +
                std::to_string(resource.totalFiles);
            ImGui::ProgressBar(fraction, ImVec2(-1.0F, 0.0F), overlay.c_str());
            if ( !resource.currentFile.empty() ) {
                ImGui::TextWrapped("%s", resource.currentFile.c_str());
            }
        } else if ( resource.phase == ResourcePhase::Ready ) {
            ImGui::TextDisabled(
                TR("ui.collaboration.resource.summary").data(),
                static_cast<unsigned int>(resource.completedFiles),
                static_cast<unsigned int>(resource.cachedFiles));
        }
    }

    if ( FeedbackButton(TR("ui.collaboration.show_log").data()) ) {
        showLogWindow(sourceManager);
    }
    ImGui::SameLine();
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

bool CollaborationView::applyServerEndpoint()
{
    if ( !m_room || m_serverAddress[0] == '\0' || m_signalingPort <= 0 ||
         m_signalingPort > 65535 ) {
        return false;
    }
    Network::Collaboration::CollaborationServerEndpoint endpoint;
    endpoint.address       = m_serverAddress.data();
    endpoint.signalingPort = static_cast<std::uint16_t>(m_signalingPort);
    endpoint.useTls        = m_useTls;
    return m_room->setServerEndpoint(std::move(endpoint));
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
