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
///
/// 配置在用户点击时冻结，避免等待异步指纹期间读取继续变化的输入缓冲。
/// 该状态不保存项目或会话指针，推进时始终重新验证当前宿主数据。
/// 指纹失败或宿主前置条件消失时可直接丢弃，不需要回滚网络连接。
/// 同一视图实例最多持有一个请求。
struct CollaborationView::PendingHostStart {
    /// @brief 用户点击时冻结的房主配置。
    Network::Collaboration::CollaborationHostRoomConfig config;
};

/// @brief 等待构建指纹和本机项目关闭完成的访客加入请求。
///
/// 加入协作前必须释放本地编辑会话；状态对象跨帧保存配置和关闭阶段，所有字段只由
/// UI 线程访问。
/// 配置一旦建立便不再读取房间目录条目，目录刷新不会改变目标身份。
/// 生命周期结束时必须解除 ProjectController 的本地项目打开门。
struct CollaborationView::PendingGuestJoin {
    /// @brief 已冻结的访客连接配置，避免等待关闭期间读取变化中的 UI 输入。
    Network::Collaboration::CollaborationJoinRoomConfig config;
    /// @brief 是否已经在构建指纹就绪后请求关闭本机项目状态。
    ///
    /// false 阶段仍可取消或等待指纹，true 阶段只轮询项目关闭完成。
    bool closeRequested = false;
    /// @brief 开始请求关闭本机状态的单调时间点。
    ///
    /// 仅用于低频状态诊断或超时展示，不依赖系统墙钟变化。
    std::chrono::steady_clock::time_point closeRequestedAt;
};

namespace
{
/// @brief 开房预览在纹理缓存中的固定键。
///
/// 主机配置预览和异步纹理上传必须共用该内部键。
constexpr std::string_view HOST_ROOM_COVER_TEXTURE_KEY = "##HostRoomCover";
/// @brief 统一文件选择器的固定窗口 ID。
///
/// 打开和后续帧 Display 查询均使用同一非本地化标识。
constexpr const char* ROOM_COVER_FILE_DIALOG_ID =
    "CollaborationRoomCoverPicker";

/// @brief 判断编辑器是否仍存在非欢迎页谱面会话。
/// @return 任一会话不是 Logo 占位时返回 true。
///
/// 协作访客加入前必须关闭所有本地实际谱面，欢迎页占位不包含需保护的编辑状态。
/// @warning UI 低频协作入口路径：最多遍历当前少量 Session 快照。
[[nodiscard]] bool hasNonLogoBeatmapSession()
{
    // 获取值快照后在无锁 UI 逻辑中检查占位标记。
    const auto entries = Logic::EditorEngine::instance().getSessionEntries();
    return std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
        // 任一真实谱面会话都会阻止直接开始访客连接。
        return !entry.isLogoPlaceholder;
    });
}

/// @brief 把较长说明收进悬停帮助提示。
/// @param text 需要按段落换行展示的本地化说明。
///
/// 默认只占用短问号标签，悬停时用约 24 个字体宽度限制说明文本，避免宽 tooltip
/// 遮挡整个协作面板。
/// @warning UI 热路径：仅绘制一个短标签，悬停时才创建 tooltip。
void drawHelpMarker(const char* text)
{
    // 禁用色提示该标记不是主要操作按钮。
    ImGui::TextDisabled("(?)");
    // 未悬停时不创建 tooltip 或计算换行内容。
    if ( !ImGui::IsItemHovered() ) return;
    // tooltip 的 Begin/End 和换行栈必须严格对称。
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
///
/// cover_path 是独立图片封面；只有背景类型明确为 IMAGE 时才允许 main_cover_path
/// 作为回退，避免把视频路径交给静态房卡编码器。相对路径按项目或谱面目录解析。
std::filesystem::path resolveDefaultRoomCoverPath(
    const MMM::BaseMapMeta& metadata, const MMM::Project* project)
{
    // 独立封面具有最高优先级。
    std::filesystem::path relativePath = metadata.cover_path;
    if ( relativePath.empty() &&
         metadata.cover_type == MMM::CoverType::IMAGE ) {
        // 视频背景不能作为静态房卡预览。
        relativePath = metadata.main_cover_path;
    }
    if ( relativePath.empty() || relativePath.is_absolute() ) {
        // 空值保持未选择，绝对路径无需拼接根目录。
        return relativePath;
    }
    // 项目可用时资源路径相对项目根，否则相对谱面文件所在目录。
    return project ? project->m_projectRoot / relativePath
                   : metadata.map_path.parent_path() / relativePath;
}

/// @brief 返回封面生成错误对应的本地化键。
/// @param error 封面读取、解码或编码阶段错误。
/// @return 对应翻译键；None 返回空字符串。
///
/// 解码与编码失败都向用户归类为无效图片，文件不可用与大小超限保留独立提示。
const char* roomCoverErrorTranslationKey(CollaborationRoomCoverImageError error)
{
    switch ( error ) {
    case CollaborationRoomCoverImageError::FileUnavailable:
        return "ui.collaboration.cover_error_unavailable";
    case CollaborationRoomCoverImageError::PayloadTooLarge:
        return "ui.collaboration.cover_error_too_large";
    case CollaborationRoomCoverImageError::DecodeFailed:
    case CollaborationRoomCoverImageError::EncodeFailed:
        // 两类内部处理失败共享用户可理解的图片无效提示。
        return "ui.collaboration.cover_error_invalid";
    case CollaborationRoomCoverImageError::None:
    default: return "";
    }
}
/// @brief 将字符串写入固定大小且以零结尾的输入缓冲区。
/// @tparam Size 缓冲区编译期容量，必须至少为一。
/// @param buffer 目标 ImGui 输入缓冲。
/// @param value 待写入 UTF-8 字节序列。
///
/// 先清零整个数组，再最多复制 Size-1 字节；剩余零值自然提供终止符并清除旧尾部。
template<std::size_t Size>
void setInputBuffer(std::array<char, Size>& buffer, std::string_view value)
{
    // 整体清零同时处理新值短于旧值的情况。
    buffer.fill('\0');
    // 始终为字符串终止符保留一个字节。
    const auto copyLength = std::min(value.size(), Size - 1);
    std::copy_n(value.begin(), copyLength, buffer.begin());
}

/// @brief 返回房间状态对应的本地化文本。
/// @param state 协作房间当前连接状态。
/// @return 生命周期状态的本地化只读文本。
///
/// 未知枚举值与 Idle 统一回退空闲状态，避免 UI 暴露空字符串。
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
        // Idle 和未来未知值都使用安全的空闲标签。
    default: return TR("ui.collaboration.state.idle").data();
    }
}

/// @brief 返回资源同步阶段对应的本地化文本。
/// @param phase 访客资源清单和下载流程当前阶段。
/// @return 对应阶段的本地化只读文本。
///
/// 该映射只负责展示，不驱动状态机；未知枚举按 Idle 处理。
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
        // 未开始和未知阶段都显示空闲。
    default: return TR("ui.collaboration.resource.idle").data();
    }
}

/// @brief 绘制房间信息表中的一行左侧标签。
/// @param label 本地化标签。
///
/// 每次调用创建新行，把灰色标签放在第一列，并把当前列切到第二列供调用方立即输出值。
/// @warning UI 热路径：只切换表格列并提交一段禁用色文本。
void drawRoomInfoLabel(const char* label)
{
    // 行创建与两次列切换封装为固定布局惯例。
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
}

/// @brief 绘制当前访客从房主收到的只读权限明细。
/// @param room 当前协作房间。
///
/// localPermissions 是房主下发给当前访客的位掩码快照。主编辑权限和五类细分权限
/// 分别展示，不提供修改控件；房主权限配置在参与者表弹窗中完成。
/// @warning UI 热路径：固定绘制六个内存权限位，不执行网络发送。
/// @details Edit
/// 是总体写权限，但细分位仍逐项展示，帮助访客理解物件、时间线、元数据、
/// 音频采样和标注分别是否可改。权限变化由房间快照在后续帧自然刷新。
/// 该摘要不会推导或修改权限组合，只忠实检查各 bit。
void drawLocalPermissionSummary(
    const Network::Collaboration::CollaborationRoom& room)
{
    // 权限快照在本次绘制中保持一致，lambda 按值捕获。
    const auto permissions = room.localPermissions();
    const auto drawPermission =
        [permissions](
            const char*                                     label,
            Network::Collaboration::CollaborationPermission permission) {
            const bool enabled =
                Network::Collaboration::hasCollaborationPermission(permissions,
                                                                   permission);
            // [+]/[-] 前缀在无颜色差异环境下仍可辨识授权状态。
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
/// @param identity 成员展示名和稳定参与者身份。
///
/// 本地成员只显示“你”标记；远端成员可切换视野跟随。房主额外获得权限编辑和移除
/// 操作，权限弹窗以 participantId 作为 ImGui ID 隔离不同成员的同名按钮。
/// @warning UI 热路径：成员表可见时每帧最多调用 8 次；只绘制内存状态，
/// 不执行网络发送或文件系统访问。
/// @details 主 Edit 位关闭时细分 checkbox 在 UI 上禁用，但其已有 bit
/// 值仍保留在局部 掩码中；重新开启 Edit
/// 后可恢复之前细分选择。实际授权由网络层同时考虑主位。
/// follow、权限更新和移除都只传递稳定 PeerId，不以可重名 Creator 定位成员。
void drawParticipantRow(
    Network::Collaboration::CollaborationRoom&         room,
    Network::Collaboration::PeerId                     peerId,
    const Network::Collaboration::ParticipantIdentity& identity)
{
    // 本地和正在跟随状态决定第二列动作。
    const bool local     = peerId == room.localPeerId();
    const bool following = room.followedPeerId() == peerId;

    // participantId 比 creator 展示名稳定且唯一。
    ImGui::PushID(identity.participantId.c_str());
    ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(identity.creator.c_str());
    ImGui::SameLine();
    // 只展示前八位参与者 ID，完整身份仍保存在房间状态中。
    ImGui::TextDisabled("(%.*s)", 8, identity.participantId.c_str());
    // 未收到权限条目时按零权限保守展示。
    const auto permission = room.participantPermissions().find(peerId);
    const auto permissionMask =
        permission == room.participantPermissions().end() ? 0U
                                                          : permission->second;
    if ( !Network::Collaboration::hasCollaborationPermission(
             permissionMask,
             Network::Collaboration::CollaborationPermission::Edit) ) {
        // 缺少主 Edit 权限时标记整名成员为只读。
        ImGui::SameLine();
        ImGui::TextDisabled(
            "%s", TR("ui.collaboration.permissions.read_only").data());
    }

    ImGui::TableSetColumnIndex(1);
    if ( local ) {
        // 本地成员不能跟随、移除或修改自己的房主权限。
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", TR("ui.collaboration.you_suffix").data());
    } else {
        // 同一按钮在跟随中切换为停止跟随。
        const char* actionLabel =
            following ? TR("ui.collaboration.stop_following").data()
                      : TR("ui.collaboration.follow").data();
        if ( FeedbackSmallButton(actionLabel) ) {
            // PeerId 0 表示清除当前跟随目标。
            static_cast<void>(room.setFollowedPeer(following ? 0 : peerId));
        }
        if ( room.isHost() ) {
            // 权限管理和移除仅对房主可见。
            ImGui::SameLine();
            if ( FeedbackSmallButton(
                     TR("ui.collaboration.permissions.manage").data()) ) {
                FeedbackOpenPopup("CollaborationPermissionsPopup");
            }
            if ( ImGui::BeginPopup("CollaborationPermissionsPopup") ) {
                // 弹窗先标明正在配置的成员。
                ImGui::TextUnformatted(identity.creator.c_str());
                ImGui::Separator();

                // 在本地副本上累积本帧 checkbox 变更，最后统一发送。
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
                            // 枚举值本身是权限位，可直接转换为 mask。
                            const auto bit =
                                static_cast<Network::Collaboration::
                                                CollaborationPermissionMask>(
                                    target);
                            // 勾选置位，取消清位，不影响其他权限。
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
                // 细分权限只有在主 Edit 权限存在时才允许编辑。
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
                    // 一个 UI 帧只提交一次合并后的权限掩码。
                    static_cast<void>(room.setParticipantPermissions(
                        peerId, updatedPermissions));
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if ( FeedbackSmallButton(
                     TR("ui.collaboration.remove_participant").data()) ) {
                // 房间层负责断开成员并广播更新。
                static_cast<void>(room.removeParticipant(peerId));
            }
        }
    }
    ImGui::PopID();
}
}  // namespace

/// @brief 创建协作侧栏并接管房间服务共享所有权。
/// @param subViewName 侧栏注册使用的稳定子视图名。
/// @param room 协作网络与同步状态服务；允许为空以显示不可用占位。
///
/// 初始视野发布频率从房间读取，后续 UI 修改再写回服务。
CollaborationView::CollaborationView(
    const std::string&                                         subViewName,
    std::shared_ptr<Network::Collaboration::CollaborationRoom> room)
    : ISubView(subViewName), m_room(std::move(room))
{
    if ( m_room ) {
        // UI 整数值以服务当前配置为真实初值。
        m_viewportPublishRateHz =
            static_cast<int>(m_room->viewportPublishRateHz());
    }
}

/// @brief 结束协作视图生命周期并解除访客准备期的项目打开阻塞。
///
/// 只有 PendingGuestJoin 会主动设置该门；活动房间的正常解除由房间生命周期负责。
CollaborationView::~CollaborationView()
{
    if ( m_pendingGuestJoin ) {
        // 防止视图销毁后本机永久无法打开项目。
        Logic::ProjectController::instance()
            .setLocalProjectOpeningBlockedByCollaboration(false);
    }
}

/// @brief 驱动协作入口或活动房间的一帧侧栏界面。
/// @param sourceManager 用于项目状态、设置页、日志窗口和工具视图交互。
///
/// 每帧先推进等待构建指纹的房主/访客请求。活动房间显示状态、参与者和聊天；离线
/// 状态显示身份校验、开房/房间目录、封面选择和日志。房间可能在绘制中断开，因此
/// 聊天前再次检查 isActive。
/// @warning UI 热路径：每帧调用；网络和文件工作只通过显式动作或状态机请求触发。
/// @details active 与 offline
/// 两套界面互斥，但日志在两种状态都可用。离线流程返回后
/// 仍需驱动内置文件选择器，因为 ImGuiFileDialog 拥有独立跨帧窗口状态。
/// advancePendingGuestJoin 可能成功切换 room
/// 状态；本帧分支读取推进后的状态，从而
/// 无需额外延迟或固定等待即可立即显示房间内容。
void CollaborationView::onUpdate(LayoutContext&, UIManager* sourceManager)
{
    if ( !m_room ) {
        // 服务未注入时只显示只读占位，不访问任何网络状态。
        ImGui::TextDisabled("%s", TR("ui.collaboration.unavailable").data());
        return;
    }

    // 两类挂起动作在绘制按钮前推进，界面即时反映最新状态。
    advancePendingHostStart();
    advancePendingGuestJoin(sourceManager);

    if ( m_room->isActive() ) {
        // 活动房间先绘制可改变连接状态的主区。
        drawActiveRoom();
        // drawActiveRoom 可能处理断开按钮，因此聊天前复查。
        if ( m_room->isActive() ) drawChatSection();
        drawLogSection(sourceManager);
        return;
    }

    // 离线入口先验证 Creator 身份，再把结果用于按钮禁用条件。
    const bool creatorValid = drawIdentitySection(sourceManager);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    drawOfflineFlow(sourceManager, creatorValid);
    // 内置封面选择器必须在离线主控件之后持续驱动。
    renderRoomCoverFilePicker();
    drawLogSection(sourceManager);
}

/// @brief 返回协作侧栏可读内容的最小尺寸。
/// @param dpiScale 当前内容缩放。
/// @return 按至少一倍 DPI 向上取整的 340x420 逻辑尺寸。
ImVec2 CollaborationView::getMinContentSize(float dpiScale) const
{
    // 异常小于一的缩放不压缩最低可用区域。
    const float scale = std::max(1.0f, dpiScale);
    return ImVec2(std::ceil(340.0f * scale), std::ceil(420.0f * scale));
}

/// @brief 查询房卡纹理是否有待上传或待删除工作。
/// @return 任一队列非空时返回 true。
///
/// 只读取 UI 线程维护的内存容器，不执行 Base64 解码或 GPU 操作。
bool CollaborationView::needsTextureReload() const
{
    return !m_pendingRoomCoverTextures.empty() ||
           !m_roomCoverTextureRemovals.empty();
}

/// @brief 在渲染资源阶段应用房卡纹理删除并上传新图片。
/// @param physicalDevice Vulkan 物理设备。
/// @param logicalDevice Vulkan 逻辑设备。
/// @param commandPool 纹理上传命令池。
/// @param queue 纹理上传队列。
///
/// 删除始终先于上传；待上传 map
/// 整体移动到局部变量，允许本轮失败键进入失败集合。 Base64 解码和 VKTexture
/// 有效性均需通过，成功后预取 ImTextureID 并替换同键纹理。
/// @warning 低频资源路径：只在队列非空时调用，可能分配纹理并提交上传命令。
/// @details 纹理键覆盖本机房主预览和服务端目录 roomId，两类资源共用缓存但由不同
/// 入口排队。删除集合可同时撤销成功纹理和失败节流，之后同键新负载才允许重试。
///
/// pending map 在处理前整体移出成员，避免解码或 VKTexture 构造期间新的 UI
/// 请求被 当前循环误消费。每个失败只影响自身键，其余房卡继续上传。
void CollaborationView::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                       vk::Device&         logicalDevice,
                                       vk::CommandPool&    commandPool,
                                       vk::Queue&          queue)
{
    // 删除请求同时清除成功缓存与失败节流状态。
    for ( const auto& key : m_roomCoverTextureRemovals ) {
        m_roomCoverTextures.erase(key);
        m_failedRoomCoverTextures.erase(key);
    }
    // 清空成员删除集合，避免下一帧重复执行。
    m_roomCoverTextureRemovals.clear();

    // 移出本批工作，使处理中新增请求留给下一轮。
    auto pending = std::move(m_pendingRoomCoverTextures);
    m_pendingRoomCoverTextures.clear();
    for ( auto& [key, base64] : pending ) {
        // 网络和本地预览都统一从 Base64 房卡格式解码。
        auto decoded = decodeCollaborationRoomCoverImage(base64);
        if ( !decoded ) {
            // 失败键进入节流集合，避免每帧重复解码同一坏数据。
            m_failedRoomCoverTextures.insert(std::move(key));
            continue;
        }

        // 像素缓冲在构造调用内上传，纹理对象接管 GPU 资源。
        auto texture =
            std::make_unique<Graphic::VKTexture>(decoded.pixels.data(),
                                                 decoded.width,
                                                 decoded.height,
                                                 physicalDevice,
                                                 logicalDevice,
                                                 commandPool,
                                                 queue);
        if ( !texture->isValid() ) {
            // Vulkan 创建失败与解码失败使用相同重试抑制集合。
            m_failedRoomCoverTextures.insert(std::move(key));
            continue;
        }
        // 预取并注册 ImGui 纹理描述符，保证后续 drawRoomCover 可直接使用。
        static_cast<void>(texture->getImTextureID());
        // 成功上传允许该键从失败状态恢复。
        m_failedRoomCoverTextures.erase(key);
        m_roomCoverTextures.insert_or_assign(std::move(key),
                                             std::move(texture));
    }
}

/// @brief 打开用于房主房卡的图片选择器。
///
/// 初始目录依次取当前自定义封面、谱面默认封面和软件最近目录。原生选择器同步返回，
/// 内置选择器跨帧显示；两者只允许单张常见图片并在成功后保存最近父目录。
/// @warning 低频用户路径：原生选择器会阻塞 UI，只由“选择封面”按钮触发。
/// @details lastFilePickerPath
/// 只在用户确认有效图片后更新并保存，取消或对话框错误不
/// 污染最近目录。当前路径父目录优先保证连续更换封面时回到同一素材位置。
/// 内置选择器的 wasOpen
/// 检查确保重复点击或状态恢复不会重复播放打开反馈；原生窗口
/// 每次调用都是新的同步交互，因此直接播放一次。
void CollaborationView::openRoomCoverFilePicker()
{
    auto& app      = Config::AppConfig::instance();
    auto& settings = app.getEditorSettings();
    // 优先在用户当前封面所在目录重新打开。
    std::filesystem::path defaultDirectory = m_roomCoverPath.parent_path();
    if ( defaultDirectory.empty() && !m_defaultRoomCoverPath.empty() ) {
        // 未自定义时回退谱面封面目录。
        defaultDirectory = m_defaultRoomCoverPath.parent_path();
    }
    if ( defaultDirectory.empty() && !settings.lastFilePickerPath.empty() ) {
        // 再回退应用全局最近选择位置。
        defaultDirectory = Config::utf8ToPath(settings.lastFilePickerPath);
    }
    // 所有回退均为空时使用当前工作目录点号。
    const std::string defaultPath = defaultDirectory.empty()
                                        ? std::string(".")
                                        : Config::pathToUtf8(defaultDirectory);

    if ( settings.filePickerStyle == Config::FilePickerStyle::Native ) {
        // 原生对话框打开前播放统一弹窗反馈。
        PlayPopupOpenFeedback();
        nfdu8char_t*      selectedPath = nullptr;
        nfdu8filteritem_t filters[1]   = { { "Image Files",
                                             "png,jpg,jpeg,bmp,tga" } };
        const nfdresult_t result       = NativeFileDialog::openFile(
            &selectedPath, filters, 1, defaultPath.c_str());
        if ( result == NFD_OKAY && selectedPath ) {
            // 先复制平台分配字符串，再释放 NFD 内存。
            const auto path = Config::utf8ToPath(selectedPath);
            NFD_FreePathU8(selectedPath);
            // 自定义选择成功后切换默认模式并排队预览纹理。
            setRoomCoverPath(path, true);
            if ( !path.parent_path().empty() ) {
                // 最近目录属于低频用户配置，选择成功后立即保存。
                settings.lastFilePickerPath =
                    Config::pathToUtf8(path.parent_path());
                app.save();
            }
        } else if ( result == NFD_ERROR ) {
            // 用户取消不记录错误，NFD 故障写入日志。
            XERROR("Failed to open collaboration room cover picker: {}",
                   NFD_GetError() ? NFD_GetError() : "Unknown NFD error");
        }
        return;
    }

    // 内置对话框使用单选和只读文件名配置。
    IGFD::FileDialogConfig dialogConfig;
    dialogConfig.path              = defaultPath;
    dialogConfig.countSelectionMax = 1;
    dialogConfig.flags             = ImGuiFileDialogFlags_Modal |
                                     ImGuiFileDialogFlags_HideColumnType |
                                     ImGuiFileDialogFlags_ReadOnlyFileNameField;
    // 只在从关闭变为打开时播放音效，重复请求不重复反馈。
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

/// @brief 驱动内置房卡图片选择器并消费确认结果。
///
/// 只有选择器处于打开状态时调用 Display；确认后更新自定义封面并持久化最近目录，
/// 取消则只关闭窗口。原生选择器不会进入该函数。
/// @details Display 返回 true
/// 表示本轮对话框交互已结束，而不一定确认；必须在该分支 统一
/// Close。确认路径先调用 setRoomCoverPath，即使编码失败也仍可记住浏览目录。
/// 文件过滤只改善选择体验，最终图片大小与解码合法性由编码 helper 再验证。
void CollaborationView::renderRoomCoverFilePicker()
{
    if ( !ImGuiFileDialog::Instance()->IsOpened(ROOM_COVER_FILE_DIALOG_ID) ) {
        // 常见路径不创建文件对话框窗口。
        return;
    }
    if ( ImGuiFileDialog::Instance()->Display(
             ROOM_COVER_FILE_DIALOG_ID,
             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings,
             { 600.0F, 400.0F }) ) {
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            // 获取路径后复用统一编码与纹理队列入口。
            const auto path = Config::utf8ToPath(
                ImGuiFileDialog::Instance()->GetFilePathName());
            setRoomCoverPath(path, true);
            if ( !path.parent_path().empty() ) {
                // 只有有效父目录才覆盖最近路径设置。
                auto& app = Config::AppConfig::instance();
                app.getEditorSettings().lastFilePickerPath =
                    Config::pathToUtf8(path.parent_path());
                app.save();
            }
        }
        // 确认和取消都结束本轮对话框状态。
        ImGuiFileDialog::Instance()->Close();
    }
}

/// @brief 编码房卡图片并同步预览纹理队列与默认跟随模式。
/// @param path 图片文件路径；空路径表示清除房卡。
/// @param customized true 表示用户显式选择或清除，false 表示跟随谱面默认值。
///
/// 空路径清除编码内容并排队删除 GPU
/// 纹理。编码失败时自定义选择保留此前成功预览，
/// 自动跟随失败则清空无效默认资源。成功结果保存
/// Base64，并取消同键删除和失败状态。
/// @warning
/// 低频路径：非空路径会读取、解码并重新编码图片，只由选择或默认变化触发。
/// @details customized 参数同时决定错误恢复和默认模式：用户选择失败时保留上一个
/// 可用预览，便于继续开房；谱面默认失败时清空预览，避免房卡与当前谱面不一致。
///
/// 成功编码的 Base64 是实际网络配置负载，GPU 纹理只负责本机视觉缓存。两者状态
/// 分离使 Vulkan 资源失败不会改写待发送的房间封面内容。
void CollaborationView::setRoomCoverPath(const std::filesystem::path& path,
                                         bool customized)
{
    // 每次尝试先清除旧可见错误，失败分支再写入具体翻译键。
    m_roomCoverErrorKey.clear();
    if ( path.empty() ) {
        // 空选择同时清除路径和待发送 Base64 内容。
        m_roomCoverPath.clear();
        m_roomCoverImage.clear();
        // 用户清除进入 Custom，自动无默认值继续保持 Follow。
        m_roomCoverDefaultMode = customized ? CollaborationDefaultMode::Custom
                                            : CollaborationDefaultMode::Follow;
        // 删除尚未上传的同键请求，防止清除后又创建旧纹理。
        if ( const auto pending =
                 m_pendingRoomCoverTextures.find(HOST_ROOM_COVER_TEXTURE_KEY);
             pending != m_pendingRoomCoverTextures.end() ) {
            m_pendingRoomCoverTextures.erase(pending);
        }
        // 已上传纹理由下一次资源准备阶段安全销毁。
        m_roomCoverTextureRemovals.emplace(HOST_ROOM_COVER_TEXTURE_KEY);
        return;
    }

    // 编码 helper 负责尺寸、格式和负载上限校验。
    const auto result = encodeCollaborationRoomCoverImage(path);
    if ( result.error != CollaborationRoomCoverImageError::None ) {
        // 错误枚举映射为 UI 翻译键，不直接展示底层实现细节。
        m_roomCoverErrorKey = roomCoverErrorTranslationKey(result.error);
        if ( !customized ) {
            // 跟随谱面默认失败时清除旧默认预览，避免展示错误图片。
            m_roomCoverPath.clear();
            m_roomCoverImage.clear();
            // 同时取消待上传并请求移除已存在纹理。
            if ( const auto pending = m_pendingRoomCoverTextures.find(
                     HOST_ROOM_COVER_TEXTURE_KEY);
                 pending != m_pendingRoomCoverTextures.end() ) {
                m_pendingRoomCoverTextures.erase(pending);
            }
            m_roomCoverTextureRemovals.emplace(HOST_ROOM_COVER_TEXTURE_KEY);
        }
        // 自定义失败保留上一次成功封面，用户可重新选择。
        return;
    }

    // 成功结果作为开房配置和本地预览的共同来源。
    m_roomCoverPath        = path;
    m_roomCoverImage       = result.base64;
    m_roomCoverDefaultMode = customized ? CollaborationDefaultMode::Custom
                                        : CollaborationDefaultMode::Follow;
    // 新上传覆盖删除请求，保证同一键最终状态为存在。
    if ( const auto removal =
             m_roomCoverTextureRemovals.find(HOST_ROOM_COVER_TEXTURE_KEY);
         removal != m_roomCoverTextureRemovals.end() ) {
        m_roomCoverTextureRemovals.erase(removal);
    }
    // 新有效数据允许此前失败键重新尝试上传。
    if ( const auto failed =
             m_failedRoomCoverTextures.find(HOST_ROOM_COVER_TEXTURE_KEY);
         failed != m_failedRoomCoverTextures.end() ) {
        m_failedRoomCoverTextures.erase(failed);
    }
    // insert_or_assign 合并连续选择，只保留最新 Base64。
    m_pendingRoomCoverTextures.insert_or_assign(
        std::string(HOST_ROOM_COVER_TEXTURE_KEY), result.base64);
}

/// @brief 为房间目录中的远端封面排队一次纹理上传。
/// @param key 房间 ID 或其他稳定纹理键。
/// @param base64 已获取的房卡图片负载。
///
/// 已成功、已排队或已失败的键均跳过，避免可见房间卡每帧重复解码或上传。目录更新
/// 若要重试必须先通过删除集合清除对应状态。
/// @details failed 集合是有界目录会话内的失败记忆，防止坏 Base64
/// 在可见房卡上每帧 重复解码。目录资源更新或房间移除应通过 removal
/// 流程释放该记忆，再允许新内容上传。 函数不验证 Base64，本批统一在 Vulkan
/// 资源准备阶段解码。
void CollaborationView::queueRoomCoverTexture(std::string      key,
                                              std::string_view base64)
{
    if ( key.empty() || base64.empty() || m_roomCoverTextures.contains(key) ||
         m_pendingRoomCoverTextures.contains(key) ||
         m_failedRoomCoverTextures.contains(key) ) {
        // 空输入和任何已知终态都不创建重复工作。
        return;
    }
    // Base64 从 string_view 复制进成员 map，生命周期跨到资源准备阶段。
    m_pendingRoomCoverTextures.emplace(std::move(key), base64);
}

/// @brief 绘制指定缓存键的房卡纹理或 MMM 占位图。
/// @param textureKey 本地预览键或远端房间 ID。
/// @param size 期望的屏幕尺寸。
///
/// InvisibleButton 建立布局和统一 item
/// 矩形；纹理存在时铺满矩形，否则绘制主题背景
/// 与居中占位文字，最后始终加边框。函数不触发上传或网络请求。
/// @warning UI 热路径：房间卡可见时调用，只查询纹理 map 并追加绘制命令。
/// @details 调用方在本函数后仍可对 InvisibleButton 使用
/// IsItemHovered，因此预览路径 tooltip
/// 和房卡交互无需额外覆盖控件。绘制命令直接写当前窗口 DrawList，不改变
/// 光标位置之外的布局状态。
///
/// 图片当前按矩形拉伸到 16:9 卡片区域，裁切或保持比例由编码阶段预处理约束负责。
void CollaborationView::drawRoomCover(std::string_view textureKey, ImVec2 size)
{
    // 两轴至少一个像素，避免 ImGui 产生退化 item。
    size.x = std::max(size.x, 1.0F);
    size.y = std::max(size.y, 1.0F);
    // 隐形 item 让调用方可使用 hover、tooltip 和布局结果。
    ImGui::InvisibleButton("##RoomCoverImage", size);
    const ImVec2 minimum  = ImGui::GetItemRectMin();
    const ImVec2 maximum  = ImGui::GetItemRectMax();
    auto*        drawList = ImGui::GetWindowDrawList();
    // 圆角与当前 Frame 样式保持一致。
    const float rounding = ImGui::GetStyle().FrameRounding;

    // VKTexture 由资源准备阶段拥有和填充。
    const auto texture = m_roomCoverTextures.find(textureKey);
    if ( texture != m_roomCoverTextures.end() && texture->second ) {
        drawList->AddImage(texture->second->getImTextureID(), minimum, maximum);
    } else {
        // 缺图、下载中或上传失败统一显示轻量占位。
        drawList->AddRectFilled(
            minimum, maximum, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
        // 占位文字按 item 矩形几何居中。
        const char*  placeholder = "MMM";
        const ImVec2 textSize    = ImGui::CalcTextSize(placeholder);
        drawList->AddText(ImVec2((minimum.x + maximum.x - textSize.x) * 0.5F,
                                 (minimum.y + maximum.y - textSize.y) * 0.5F),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled),
                          placeholder);
    }
    // 边框覆盖图片和占位两种路径，保持房卡边界清晰。
    drawList->AddRect(
        minimum, maximum, ImGui::GetColorU32(ImGuiCol_Border), rounding);
}

/// @brief 展示当前 Creator 身份并提供缺失时的设置入口。
/// @param sourceManager 用于打开软件设置页。
/// @return 规范化 Creator 非空时返回 true。
///
/// Creator 是开房和加入请求的用户身份前置条件；这里只读取默认设置并规范化，不在
/// 协作面板内直接编辑。无效状态以警告和跳转按钮呈现。
/// @details normalizeCreatorIdentity
/// 会处理空白等无效输入，面板展示的也是规范化结果； 网络配置仍从 AppConfig
/// 读取原始值，由网络层按同一规则建立身份。 设置按钮仅在 UIManager
/// 有效时可执行，缺少管理器仍返回 false 并禁用连接入口。
bool CollaborationView::drawIdentitySection(UIManager* sourceManager)
{
    // 使用与网络配置构造相同的规范化规则判断空身份。
    const auto creator = Config::normalizeCreatorIdentity(
        Config::AppConfig::instance().getEditorSettings().defaultCreator);
    ImGui::TextUnformatted(TR("ui.collaboration.identity").data());
    ImGui::SameLine();
    if ( creator.empty() ) {
        // 警告色与其他协作前置条件保持一致。
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                           "%s",
                           TR("ui.collaboration.creator_missing").data());
        ImGui::TextWrapped("%s",
                           TR("ui.collaboration.creator_required").data());
        if ( FeedbackButton(
                 TR("ui.collaboration.open_creator_settings").data()) &&
             sourceManager ) {
            // Creator 位于软件设置页，由 UIManager 统一打开窗口。
            sourceManager->openSettingsWindow(Event::SettingsTab::Software);
        }
        return false;
    }

    // 有效身份使用主题选中色突出显示。
    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextSelectedBg),
                       "%s",
                       creator.c_str());
    return true;
}

/// @brief 绘制未连接状态下的开房配置和在线房间目录。
/// @param sourceManager 用于项目状态校验和访客加入准备。
/// @param creatorValid 当前默认 Creator 是否满足身份要求。
///
/// 房主区跟随活动谱面的默认房名和封面，允许用户切换为自定义值，并在构建指纹就绪
/// 后启动房间。访客区刷新服务端目录、虚拟化房卡列表，并在点击加入后进入本机状态
/// 关闭与指纹等待流程。
/// @warning UI
/// 热路径：每帧绘制内存房间目录；封面编码和网络请求仅由状态变化或按钮触发。
/// @details 房间名称的 Follow 模式以谱面 title 为首选、内部 name 为回退；Custom
/// 模式在活动谱面切换后仍保留用户输入。封面遵循相同模式，但其默认来源还会排除
/// 视频背景，并在默认图片变化时重新编码本地预览。
///
/// 开房按钮把房名、封面、身份、endpoint 和指纹策略冻结成配置。指纹尚未生成时只
/// 建立 PendingHostStart，不在 UI
/// 线程计算摘要；项目或活动谱面随后失效会取消请求。
///
/// 房间目录使用
/// ImGuiListClipper，只有可见卡片会查询封面负载和排队纹理。加入按钮
/// 的禁用条件同时覆盖容量、身份、指纹、挂起动作和本机项目策略。
void CollaborationView::drawOfflineFlow(UIManager* sourceManager,
                                        bool       creatorValid)
{
    // 当前活动会话和项目共同决定能否作为房主。
    auto&       engine        = Logic::EditorEngine::instance();
    auto        activeSession = engine.getActiveNonLogoSession();
    const auto* project       = engine.getCurrentProject();
    // UIManager 项目状态与逻辑项目指针必须同时有效。
    const bool hasProject =
        sourceManager && sourceManager->hasActiveProjectUiState() && project;
    // 开房额外要求活动非 Logo 会话已装载 Beatmap。
    const bool hostReady = hasProject && activeSession &&
                           activeSession->getContext().currentBeatmap;
    // 指纹失败会同时禁用房主和访客入口。
    const bool fingerprintFailed =
        Network::Collaboration::collaborationBuildFingerprintState() ==
        Network::Collaboration::CollaborationBuildFingerprintState::Failed;
    if ( !m_room->lastError().empty() ) {
        // 上一个房间结束原因保留在离线入口顶部供用户理解。
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
        // 活动谱面元数据是 Follow 模式的默认房间信息来源。
        const auto& metadata =
            activeSession->getContext().currentBeatmap->m_baseMapMetadata;
        const std::string_view defaultRoomName =
            !metadata.title.empty() ? std::string_view(metadata.title)
                                    : std::string_view(metadata.name);
        if ( defaultRoomName != m_defaultRoomName ) {
            // 仅默认值变化且仍处于 Follow 时覆盖输入框。
            m_defaultRoomName.assign(defaultRoomName);
            if ( shouldFollowCollaborationDefault(m_roomNameDefaultMode) ) {
                setInputBuffer(m_roomName, m_defaultRoomName);
            }
        }

        // 封面默认值优先独立 cover，再回退图片背景。
        const auto defaultCover =
            resolveDefaultRoomCoverPath(metadata, project);
        if ( defaultCover != m_defaultRoomCoverPath ) {
            // Custom 模式不随谱面切换覆盖用户选择。
            m_defaultRoomCoverPath = defaultCover;
            if ( shouldFollowCollaborationDefault(m_roomCoverDefaultMode) ) {
                setRoomCoverPath(defaultCover, false);
            }
        }
    }
    if ( fingerprintFailed ) {
        // 构建指纹失败属于本机准备问题，不能发起兼容性未知的连接。
        ImGui::TextColored(
            ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
            "%s",
            TR("ui.collaboration.build_fingerprint_failed").data());
    }

    // 房主和目录区域默认展开，降低首次使用的发现成本。
    const ImGuiTreeNodeFlags headerFlags = ImGuiTreeNodeFlags_DefaultOpen;
    if ( FeedbackCollapsingHeader(TR("ui.collaboration.host_room").data(),
                                  headerFlags) ) {
        // 封面标签旁的帮助标记解释房卡用途和限制。
        ImGui::TextDisabled("%s", TR("ui.collaboration.room_cover").data());
        ImGui::SameLine();
        drawHelpMarker(TR("ui.collaboration.host_desc").data());
        if ( !hasProject ) {
            // 开房必须依赖本机项目与活动谱面。
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                               "%s",
                               TR("ui.collaboration.project_required").data());
        }

        // 本地预览使用稳定 ID 隔离同窗口内的其他 InvisibleButton。
        ImGui::PushID("HostRoomCoverPreview");
        // 房卡按内容区宽度绘制 16:9 比例。
        const float coverWidth = ImGui::GetContentRegionAvail().x;
        drawRoomCover(HOST_ROOM_COVER_TEXTURE_KEY,
                      ImVec2(coverWidth, coverWidth * 9.0F / 16.0F));
        if ( ImGui::IsItemHovered() && !m_roomCoverPath.empty() ) {
            // tooltip 展示完整路径，主界面只保留图像预览。
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(Config::pathToUtf8(m_roomCoverPath).c_str());
            ImGui::EndTooltip();
        }
        ImGui::PopID();

        // 三等分按钮分别选择、恢复谱面默认和清除封面。
        if ( ImGui::BeginTable("CollaborationRoomCoverActions",
                               3,
                               ImGuiTableFlags_SizingStretchSame) ) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if ( FeedbackSmallButton(
                     TR("ui.collaboration.cover_choose").data()) ) {
                // 选择器结果会进入 Custom 模式。
                openRoomCoverFilePicker();
            }
            ImGui::TableSetColumnIndex(1);
            if ( FeedbackSmallButton(
                     TR("ui.collaboration.cover_use_beatmap").data()) ) {
                // 显式切回 Follow 后立即应用当前默认路径。
                m_roomCoverDefaultMode = CollaborationDefaultMode::Follow;
                setRoomCoverPath(m_defaultRoomCoverPath, false);
            }
            ImGui::TableSetColumnIndex(2);
            // 没有编码图片时清除按钮无有效动作。
            ImGui::BeginDisabled(m_roomCoverImage.empty());
            if ( FeedbackSmallButton(
                     TR("ui.collaboration.cover_clear").data()) ) {
                setRoomCoverPath({}, true);
            }
            ImGui::EndDisabled();
            ImGui::EndTable();
        }
        if ( !m_roomCoverErrorKey.empty() ) {
            // 编码错误通过翻译键映射为当前语言提示。
            ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                               "%s",
                               TR(m_roomCoverErrorKey.c_str()).data());
        }

        // 房间名默认跟随活动谱面标题，按钮可从 Custom 回到 Follow。
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
            // 输入值等于当前默认值时自动恢复 Follow，否则标记 Custom。
            m_roomNameDefaultMode = resolveCollaborationTextDefaultMode(
                std::string_view(m_roomName.data()), m_defaultRoomName);
        }
        // 指纹要求决定服务端是否拒绝不同构建的访客。
        FeedbackCheckbox(TR("ui.collaboration.require_matching_build").data(),
                         &m_requireMatchingBuildFingerprint);
        ImGui::SameLine();
        drawHelpMarker(
            TR("ui.collaboration.require_matching_build_desc").data());
        if ( m_pendingHostStart ) {
            // 配置已冻结，只等待后台指纹生成，不允许重复点击。
            ImGui::TextColored(
                ImGui::GetStyleColorVec4(ImGuiCol_TextSelectedBg),
                "%s",
                TR("ui.collaboration.build_fingerprint_calculating").data());
        }
        // 开房需要无挂起动作、有效身份/指纹、非空名称和完整本机项目。
        ImGui::BeginDisabled(
            m_pendingHostStart || m_pendingGuestJoin || !creatorValid ||
            fingerprintFailed || m_roomName[0] == '\0' ||
            !isCollaborationProjectRequirementSatisfied(true, hostReady));
        if ( FeedbackButton(TR("ui.collaboration.start_room").data(),
                            ImVec2(-1.0F, 0.0F)) ) {
            // 点击时按值冻结全部房主配置，后续输入变化不会影响请求。
            Network::Collaboration::CollaborationHostRoomConfig config;
            config.creator = Config::AppConfig::instance()
                                 .getEditorSettings()
                                 .defaultCreator;
            // 稳定参与者 ID 来自应用配置，不使用可变 Creator 作为身份键。
            config.participantId =
                Config::AppConfig::instance().getCollaborationParticipantId();
            config.roomName       = m_roomName.data();
            config.roomCoverImage = m_roomCoverImage;
            // endpoint 来自房间服务当前服务器配置。
            config.endpoint = m_room->serverEndpoint();
            config.requireMatchingBuildFingerprint =
                m_requireMatchingBuildFingerprint;
            config.buildFingerprint =
                Network::Collaboration::collaborationBuildFingerprint();
            if ( config.buildFingerprint.empty() ) {
                // 指纹未就绪时跨帧保存配置，而不是阻塞 UI 等待。
                auto pending       = std::make_unique<PendingHostStart>();
                pending->config    = std::move(config);
                m_pendingHostStart = std::move(pending);
            } else {
                // 已缓存指纹时可立即提交开房请求。
                static_cast<void>(m_room->startHost(std::move(config)));
            }
        }
        ImGui::EndDisabled();
    }

    // 在线目录与房主配置分区，允许单独折叠。
    ImGui::Spacing();
    if ( FeedbackCollapsingHeader(TR("ui.collaboration.online_rooms").data(),
                                  headerFlags) ) {
        drawHelpMarker(TR("ui.collaboration.join_desc").data());
        ImGui::SameLine();
        if ( FeedbackSmallButton(
                 TR("ui.collaboration.refresh_rooms").data()) ) {
            // 网络刷新由房间服务异步执行，UI 不等待结果。
            static_cast<void>(m_room->refreshDirectory());
        }
        if ( m_pendingGuestJoin ) {
            // 两阶段准备分别显示指纹计算和本机状态关闭。
            ImGui::TextColored(
                ImGui::GetStyleColorVec4(ImGuiCol_TextSelectedBg),
                "%s",
                !m_pendingGuestJoin->closeRequested
                    ? TR("ui.collaboration.build_fingerprint_calculating")
                          .data()
                    : TR("ui.collaboration.closing_local_state").data());
        } else if ( m_guestJoinPreparationCancelled ) {
            // 用户取消本机关闭后保留一次明确反馈。
            ImGui::TextColored(
                ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                "%s",
                TR("ui.collaboration.local_close_cancelled").data());
        }
        // directoryRooms 是网络线程发布的只读快照。
        const auto& rooms = m_room->directoryRooms();
        if ( rooms.empty() ) {
            // 空目录直接结束离线绘制，日志仍由 onUpdate 后续调用。
            ImGui::TextDisabled("%s", TR("ui.collaboration.no_rooms").data());
            return;
        }

        // 房卡高度按 DPI 固定，供 ImGuiListClipper 精确虚拟化。
        const float dpiScale = std::max(
            1.0F, Config::AppConfig::instance().getWindowContentScale());
        const float cardHeight = 92.0F * dpiScale;
        // 只绘制当前滚动视口覆盖的房间卡。
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rooms.size()),
                      cardHeight + ImGui::GetStyle().ItemSpacing.y);
        while ( clipper.Step() ) {
            for ( int index = clipper.DisplayStart; index < clipper.DisplayEnd;
                  ++index ) {
                const auto& room = rooms[static_cast<std::size_t>(index)];
                // roomId 隔离每张卡内部相同控件名称。
                ImGui::PushID(room.roomId.c_str());
                // 卡片本身不滚动，外层目录负责整体滚动。
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
                    // 左列固定封面宽度，右列吸收房名和按钮剩余空间。
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
                        // 目录元数据先报告存在性，实际 Base64 可能尚未下载。
                        const auto cover =
                            m_room->directoryRoomCover(room.roomId);
                        if ( cover.empty() ) {
                            // 缺少负载时向服务请求一次，房间层负责去重。
                            static_cast<void>(
                                m_room->requestDirectoryRoomCover(room.roomId));
                        } else {
                            // 已下载负载进入 GPU 纹理去重队列。
                            queueRoomCoverTexture(room.roomId, cover);
                        }
                    }
                    // 无封面或上传未完成时 drawRoomCover 自动显示占位。
                    drawRoomCover(
                        room.roomId,
                        ImVec2(coverWidth, coverWidth * 9.0F / 16.0F));

                    // 详情区显示房间名、房主和容量摘要。
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s", room.roomName.c_str());
                    ImGui::TextDisabled(
                        "%s  ·  %u/%u",
                        room.hostCreator.c_str(),
                        static_cast<unsigned int>(room.participants),
                        static_cast<unsigned int>(room.capacity));
                    // 达到容量时按钮显示满员并禁用。
                    const bool full = room.participants >= room.capacity;
                    // 访客加入还要求身份、指纹和本地项目策略全部满足。
                    ImGui::BeginDisabled(
                        m_pendingHostStart || m_pendingGuestJoin ||
                        fingerprintFailed || !creatorValid || full ||
                        !isCollaborationProjectRequirementSatisfied(
                            false, hasProject));
                    if ( FeedbackButton(
                             full ? TR("ui.collaboration.room_full").data()
                                  : TR("ui.collaboration.join_now").data(),
                             ImVec2(-1.0F, 0.0F)) ) {
                        // 配置在 beginGuestJoin 中冻结并进入非阻塞准备状态机。
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

/// @brief 冻结访客连接配置并启动本机状态准备流程。
/// @param creator 当前默认 Creator 展示名。
/// @param roomId 目录中目标房间的稳定 ID。
/// @param roomName 目标房间显示名。
/// @param sourceManager 用于立即推进一次项目关闭状态。
///
/// 同一时刻只允许一个开房或加入请求。配置包含稳定参与者 ID、服务端 endpoint 和
/// 专用资源缓存根；建立 PendingGuestJoin
/// 后阻止用户打开新本机项目，直到加入成功、 失败或准备取消。
/// @details
/// 资源缓存根固定在应用配置目录而非项目目录，使访客可在本机项目关闭后继续
/// 下载并校验房主资源，也避免临时文件污染用户项目。目录创建与缓存维护由网络层负责。
///
/// 设置项目打开阻塞必须早于首次推进状态机；即使当前无需关闭本机状态，join 失败
/// 分支也会显式解除。视图析构则覆盖仍处于 PendingGuestJoin 的提前退出情况。
void CollaborationView::beginGuestJoin(std::string creator, std::string roomId,
                                       std::string roomName,
                                       UIManager*  sourceManager)
{
    if ( !m_room || m_pendingHostStart || m_pendingGuestJoin ||
         m_room->isActive() ) {
        // 服务缺失、已有请求或活动连接都拒绝重复准备。
        return;
    }

    // unique_ptr 明确表示最多存在一个跨帧访客准备状态。
    auto pending = std::make_unique<PendingGuestJoin>();
    // 字符串移动进配置，避免复制目录快照中的较长字段。
    pending->config.creator = std::move(creator);
    // 参与者 ID 独立于可修改 Creator，保证重连身份稳定。
    pending->config.participantId =
        Config::AppConfig::instance().getCollaborationParticipantId();
    pending->config.roomId   = std::move(roomId);
    pending->config.roomName = std::move(roomName);
    // endpoint 在点击时冻结，等待期间服务器设置变化不影响本次请求。
    pending->config.endpoint = m_room->serverEndpoint();
    // 协作下载使用配置根下独立缓存，不写入当前本机项目。
    pending->config.resourceCacheRoot =
        Config::AppPaths::configRootPath() / "collaboration-cache";
    m_pendingGuestJoin = std::move(pending);
    // 新请求清除上一次用户取消关闭的提示。
    m_guestJoinPreparationCancelled = false;

    // 准备期间阻止并发打开本地项目，避免关闭条件永远追不上。
    Logic::ProjectController::instance()
        .setLocalProjectOpeningBlockedByCollaboration(true);
    // 点击帧立即推进，若指纹和本机状态已就绪可直接加入。
    advancePendingGuestJoin(sourceManager);
}

/// @brief 非阻塞推进访客加入的指纹、项目关闭和连接阶段。
/// @param sourceManager 当前 UI 管理器，用于项目关闭请求相关上下文。
///
/// 第一阶段等待后台构建指纹；第二阶段检查本机项目和谱面画布，必要时只请求一次关闭；
/// 状态完全清空后移动配置调用
/// room.join。若关闭请求不再挂起但本地状态仍存在，宽限
/// 期后视为用户取消并解除项目打开阻塞。
/// @warning UI 热路径：每帧轮询内存状态，不 sleep、不阻塞等待、不执行文件扫描。
/// @details 指纹阶段只读取后台缓存，Failed 会结束请求，Calculating
/// 会立即返回。取得 指纹后，每帧同时观察 ProjectController 当前项目与
/// EditorEngine 非 Logo 会话， 两者都清空才满足加入条件。
///
/// requestCloseProject 可能打开保存确认，状态机不模拟用户选择。若控制器不再报告
/// 挂起动作而本机状态仍在，短宽限期后标记 cancelled；宽限通过 steady_clock
/// 比较， 没有 sleep 或阻塞式 wait。
void CollaborationView::advancePendingGuestJoin(UIManager* sourceManager)
{
    // 没有完整准备对象和房间服务时无状态可推进。
    if ( !m_pendingGuestJoin || !m_room ) return;

    // 指纹由后台服务生成，空字符串表示仍未就绪或已失败。
    const auto fingerprint =
        Network::Collaboration::collaborationBuildFingerprint();
    if ( fingerprint.empty() ) {
        if ( Network::Collaboration::collaborationBuildFingerprintState() ==
             Network::Collaboration::CollaborationBuildFingerprintState::
                 Failed ) {
            // 明确失败时取消请求并恢复本地项目打开能力。
            m_pendingGuestJoin.reset();
            Logic::ProjectController::instance()
                .setLocalProjectOpeningBlockedByCollaboration(false);
        }
        // 计算中保持挂起，下一帧继续查询。
        return;
    }
    // 一旦取得便写入冻结配置，后续阶段不再依赖全局缓存。
    m_pendingGuestJoin->config.buildFingerprint = fingerprint;

    // 项目控制器和实际非 Logo 画布共同描述需关闭的本机状态。
    auto&      projectController = Logic::ProjectController::instance();
    const bool hasProject       = projectController.currentProject() != nullptr;
    const bool hasBeatmapCanvas = hasNonLogoBeatmapSession();
    if ( !needsLocalStateCloseBeforeGuestJoin(hasProject, hasBeatmapCanvas) ) {
        // 清除遗留切换请求，防止加入后又打开本地项目。
        projectController.cancelPendingProjectSwitch();
        // join 接管完整配置，先结束 Pending 防止回调重入重复提交。
        auto config = std::move(m_pendingGuestJoin->config);
        m_pendingGuestJoin.reset();
        if ( m_room->join(std::move(config)) ) {
            // 成功进入房间后由协作生命周期继续保持项目打开阻塞。
            return;
        }
        // 同步提交失败时确保房间回到离线干净状态。
        m_room->disconnect();
        projectController.setLocalProjectOpeningBlockedByCollaboration(false);
        return;
    }

    if ( !m_pendingGuestJoin->closeRequested ) {
        // 关闭请求只发一次，避免每帧重复覆盖用户确认状态。
        m_pendingGuestJoin->closeRequested = true;
        // 单调时间记录请求完成后的短宽限期起点。
        m_pendingGuestJoin->closeRequestedAt = std::chrono::steady_clock::now();
        // ProjectController 自行驱动保存确认和关闭流程。
        projectController.requestCloseProject();
        return;
    }

    if ( projectController.hasPendingProjectAction() ||
         projectController.hasPendingProjectSwitch() ) {
        // 保存或关闭交互仍在进行时继续等待，不占用线程。
        return;
    }

    // 宽限一个短 UI 周期，让会话清理事件完成发布和消费。
    constexpr auto CLOSE_RESULT_GRACE_PERIOD = std::chrono::seconds(1);
    if ( std::chrono::steady_clock::now() -
             m_pendingGuestJoin->closeRequestedAt <
         CLOSE_RESULT_GRACE_PERIOD ) {
        // 仅比较 steady_clock，不依赖系统时间调整。
        return;
    }

    // 无挂起动作但本地状态仍在，解释为用户取消了关闭。
    m_pendingGuestJoin.reset();
    m_guestJoinPreparationCancelled = true;
    // 取消后允许用户继续打开或操作本机项目。
    projectController.setLocalProjectOpeningBlockedByCollaboration(false);
}

/// @brief 在构建指纹就绪后提交挂起的房主开房请求。
///
/// 等待期间持续验证当前项目、活动会话和 Beatmap
/// 仍存在；前置条件失效或指纹失败会
/// 取消挂起。成功取得指纹后移动冻结配置并只调用一次 startHost。
/// @warning UI 热路径：存在挂起请求时每帧只查询内存状态。
/// @details
/// 房主等待不需要关闭本机状态，因为活动谱面正是房间初始数据来源。等待期间
/// 仍逐帧验证项目和 Beatmap，防止提交保存了陈旧资源引用的配置。
///
/// 构建指纹一旦可用便移动到冻结配置，先清除 pending 再调用 startHost，确保同步
/// 回调或下一帧不会重复发起同一开房请求。
void CollaborationView::advancePendingHostStart()
{
    // 无请求或服务已销毁时不推进。
    if ( !m_pendingHostStart || !m_room ) return;

    // 开房必须继续绑定当前本机项目和活动谱面。
    auto&       engine        = Logic::EditorEngine::instance();
    auto        activeSession = engine.getActiveNonLogoSession();
    const auto* project       = engine.getCurrentProject();
    if ( !project || !activeSession ||
         !activeSession->getContext().currentBeatmap ) {
        // 等待期间关闭项目时废弃已冻结配置。
        m_pendingHostStart.reset();
        return;
    }

    // 从共享后台缓存读取构建指纹。
    auto fingerprint = Network::Collaboration::collaborationBuildFingerprint();
    if ( fingerprint.empty() ) {
        if ( Network::Collaboration::collaborationBuildFingerprintState() ==
             Network::Collaboration::CollaborationBuildFingerprintState::
                 Failed ) {
            // 失败状态不可恢复地结束本次点击请求。
            m_pendingHostStart.reset();
        }
        // 仍在计算时保留请求到下一帧。
        return;
    }

    // 在清空 pending 前移动配置，确保 startHost 只提交一次。
    auto config             = std::move(m_pendingHostStart->config);
    config.buildFingerprint = std::move(fingerprint);
    m_pendingHostStart.reset();
    // 房间服务异步推进连接，UI 不等待返回后的网络结果。
    static_cast<void>(m_room->startHost(std::move(config)));
}

/// @brief 绘制已连接房间的详情、请求、同步设置、参与者和断开操作。
///
/// 房间详情对所有成员只读；加入请求与权限管理仅房主可见。资源同步进度来自房间
/// 快照，视野发布率在滑块编辑结束时提交，渲染模式属于本机设置并立即持久化。
/// 断开按钮可能使 room 在本帧转为非活动，调用方会在进入聊天前再次检查。
/// @warning UI
/// 热路径：活动房间每帧执行，只读取内存快照；网络写入由明确控件触发。
/// @details 加入申请循环先记录
/// requestId，结束表格遍历后才调用批准或拒绝接口，避免 房间服务同步更新
/// pendingJoinRequests 导致正在遍历的引用失效。同一帧批准优先。
///
/// 资源进度在准备和缓存比较阶段按文件数计算，在下载等阶段按字节数计算，最终夹到
/// 0..1。Ready 阶段改为显示完成与缓存命中摘要，不保留满进度条。
///
/// 参与者表把本地 Peer 固定放在首行，远端随后按快照顺序绘制；访客额外看到自己
/// 权限的只读摘要，房主则在每个远端行内管理权限和移除成员。
void CollaborationView::drawActiveRoom()
{
    // 样式引用用于计算表格标签和动作列宽度。
    const auto& style = ImGui::GetStyle();
    // 主要信息分区默认展开。
    const ImGuiTreeNodeFlags headerFlags = ImGuiTreeNodeFlags_DefaultOpen;

    if ( FeedbackCollapsingHeader(TR("ui.collaboration.room_details").data(),
                                  headerFlags) ) {
        // 两列表格让本地化标签与可变长度值保持对齐。
        if ( ImGui::BeginTable("CollaborationRoomDetailsTable",
                               2,
                               ImGuiTableFlags_SizingStretchProp) ) {
            // 以状态和房名标签较宽者确定固定标签列。
            const float labelWidth = std::max(
                ImGui::CalcTextSize(TR("ui.collaboration.status").data()).x,
                ImGui::CalcTextSize(TR("ui.collaboration.room_name").data()).x);
            ImGui::TableSetupColumn("##RoomDetailLabel",
                                    ImGuiTableColumnFlags_WidthFixed,
                                    labelWidth + style.ItemSpacing.x);
            ImGui::TableSetupColumn("##RoomDetailValue",
                                    ImGuiTableColumnFlags_WidthStretch);
            // 状态文本由枚举映射 helper 本地化。
            drawRoomInfoLabel(TR("ui.collaboration.status").data());
            ImGui::TextUnformatted(roomStateText(m_room->state()));
            // 角色根据当前 room 服务身份即时显示。
            drawRoomInfoLabel(TR("ui.collaboration.role").data());
            ImGui::TextUnformatted(
                m_room->isHost() ? TR("ui.collaboration.role.host").data()
                                 : TR("ui.collaboration.role.guest").data());
            // 房间名允许换行，适应窄侧栏。
            drawRoomInfoLabel(TR("ui.collaboration.room_name").data());
            ImGui::TextWrapped("%s", m_room->roomName().c_str());
            // 本地 PeerId 用数值形式供连接诊断。
            drawRoomInfoLabel(TR("ui.collaboration.peer_id").data());
            ImGui::Text("#%llu",
                        static_cast<unsigned long long>(m_room->localPeerId()));
            if ( !m_room->roomId().empty() ) {
                // 只有服务端已分配房间 ID 后才显示该行。
                drawRoomInfoLabel(TR("ui.collaboration.room_id").data());
                ImGui::TextWrapped("%s", m_room->roomId().c_str());
            }
            ImGui::EndTable();
        }
        if ( !m_room->lastError().empty() ) {
            // 活动状态中的非致命错误仍在详情区展示。
            ImGui::TextWrapped("%s: %s",
                               TR("ui.collaboration.error").data(),
                               m_room->lastError().c_str());
        }
    }

    if ( m_room->isHost() ) {
        // 访客不接收或审批其他人的加入请求。
        ImGui::Spacing();
        if ( FeedbackCollapsingHeader(
                 TR("ui.collaboration.join_requests").data(), headerFlags) ) {
            // pendingJoinRequests 是房间服务发布的只读快照。
            const auto& requests = m_room->pendingJoinRequests();
            if ( requests.empty() ) {
                // 空状态保留分区结构并提供明确反馈。
                ImGui::TextDisabled(
                    "%s", TR("ui.collaboration.no_join_requests").data());
            } else {
                const ImGuiTableFlags tableFlags =
                    ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH |
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
                // 操作先记录 requestId，表格遍历完成后再调用房间服务。
                std::string approveRequest;
                std::string rejectRequest;
                if ( ImGui::BeginTable(
                         "CollaborationJoinRequestsTable", 2, tableFlags) ) {
                    // 动作列宽度恰好容纳批准、拒绝按钮及间距。
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
                        // requestId 隔离不同请求行的相同按钮标签。
                        ImGui::PushID(request.requestId.c_str());
                        ImGui::TableNextRow(ImGuiTableRowFlags_None,
                                            ImGui::GetFrameHeight());
                        ImGui::TableSetColumnIndex(0);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(request.creator.c_str());
                        ImGui::TableSetColumnIndex(1);
                        if ( FeedbackSmallButton(
                                 TR("ui.collaboration.approve").data()) ) {
                            // 延迟执行避免网络回调修改正在遍历的请求集合。
                            approveRequest = request.requestId;
                        }
                        ImGui::SameLine();
                        if ( FeedbackSmallButton(
                                 TR("ui.collaboration.reject").data()) ) {
                            // 同一帧最终只处理一个动作。
                            rejectRequest = request.requestId;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                if ( !approveRequest.empty() ) {
                    // 批准优先于拒绝，符合表格中只触发一个按钮的常规路径。
                    static_cast<void>(
                        m_room->approveJoinRequest(approveRequest));
                } else if ( !rejectRequest.empty() ) {
                    // 房间服务负责向申请者发送拒绝结果。
                    static_cast<void>(m_room->rejectJoinRequest(rejectRequest));
                }
            }
        }
    }

    // 资源进度快照同时供状态表和可选进度条使用。
    const auto resource = m_room->resourceProgress();
    using ResourcePhase =
        Network::Collaboration::CollaborationResourceSyncPhase;
    // 只有实际执行工作量的阶段显示连续进度条。
    const bool showProgress = resource.phase == ResourcePhase::Preparing ||
                              resource.phase == ResourcePhase::ComparingCache ||
                              resource.phase == ResourcePhase::Downloading ||
                              resource.phase == ResourcePhase::Verifying;

    ImGui::Spacing();
    if ( FeedbackCollapsingHeader(TR("ui.collaboration.sync_settings").data(),
                                  headerFlags) ) {
        // 固定标签列宽按三个本地化标签最大值计算。
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
            // 视野发布率只允许 5 至 60 Hz，限制网络更新频率。
            drawRoomInfoLabel(TR("ui.collaboration.viewport_rate").data());
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::SliderInt("##CollaborationViewportRate",
                             &m_viewportPublishRateHz,
                             5,
                             60,
                             "%d Hz");
            if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                // 拖动结束才提交最终值，避免每个像素变化发送配置更新。
                m_room->setViewportPublishRateHz(
                    static_cast<std::uint32_t>(m_viewportPublishRateHz));
            }

            // 远端视野渲染模式是本机视觉偏好，不同步给房间。
            drawRoomInfoLabel(
                TR("ui.collaboration.viewport_render_mode").data());
            auto& settings = Config::AppConfig::instance().getEditorSettings();
            int   renderMode =
                static_cast<int>(settings.collaborationViewportRenderMode);
            // 枚举顺序与下拉文本数组严格对应。
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
                // 设置变化低频立即持久化，重启后保持选择。
                settings.collaborationViewportRenderMode =
                    static_cast<Config::CollaborationViewportRenderMode>(
                        renderMode);
                Config::AppConfig::instance().save();
            }

            // 资源阶段始终展示，即使当前没有连续进度。
            drawRoomInfoLabel(TR("ui.collaboration.resource.status").data());
            ImGui::TextWrapped("%s", resourcePhaseText(resource.phase));
            ImGui::EndTable();
        }
        if ( showProgress ) {
            // 默认零进度覆盖尚未获得总量的阶段。
            float fraction = 0.0F;
            if ( (resource.phase == ResourcePhase::Preparing ||
                  resource.phase == ResourcePhase::ComparingCache) &&
                 resource.totalFiles > 0 ) {
                // 准备按完成文件，缓存比较按已比较文件计算。
                const auto progressedFiles =
                    resource.phase == ResourcePhase::Preparing
                        ? resource.completedFiles
                        : resource.comparedFiles;
                fraction = static_cast<float>(progressedFiles) /
                           static_cast<float>(resource.totalFiles);
            } else if ( resource.totalBytes > 0 ) {
                // 下载和校验阶段按传输字节占比展示。
                fraction = static_cast<float>(resource.transferredBytes) /
                           static_cast<float>(resource.totalBytes);
            }
            // 防御网络快照在更新边界暂时超出总量。
            fraction = std::clamp(fraction, 0.0F, 1.0F);
            // overlay 保留完成文件数，补充字节比例信息。
            const std::string overlay =
                std::to_string(resource.completedFiles) + "/" +
                std::to_string(resource.totalFiles);
            ImGui::ProgressBar(fraction, ImVec2(-1.0F, 0.0F), overlay.c_str());
            if ( !resource.currentFile.empty() ) {
                // 当前文件名允许换行，不改变进度计算。
                ImGui::TextWrapped("%s", resource.currentFile.c_str());
            }
        } else if ( resource.phase == ResourcePhase::Ready ) {
            // 完成阶段显示总完成数和缓存命中数摘要。
            ImGui::TextDisabled(
                TR("ui.collaboration.resource.summary").data(),
                static_cast<unsigned int>(resource.completedFiles),
                static_cast<unsigned int>(resource.cachedFiles));
        }
    }

    ImGui::Spacing();
    if ( FeedbackButton(TR("ui.collaboration.disconnect").data(),
                        ImVec2(-1.0F, 0.0F)) ) {
        // disconnect 可能同步改变 isActive，立即结束本函数。
        m_room->disconnect();
        return;
    }

    ImGui::Spacing();
    if ( FeedbackCollapsingHeader(TR("ui.collaboration.participants").data(),
                                  headerFlags) ) {
        // 参与者 map 由房间快照维护，绘制期间不持久化元素指针。
        const auto&           participants = m_room->participants();
        const ImGuiTableFlags tableFlags =
            ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
        if ( ImGui::BeginTable(
                 "CollaborationParticipantsTable", 2, tableFlags) ) {
            // 动作列按访客跟随按钮和房主额外两个按钮的总宽度计算。
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

            // 本地成员固定绘制在第一行，便于用户识别自身状态。
            const auto localPeer = participants.find(m_room->localPeerId());
            if ( localPeer != participants.end() ) {
                drawParticipantRow(
                    *m_room, localPeer->first, localPeer->second);
            }
            for ( const auto& [peerId, identity] : participants ) {
                if ( peerId != m_room->localPeerId() ) {
                    // 跳过已单独绘制的本地成员，远端按 map 顺序追加。
                    drawParticipantRow(*m_room, peerId, identity);
                }
            }
            ImGui::EndTable();
        }
        if ( !m_room->isHost() ) {
            // 访客在表格下方查看房主授予自己的只读权限摘要。
            ImGui::Spacing();
            ImGui::TextUnformatted(
                TR("ui.collaboration.permissions.mine").data());
            drawLocalPermissionSummary(*m_room);
        }
    }
}

/// @brief 绘制协作聊天历史、输入框和发送反馈。
///
/// 历史区固定约八行高度，新 sequence 出现时自动滚到底部。输入缓冲容量在编译期
/// 校验为协议最大字节数加终止符；Enter
/// 和按钮共用发送路径，失败保留原输入供重试。
/// @warning UI 热路径：活动房间每帧绘制内存消息；网络发送只在显式提交时发生。
/// @details 聊天历史的时间是进入房间后的相对经过时间，不表示本地时区墙钟。消息
/// sequence 同时用于行 ID 和判断是否有新末尾消息，只有后者变化才自动滚动。
///
/// 发送结果不是 Accepted 时保留输入缓冲并显示错误；无论成功失败都请求下一帧重新
/// 聚焦输入框，使连续聊天和修正重试都不需要再次点击。
void CollaborationView::drawChatSection()
{
    // 保证 UI 无法构造超过网络协议上限的消息。
    static_assert(CHAT_INPUT_BUFFER_BYTES ==
                  Network::Collaboration::MAX_COLLABORATION_CHAT_MESSAGE_BYTES +
                      1U);
    ImGui::Spacing();
    if ( !FeedbackCollapsingHeader(TR("ui.collaboration.chat").data(),
                                   ImGuiTreeNodeFlags_DefaultOpen) ) {
        // 折叠时不遍历消息历史。
        return;
    }

    // 消息快照由房间服务拥有，历史区固定八行便于侧栏滚动。
    const auto& messages      = m_room->chatMessages();
    const float historyHeight = ImGui::GetTextLineHeightWithSpacing() * 8.0F;
    if ( ImGui::BeginChild("CollaborationChatHistory",
                           ImVec2(0.0F, historyHeight),
                           ImGuiChildFlags_Borders) ) {
        if ( messages.empty() ) {
            // 没有历史时显示只读占位。
            ImGui::TextDisabled("%s", TR("ui.collaboration.chat.empty").data());
        } else {
            for ( const auto& message : messages ) {
                // elapsedMilliseconds 转为房间内相对分钟和秒数。
                const auto totalSeconds = message.elapsedMilliseconds / 1000U;
                const auto minutes      = totalSeconds / 60U;
                const auto seconds      = totalSeconds % 60U;
                // sequence 低位作为本帧稳定 ImGui ID，文本本身无需隐藏后缀。
                ImGui::PushID(static_cast<int>(message.sequence & 0x7FFFFFFFU));
                ImGui::TextDisabled("[%02llu:%02llu]",
                                    static_cast<unsigned long long>(minutes),
                                    static_cast<unsigned long long>(seconds));
                ImGui::SameLine();
                // Creator 使用主题选中色，与普通消息正文区分。
                ImGui::TextColored(
                    ImGui::GetStyleColorVec4(ImGuiCol_TextSelectedBg),
                    "%s:",
                    message.creator.c_str());
                ImGui::SameLine();
                ImGui::TextWrapped("%s", message.text.c_str());
                ImGui::PopID();
            }
            if ( messages.back().sequence != m_lastRenderedChatSequence ) {
                // 只在出现新末尾消息时滚动，不打断用户阅读旧历史。
                ImGui::SetScrollHereY(1.0F);
                m_lastRenderedChatSequence = messages.back().sequence;
            }
        }
    }
    ImGui::EndChild();

    // 发送按钮按本地化文本测量，输入框吸收剩余宽度。
    const float sendButtonWidth =
        ImGui::CalcTextSize(TR("ui.collaboration.chat.send").data()).x +
        ImGui::GetStyle().FramePadding.x * 2.0F;
    const float inputWidth =
        std::max(1.0F,
                 ImGui::GetContentRegionAvail().x - sendButtonWidth -
                     ImGui::GetStyle().ItemSpacing.x);
    // 未分配本地 PeerId 时房间尚不能发送消息。
    const bool canSend = m_room->localPeerId() != 0;
    ImGui::BeginDisabled(!canSend);
    ImGui::SetNextItemWidth(inputWidth);
    // 发送发生在输入框绘制后，因此延迟到下一帧请求焦点。
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
        // 无论发送是否被拒绝，都让用户可以连续输入或修正后重试。
        m_shouldFocusChatInput = true;
        // 房间服务负责验证空白、长度和连接状态。
        const auto result = m_room->sendChatMessage(m_chatInput.data());
        m_chatSendFailed =
            result != Network::Collaboration::SubmitChatMessageResult::Accepted;
        if ( !m_chatSendFailed ) {
            // 只有服务接受后才清空输入，拒绝时保留用户文本。
            m_chatInput.fill('\0');
        }
    }
    if ( m_chatSendFailed ) {
        // 失败状态持续显示到下一次成功发送。
        ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                           "%s",
                           TR("ui.collaboration.chat.send_failed").data());
    }
}

/// @brief 在协作侧栏嵌入已注册日志窗口的只读内容。
/// @param sourceManager 用于按稳定名称查找 CollaborationLogWindow。
///
/// 没有管理器、房间或日志时完全省略分区；折叠状态不调用日志渲染，实际日志存储和
/// 过滤仍由独立窗口实现负责。
/// @warning UI 热路径：只查询内存日志和视图表，不复制完整日志集合。
/// @details
/// 侧栏不直接访问或格式化每条日志，只判断集合非空并委托已注册的日志窗口
/// renderInline。这样独立日志窗口和嵌入区域共享筛选、颜色和滚动行为。
/// CollaborationLogWindow 缺失时安全省略内容，不在这里动态创建视图。
void CollaborationView::drawLogSection(UIManager* sourceManager) const
{
    // 空日志不绘制无意义标题和占位空间。
    if ( !sourceManager || !m_room || m_room->logs().empty() ) return;

    ImGui::Spacing();
    if ( !FeedbackCollapsingHeader(TR("title.collaboration_log").data(),
                                   ImGuiTreeNodeFlags_DefaultOpen) ) {
        // 折叠时跳过可能较长的日志行绘制。
        return;
    }

    if ( auto* logWindow = sourceManager->getView<CollaborationLogWindow>(
             "CollaborationLogWindow") ) {
        // 复用独立窗口的格式化与筛选实现，避免两套日志 UI。
        logWindow->renderInline();
    }
}
}  // namespace MMM::UI
