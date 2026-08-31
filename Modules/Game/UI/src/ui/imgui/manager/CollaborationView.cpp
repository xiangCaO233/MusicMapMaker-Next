#include "ui/imgui/manager/CollaborationView.h"

#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/CreatorIdentity.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "event/ui/UISettingsTabEvent.h"
#include "graphic/imguivk/VKTexture.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "network/collaboration/CollaborationBuildFingerprint.h"
#include "network/collaboration/CollaborationRoom.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/CollaborationEntryPolicy.h"
#include "ui/imgui/manager/CollaborationLogWindow.h"
#include "ui/imgui/manager/CollaborationRoomCoverImage.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIWidgetUtils.h"

#include <ImGuiFileDialog.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <nfd.h>
#include <string>
#include <system_error>

namespace MMM::UI
{
/// @brief 等待后台构建指纹完成的房主开房请求。
struct CollaborationView::PendingHostStart {
    /// @brief 用户点击时冻结的房主配置。
    Network::Collaboration::CollaborationHostRoomConfig config;
};

struct CollaborationView::PendingGuestJoin {
    /// @brief 已冻结的访客连接配置，避免等待关闭期间读取变化中的 UI 输入。
    Network::Collaboration::CollaborationJoinRoomConfig config;
    /// @brief 是否已经在构建指纹就绪后请求关闭本机项目状态。
    bool closeRequested = false;
    /// @brief 开始请求关闭本机状态的单调时间点。
    std::chrono::steady_clock::time_point closeRequestedAt;
};

namespace
{
/// @brief 开房预览在纹理缓存中的固定键。
constexpr std::string_view HOST_ROOM_COVER_TEXTURE_KEY = "##HostRoomCover";
/// @brief 统一文件选择器的固定窗口 ID。
constexpr const char* ROOM_COVER_FILE_DIALOG_ID =
    "CollaborationRoomCoverPicker";

/// @brief 判断编辑器是否仍存在非欢迎页谱面会话。
/// @warning UI 低频协作入口路径：最多遍历当前少量 Session 快照。
[[nodiscard]] bool hasNonLogoBeatmapSession()
{
    const auto entries = Logic::EditorEngine::instance().getSessionEntries();
    return std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
        return !entry.isLogoPlaceholder;
    });
}

/// @brief 把较长说明收进悬停帮助提示。
/// @param text 需要按段落换行展示的本地化说明。
/// @warning UI 热路径：仅绘制一个短标签，悬停时才创建 tooltip。
void drawHelpMarker(const char* text)
{
    ImGui::TextDisabled("(?)");
    if ( !ImGui::IsItemHovered() ) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0F);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

/// @brief 解析当前谱面的默认房卡封面绝对路径。
/// @param metadata 当前谱面基础元数据。
/// @param project 当前本机项目；为空时以谱面文件目录为根。
/// @return 优先使用 cover_path，其次使用图片类型的 main_cover_path。
std::filesystem::path resolveDefaultRoomCoverPath(
    const MMM::BaseMapMeta& metadata, const MMM::Project* project)
{
    std::filesystem::path relativePath = metadata.cover_path;
    if ( relativePath.empty() &&
         metadata.cover_type == MMM::CoverType::IMAGE ) {
        relativePath = metadata.main_cover_path;
    }
    if ( relativePath.empty() || relativePath.is_absolute() ) {
        return relativePath;
    }
    return project ? project->m_projectRoot / relativePath
                   : metadata.map_path.parent_path() / relativePath;
}

/// @brief 返回封面生成错误对应的本地化键。
const char* roomCoverErrorTranslationKey(CollaborationRoomCoverImageError error)
{
    switch ( error ) {
    case CollaborationRoomCoverImageError::FileUnavailable:
        return "ui.collaboration.cover_error_unavailable";
    case CollaborationRoomCoverImageError::PayloadTooLarge:
        return "ui.collaboration.cover_error_too_large";
    case CollaborationRoomCoverImageError::DecodeFailed:
    case CollaborationRoomCoverImageError::EncodeFailed:
        return "ui.collaboration.cover_error_invalid";
    case CollaborationRoomCoverImageError::None:
    default: return "";
    }
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

    advancePendingHostStart();
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
    renderRoomCoverFilePicker();
    drawLogSection(sourceManager);
}

ImVec2 CollaborationView::getMinContentSize(float dpiScale) const
{
    const float scale = std::max(1.0f, dpiScale);
    return ImVec2(std::ceil(340.0f * scale), std::ceil(420.0f * scale));
}

bool CollaborationView::needsTextureReload() const
{
    return !m_pendingRoomCoverTextures.empty() ||
           !m_roomCoverTextureRemovals.empty();
}

void CollaborationView::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                       vk::Device&         logicalDevice,
                                       vk::CommandPool&    commandPool,
                                       vk::Queue&          queue)
{
    for ( const auto& key : m_roomCoverTextureRemovals ) {
        m_roomCoverTextures.erase(key);
        m_failedRoomCoverTextures.erase(key);
    }
    m_roomCoverTextureRemovals.clear();

    auto pending = std::move(m_pendingRoomCoverTextures);
    m_pendingRoomCoverTextures.clear();
    for ( auto& [key, base64] : pending ) {
        auto decoded = decodeCollaborationRoomCoverImage(base64);
        if ( !decoded ) {
            m_failedRoomCoverTextures.insert(std::move(key));
            continue;
        }

        auto texture =
            std::make_unique<Graphic::VKTexture>(decoded.pixels.data(),
                                                 decoded.width,
                                                 decoded.height,
                                                 physicalDevice,
                                                 logicalDevice,
                                                 commandPool,
                                                 queue);
        if ( !texture->isValid() ) {
            m_failedRoomCoverTextures.insert(std::move(key));
            continue;
        }
        static_cast<void>(texture->getImTextureID());
        m_failedRoomCoverTextures.erase(key);
        m_roomCoverTextures.insert_or_assign(std::move(key),
                                             std::move(texture));
    }
}

void CollaborationView::openRoomCoverFilePicker()
{
    auto&                 app              = Config::AppConfig::instance();
    auto&                 settings         = app.getEditorSettings();
    std::filesystem::path defaultDirectory = m_roomCoverPath.parent_path();
    if ( defaultDirectory.empty() && !m_defaultRoomCoverPath.empty() ) {
        defaultDirectory = m_defaultRoomCoverPath.parent_path();
    }
    if ( defaultDirectory.empty() && !settings.lastFilePickerPath.empty() ) {
        defaultDirectory = Config::utf8ToPath(settings.lastFilePickerPath);
    }
    const std::string defaultPath = defaultDirectory.empty()
                                        ? std::string(".")
                                        : Config::pathToUtf8(defaultDirectory);

    if ( settings.filePickerStyle == Config::FilePickerStyle::Native ) {
        PlayPopupOpenFeedback();
        nfdu8char_t*      selectedPath = nullptr;
        nfdu8filteritem_t filters[1]   = { { "Image Files",
                                             "png,jpg,jpeg,bmp,tga" } };
        const nfdresult_t result       = NativeFileDialog::openFile(
            &selectedPath, filters, 1, defaultPath.c_str());
        if ( result == NFD_OKAY && selectedPath ) {
            const auto path = Config::utf8ToPath(selectedPath);
            NFD_FreePathU8(selectedPath);
            setRoomCoverPath(path, true);
            if ( !path.parent_path().empty() ) {
                settings.lastFilePickerPath =
                    Config::pathToUtf8(path.parent_path());
                app.save();
            }
        } else if ( result == NFD_ERROR ) {
            XERROR("Failed to open collaboration room cover picker: {}",
                   NFD_GetError() ? NFD_GetError() : "Unknown NFD error");
        }
        return;
    }

    IGFD::FileDialogConfig dialogConfig;
    dialogConfig.path              = defaultPath;
    dialogConfig.countSelectionMax = 1;
    dialogConfig.flags             = ImGuiFileDialogFlags_Modal |
                                     ImGuiFileDialogFlags_HideColumnType |
                                     ImGuiFileDialogFlags_ReadOnlyFileNameField;
    const bool wasOpen =
        ImGuiFileDialog::Instance()->IsOpened(ROOM_COVER_FILE_DIALOG_ID);
    ImGuiFileDialog::Instance()->OpenDialog(
        ROOM_COVER_FILE_DIALOG_ID,
        TR("ui.collaboration.cover_picker_title").data(),
        ".png,.jpg,.jpeg,.bmp,.tga",
        dialogConfig);
    if ( !wasOpen &&
         ImGuiFileDialog::Instance()->IsOpened(ROOM_COVER_FILE_DIALOG_ID) ) {
        PlayPopupOpenFeedback();
    }
}

void CollaborationView::renderRoomCoverFilePicker()
{
    if ( !ImGuiFileDialog::Instance()->IsOpened(ROOM_COVER_FILE_DIALOG_ID) ) {
        return;
    }
    if ( ImGuiFileDialog::Instance()->Display(
             ROOM_COVER_FILE_DIALOG_ID,
             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings,
             { 600.0F, 400.0F }) ) {
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            const auto path = Config::utf8ToPath(
                ImGuiFileDialog::Instance()->GetFilePathName());
            setRoomCoverPath(path, true);
            if ( !path.parent_path().empty() ) {
                auto& app = Config::AppConfig::instance();
                app.getEditorSettings().lastFilePickerPath =
                    Config::pathToUtf8(path.parent_path());
                app.save();
            }
        }
        ImGuiFileDialog::Instance()->Close();
    }
}

void CollaborationView::setRoomCoverPath(const std::filesystem::path& path,
                                         bool customized)
{
    m_roomCoverErrorKey.clear();
    if ( path.empty() ) {
        m_roomCoverPath.clear();
        m_roomCoverImage.clear();
        m_roomCoverDefaultMode = customized ? CollaborationDefaultMode::Custom
                                            : CollaborationDefaultMode::Follow;
        if ( const auto pending =
                 m_pendingRoomCoverTextures.find(HOST_ROOM_COVER_TEXTURE_KEY);
             pending != m_pendingRoomCoverTextures.end() ) {
            m_pendingRoomCoverTextures.erase(pending);
        }
        m_roomCoverTextureRemovals.emplace(HOST_ROOM_COVER_TEXTURE_KEY);
        return;
    }

    const auto result = encodeCollaborationRoomCoverImage(path);
    if ( result.error != CollaborationRoomCoverImageError::None ) {
        m_roomCoverErrorKey = roomCoverErrorTranslationKey(result.error);
        if ( !customized ) {
            m_roomCoverPath.clear();
            m_roomCoverImage.clear();
            if ( const auto pending = m_pendingRoomCoverTextures.find(
                     HOST_ROOM_COVER_TEXTURE_KEY);
                 pending != m_pendingRoomCoverTextures.end() ) {
                m_pendingRoomCoverTextures.erase(pending);
            }
            m_roomCoverTextureRemovals.emplace(HOST_ROOM_COVER_TEXTURE_KEY);
        }
        return;
    }

    m_roomCoverPath        = path;
    m_roomCoverImage       = result.base64;
    m_roomCoverDefaultMode = customized ? CollaborationDefaultMode::Custom
                                        : CollaborationDefaultMode::Follow;
    if ( const auto removal =
             m_roomCoverTextureRemovals.find(HOST_ROOM_COVER_TEXTURE_KEY);
         removal != m_roomCoverTextureRemovals.end() ) {
        m_roomCoverTextureRemovals.erase(removal);
    }
    if ( const auto failed =
             m_failedRoomCoverTextures.find(HOST_ROOM_COVER_TEXTURE_KEY);
         failed != m_failedRoomCoverTextures.end() ) {
        m_failedRoomCoverTextures.erase(failed);
    }
    m_pendingRoomCoverTextures.insert_or_assign(
        std::string(HOST_ROOM_COVER_TEXTURE_KEY), result.base64);
}

void CollaborationView::queueRoomCoverTexture(std::string      key,
                                              std::string_view base64)
{
    if ( key.empty() || base64.empty() || m_roomCoverTextures.contains(key) ||
         m_pendingRoomCoverTextures.contains(key) ||
         m_failedRoomCoverTextures.contains(key) ) {
        return;
    }
    m_pendingRoomCoverTextures.emplace(std::move(key), base64);
}

void CollaborationView::drawRoomCover(std::string_view textureKey, ImVec2 size)
{
    size.x = std::max(size.x, 1.0F);
    size.y = std::max(size.y, 1.0F);
    ImGui::InvisibleButton("##RoomCoverImage", size);
    const ImVec2 minimum  = ImGui::GetItemRectMin();
    const ImVec2 maximum  = ImGui::GetItemRectMax();
    auto*        drawList = ImGui::GetWindowDrawList();
    const float  rounding = ImGui::GetStyle().FrameRounding;

    const auto texture = m_roomCoverTextures.find(textureKey);
    if ( texture != m_roomCoverTextures.end() && texture->second ) {
        drawList->AddImage(texture->second->getImTextureID(), minimum, maximum);
    } else {
        drawList->AddRectFilled(
            minimum, maximum, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
        const char*  placeholder = "MMM";
        const ImVec2 textSize    = ImGui::CalcTextSize(placeholder);
        drawList->AddText(ImVec2((minimum.x + maximum.x - textSize.x) * 0.5F,
                                 (minimum.y + maximum.y - textSize.y) * 0.5F),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled),
                          placeholder);
    }
    drawList->AddRect(
        minimum, maximum, ImGui::GetColorU32(ImGuiCol_Border), rounding);
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
    const bool fingerprintFailed =
        Network::Collaboration::collaborationBuildFingerprintState() ==
        Network::Collaboration::CollaborationBuildFingerprintState::Failed;
    if ( !m_room->lastError().empty() ) {
        ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                           "%s",
                           TR("ui.collaboration.previous_room_ended").data());
        ImGui::TextWrapped("%s: %s",
                           TR("ui.collaboration.error").data(),
                           m_room->lastError().c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }
    if ( hostReady ) {
        const auto& metadata =
            activeSession->getContext().currentBeatmap->m_baseMapMetadata;
        const std::string_view defaultRoomName =
            !metadata.title.empty() ? std::string_view(metadata.title)
                                    : std::string_view(metadata.name);
        if ( defaultRoomName != m_defaultRoomName ) {
            m_defaultRoomName.assign(defaultRoomName);
            if ( shouldFollowCollaborationDefault(m_roomNameDefaultMode) ) {
                setInputBuffer(m_roomName, m_defaultRoomName);
            }
        }

        const auto defaultCover =
            resolveDefaultRoomCoverPath(metadata, project);
        if ( defaultCover != m_defaultRoomCoverPath ) {
            m_defaultRoomCoverPath = defaultCover;
            if ( shouldFollowCollaborationDefault(m_roomCoverDefaultMode) ) {
                setRoomCoverPath(defaultCover, false);
            }
        }
    }
    if ( fingerprintFailed ) {
        ImGui::TextColored(
            ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
            "%s",
            TR("ui.collaboration.build_fingerprint_failed").data());
    }

    const ImGuiTreeNodeFlags headerFlags = ImGuiTreeNodeFlags_DefaultOpen;
    if ( FeedbackCollapsingHeader(TR("ui.collaboration.host_room").data(),
                                  headerFlags) ) {
        ImGui::TextDisabled("%s", TR("ui.collaboration.room_cover").data());
        ImGui::SameLine();
        drawHelpMarker(TR("ui.collaboration.host_desc").data());
        if ( !hasProject ) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                               "%s",
                               TR("ui.collaboration.project_required").data());
        }

        ImGui::PushID("HostRoomCoverPreview");
        const float coverWidth = ImGui::GetContentRegionAvail().x;
        drawRoomCover(HOST_ROOM_COVER_TEXTURE_KEY,
                      ImVec2(coverWidth, coverWidth * 9.0F / 16.0F));
        if ( ImGui::IsItemHovered() && !m_roomCoverPath.empty() ) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(Config::pathToUtf8(m_roomCoverPath).c_str());
            ImGui::EndTooltip();
        }
        ImGui::PopID();

        if ( ImGui::BeginTable("CollaborationRoomCoverActions",
                               3,
                               ImGuiTableFlags_SizingStretchSame) ) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if ( FeedbackSmallButton(
                     TR("ui.collaboration.cover_choose").data()) ) {
                openRoomCoverFilePicker();
            }
            ImGui::TableSetColumnIndex(1);
            if ( FeedbackSmallButton(
                     TR("ui.collaboration.cover_use_beatmap").data()) ) {
                m_roomCoverDefaultMode = CollaborationDefaultMode::Follow;
                setRoomCoverPath(m_defaultRoomCoverPath, false);
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::BeginDisabled(m_roomCoverImage.empty());
            if ( FeedbackSmallButton(
                     TR("ui.collaboration.cover_clear").data()) ) {
                setRoomCoverPath({}, true);
            }
            ImGui::EndDisabled();
            ImGui::EndTable();
        }
        if ( !m_roomCoverErrorKey.empty() ) {
            ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                               "%s",
                               TR(m_roomCoverErrorKey.c_str()).data());
        }

        ImGui::TextDisabled("%s", TR("ui.collaboration.room_name").data());
        ImGui::SameLine();
        if ( FeedbackSmallButton(
                 TR("ui.collaboration.room_name_follow_beatmap").data()) ) {
            m_roomNameDefaultMode = CollaborationDefaultMode::Follow;
            setInputBuffer(m_roomName, m_defaultRoomName);
        }
        ImGui::SetNextItemWidth(-1.0F);
        if ( ImGui::InputTextWithHint(
                 "##CollaborationRoomName",
                 TR("ui.collaboration.room_name_hint").data(),
                 m_roomName.data(),
                 m_roomName.size()) ) {
            m_roomNameDefaultMode = resolveCollaborationTextDefaultMode(
                std::string_view(m_roomName.data()), m_defaultRoomName);
        }
        FeedbackCheckbox(TR("ui.collaboration.require_matching_build").data(),
                         &m_requireMatchingBuildFingerprint);
        ImGui::SameLine();
        drawHelpMarker(
            TR("ui.collaboration.require_matching_build_desc").data());
        if ( m_pendingHostStart ) {
            ImGui::TextColored(
                ImGui::GetStyleColorVec4(ImGuiCol_TextSelectedBg),
                "%s",
                TR("ui.collaboration.build_fingerprint_calculating").data());
        }
        ImGui::BeginDisabled(
            m_pendingHostStart || m_pendingGuestJoin || !creatorValid ||
            fingerprintFailed || m_roomName[0] == '\0' ||
            !isCollaborationProjectRequirementSatisfied(true, hostReady));
        if ( FeedbackButton(TR("ui.collaboration.start_room").data(),
                            ImVec2(-1.0F, 0.0F)) ) {
            Network::Collaboration::CollaborationHostRoomConfig config;
            config.creator = Config::AppConfig::instance()
                                 .getEditorSettings()
                                 .defaultCreator;
            config.participantId =
                Config::AppConfig::instance().getCollaborationParticipantId();
            config.roomName       = m_roomName.data();
            config.roomCoverImage = m_roomCoverImage;
            config.endpoint       = m_room->serverEndpoint();
            config.requireMatchingBuildFingerprint =
                m_requireMatchingBuildFingerprint;
            config.buildFingerprint =
                Network::Collaboration::collaborationBuildFingerprint();
            if ( config.buildFingerprint.empty() ) {
                auto pending       = std::make_unique<PendingHostStart>();
                pending->config    = std::move(config);
                m_pendingHostStart = std::move(pending);
            } else {
                static_cast<void>(m_room->startHost(std::move(config)));
            }
        }
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    if ( FeedbackCollapsingHeader(TR("ui.collaboration.online_rooms").data(),
                                  headerFlags) ) {
        drawHelpMarker(TR("ui.collaboration.join_desc").data());
        ImGui::SameLine();
        if ( FeedbackSmallButton(
                 TR("ui.collaboration.refresh_rooms").data()) ) {
            static_cast<void>(m_room->refreshDirectory());
        }
        if ( m_pendingGuestJoin ) {
            ImGui::TextColored(
                ImGui::GetStyleColorVec4(ImGuiCol_TextSelectedBg),
                "%s",
                !m_pendingGuestJoin->closeRequested
                    ? TR("ui.collaboration.build_fingerprint_calculating")
                          .data()
                    : TR("ui.collaboration.closing_local_state").data());
        } else if ( m_guestJoinPreparationCancelled ) {
            ImGui::TextColored(
                ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                "%s",
                TR("ui.collaboration.local_close_cancelled").data());
        }
        const auto& rooms = m_room->directoryRooms();
        if ( rooms.empty() ) {
            ImGui::TextDisabled("%s", TR("ui.collaboration.no_rooms").data());
            return;
        }

        const float dpiScale = std::max(
            1.0F, Config::AppConfig::instance().getWindowContentScale());
        const float      cardHeight = 92.0F * dpiScale;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rooms.size()),
                      cardHeight + ImGui::GetStyle().ItemSpacing.y);
        while ( clipper.Step() ) {
            for ( int index = clipper.DisplayStart; index < clipper.DisplayEnd;
                  ++index ) {
                const auto& room = rooms[static_cast<std::size_t>(index)];
                ImGui::PushID(room.roomId.c_str());
                const bool cardVisible =
                    ImGui::BeginChild("##RoomCard",
                                      ImVec2(0.0F, cardHeight),
                                      ImGuiChildFlags_Borders,
                                      ImGuiWindowFlags_NoScrollbar |
                                          ImGuiWindowFlags_NoScrollWithMouse);
                if ( cardVisible &&
                     ImGui::BeginTable("##RoomCardLayout",
                                       2,
                                       ImGuiTableFlags_SizingStretchProp) ) {
                    const float coverWidth =
                        std::min(112.0F * dpiScale,
                                 ImGui::GetContentRegionAvail().x * 0.38F);
                    ImGui::TableSetupColumn("##RoomCover",
                                            ImGuiTableColumnFlags_WidthFixed,
                                            coverWidth);
                    ImGui::TableSetupColumn("##RoomDetails",
                                            ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if ( room.hasCoverImage ) {
                        const auto cover =
                            m_room->directoryRoomCover(room.roomId);
                        if ( cover.empty() ) {
                            static_cast<void>(
                                m_room->requestDirectoryRoomCover(room.roomId));
                        } else {
                            queueRoomCoverTexture(room.roomId, cover);
                        }
                    }
                    drawRoomCover(
                        room.roomId,
                        ImVec2(coverWidth, coverWidth * 9.0F / 16.0F));

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s", room.roomName.c_str());
                    ImGui::TextDisabled(
                        "%s  ·  %u/%u",
                        room.hostCreator.c_str(),
                        static_cast<unsigned int>(room.participants),
                        static_cast<unsigned int>(room.capacity));
                    const bool full = room.participants >= room.capacity;
                    ImGui::BeginDisabled(
                        m_pendingHostStart || m_pendingGuestJoin ||
                        fingerprintFailed || !creatorValid || full ||
                        !isCollaborationProjectRequirementSatisfied(
                            false, hasProject));
                    if ( FeedbackButton(
                             full ? TR("ui.collaboration.room_full").data()
                                  : TR("ui.collaboration.join_now").data(),
                             ImVec2(-1.0F, 0.0F)) ) {
                        beginGuestJoin(Config::AppConfig::instance()
                                           .getEditorSettings()
                                           .defaultCreator,
                                       room.roomId,
                                       room.roomName,
                                       sourceManager);
                    }
                    ImGui::EndDisabled();
                    ImGui::EndTable();
                }
                ImGui::EndChild();
                ImGui::PopID();
            }
        }
    }
}

void CollaborationView::beginGuestJoin(std::string creator, std::string roomId,
                                       std::string roomName,
                                       UIManager*  sourceManager)
{
    if ( !m_room || m_pendingHostStart || m_pendingGuestJoin ||
         m_room->isActive() ) {
        return;
    }

    auto pending            = std::make_unique<PendingGuestJoin>();
    pending->config.creator = std::move(creator);
    pending->config.participantId =
        Config::AppConfig::instance().getCollaborationParticipantId();
    pending->config.roomId   = std::move(roomId);
    pending->config.roomName = std::move(roomName);
    pending->config.endpoint = m_room->serverEndpoint();
    pending->config.resourceCacheRoot =
        Config::AppPaths::configRootPath() / "collaboration-cache";
    m_pendingGuestJoin              = std::move(pending);
    m_guestJoinPreparationCancelled = false;

    Logic::ProjectController::instance()
        .setLocalProjectOpeningBlockedByCollaboration(true);
    advancePendingGuestJoin(sourceManager);
}

void CollaborationView::advancePendingGuestJoin(UIManager* sourceManager)
{
    if ( !m_pendingGuestJoin || !m_room ) return;

    const auto fingerprint =
        Network::Collaboration::collaborationBuildFingerprint();
    if ( fingerprint.empty() ) {
        if ( Network::Collaboration::collaborationBuildFingerprintState() ==
             Network::Collaboration::CollaborationBuildFingerprintState::
                 Failed ) {
            m_pendingGuestJoin.reset();
            Logic::ProjectController::instance()
                .setLocalProjectOpeningBlockedByCollaboration(false);
        }
        return;
    }
    m_pendingGuestJoin->config.buildFingerprint = fingerprint;

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

    if ( !m_pendingGuestJoin->closeRequested ) {
        m_pendingGuestJoin->closeRequested   = true;
        m_pendingGuestJoin->closeRequestedAt = std::chrono::steady_clock::now();
        projectController.requestCloseProject();
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

void CollaborationView::advancePendingHostStart()
{
    if ( !m_pendingHostStart || !m_room ) return;

    auto&       engine        = Logic::EditorEngine::instance();
    auto        activeSession = engine.getActiveNonLogoSession();
    const auto* project       = engine.getCurrentProject();
    if ( !project || !activeSession ||
         !activeSession->getContext().currentBeatmap ) {
        m_pendingHostStart.reset();
        return;
    }

    auto fingerprint = Network::Collaboration::collaborationBuildFingerprint();
    if ( fingerprint.empty() ) {
        if ( Network::Collaboration::collaborationBuildFingerprintState() ==
             Network::Collaboration::CollaborationBuildFingerprintState::
                 Failed ) {
            m_pendingHostStart.reset();
        }
        return;
    }

    auto config             = std::move(m_pendingHostStart->config);
    config.buildFingerprint = std::move(fingerprint);
    m_pendingHostStart.reset();
    static_cast<void>(m_room->startHost(std::move(config)));
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
    // 发送动作发生在输入框绘制之后，因此延迟到下一帧请求焦点。
    if ( m_shouldFocusChatInput && canSend ) {
        ImGui::SetKeyboardFocusHere();
        m_shouldFocusChatInput = false;
    }
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
        // 无论发送是否被拒绝，都让用户可以连续输入或立即修正后重试。
        m_shouldFocusChatInput = true;
        const auto result      = m_room->sendChatMessage(m_chatInput.data());
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
