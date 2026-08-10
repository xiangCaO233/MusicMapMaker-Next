#include "ui/imgui/manager/CollaborationLogWindow.h"

#include "config/skin/translation/Translation.h"
#include "fmt/format.h"
#include "imgui.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/beatmap/BeatmapMutationObserver.h"
#include "network/collaboration/CollaborationRoom.h"
#include "ui/IUIView.h"
#include "ui/imgui/manager/CollaborationEntryPolicy.h"

#include <algorithm>
#include <string>

namespace MMM::UI
{
CollaborationLogWindow::CollaborationLogWindow(
    const std::string&                                         name,
    std::shared_ptr<Network::Collaboration::CollaborationRoom> room)
    : IUIView(name), m_room(std::move(room))
{
    if ( m_room ) {
        m_room->setApplyBeatmapCallback([this](
                                            std::shared_ptr<::MMM::BeatMap>
                                                                        beatmap,
                                            ::MMM::BeatmapMutationFlags flags) {
            auto session = m_boundSession.lock();
            if ( !beatmap ) return;
            if ( !session && !m_room->isHost() ) {
                auto&             engine = Logic::EditorEngine::instance();
                const std::string displayName =
                    beatmap->m_baseMapMetadata.name.empty()
                        ? TR("title.collaboration_manager").toString()
                        : beatmap->m_baseMapMetadata.name;
                static_cast<void>(
                    engine.createSession(beatmap, displayName, false));
                session = engine.getActiveSession();
                if ( !session ) return;
                session->setCollaborationOfflineReadOnly(
                    m_room->state() !=
                    Network::Collaboration::CollaborationRoomState::Connected);
                session->setMutationObserver(m_room, false);
                m_room->onBeatmapSynchronized(*beatmap);
                m_boundSession        = session;
                m_boundSessionIsGuest = true;
                bindPendingResources();
                return;
            }
            if ( !session ) return;
            session->pushCommand(
                Logic::LogicCommand(Logic::CmdReplaceBeatmapData{
                    .sourceBeatmap  = std::move(beatmap),
                    .replaceObjects = hasBeatmapMutationFlag(
                        flags, ::MMM::BeatmapMutationFlags::Objects),
                    .replaceTimelines = hasBeatmapMutationFlag(
                        flags, ::MMM::BeatmapMutationFlags::Timelines),
                    .replaceMetadata = hasBeatmapMutationFlag(
                        flags, ::MMM::BeatmapMutationFlags::Metadata),
                    .replaceAudioSamples = hasBeatmapMutationFlag(
                        flags, ::MMM::BeatmapMutationFlags::AudioSamples),
                    .notifyMutationObserver = false,
                    .authoritativeRemote    = true,
                }));
        });
        m_room->setResourceBundleCallback(
            [this](Network::Collaboration::CollaborationResourceBundle bundle) {
                m_pendingResourceBundle = std::make_shared<
                    Network::Collaboration::CollaborationResourceBundle>(
                    std::move(bundle));
                bindPendingResources();
            });
    }
}

CollaborationLogWindow::~CollaborationLogWindow()
{
    if ( auto session = m_boundSession.lock() ) {
        session->setMutationObserver(nullptr);
        if ( m_boundSessionIsGuest ) {
            session->setCollaborationOfflineReadOnly(true);
        }
    }
    if ( m_guestProjectGateHeld ) {
        Logic::ProjectController::instance()
            .setLocalProjectOpeningBlockedByCollaboration(false);
    }
    if ( m_room ) {
        m_room->setApplyBeatmapCallback(nullptr);
        m_room->setResourceBundleCallback(nullptr);
    }
}

void CollaborationLogWindow::update(UIManager*)
{
    if ( !m_room ) return;
    m_room->update();
    updateSessionBinding();
}

void CollaborationLogWindow::renderInline()
{
    if ( !m_room ) return;

    ImGui::Text("%s: %zu",
                TR("ui.collaboration.log.entries").data(),
                m_room->logs().size());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", TR("ui.collaboration.log.realtime").data());
    ImGui::Separator();

    const float logHeight = ImGui::GetTextLineHeightWithSpacing() * 10.0F +
                            ImGui::GetStyle().FramePadding.y * 2.0F;
    if ( ImGui::BeginChild("##CollaborationLogEntries",
                           ImVec2(0.0f, logHeight),
                           ImGuiChildFlags_Borders,
                           ImGuiWindowFlags_HorizontalScrollbar) ) {
        for ( const auto& entry : m_room->logs() ) {
            const std::string line = formatEntry(entry);
            if ( entry.type ==
                 Network::Collaboration::CollaborationLogEventType::Error ) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s", line.c_str());
            } else {
                ImGui::TextUnformatted(line.c_str());
            }
        }
        if ( m_room->logs().size() > m_lastLogCount ) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    m_lastLogCount = m_room->logs().size();
}

void CollaborationLogWindow::updateSessionBinding()
{
    auto       bound = m_boundSession.lock();
    const auto state = m_room->state();
    const bool guestConnectionBlocksProjects =
        !m_room->isHost() &&
        (state == Network::Collaboration::CollaborationRoomState::Joining ||
         state ==
             Network::Collaboration::CollaborationRoomState::AwaitingApproval ||
         state == Network::Collaboration::CollaborationRoomState::Connected);
    if ( guestConnectionBlocksProjects ) {
        Logic::ProjectController::instance()
            .setLocalProjectOpeningBlockedByCollaboration(true);
        m_guestProjectGateHeld = true;
    } else if ( m_guestProjectGateHeld ) {
        Logic::ProjectController::instance()
            .setLocalProjectOpeningBlockedByCollaboration(false);
        m_guestProjectGateHeld = false;
    }

    if ( !m_room->isActive() ) {
        if ( bound ) {
            bound->setMutationObserver(nullptr);
            if ( m_boundSessionIsGuest ) {
                bound->setCollaborationOfflineReadOnly(true);
            }
        }
        m_boundSession.reset();
        m_boundSessionIsGuest = false;
        m_pendingResourceBundle.reset();
        m_hostResourceProject = nullptr;
        m_hostResourceBeatmap = nullptr;
        return;
    }
    if ( bound ) {
        if ( m_boundSessionIsGuest ) {
            bound->setCollaborationOfflineReadOnly(
                state !=
                Network::Collaboration::CollaborationRoomState::Connected);
        }
        refreshHostResources();
        return;
    }

    if ( !mayBindExistingSessionForCollaboration(m_room->isHost()) ) return;

    auto active = Logic::EditorEngine::instance().getActiveNonLogoSession();
    if ( !active ) return;
    active->setMutationObserver(m_room, m_room->isHost());
    m_boundSession        = active;
    m_boundSessionIsGuest = false;
    refreshHostResources();
    bindPendingResources();
}

void CollaborationLogWindow::refreshHostResources()
{
    if ( !m_room->isHost() ||
         m_room->state() !=
             Network::Collaboration::CollaborationRoomState::Hosting ) {
        return;
    }

    auto session = m_boundSession.lock();
    if ( !session || !session->getContext().currentBeatmap ) return;
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    auto* beatmap = session->getContext().currentBeatmap.get();
    if ( !project || (project == m_hostResourceProject &&
                      beatmap == m_hostResourceBeatmap) ) {
        return;
    }

    m_hostResourceProject = project;
    m_hostResourceBeatmap = beatmap;
    m_room->prepareHostResources(*project, *beatmap);
}

void CollaborationLogWindow::bindPendingResources()
{
    if ( !m_pendingResourceBundle ) return;
    auto session = m_boundSession.lock();
    if ( !session ) return;
    session->pushCommand(Logic::LogicCommand{
        Logic::CmdSetCollaborationResources{
            .project   = m_pendingResourceBundle->project,
            .pathRemap = std::move(m_pendingResourceBundle->pathRemap),
        },
    });
    m_pendingResourceBundle.reset();
}

bool CollaborationLogWindow::isOpen() const
{
    return true;
}

void CollaborationLogWindow::setOpen(bool open)
{
    (void)open;
}

std::string CollaborationLogWindow::formatEntry(
    const Network::Collaboration::CollaborationLogEntry& entry) const
{
    const double seconds =
        static_cast<double>(entry.elapsedMilliseconds) / 1000.0;
    const std::string stableLabel = entry.participantId.empty()
                                        ? fmt::format("#{}", entry.peerId)
                                        : entry.participantId.substr(0, 8);
    const std::string actor =
        entry.creator.empty()
            ? stableLabel
            : fmt::format("{} ({})", entry.creator, stableLabel);
    const char* formatKey = "ui.collaboration.log.error_fmt";
    switch ( entry.type ) {
    case Network::Collaboration::CollaborationLogEventType::RoomStarted:
        formatKey = "ui.collaboration.log.room_started_fmt";
        break;
    case Network::Collaboration::CollaborationLogEventType::SignalingConnected:
        formatKey = "ui.collaboration.log.signaling_fmt";
        break;
    case Network::Collaboration::CollaborationLogEventType::ParticipantJoined:
        formatKey = "ui.collaboration.log.joined_fmt";
        break;
    case Network::Collaboration::CollaborationLogEventType::ParticipantLeft:
        formatKey = "ui.collaboration.log.left_fmt";
        break;
    case Network::Collaboration::CollaborationLogEventType::OperationCommitted:
        formatKey = "ui.collaboration.log.operation_fmt";
        break;
    case Network::Collaboration::CollaborationLogEventType::ResourceManifest:
        formatKey = "ui.collaboration.log.resource_manifest_fmt";
        break;
    case Network::Collaboration::CollaborationLogEventType::ResourceCompleted:
        formatKey = "ui.collaboration.log.resource_completed_fmt";
        break;
    case Network::Collaboration::CollaborationLogEventType::Disconnected:
        formatKey = "ui.collaboration.log.disconnected_fmt";
        break;
    case Network::Collaboration::CollaborationLogEventType::Error:
        formatKey = "ui.collaboration.log.error_fmt";
        break;
    }
    const std::string message =
        fmt::format(fmt::runtime(TR(formatKey).data()), actor, entry.detail);
    return fmt::format("+{:07.3f}s  {}", seconds, message);
}
}  // namespace MMM::UI
