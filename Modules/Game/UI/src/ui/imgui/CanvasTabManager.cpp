#include "ui/imgui/CanvasTabManager.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "mmm/project/ProjectSettings.h"
#include "ui/ICanvasView.h"
#include "ui/ICanvasWorkspaceService.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/WorkspaceDockRestore.h"
#include "ui/walkthrough/WelcomeView.h"

#include <algorithm>

/// @file CanvasTabManager.cpp
/// @brief 逻辑画布会话与 UI 视图注册、停靠、聚焦和关闭流程的同步实现。
/// @details CanvasWorkspaceService 提供稳定条目快照，管理器只保存已注册相机
/// ID； 项目切换时逐个请求关闭，允许单个画布的未保存确认取消整个切换。

namespace MMM::UI
{

/// @brief 创建具名画布标签管理器。
/// @param name UIManager 注册该帧驱动视图时使用的稳定名称。
CanvasTabManager::CanvasTabManager(const std::string& name) : IUIView(name) {}

/// @brief 解析项目保存的画布停靠身份，忽略原本浮动的画布。
/// @param workspace 项目已加载的工作区快照。
/// @param entries 本次项目打开后实际存在的画布条目。
/// @details 工作区的列表是保存时状态，实际会话列表是本次载图成功后的结果。
/// 直接等保存列表会让已删除文件或显式打开单谱面永久阻止布局捕获。
/// 浮动画布虽然无需补 DockId，仍需完成第一次 Begin 才能安全保存新快照。
/// @warning 项目打开时调用一次；解析 ini 和扩容均不进入常态每帧路径。
void CanvasTabManager::prepareProjectWorkspaceDockRestore(
    const ProjectWorkspaceState&             workspace,
    const std::vector<CanvasWorkspaceEntry>& entries)
{
    m_pendingWorkspaceDocks.clear();
    // 恢复请求数量不会超过本次实际发布的会话数量。
    m_pendingWorkspaceDocks.reserve(entries.size());
    // 旧 ImGui 窗口可能还在上下文里，但其上次绘制帧不能代表新项目已呈现。
    m_workspaceDockRestoreFrame        = ImGui::GetFrameCount();
    m_workspaceCanvasFirstFramePending = false;
    m_workspaceHasDockLayout           = !workspace.m_imguiIniData.empty();
    for ( const auto& entry : entries ) {
        // 显式打开单谱面时不会恢复旧列表；丢失的谱面也不在实际会话中。
        if ( entry.isLogoPlaceholder || !entry.restoreDockFromWorkspace )
            continue;
        // 是否需要等首帧由实际会话决定，不能只看 ini 中是否写了 DockId。
        m_workspaceCanvasFirstFramePending = true;
        const auto dockId = savedWorkspaceWindowDockId(workspace.m_imguiIniData,
                                                       entry.cameraId);
        if ( dockId ) {
            m_pendingWorkspaceDocks.push_back({ entry.cameraId, *dockId, 0 });
        }
    }
}

/// @brief 在画布第一次真实绘制后核对项目停靠关系。
/// @param sourceManager 当前 UI 管理器。
/// @param entries 已发布的逻辑画布条目。
/// @details 先验证会话仍存在，再等视图首次 Begin；只有同一窗口经过本次
/// 恢复帧之后，DockId 才具有可比较的意义。补停靠最多重复四次。
/// 保存门闩独立于 Dock 请求：浮动画布也必须先提交实际窗口状态。
/// @warning 项目恢复短路径：只有待核对列表非空才查找 ImGui 窗口和节点。
void CanvasTabManager::reconcileProjectWorkspaceDocks(
    UIManager* sourceManager, const std::vector<CanvasWorkspaceEntry>& entries)
{
    if ( ImGui::GetFrameCount() <= m_workspaceDockRestoreFrame ) return;

    // 无待处理请求时循环直接跳过；正常编辑帧无需遍历完整窗口集合。
    for ( auto it = m_pendingWorkspaceDocks.begin();
          it != m_pendingWorkspaceDocks.end(); ) {
        // 缺失谱面或切换后仅剩 Logo 时，旧项目的补停靠请求必须丢弃。
        const auto entry =
            std::find_if(entries.begin(),
                         entries.end(),
                         [&](const CanvasWorkspaceEntry& item) {
                             return item.cameraId == it->cameraId;
                         });
        if ( entry == entries.end() || entry->isLogoPlaceholder ) {
            it = m_pendingWorkspaceDocks.erase(it);
            continue;
        }

        // 注册不等于窗口已 Begin；不能把旧窗口或尚未绘制的窗口误判为恢复结果。
        auto*        canvas = sourceManager->getCanvasView(it->cameraId);
        ImGuiWindow* window = ImGui::FindWindowByName(it->cameraId.c_str());
        if ( !canvas || !window ||
             window->LastFrameActive < m_workspaceDockRestoreFrame ) {
            // `registerView` 发生在视图遍历中，新画布通常下一帧才 Begin。
            ++it;
            continue;
        }

        if ( reconcileSavedWorkspaceWindowDock(
                 it->cameraId,
                 it->dockId,
                 MainDockSpaceUI::getCenterDockId()) ) {
            // 已在原叶节点即可停止核对，允许后续正常拖动布局。
            it = m_pendingWorkspaceDocks.erase(it);
            continue;
        }
        // 节点布局异常时有界重试，避免把低频恢复变成永久热路径。
        if ( ++it->attempts >= 4 ) {
            // 放弃异常节点，避免工作区捕获和常态 UI 都永久受阻。
            // 此时最后一次停靠请求已发出；下一帧 ImGui 仍可自行完成它。
            XWARN("CanvasTabManager: Could not restore dock for {}",
                  it->cameraId);
            it = m_pendingWorkspaceDocks.erase(it);
        } else {
            ++it;
        }
    }

    if ( m_workspaceCanvasFirstFramePending ) {
        // 注册阶段可能先于 Begin 一帧；所有实际存在的谱面窗口至少绘制一次后
        // 才允许下一次工作区捕获覆盖已保存的完整 Dock 树。
        // 本次显式打开的新画布不复用旧布局，不应该增加恢复门闩的等待集合。
        bool allCanvasWindowsDrawn = true;
        for ( const auto& entry : entries ) {
            if ( entry.isLogoPlaceholder || !entry.restoreDockFromWorkspace )
                continue;
            ImGuiWindow* window =
                ImGui::FindWindowByName(entry.cameraId.c_str());
            if ( !window ||
                 window->LastFrameActive < m_workspaceDockRestoreFrame ) {
                // 延续使用磁盘上的完整 ini，避免半成品树写回项目配置。
                allCanvasWindowsDrawn = false;
                break;
            }
        }
        if ( allCanvasWindowsDrawn ) m_workspaceCanvasFirstFramePending = false;
        // 标志在第一次完整提交后永久清除；后续用户拖动属于正常布局编辑，
        // 不再按项目初始快照覆盖用户的新位置。
    }
}

/// @brief 推进待处理项目切换中的画布逐个关闭状态机。
/// @param sourceManager 当前 UI 管理器。
/// @param entries 本帧 CanvasWorkspaceService 条目快照。
/// @details 首帧先捕获并保存旧项目工作区，之后同一时间只请求一个真实谱面
/// 画布关闭；关闭取消事件终止切换，全部关闭后由占位会话完成切换握手。
/// @warning UI 热路径：项目切换期间每帧调用，不执行全量资源加载。
void CanvasTabManager::handlePendingProjectSwitch(
    UIManager* sourceManager, const std::vector<CanvasWorkspaceEntry>& entries)
{
    // 工作区服务是逻辑会话操作的唯一入口。
    auto* workspace = sourceManager->getCanvasWorkspaceService();
    if ( !workspace || !workspace->hasPendingProjectSwitch() ) {
        // 没有活动切换时清除上次关闭目标和捕获门闩。
        // 该重置也覆盖项目切换成功或由其他入口取消后的下一帧。
        m_projectSwitchClosingCanvas.clear();
        m_capturedProjectSwitchWorkspace = false;
        return;
    }

    if ( !m_capturedProjectSwitchWorkspace ) {
        // 关闭任何画布前捕获停靠、活动标签和侧栏状态。
        sourceManager->captureProjectWorkspaceState();
        // 保存旧项目，使切换后仍能恢复其最后工作区。
        workspace->saveProject();
        // 门闩保证多帧关闭流程只捕获和保存一次。
        m_capturedProjectSwitchWorkspace = true;
    }

    if ( entries.empty() ) {
        // 空会话列表先建立 Logo 占位，下一帧用它确认关闭流程结束。
        // 不在同帧发布完成事件，等待工作区条目真实反映新占位会话。
        workspace->createLogoPlaceholderSession(
            TR("canvas.welcome").toString());
        return;
    }

    // 项目切换只需要关闭真实谱面会话，Logo 占位无需确认。
    bool hasBeatmapSession = false;
    for ( const auto& entry : entries ) {
        if ( !entry.isLogoPlaceholder ) {
            // 找到首个真实会话即可停止扫描。
            hasBeatmapSession = true;
            break;
        }
    }

    if ( !hasBeatmapSession ) {
        if ( entries.size() == 1 && entries.front().isLogoPlaceholder ) {
            // 单一占位说明所有真实画布和逻辑会话都已关闭。
            m_projectSwitchClosingCanvas.clear();
            // 控制器收到完成事件后可安全打开目标项目。
            Event::EventBus::instance().publish(
                Event::ProjectSwitchCompletedEvent{});
        }
        // 多占位或过渡快照等待工作区在后续帧归一化。
        return;
    }

    if ( !m_projectSwitchClosingCanvas.empty() ) {
        // 已发出关闭请求时只观察对应画布结果，不重复请求其他画布。
        auto* canvas =
            sourceManager->getCanvasView(m_projectSwitchClosingCanvas);
        if ( canvas && canvas->consumeCloseCancelled() ) {
            // 未保存确认被取消时记录具体相机 ID，便于定位阻塞会话。
            XINFO(
                "CanvasTabManager: Project switch cancelled while closing "
                "cameraId={}",
                m_projectSwitchClosingCanvas);
            m_projectSwitchClosingCanvas.clear();
            // 取消事件让项目控制器保留当前项目和剩余画布。
            Event::EventBus::instance().publish(
                Event::ProjectSwitchCancelledEvent{});
        }
        // 画布仍存在且未取消时等待其关闭/保存流程推进。
        return;
    }

    // 没有在途关闭请求时选择本帧第一个真实谱面会话。
    for ( const auto& entry : entries ) {
        if ( entry.isLogoPlaceholder ) {
            // 占位画布留到所有真实会话关闭后作为完成标记。
            continue;
        }

        auto* canvas = sourceManager->getCanvasView(entry.cameraId);
        if ( !canvas ) {
            // 已被 UIManager 移除的条目由 update 的会话关闭检测处理。
            continue;
        }

        XINFO(
            "CanvasTabManager: Requesting canvas close before project switch "
            "cameraId={}",
            entry.cameraId);
        // requestClose 允许画布按自身未保存状态弹出确认。
        canvas->requestClose();
        // 保存唯一在途目标，后续帧判断关闭属于项目切换还是用户操作。
        m_projectSwitchClosingCanvas = entry.cameraId;
        // 一次只关闭一个，防止同时弹出多个保存确认窗口。
        return;
    }
}

/// @brief 消费逻辑层的画布聚焦请求并转发给对应主画布。
/// @param sourceManager 当前 UI 管理器。
/// @param entries 本帧工作区条目快照。
/// @warning UI 热路径：每帧至多消费一个索引并执行常量级视图查询。
void CanvasTabManager::focusPendingSessionCanvas(
    UIManager* sourceManager, const std::vector<CanvasWorkspaceEntry>& entries)
{
    // 没有工作区服务时无法解释逻辑会话索引。
    auto* workspace = sourceManager->getCanvasWorkspaceService();
    if ( !workspace ) {
        // 请求已被 consume，服务缺失时没有安全的目标可重新排队。
        return;
    }
    // consume 保证同一聚焦请求只处理一次。
    int32_t focusIndex = workspace->consumePendingFocusIndex();
    if ( focusIndex < 0 ||
         focusIndex >= static_cast<int32_t>(entries.size()) ) {
        // 负值表示无请求，越界值可能来自条目在跨线程间变化。
        return;
    }

    // 索引经边界验证后安全转换为无符号容器下标。
    const auto& entry = entries[static_cast<size_t>(focusIndex)];
    // Welcome 视图用于保护用户主动打开的欢迎页标签。
    const auto* welcome = sourceManager->getView<WelcomeView>("Welcome");
    // 逻辑层迟到的占位聚焦不得覆盖用户当前欢迎页选择。
    if ( entry.isLogoPlaceholder && welcome && welcome->isOpen() ) return;
    // 相机 ID 映射到当前注册的非拥有画布接口。
    auto* canvas = sourceManager->getCanvasView(entry.cameraId);
    if ( !canvas ) {
        // UI 视图尚未在本帧注册时把请求交还工作区，留待下一帧重试。
        workspace->requestEntryFocus(focusIndex);
        return;
    }

    // 实际 Dock 标签聚焦由画布在自身 Begin 前消费。
    canvas->requestFocus();
}

/// @brief 同步工作区会话与 UIManager 中的主画布视图集合。
/// @param sourceManager 当前 UI 管理器。
/// @details 按“移除失效视图、创建缺失视图、处理聚焦、回收关闭会话、推进项目
/// 切换”的顺序执行，任何会话列表变更都延迟到下一帧继续同步。
/// @warning UI 热路径：每帧遍历当前画布条目和已初始化 ID，禁止文件系统扫描。
void CanvasTabManager::update(UIManager* sourceManager)
{
    // 工作区服务缺失属于应用装配错误，本帧无法继续同步。
    auto* workspace = sourceManager->getCanvasWorkspaceService();
    if ( !workspace ) {
        XERROR("CanvasTabManager: Canvas workspace service is not configured");
        return;
    }
    // 复用成员数组填充本帧值快照，避免复制共享所有权对象。
    workspace->fillEntries(m_workspaceEntries);
    const auto& entries = m_workspaceEntries;
    // 入口门禁只需要归约是否存在真实谱面，不向欢迎页暴露会话容器。
    // 该快照同样排除始终存在的 Logo 占位标签，避免阶段三被提前解锁。
    m_hasOpenBeatmapCanvas = std::any_of(
        entries.begin(), entries.end(), [](const CanvasWorkspaceEntry& entry) {
            return !entry.isLogoPlaceholder;
        });

    // 第一阶段移除逻辑条目中已不存在的旧 UI 视图注册记录。
    for ( auto initializedIt = m_initializedCanvases.begin();
          initializedIt != m_initializedCanvases.end(); ) {
        // 擦除分支直接使用 erase 返回值，避免迭代器失效后递增。
        bool isActive = false;
        // 通过稳定 cameraId 判断已注册画布是否仍有逻辑条目。
        for ( const auto& entry : entries ) {
            if ( entry.cameraId == *initializedIt ) {
                isActive = true;
                break;
            }
        }
        if ( isActive ) {
            // 活动 ID 保留，并推进迭代器。
            ++initializedIt;
            continue;
        }

        // 注销前把 ID 值传给 UIManager，后续擦除本地集合节点。
        sourceManager->unregisterView(*initializedIt);
        initializedIt = m_initializedCanvases.erase(initializedIt);
    }

    // 第二阶段为每个尚未注册的逻辑会话创建主画布。
    for ( const auto& entry : entries ) {
        if ( m_initializedCanvases.find(entry.cameraId) ==
             m_initializedCanvases.end() ) {
            // 日志记录逻辑相机 ID，便于追踪重复创建或恢复问题。
            XINFO("CanvasTabManager: Creating Basic2DCanvas for cameraId={}",
                  entry.cameraId);

            // 工作区工厂按条目类型创建真实画布或 Logo 占位画布。
            auto       newCanvas = workspace->createMainCanvas(entry, 200, 200);
            const bool useDefaultDock =
                !entry.restoreDockFromWorkspace || !m_workspaceHasDockLayout;
            // `restoreDockFromWorkspace` 只说明保留了 cameraId，不保证有 ini。
            if ( useDefaultDock ) {
                // 旧项目可能保存了相机 ID 却没有 ini，仍须使用默认中央停靠。
                if ( auto* canvas = newCanvas->asCanvasView() ) {
                    canvas->requestDockToCenter();
                }
            }

            // UIManager 取得视图所有权，本地集合只保存稳定 ID。
            sourceManager->registerView(entry.cameraId, std::move(newCanvas));
            m_initializedCanvases.insert(entry.cameraId);

            // DockBuilder 只能在中央节点已经由 MainDockSpaceUI 创建后调用。
            ImGuiID centerDockId = MainDockSpaceUI::getCenterDockId();
            if ( centerDockId != 0 && useDefaultDock ) {
                // 仅默认布局使用中心节点，工作区恢复由 ini/DockBuilder
                // 状态负责。
                XINFO("CanvasTabManager: Docking {} to center dock #{}",
                      entry.cameraId,
                      centerDockId);
                ImGui::DockBuilderDockWindow(entry.cameraId.c_str(),
                                             centerDockId);
            }
        }
    }

    // 项目 ini 在运行中加载后，核对真正绘制过的画布，补回偶发丢失的 DockId。
    reconcileProjectWorkspaceDocks(sourceManager, entries);

    // 新画布注册完成后再消费聚焦，确保目标视图本帧已经存在。
    focusPendingSessionCanvas(sourceManager, entries);

    // 第三阶段检测 UIManager 已回收、但逻辑条目仍存在的关闭标签。
    bool sessionClosed = false;
    for ( int32_t i = 0; i < static_cast<int32_t>(entries.size()); ++i ) {
        // 使用有符号索引与 workspace::closeSession 接口保持一致。
        const auto& entry = entries[i];
        if ( m_initializedCanvases.find(entry.cameraId) !=
             m_initializedCanvases.end() ) {
            // 集合记录曾创建过该画布，视图缺失才代表关闭已完成。
            auto* canvas = sourceManager->getCanvasView(entry.cameraId);
            if ( !canvas ) {
                // UIManager 垃圾回收后的缺失视图映射为逻辑会话关闭。
                XINFO(
                    "CanvasTabManager: Detected closed tab for cameraId={}, "
                    "closing logic session #{}",
                    entry.cameraId,
                    i);

                // 在途目标相同表示关闭来自项目切换状态机。
                bool isProjectSwitchClose =
                    m_projectSwitchClosingCanvas == entry.cameraId;
                m_initializedCanvases.erase(entry.cameraId);
                if ( isProjectSwitchClose ) {
                    // 清空目标允许下一帧请求关闭下一个真实画布。
                    m_projectSwitchClosingCanvas.clear();
                }
                // 用户关闭需要保存工作区，项目切换关闭已在流程开始时保存。
                workspace->closeSession(i, !isProjectSwitchClose);

                // 空工作区必须恢复 Logo 占位，维持中心区始终有稳定会话条目。
                if ( workspace->getEntryCount() == 0 ) {
                    XINFO(
                        "CanvasTabManager: All sessions closed. Creating "
                        "initial Logo placeholder session.");
                    workspace->createLogoPlaceholderSession(
                        TR("canvas.welcome").toString());
                }
                // closeSession 改变条目索引，本帧不再继续使用旧 entries 快照。
                sessionClosed = true;
                break;
            }
        }
    }

    if ( sessionClosed ) {
        // 下一帧重新 fillEntries 后再创建占位视图或继续项目切换。
        return;
    }

    // 只有条目快照未发生结构变化时才安全推进项目切换关闭状态机。
    handlePendingProjectSwitch(sourceManager, entries);
}

}  // namespace MMM::UI
