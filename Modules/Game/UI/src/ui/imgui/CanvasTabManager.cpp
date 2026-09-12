#include "ui/imgui/CanvasTabManager.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "ui/ICanvasView.h"
#include "ui/ICanvasWorkspaceService.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/walkthrough/WelcomeView.h"

/// @file CanvasTabManager.cpp
/// @brief 逻辑画布会话与 UI 视图注册、停靠、聚焦和关闭流程的同步实现。
/// @details CanvasWorkspaceService 提供稳定条目快照，管理器只保存已注册相机
/// ID； 项目切换时逐个请求关闭，允许单个画布的未保存确认取消整个切换。

namespace MMM::UI
{

/// @brief 创建具名画布标签管理器。
/// @param name UIManager 注册该帧驱动视图时使用的稳定名称。
CanvasTabManager::CanvasTabManager(const std::string& name) : IUIView(name) {}

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
            auto newCanvas = workspace->createMainCanvas(entry, 200, 200);
            if ( !entry.restoreDockFromWorkspace ) {
                // 没有持久化 Dock 布局时请求画布采用默认中央停靠。
                if ( auto* canvas = newCanvas->asCanvasView() ) {
                    canvas->requestDockToCenter();
                }
            }

            // UIManager 取得视图所有权，本地集合只保存稳定 ID。
            sourceManager->registerView(entry.cameraId, std::move(newCanvas));
            m_initializedCanvases.insert(entry.cameraId);

            // DockBuilder 只能在中央节点已经由 MainDockSpaceUI 创建后调用。
            ImGuiID centerDockId = MainDockSpaceUI::getCenterDockId();
            if ( centerDockId != 0 && !entry.restoreDockFromWorkspace ) {
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
