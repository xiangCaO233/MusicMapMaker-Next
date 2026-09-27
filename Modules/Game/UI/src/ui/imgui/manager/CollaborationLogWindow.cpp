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
#include <optional>
#include <string>
#include <vector>

namespace MMM::UI
{
/// @brief 创建协作日志与会话绑定桥，并向房间注册同步回调。
/// @param name 视图注册名称。
/// @param room 协作房间共享所有权，保证回调期间房间对象存活。
///
/// 房间回调把远端谱面、确认序列和资源包转交逻辑会话。访客首次收到谱面时
/// 可以创建会话；已有会话则只排队替换命令。所有模型变更均通过 Session 命令
/// 队列提交，不在网络回调中直接修改谱面实体。
///
/// 回调契约：
/// - ApplyBeatmap 接受完整谱面值和按类别替换标志；
/// - includedLocalMutationSequence 用于确认已包含的本地变更；
/// - authoritativeRevision 标识远端权威版本；
/// - objectDeltaIdentities 存在时同时表明对象编码基线已准备；
/// - Acknowledged 回调只把序列号转交当前绑定会话；
/// - ResourceBundle 回调允许早于谱面到达并暂存在窗口成员中。
///
/// 所有回调捕获 this，因此本对象析构必须先从 CollaborationRoom 清除注册。
/// @warning 回调可能由协作更新流程触发；不得在其中调用 ImGui 或阻塞等待。
CollaborationLogWindow::CollaborationLogWindow(
    const std::string&                                         name,
    std::shared_ptr<Network::Collaboration::CollaborationRoom> room)
    : IUIView(name), m_room(std::move(room))
{
    if ( m_room ) {
        // 谱面回调同时覆盖访客首次建会话和已绑定会话的增量替换。
        m_room->setApplyBeatmapCallback(
            [this](std::shared_ptr<::MMM::BeatMap> beatmap,
                   ::MMM::BeatmapMutationFlags     flags,
                   std::uint64_t includedLocalMutationSequence,
                   std::uint64_t authoritativeRevision,
                   std::optional<std::vector<std::string>>
                       objectDeltaIdentities) {
                // weak_ptr 防止会话销毁后回调延长其生命周期。
                auto session = m_boundSession.lock();
                // 空谱面不能建立会话或替换数据，直接忽略异常通知。
                if ( !beatmap ) return;
                if ( !session && !m_room->isHost() ) {
                    // 访客没有本地会话时，以远端谱面创建不关联本机工程的会话。
                    auto&             engine = Logic::EditorEngine::instance();
                    const std::string displayName =
                        // 空谱面名回退到协作管理器标题，确保窗口标签可见。
                        beatmap->m_baseMapMetadata.name.empty()
                            ? TR("title.collaboration_manager").toString()
                            : beatmap->m_baseMapMetadata.name;
                    static_cast<void>(
                        engine.createSession(beatmap, displayName, false));
                    // createSession 结果由 EditorEngine
                    // 的活动会话入口重新取得。
                    session = engine.getActiveSession();
                    if ( !session ) return;
                    session->setCollaborationOfflineReadOnly(
                        // 访客只有在房间真正 Connected 时可按权限编辑。
                        shouldCollaborationSessionBeReadOnly(
                            true,
                            m_room->state() ==
                                Network::Collaboration::CollaborationRoomState::
                                    Connected));
                    session->setCollaborationClipboardIsolated(true);
                    // 协作剪贴板隔离防止本机工程对象跨房间粘贴。
                    const auto allowedFlags =
                        m_room->localAllowedMutationFlags();
                    session->setCollaborationAllowedMutationFlags(allowedFlags);
                    m_lastAppliedPermissionFlags =
                        static_cast<std::uint8_t>(allowedFlags);
                    session->setMutationObserver(m_room, false);
                    // 访客 observer 不作为主机发布源，只上报允许的本地变更。
                    m_room->onBeatmapSynchronized(*beatmap);
                    m_boundSession        = session;
                    m_boundSessionIsGuest = true;
                    // 若资源包先于谱面到达，建会话后立即尝试绑定缓存资源。
                    bindPendingResources();
                    return;
                }
                if ( !session ) return;
                // identities 存在即说明发送端已准备对象编码基线。
                const bool objectEncodingBaselinePrepared =
                    objectDeltaIdentities.has_value();
                session->pushCommand(
                    // 按 flags 逐类替换，未声明类别保留本地现状。
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
                        .replaceAnnotations = hasBeatmapMutationFlag(
                            flags, ::MMM::BeatmapMutationFlags::Annotations),
                        .notifyMutationObserver = false,
                        // 远端权威替换不可再次回传 observer，避免协作回环。
                        .authoritativeRemote = true,
                        .includedLocalMutationSequence =
                            includedLocalMutationSequence,
                        .objectDeltaIdentities =
                            std::move(objectDeltaIdentities),
                        .authoritativeRevision = authoritativeRevision,
                        .objectEncodingBaselinePrepared =
                            objectEncodingBaselinePrepared,
                    }));
            });
        // 本地序列确认也进入 Session 命令队列，与本地变更按序处理。
        m_room->setLocalMutationAcknowledgedCallback(
            [this](std::uint64_t sequence) {
                if ( auto session = m_boundSession.lock() ) {
                    // 会话已销毁时确认无需保留，房间状态会在后续重新同步。
                    session->pushCommand(Logic::LogicCommand(
                        Logic::CmdAcknowledgeCollaborationMutation{
                            .sequence = sequence,
                        }));
                }
            });
        // 资源包可早于会话到达，因此先保存共享载荷再尝试绑定。
        m_room->setResourceBundleCallback(
            [this](Network::Collaboration::CollaborationResourceBundle bundle) {
                m_pendingResourceBundle = std::make_shared<
                    Network::Collaboration::CollaborationResourceBundle>(
                    std::move(bundle));
                bindPendingResources();
            });
    }
}

/// @brief 解除会话协作约束、工程打开门禁和房间回调。
///
/// 析构先恢复仍存活会话的 observer、剪贴板及权限，再释放全局工程门禁，最后
/// 清除房间回调，防止回调继续捕获已销毁的 this。
CollaborationLogWindow::~CollaborationLogWindow()
{
    if ( auto session = m_boundSession.lock() ) {
        // Observer 必须先清除，后续本地状态恢复不能被误广播。
        session->setMutationObserver(nullptr);
        session->setCollaborationClipboardIsolated(false);
        session->setCollaborationAllowedMutationFlags(
            ::MMM::BeatmapMutationFlags::All);
        if ( m_boundSessionIsGuest ) {
            // 访客离开协作桥后仍保持离线只读，避免缓存会话被当作本机工程编辑。
            session->setCollaborationOfflineReadOnly(true);
        }
    }
    if ( m_guestProjectGateHeld ) {
        // 只在本对象确实持有门禁时释放，避免覆盖其他来源的状态。
        Logic::ProjectController::instance()
            .setLocalProjectOpeningBlockedByCollaboration(false);
    }
    if ( m_room ) {
        // 三个回调都捕获 this，析构结束前必须全部置空。
        m_room->setApplyBeatmapCallback(nullptr);
        m_room->setLocalMutationAcknowledgedCallback(nullptr);
        m_room->setResourceBundleCallback(nullptr);
    }
}

/// @brief 推进协作房间网络状态并同步本地会话绑定。
/// @warning UI 热路径：每帧调用；room->update 不得执行无界阻塞网络等待。
void CollaborationLogWindow::update(UIManager*)
{
    // 无房间对象时视图保持空闲，不尝试创建替代房间。
    if ( !m_room ) return;
    m_room->update();
    updateSessionBinding();
}

/// @brief 在协作管理器中内嵌绘制实时事件日志。
///
/// 列表使用 ImGuiListClipper 只格式化可见条目；新日志到达时滚动到底部，用户
/// 查看旧记录且数量不变时不强制改变滚动位置。
/// @warning UI 热路径：协作页可见时每帧调用，只遍历裁剪后的可见日志。
void CollaborationLogWindow::renderInline()
{
    if ( !m_room ) return;

    ImGui::Text("%s: %zu",
                TR("ui.collaboration.log.entries").data(),
                m_room->logs().size());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", TR("ui.collaboration.log.realtime").data());
    ImGui::Separator();

    // 固定十行高度让日志区稳定，不随条目数量推高整个协作页面。
    const float logHeight = ImGui::GetTextLineHeightWithSpacing() * 10.0F +
                            ImGui::GetStyle().FramePadding.y * 2.0F;
    if ( ImGui::BeginChild("##CollaborationLogEntries",
                           ImVec2(0.0f, logHeight),
                           ImGuiChildFlags_Borders,
                           ImGuiWindowFlags_HorizontalScrollbar) ) {
        ImGuiListClipper clipper;
        // Clipper 以日志快照数量计算可见索引范围，避免完整格式化历史。
        clipper.Begin(static_cast<int>(m_room->logs().size()));
        while ( clipper.Step() ) {
            for ( int index = clipper.DisplayStart; index < clipper.DisplayEnd;
                  ++index ) {
                const auto& entry = m_room->logs()[index];
                // 仅可见条目构造本地化字符串，降低长房间日志的每帧开销。
                const std::string line = formatEntry(entry);
                if ( entry.type == Network::Collaboration::
                                       CollaborationLogEventType::Error ) {
                    // 错误使用固定危险色，其他事件沿用普通文本色。
                    ImGui::TextColored(
                        ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s", line.c_str());
                } else {
                    ImGui::TextUnformatted(line.c_str());
                }
            }
        }
        if ( m_room->logs().size() > m_lastLogCount ) {
            // 只在条目数量增长时跟随最新事件，不干扰用户手动查看历史。
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    // 在完整绘制后记录数量，作为下一帧自动滚动判断基线。
    m_lastLogCount = m_room->logs().size();
}

/// @brief 按房间角色和连接状态维护会话 observer、权限及工程门禁。
///
/// 访客在 Joining、AwaitingApproval、Connected 期间禁止打开本机工程；已绑定访客
/// 会话在断线后保持只读。主机只绑定现有非 Logo 会话，并持续刷新资源清单。
///
/// 状态不变量：
/// - guestProjectGateHeld 精确记录本实例是否占用工程打开门禁；
/// - boundSession 使用 weak_ptr，不拥有编辑会话生命周期；
/// - boundSessionIsGuest 决定断线后的只读策略；
/// - lastAppliedPermissionFlags 避免重复写相同细分权限；
/// - pendingResourceBundle 只在资源已到而会话未就绪时保留；
/// - hostResourceProject/Beatmap 只用于主机资源身份去重。
///
/// 非活动房间会清除 observer、剪贴板隔离、绑定与所有缓存身份；访客会话仍保持
/// 离线只读，直到新的权威协作连接重新绑定。
///
/// 绑定顺序：
/// - 先更新访客工程打开门禁；
/// - 房间不活动时执行完整解绑并返回；
/// - 已绑定时校正只读与权限并刷新主机资源；
/// - 未绑定时先检查角色绑定策略；
/// - 主机从活动非 Logo 会话建立 observer；
/// - 最后绑定可能提前到达的资源包。
/// 该顺序避免访客错误绑定本机工程，也避免资源包在会话创建前丢失。
/// @warning UI 热路径：每帧执行；只在绑定变化时写 Session 状态或准备资源。
void CollaborationLogWindow::updateSessionBinding()
{
    // weak_ptr 锁定结果只在本函数范围内保持会话存活。
    auto       bound = m_boundSession.lock();
    const auto state = m_room->state();
    const bool guestConnectionBlocksProjects =
        // 主机始终保留本机工程控制；只有访客连接流程持有门禁。
        !m_room->isHost() &&
        (state == Network::Collaboration::CollaborationRoomState::Joining ||
         state ==
             Network::Collaboration::CollaborationRoomState::AwaitingApproval ||
         state == Network::Collaboration::CollaborationRoomState::Connected);
    if ( guestConnectionBlocksProjects ) {
        // 重复设置 true 允许 ProjectController 保持权威门禁状态。
        Logic::ProjectController::instance()
            .setLocalProjectOpeningBlockedByCollaboration(true);
        m_guestProjectGateHeld = true;
    } else if ( m_guestProjectGateHeld ) {
        // 房间退出相关状态后只释放本实例曾获取的门禁。
        Logic::ProjectController::instance()
            .setLocalProjectOpeningBlockedByCollaboration(false);
        m_guestProjectGateHeld = false;
    }

    if ( !m_room->isActive() ) {
        // 非活动状态完整解除绑定，但访客缓存会话仍保持离线只读。
        if ( bound ) {
            bound->setMutationObserver(nullptr);
            bound->setCollaborationClipboardIsolated(false);
            if ( m_boundSessionIsGuest ) {
                // 断线后的访客会话仍使用已校验的本地缓存资源；
                // 资源绑定只在换谱或会话销毁时释放。
                bound->setCollaborationOfflineReadOnly(true);
            }
        }
        m_boundSession.reset();
        // 清除角色、权限缓存、待资源和主机资源身份，供下次房间重新初始化。
        m_boundSessionIsGuest        = false;
        m_lastAppliedPermissionFlags = 0xFFU;
        m_pendingResourceBundle.reset();
        m_hostResourceProject = nullptr;
        m_hostResourceBeatmap = nullptr;
        return;
    }
    if ( bound ) {
        // 已绑定路径每帧校正离线只读，并只在权限位变化时写会话。
        bound->setCollaborationOfflineReadOnly(
            shouldCollaborationSessionBeReadOnly(
                m_boundSessionIsGuest,
                state ==
                    Network::Collaboration::CollaborationRoomState::Connected));
        const auto allowedFlags =
            m_boundSessionIsGuest
                // 访客使用服务端授权，主机始终拥有全部变更类别。
                ? m_room->localAllowedMutationFlags()
                : ::MMM::BeatmapMutationFlags::All;
        const auto allowedBits = static_cast<std::uint8_t>(allowedFlags);
        if ( allowedBits != m_lastAppliedPermissionFlags ) {
            // 位缓存避免每帧重复提交相同权限到会话。
            bound->setCollaborationAllowedMutationFlags(allowedFlags);
            m_lastAppliedPermissionFlags = allowedBits;
        }
        // 主机项目或谱面身份变化时刷新资源清单；访客调用会快速返回。
        refreshHostResources();
        return;
    }

    // 只有策略允许的角色能把当前本机会话绑定到协作房间。
    if ( !mayBindExistingSessionForCollaboration(m_room->isHost()) ) return;

    auto active = Logic::EditorEngine::instance().getActiveNonLogoSession();
    // Logo 会话不含可协作谱面，没有活动业务会话时等待后续帧。
    if ( !active ) return;
    active->setCollaborationOfflineReadOnly(false);
    active->setCollaborationClipboardIsolated(true);
    // 现有会话绑定路径用于主机，因此以全部权限和可编辑状态初始化。
    active->setCollaborationAllowedMutationFlags(
        ::MMM::BeatmapMutationFlags::All);
    m_lastAppliedPermissionFlags =
        static_cast<std::uint8_t>(::MMM::BeatmapMutationFlags::All);
    active->setMutationObserver(m_room, m_room->isHost());
    // observer 的 isHost 参数决定本地变更如何广播和确认。
    m_boundSession        = active;
    m_boundSessionIsGuest = false;
    refreshHostResources();
    bindPendingResources();
}

/// @brief 在主机工程或谱面身份变化时重建房间资源清单。
///
/// 通过缓存 Project 与 BeatMap 观察指针避免同一绑定每帧重复扫描资源。缓存只
/// 用于身份比较，不解引用旧对象；会话解绑时两者会清零。
///
/// 只有主机 Hosting 阶段负责构造资源包；连接建立后的发送由房间对象管理。
/// 同一个 Project/BeatMap 指针对只准备一次，换谱或换工程时任一身份变化都会
/// 重新调用 prepareHostResources。
/// @warning 低频资源准备路径：prepareHostResources 可能扫描项目资源。
void CollaborationLogWindow::refreshHostResources()
{
    // 仅 Hosting 状态的主机负责提供资源；访客和已连接后的其他状态直接返回。
    if ( !m_room->isHost() ||
         m_room->state() !=
             Network::Collaboration::CollaborationRoomState::Hosting ) {
        return;
    }

    auto session = m_boundSession.lock();
    // 资源清单必须对应一张实际谱面，空会话或空谱面没有可发布资源。
    if ( !session || !session->getContext().currentBeatmap ) return;
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    auto* beatmap = session->getContext().currentBeatmap.get();
    // 项目缺失或身份组合未变化时不重复准备相同资源。
    if ( !project || (project == m_hostResourceProject &&
                      beatmap == m_hostResourceBeatmap) ) {
        return;
    }

    // 先更新身份缓存，再调用房间准备，防止同步回调重入重复触发。
    m_hostResourceProject = project;
    m_hostResourceBeatmap = beatmap;
    m_room->prepareHostResources(*project, *beatmap);
}

/// @brief 把已接收资源包绑定到当前会话。
///
/// 资源包与谱面会话可以乱序到达；任一缺失时保留待处理包，二者齐备后通过
/// CmdSetCollaborationResources 交给逻辑线程，并清除 UI 侧缓存。
void CollaborationLogWindow::bindPendingResources()
{
    // 没有待处理资源时保持幂等，允许多个绑定入口安全调用。
    if ( !m_pendingResourceBundle ) return;
    auto session = m_boundSession.lock();
    // 会话尚未建立时保留 bundle，首次谱面同步后会再次调用。
    if ( !session ) return;
    session->pushCommand(Logic::LogicCommand{
        // project 共享所有权跨过命令队列，pathRemap 则移动以避免大容器复制。
        Logic::CmdSetCollaborationResources{
            .project   = m_pendingResourceBundle->project,
            .pathRemap = std::move(m_pendingResourceBundle->pathRemap),
        },
    });
    // 命令已经取得所需所有权，释放 UI 缓存防止重复绑定。
    m_pendingResourceBundle.reset();
}

/// @brief 报告内嵌日志视图始终处于打开状态。
/// @return 始终为 true；可见性由父协作页面控制。
/// @warning 常量查询，不访问房间或会话状态。
bool CollaborationLogWindow::isOpen() const
{
    return true;
}

/// @brief 接收统一视图接口的开关请求。
/// @param open 被忽略的目标状态；内嵌日志不维护独立窗口开关。
/// @warning 兼容 IUIView 接口，不产生任何可见性副作用。
void CollaborationLogWindow::setOpen(bool open)
{
    (void)open;
}

/// @brief 将协作日志条目格式化为带相对时间和参与者的本地化单行文本。
/// @param entry 待格式化的不可变日志条目。
/// @return `+秒数  消息` 格式文本。
///
/// 参与者优先显示 creator，并附加稳定 ID 前缀；creator 为空时只显示稳定标签。
/// 不同事件类型映射到各自翻译格式，detail 作为事件上下文插入。
///
/// 标识规则：
/// - participantId 非空时只显示前八个字符，兼顾可辨识性和日志宽度；
/// - participantId 为空时使用 peerId 数字标签；
/// - creator 非空时显示昵称并在括号中附稳定标签；
/// - creator 为空时稳定标签直接作为 actor；
/// - 所有事件共用 actor、detail 两个翻译格式参数；
/// - 最外层统一添加自房间启动起算的相对时间。
std::string CollaborationLogWindow::formatEntry(
    const Network::Collaboration::CollaborationLogEntry& entry) const
{
    // 毫秒转秒后固定三位小数，便于比较事件先后间隔。
    const double seconds =
        static_cast<double>(entry.elapsedMilliseconds) / 1000.0;
    const std::string stableLabel =
        entry.participantId.empty()
            // 无参与者 UUID 时以底层 peerId 构造稳定回退。
            ? fmt::format("#{}", entry.peerId)
            : entry.participantId.substr(0, 8);
    const std::string actor =
        entry.creator.empty()
            ? stableLabel
            : fmt::format("{} ({})", entry.creator, stableLabel);
    // 默认使用错误格式，switch 覆盖所有当前事件枚举。
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
    case Network::Collaboration::CollaborationLogEventType::HostDisconnected:
        formatKey = "ui.collaboration.log.host_disconnected_fmt";
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
        // 翻译字符串作为运行时 fmt 模板，actor 与 detail 参数顺序保持统一。
        fmt::format(fmt::runtime(TR(formatKey).data()), actor, entry.detail);
    return fmt::format("+{:07.3f}s  {}", seconds, message);
}
}  // namespace MMM::UI
