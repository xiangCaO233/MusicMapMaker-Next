#include "ui/imgui/manager/CollaborationView.h"

#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/CreatorIdentity.h"
#include "config/skin/translation/Translation.h"
#include "event/ui/UISettingsTabEvent.h"
#include "imgui.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "network/collaboration/CollaborationRoom.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/CollaborationEntryPolicy.h"
#include "ui/imgui/manager/CollaborationLogWindow.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

namespace MMM::UI
{
struct CollaborationView::PendingGuestJoin {
    /// @brief 已冻结的访客连接配置，避免等待关闭期间读取变化中的 UI 输入。
    Network::Collaboration::CollaborationJoinRoomConfig config;
    /// @brief 开始请求关闭本机状态的单调时间点。
    std::chrono::steady_clock::time_point closeRequestedAt;
};

namespace
{
/// @brief 判断编辑器是否仍存在非欢迎页谱面会话。
/// @warning UI 低频协作入口路径：最多遍历当前少量 Session 快照。
[[nodiscard]] bool hasNonLogoBeatmapSession()
{
    const auto entries = Logic::EditorEngine::instance().getSessionEntries();
    return std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
        return !entry.isLogoPlaceholder;
    });
}
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
    case Network::Collaboration::CollaborationRoomState::AwaitingApproval:
        return TR("ui.collaboration.state.awaiting_approval").data();
    case Network::Collaboration::CollaborationRoomState::Connected:
        return TR("ui.collaboration.state.connected").data();
    case Network::Collaboration::CollaborationRoomState::Error:
        return TR("ui.collaboration.state.error").data();
    case Network::Collaboration::CollaborationRoomState::Idle:
    default: return TR("ui.collaboration.state.idle").data();
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

/// @brief 绘制房间信息表中的一行左侧标签。
/// @param label 本地化标签。
/// @warning UI 热路径：只切换表格列并提交一段禁用色文本。
void drawRoomInfoLabel(const char* label)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
}

/// @brief 绘制当前访客从房主收到的只读权限明细。
/// @param room 当前协作房间。
/// @warning UI 热路径：固定绘制六个内存权限位，不执行网络发送。
void drawLocalPermissionSummary(
    const Network::Collaboration::CollaborationRoom& room)
{
    const auto permissions = room.localPermissions();
    const auto drawPermission =
        [permissions](
            const char*                                     label,
            Network::Collaboration::CollaborationPermission permission) {
            const bool enabled =
                Network::Collaboration::hasCollaborationPermission(permissions,
                                                                   permission);
            ImGui::TextDisabled("%s  %s", enabled ? "[+]" : "[-]", label);
        };
    drawPermission(TR("ui.collaboration.permissions.edit").data(),
                   Network::Collaboration::CollaborationPermission::Edit);
    drawPermission(TR("ui.collaboration.permissions.objects").data(),
                   Network::Collaboration::CollaborationPermission::Objects);
    drawPermission(TR("ui.collaboration.permissions.timelines").data(),
                   Network::Collaboration::CollaborationPermission::Timelines);
    drawPermission(TR("ui.collaboration.permissions.metadata").data(),
                   Network::Collaboration::CollaborationPermission::Metadata);
    drawPermission(
        TR("ui.collaboration.permissions.audio_samples").data(),
        Network::Collaboration::CollaborationPermission::AudioSamples);
    drawPermission(
        TR("ui.collaboration.permissions.annotations").data(),
        Network::Collaboration::CollaborationPermission::Annotations);
}

/// @brief 绘制一名协作成员的表格行和可选跟随按钮。
/// @param room 当前协作房间。
/// @param peerId 成员 PeerId。
/// @param creator 成员 Creator 展示名。
/// @warning UI 热路径：成员表可见时每帧最多调用 8 次；只绘制内存状态，
/// 不执行网络发送或文件系统访问。
void drawParticipantRow(
    Network::Collaboration::CollaborationRoom&         room,
    Network::Collaboration::PeerId                     peerId,
    const Network::Collaboration::ParticipantIdentity& identity)
{
    const bool local     = peerId == room.localPeerId();
    const bool following = room.followedPeerId() == peerId;

    ImGui::PushID(identity.participantId.c_str());
    ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(identity.creator.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%.*s)", 8, identity.participantId.c_str());
    const auto permission = room.participantPermissions().find(peerId);
    const auto permissionMask =
        permission == room.participantPermissions().end() ? 0U
                                                          : permission->second;
    if ( !Network::Collaboration::hasCollaborationPermission(
             permissionMask,
             Network::Collaboration::CollaborationPermission::Edit) ) {
        ImGui::SameLine();
        ImGui::TextDisabled(
            "%s", TR("ui.collaboration.permissions.read_only").data());
    }

    ImGui::TableSetColumnIndex(1);
    if ( local ) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", TR("ui.collaboration.you_suffix").data());
    } else {
        const char* actionLabel =
            following ? TR("ui.collaboration.stop_following").data()
                      : TR("ui.collaboration.follow").data();
        if ( FeedbackSmallButton(actionLabel) ) {
            static_cast<void>(room.setFollowedPeer(following ? 0 : peerId));
        }
        if ( room.isHost() ) {
            ImGui::SameLine();
            if ( FeedbackSmallButton(
                     TR("ui.collaboration.permissions.manage").data()) ) {
                FeedbackOpenPopup("CollaborationPermissionsPopup");
            }
            if ( ImGui::BeginPopup("CollaborationPermissionsPopup") ) {
                ImGui::TextUnformatted(identity.creator.c_str());
                ImGui::Separator();

                auto       updatedPermissions = permissionMask;
                bool       permissionsChanged = false;
                const auto drawPermission =
                    [&](const char* label,
                        Network::Collaboration::CollaborationPermission
                            target) {
                        bool enabled =
                            Network::Collaboration::hasCollaborationPermission(
                                updatedPermissions, target);
                        if ( FeedbackCheckbox(label, &enabled) ) {
                            const auto bit =
                                static_cast<Network::Collaboration::
                                                CollaborationPermissionMask>(
                                    target);
                            if ( enabled ) {
                                updatedPermissions |= bit;
                            } else {
                                updatedPermissions &= ~bit;
                            }
                            permissionsChanged = true;
                        }
                    };

                drawPermission(
                    TR("ui.collaboration.permissions.edit").data(),
                    Network::Collaboration::CollaborationPermission::Edit);
                const bool mayEdit =
                    Network::Collaboration::hasCollaborationPermission(
                        updatedPermissions,
                        Network::Collaboration::CollaborationPermission::Edit);
                ImGui::BeginDisabled(!mayEdit);
                drawPermission(
                    TR("ui.collaboration.permissions.objects").data(),
                    Network::Collaboration::CollaborationPermission::Objects);
                drawPermission(
                    TR("ui.collaboration.permissions.timelines").data(),
                    Network::Collaboration::CollaborationPermission::Timelines);
                drawPermission(
                    TR("ui.collaboration.permissions.metadata").data(),
                    Network::Collaboration::CollaborationPermission::Metadata);
                drawPermission(
                    TR("ui.collaboration.permissions.audio_samples").data(),
                    Network::Collaboration::CollaborationPermission::
                        AudioSamples);
                drawPermission(
                    TR("ui.collaboration.permissions.annotations").data(),
                    Network::Collaboration::CollaborationPermission::
                        Annotations);
                ImGui::EndDisabled();

                if ( permissionsChanged ) {
                    static_cast<void>(room.setParticipantPermissions(
                        peerId, updatedPermissions));
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if ( FeedbackSmallButton(
                     TR("ui.collaboration.remove_participant").data()) ) {
                static_cast<void>(room.removeParticipant(peerId));
            }
        }
    }
    ImGui::PopID();
}
}  // namespace

CollaborationView::CollaborationView(
    const std::string&                                         subViewName,
    std::shared_ptr<Network::Collaboration::CollaborationRoom> room)
    : ISubView(subViewName), m_room(std::move(room))
{
    if ( m_room ) {
        m_viewportPublishRateHz =
            static_cast<int>(m_room->viewportPublishRateHz());
    }
}

CollaborationView::~CollaborationView()
{
    if ( m_pendingGuestJoin ) {
        Logic::ProjectController::instance()
            .setLocalProjectOpeningBlockedByCollaboration(false);
    }
}

void CollaborationView::onUpdate(LayoutContext&, UIManager* sourceManager)
{
    if ( !m_room ) {
        ImGui::TextDisabled("%s", TR("ui.collaboration.unavailable").data());
        return;
    }

    advancePendingGuestJoin(sourceManager);

    if ( m_room->isActive() ) {
        drawActiveRoom();
        if ( m_room->isActive() ) drawChatSection();
        drawLogSection(sourceManager);
        return;
    }

    const bool creatorValid = drawIdentitySection(sourceManager);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    drawOfflineFlow(sourceManager, creatorValid);
    drawLogSection(sourceManager);
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

    const ImGuiTreeNodeFlags headerFlags = ImGuiTreeNodeFlags_DefaultOpen;
    if ( FeedbackCollapsingHeader(TR("ui.collaboration.host_room").data(),
                                  headerFlags) ) {
        ImGui::TextWrapped("%s", TR("ui.collaboration.host_desc").data());
        if ( !hasProject ) {
            ImGui::TextWrapped("%s",
                               TR("ui.collaboration.project_required").data());
        }
        if ( ImGui::BeginTable("CollaborationHostSettingsTable",
                               2,
                               ImGuiTableFlags_SizingStretchProp) ) {
            ImGui::TableSetupColumn(
                "##HostSettingLabel",
                ImGuiTableColumnFlags_WidthFixed,
                ImGui::CalcTextSize(TR("ui.collaboration.room_name").data()).x +
                    ImGui::GetStyle().ItemSpacing.x);
            ImGui::TableSetupColumn("##HostSettingControl",
                                    ImGuiTableColumnFlags_WidthStretch);
            drawRoomInfoLabel(TR("ui.collaboration.room_name").data());
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::InputTextWithHint(
                "##CollaborationRoomName",
                TR("ui.collaboration.room_name_hint").data(),
                m_roomName.data(),
                m_roomName.size());
            ImGui::EndTable();
        }
        FeedbackCheckbox(TR("ui.collaboration.require_matching_build").data(),
                         &m_requireMatchingBuildFingerprint);
        ImGui::TextWrapped(
            "%s", TR("ui.collaboration.require_matching_build_desc").data());
        ImGui::BeginDisabled(
            m_pendingGuestJoin || !creatorValid || m_roomName[0] == '\0' ||
            !isCollaborationProjectRequirementSatisfied(true, hostReady));
        if ( FeedbackButton(TR("ui.collaboration.start_room").data(),
                            ImVec2(-1.0F, 0.0F)) ) {
            Network::Collaboration::CollaborationHostRoomConfig config;
            config.creator = Config::AppConfig::instance()
                                 .getEditorSettings()
                                 .defaultCreator;
            config.participantId =
                Config::AppConfig::instance().getCollaborationParticipantId();
            config.roomName = m_roomName.data();
            config.endpoint = m_room->serverEndpoint();
            config.requireMatchingBuildFingerprint =
                m_requireMatchingBuildFingerprint;
            static_cast<void>(m_room->startHost(std::move(config)));
        }
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    if ( FeedbackCollapsingHeader(TR("ui.collaboration.online_rooms").data(),
                                  headerFlags) ) {
        ImGui::TextWrapped("%s", TR("ui.collaboration.join_desc").data());
        if ( m_pendingGuestJoin ) {
            ImGui::TextColored(
                ImGui::GetStyleColorVec4(ImGuiCol_TextSelectedBg),
                "%s",
                TR("ui.collaboration.closing_local_state").data());
        } else if ( m_guestJoinPreparationCancelled ) {
            ImGui::TextColored(
                ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                "%s",
                TR("ui.collaboration.local_close_cancelled").data());
        }
        if ( FeedbackButton(TR("ui.collaboration.refresh_rooms").data()) ) {
            static_cast<void>(m_room->refreshDirectory());
        }
        const auto& rooms = m_room->directoryRooms();
        if ( rooms.empty() ) {
            ImGui::TextDisabled("%s", TR("ui.collaboration.no_rooms").data());
            return;
        }

        const ImGuiTableFlags tableFlags =
            ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
        if ( ImGui::BeginTable(
                 "CollaborationOnlineRoomsTable", 3, tableFlags) ) {
            const float actionWidth =
                std::max(
                    ImGui::CalcTextSize(TR("ui.collaboration.join_now").data())
                        .x,
                    ImGui::CalcTextSize(TR("ui.collaboration.room_full").data())
                        .x) +
                ImGui::GetStyle().FramePadding.x * 2.0F;
            ImGui::TableSetupColumn(TR("ui.collaboration.room_name").data(),
                                    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(TR("ui.collaboration.online").data(),
                                    ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("00/00").x +
                                        ImGui::GetStyle().CellPadding.x * 2.0F);
            ImGui::TableSetupColumn(TR("ui.collaboration.action").data(),
                                    ImGuiTableColumnFlags_WidthFixed,
                                    actionWidth);
            ImGui::TableHeadersRow();

            for ( const auto& room : rooms ) {
                ImGui::PushID(room.roomId.c_str());
                ImGui::TableNextRow(ImGuiTableRowFlags_None,
                                    ImGui::GetFrameHeight());
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(room.roomName.c_str());
                if ( ImGui::IsItemHovered() ) {
                    ImGui::BeginTooltip();
                    ImGui::Text("%s: %s",
                                TR("ui.collaboration.role.host").data(),
                                room.hostCreator.c_str());
                    ImGui::EndTooltip();
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%u/%u",
                            static_cast<unsigned int>(room.participants),
                            static_cast<unsigned int>(room.capacity));
                ImGui::TableSetColumnIndex(2);
                const bool full = room.participants >= room.capacity;
                ImGui::BeginDisabled(
                    m_pendingGuestJoin || !creatorValid || full ||
                    !isCollaborationProjectRequirementSatisfied(false,
                                                                hasProject));
                if ( FeedbackSmallButton(
                         full ? TR("ui.collaboration.room_full").data()
                              : TR("ui.collaboration.join_now").data()) ) {
                    beginGuestJoin(Config::AppConfig::instance()
                                       .getEditorSettings()
                                       .defaultCreator,
                                   room.roomId,
                                   room.roomName,
                                   sourceManager);
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
}

void CollaborationView::beginGuestJoin(std::string creator, std::string roomId,
                                       std::string roomName,
                                       UIManager*  sourceManager)
{
    if ( !m_room || m_pendingGuestJoin || m_room->isActive() ) return;

    auto pending            = std::make_unique<PendingGuestJoin>();
    pending->config.creator = std::move(creator);
    pending->config.participantId =
        Config::AppConfig::instance().getCollaborationParticipantId();
    pending->config.roomId   = std::move(roomId);
    pending->config.roomName = std::move(roomName);
    pending->config.endpoint = m_room->serverEndpoint();
    pending->config.resourceCacheRoot =
        Config::AppPaths::configRootPath() / "collaboration-cache";
    pending->closeRequestedAt       = std::chrono::steady_clock::now();
    m_pendingGuestJoin              = std::move(pending);
    m_guestJoinPreparationCancelled = false;

    auto& projectController = Logic::ProjectController::instance();
    projectController.setLocalProjectOpeningBlockedByCollaboration(true);
    const bool hasProject       = projectController.currentProject() != nullptr;
    const bool hasBeatmapCanvas = hasNonLogoBeatmapSession();
    if ( needsLocalStateCloseBeforeGuestJoin(hasProject, hasBeatmapCanvas) ) {
        projectController.requestCloseProject();
        return;
    }
    advancePendingGuestJoin(sourceManager);
}

void CollaborationView::advancePendingGuestJoin(UIManager* sourceManager)
{
    if ( !m_pendingGuestJoin || !m_room ) return;

    auto&      projectController = Logic::ProjectController::instance();
    const bool hasProject       = projectController.currentProject() != nullptr;
    const bool hasBeatmapCanvas = hasNonLogoBeatmapSession();
    if ( !needsLocalStateCloseBeforeGuestJoin(hasProject, hasBeatmapCanvas) ) {
        projectController.cancelPendingProjectSwitch();
        auto config = std::move(m_pendingGuestJoin->config);
        m_pendingGuestJoin.reset();
        if ( m_room->join(std::move(config)) ) {
            return;
        }
        m_room->disconnect();
        projectController.setLocalProjectOpeningBlockedByCollaboration(false);
        return;
    }

    if ( projectController.hasPendingProjectAction() ||
         projectController.hasPendingProjectSwitch() ) {
        return;
    }

    constexpr auto CLOSE_RESULT_GRACE_PERIOD = std::chrono::seconds(1);
    if ( std::chrono::steady_clock::now() -
             m_pendingGuestJoin->closeRequestedAt <
         CLOSE_RESULT_GRACE_PERIOD ) {
        return;
    }

    m_pendingGuestJoin.reset();
    m_guestJoinPreparationCancelled = true;
    projectController.setLocalProjectOpeningBlockedByCollaboration(false);
}

void CollaborationView::drawActiveRoom()
{
    const auto&              style       = ImGui::GetStyle();
    const ImGuiTreeNodeFlags headerFlags = ImGuiTreeNodeFlags_DefaultOpen;

    if ( FeedbackCollapsingHeader(TR("ui.collaboration.room_details").data(),
                                  headerFlags) ) {
        if ( ImGui::BeginTable("CollaborationRoomDetailsTable",
                               2,
                               ImGuiTableFlags_SizingStretchProp) ) {
            const float labelWidth = std::max(
                ImGui::CalcTextSize(TR("ui.collaboration.status").data()).x,
                ImGui::CalcTextSize(TR("ui.collaboration.room_name").data()).x);
            ImGui::TableSetupColumn("##RoomDetailLabel",
                                    ImGuiTableColumnFlags_WidthFixed,
                                    labelWidth + style.ItemSpacing.x);
            ImGui::TableSetupColumn("##RoomDetailValue",
                                    ImGuiTableColumnFlags_WidthStretch);
            drawRoomInfoLabel(TR("ui.collaboration.status").data());
            ImGui::TextUnformatted(roomStateText(m_room->state()));
            drawRoomInfoLabel(TR("ui.collaboration.role").data());
            ImGui::TextUnformatted(
                m_room->isHost() ? TR("ui.collaboration.role.host").data()
                                 : TR("ui.collaboration.role.guest").data());
            drawRoomInfoLabel(TR("ui.collaboration.room_name").data());
            ImGui::TextWrapped("%s", m_room->roomName().c_str());
            drawRoomInfoLabel(TR("ui.collaboration.peer_id").data());
            ImGui::Text("#%llu",
                        static_cast<unsigned long long>(m_room->localPeerId()));
            if ( !m_room->roomId().empty() ) {
                drawRoomInfoLabel(TR("ui.collaboration.room_id").data());
                ImGui::TextWrapped("%s", m_room->roomId().c_str());
            }
            ImGui::EndTable();
        }
        if ( !m_room->lastError().empty() ) {
            ImGui::TextWrapped("%s: %s",
                               TR("ui.collaboration.error").data(),
                               m_room->lastError().c_str());
        }
    }

    if ( m_room->isHost() ) {
        ImGui::Spacing();
        if ( FeedbackCollapsingHeader(
                 TR("ui.collaboration.join_requests").data(), headerFlags) ) {
            const auto& requests = m_room->pendingJoinRequests();
            if ( requests.empty() ) {
                ImGui::TextDisabled(
                    "%s", TR("ui.collaboration.no_join_requests").data());
            } else {
                const ImGuiTableFlags tableFlags =
                    ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH |
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
                std::string approveRequest;
                std::string rejectRequest;
                if ( ImGui::BeginTable(
                         "CollaborationJoinRequestsTable", 2, tableFlags) ) {
                    const float approveWidth =
                        ImGui::CalcTextSize(
                            TR("ui.collaboration.approve").data())
                            .x +
                        style.FramePadding.x * 2.0F;
                    const float rejectWidth =
                        ImGui::CalcTextSize(
                            TR("ui.collaboration.reject").data())
                            .x +
                        style.FramePadding.x * 2.0F;
                    ImGui::TableSetupColumn(TR("ui.collaboration.user").data(),
                                            ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn(
                        TR("ui.collaboration.action").data(),
                        ImGuiTableColumnFlags_WidthFixed,
                        approveWidth + rejectWidth + style.ItemSpacing.x);
                    ImGui::TableHeadersRow();
                    for ( const auto& request : requests ) {
                        ImGui::PushID(request.requestId.c_str());
                        ImGui::TableNextRow(ImGuiTableRowFlags_None,
                                            ImGui::GetFrameHeight());
                        ImGui::TableSetColumnIndex(0);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(request.creator.c_str());
                        ImGui::TableSetColumnIndex(1);
                        if ( FeedbackSmallButton(
                                 TR("ui.collaboration.approve").data()) ) {
                            approveRequest = request.requestId;
                        }
                        ImGui::SameLine();
                        if ( FeedbackSmallButton(
                                 TR("ui.collaboration.reject").data()) ) {
                            rejectRequest = request.requestId;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                if ( !approveRequest.empty() ) {
                    static_cast<void>(
                        m_room->approveJoinRequest(approveRequest));
                } else if ( !rejectRequest.empty() ) {
                    static_cast<void>(m_room->rejectJoinRequest(rejectRequest));
                }
            }
        }
    }

    const auto resource = m_room->resourceProgress();
    using ResourcePhase =
        Network::Collaboration::CollaborationResourceSyncPhase;
    const bool showProgress = resource.phase == ResourcePhase::Preparing ||
                              resource.phase == ResourcePhase::ComparingCache ||
                              resource.phase == ResourcePhase::Downloading ||
                              resource.phase == ResourcePhase::Verifying;

    ImGui::Spacing();
    if ( FeedbackCollapsingHeader(TR("ui.collaboration.sync_settings").data(),
                                  headerFlags) ) {
        const float labelWidth =
            std::max({ ImGui::CalcTextSize(
                           TR("ui.collaboration.viewport_rate").data())
                           .x,
                       ImGui::CalcTextSize(
                           TR("ui.collaboration.viewport_render_mode").data())
                           .x,
                       ImGui::CalcTextSize(
                           TR("ui.collaboration.resource.status").data())
                           .x }) +
            style.ItemSpacing.x;
        if ( ImGui::BeginTable("CollaborationSyncSettingsTable",
                               2,
                               ImGuiTableFlags_SizingStretchProp) ) {
            ImGui::TableSetupColumn("##SyncSettingLabel",
                                    ImGuiTableColumnFlags_WidthFixed,
                                    labelWidth);
            ImGui::TableSetupColumn("##SyncSettingControl",
                                    ImGuiTableColumnFlags_WidthStretch);
            drawRoomInfoLabel(TR("ui.collaboration.viewport_rate").data());
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::SliderInt("##CollaborationViewportRate",
                             &m_viewportPublishRateHz,
                             5,
                             60,
                             "%d Hz");
            if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                m_room->setViewportPublishRateHz(
                    static_cast<std::uint32_t>(m_viewportPublishRateHz));
            }

            drawRoomInfoLabel(
                TR("ui.collaboration.viewport_render_mode").data());
            auto& settings = Config::AppConfig::instance().getEditorSettings();
            int   renderMode =
                static_cast<int>(settings.collaborationViewportRenderMode);
            const char* renderModes[]{
                TR("ui.collaboration.viewport_render_mode.filled").data(),
                TR("ui.collaboration.viewport_render_mode.outline").data(),
                TR("ui.collaboration.viewport_render_mode.track_edge").data(),
            };
            ImGui::SetNextItemWidth(-1.0F);
            if ( FeedbackCombo("##CollaborationViewportRenderMode",
                               &renderMode,
                               renderModes,
                               IM_ARRAYSIZE(renderModes)) ) {
                settings.collaborationViewportRenderMode =
                    static_cast<Config::CollaborationViewportRenderMode>(
                        renderMode);
                Config::AppConfig::instance().save();
            }

            drawRoomInfoLabel(TR("ui.collaboration.resource.status").data());
            ImGui::TextWrapped("%s", resourcePhaseText(resource.phase));
            ImGui::EndTable();
        }
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

    ImGui::Spacing();
    if ( FeedbackButton(TR("ui.collaboration.disconnect").data(),
                        ImVec2(-1.0F, 0.0F)) ) {
        m_room->disconnect();
        return;
    }

    ImGui::Spacing();
    if ( FeedbackCollapsingHeader(TR("ui.collaboration.participants").data(),
                                  headerFlags) ) {
        const auto&           participants = m_room->participants();
        const ImGuiTableFlags tableFlags =
            ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
        if ( ImGui::BeginTable(
                 "CollaborationParticipantsTable", 2, tableFlags) ) {
            const float actionWidth =
                ImGui::CalcTextSize(
                    TR("ui.collaboration.stop_following").data())
                    .x +
                style.FramePadding.x * 2.0F +
                (m_room->isHost()
                     ? ImGui::CalcTextSize(
                           TR("ui.collaboration.permissions.manage").data())
                               .x +
                           style.FramePadding.x * 2.0F + style.ItemSpacing.x +
                           ImGui::CalcTextSize(
                               TR("ui.collaboration.remove_participant").data())
                               .x +
                           style.FramePadding.x * 2.0F + style.ItemSpacing.x
                     : 0.0F);
            ImGui::TableSetupColumn(TR("ui.collaboration.user").data(),
                                    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(TR("ui.collaboration.action").data(),
                                    ImGuiTableColumnFlags_WidthFixed,
                                    actionWidth);
            ImGui::TableHeadersRow();

            const auto localPeer = participants.find(m_room->localPeerId());
            if ( localPeer != participants.end() ) {
                drawParticipantRow(
                    *m_room, localPeer->first, localPeer->second);
            }
            for ( const auto& [peerId, identity] : participants ) {
                if ( peerId != m_room->localPeerId() ) {
                    drawParticipantRow(*m_room, peerId, identity);
                }
            }
            ImGui::EndTable();
        }
        if ( !m_room->isHost() ) {
            ImGui::Spacing();
            ImGui::TextUnformatted(
                TR("ui.collaboration.permissions.mine").data());
            drawLocalPermissionSummary(*m_room);
        }
    }
}

void CollaborationView::drawChatSection()
{
    static_assert(CHAT_INPUT_BUFFER_BYTES ==
                  Network::Collaboration::MAX_COLLABORATION_CHAT_MESSAGE_BYTES +
                      1U);
    ImGui::Spacing();
    if ( !FeedbackCollapsingHeader(TR("ui.collaboration.chat").data(),
                                   ImGuiTreeNodeFlags_DefaultOpen) ) {
        return;
    }

    const auto& messages      = m_room->chatMessages();
    const float historyHeight = ImGui::GetTextLineHeightWithSpacing() * 8.0F;
    if ( ImGui::BeginChild("CollaborationChatHistory",
                           ImVec2(0.0F, historyHeight),
                           ImGuiChildFlags_Borders) ) {
        if ( messages.empty() ) {
            ImGui::TextDisabled("%s", TR("ui.collaboration.chat.empty").data());
        } else {
            for ( const auto& message : messages ) {
                const auto totalSeconds = message.elapsedMilliseconds / 1000U;
                const auto minutes      = totalSeconds / 60U;
                const auto seconds      = totalSeconds % 60U;
                ImGui::PushID(static_cast<int>(message.sequence & 0x7FFFFFFFU));
                ImGui::TextDisabled("[%02llu:%02llu]",
                                    static_cast<unsigned long long>(minutes),
                                    static_cast<unsigned long long>(seconds));
                ImGui::SameLine();
                ImGui::TextColored(
                    ImGui::GetStyleColorVec4(ImGuiCol_TextSelectedBg),
                    "%s:",
                    message.creator.c_str());
                ImGui::SameLine();
                ImGui::TextWrapped("%s", message.text.c_str());
                ImGui::PopID();
            }
            if ( messages.back().sequence != m_lastRenderedChatSequence ) {
                ImGui::SetScrollHereY(1.0F);
                m_lastRenderedChatSequence = messages.back().sequence;
            }
        }
    }
    ImGui::EndChild();

    const float sendButtonWidth =
        ImGui::CalcTextSize(TR("ui.collaboration.chat.send").data()).x +
        ImGui::GetStyle().FramePadding.x * 2.0F;
    const float inputWidth =
        std::max(1.0F,
                 ImGui::GetContentRegionAvail().x - sendButtonWidth -
                     ImGui::GetStyle().ItemSpacing.x);
    const bool canSend = m_room->localPeerId() != 0;
    ImGui::BeginDisabled(!canSend);
    ImGui::SetNextItemWidth(inputWidth);
    const bool enterPressed =
        ImGui::InputTextWithHint("##CollaborationChatInput",
                                 TR("ui.collaboration.chat.hint").data(),
                                 m_chatInput.data(),
                                 m_chatInput.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool buttonPressed = FeedbackButton(
        TR("ui.collaboration.chat.send").data(), ImVec2(sendButtonWidth, 0.0F));
    ImGui::EndDisabled();

    if ( canSend && (enterPressed || buttonPressed) ) {
        const auto result = m_room->sendChatMessage(m_chatInput.data());
        m_chatSendFailed =
            result != Network::Collaboration::SubmitChatMessageResult::Accepted;
        if ( !m_chatSendFailed ) {
            m_chatInput.fill('\0');
        }
    }
    if ( m_chatSendFailed ) {
        ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                           "%s",
                           TR("ui.collaboration.chat.send_failed").data());
    }
}

void CollaborationView::drawLogSection(UIManager* sourceManager) const
{
    if ( !sourceManager || !m_room || m_room->logs().empty() ) return;

    ImGui::Spacing();
    if ( !FeedbackCollapsingHeader(TR("title.collaboration_log").data(),
                                   ImGuiTreeNodeFlags_DefaultOpen) ) {
        return;
    }

    if ( auto* logWindow = sourceManager->getView<CollaborationLogWindow>(
             "CollaborationLogWindow") ) {
        logWindow->renderInline();
    }
}
}  // namespace MMM::UI
