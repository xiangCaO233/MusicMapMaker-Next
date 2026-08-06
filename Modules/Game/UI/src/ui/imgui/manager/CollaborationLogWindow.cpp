#include "ui/imgui/manager/CollaborationLogWindow.h"

#include "config/skin/translation/Translation.h"
#include "fmt/format.h"
#include "imgui.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "mmm/beatmap/BeatmapMutationObserver.h"
#include "network/collaboration/CollaborationRoom.h"
#include "ui/IUIView.h"

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
        m_room->setApplyBeatmapCallback(
            [this](std::shared_ptr<const ::MMM::BeatMap> beatmap,
                   ::MMM::BeatmapMutationFlags           flags) {
                auto session = m_boundSession.lock();
                if ( !session || !beatmap ) return;
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
                    }));
            });
    }
}

CollaborationLogWindow::~CollaborationLogWindow()
{
    if ( auto session = m_boundSession.lock() ) {
        session->setMutationObserver(nullptr);
    }
    if ( m_room ) m_room->setApplyBeatmapCallback(nullptr);
}

void CollaborationLogWindow::update(UIManager*)
{
    if ( !m_room ) return;
    updateSessionBinding();
    m_room->update();

    const bool roomActive = m_room->isActive();
    if ( roomActive && !m_wasRoomActive ) {
        m_windowVisible = true;
    }
    m_wasRoomActive = roomActive;
    if ( !m_windowVisible ) return;

    ImGui::SetNextWindowSize(ImVec2(620.0f, 320.0f), ImGuiCond_FirstUseEver);
    const std::string title =
        TR("title.collaboration_log").toString() + "###CollaborationLogWindow";
    const bool wasVisible = m_windowVisible;
    const bool drawWindow =
        ImGui::Begin(title.c_str(), &m_windowVisible, ImGuiWindowFlags_None);
    FeedbackCurrentWindowCloseButton(wasVisible, &m_windowVisible);
    if ( drawWindow ) {
        ImGui::Text("%s: %zu",
                    TR("ui.collaboration.log.entries").data(),
                    m_room->logs().size());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", TR("ui.collaboration.log.realtime").data());
        ImGui::Separator();

        if ( ImGui::BeginChild("##CollaborationLogEntries",
                               ImVec2(0.0f, 0.0f),
                               ImGuiChildFlags_None,
                               ImGuiWindowFlags_HorizontalScrollbar) ) {
            for ( const auto& entry : m_room->logs() ) {
                const std::string line = formatEntry(entry);
                if ( entry.type == Network::Collaboration::
                                       CollaborationLogEventType::Error ) {
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
    ImGui::End();
}

void CollaborationLogWindow::updateSessionBinding()
{
    auto bound = m_boundSession.lock();
    if ( !m_room->isActive() ) {
        if ( bound ) bound->setMutationObserver(nullptr);
        m_boundSession.reset();
        return;
    }
    if ( bound ) return;

    auto active = Logic::EditorEngine::instance().getActiveSession();
    if ( !active ) return;
    active->setMutationObserver(m_room);
    m_boundSession = active;
}

bool CollaborationLogWindow::isOpen() const
{
    return true;
}

void CollaborationLogWindow::setOpen(bool open)
{
    m_windowVisible = open;
}

void CollaborationLogWindow::show()
{
    m_windowVisible = true;
}

std::string CollaborationLogWindow::formatEntry(
    const Network::Collaboration::CollaborationLogEntry& entry) const
{
    const double seconds =
        static_cast<double>(entry.elapsedMilliseconds) / 1000.0;
    const std::string actor =
        entry.creator.empty()
            ? fmt::format("#{}", entry.peerId)
            : fmt::format("{} (#{})", entry.creator, entry.peerId);
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
