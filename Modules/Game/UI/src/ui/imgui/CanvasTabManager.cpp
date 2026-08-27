#include "ui/imgui/CanvasTabManager.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"
#include "ui/ICanvasView.h"
#include "ui/ICanvasViewFactory.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"

namespace MMM::UI
{

CanvasTabManager::CanvasTabManager(const std::string& name) : IUIView(name) {}

void CanvasTabManager::handlePendingProjectSwitch(
    UIManager* sourceManager, const std::vector<Logic::SessionEntry>& entries)
{
    auto& engine = Logic::EditorEngine::instance();
    /// @brief 项目控制器单例，用于查询当前项目切换流程。
    auto& projectController = Logic::ProjectController::instance();
    if ( !projectController.hasPendingProjectSwitch() ) {
        m_projectSwitchClosingCanvas.clear();
        m_capturedProjectSwitchWorkspace = false;
        return;
    }

    if ( !m_capturedProjectSwitchWorkspace ) {
        sourceManager->captureProjectWorkspaceState();
        engine.saveProject();
        m_capturedProjectSwitchWorkspace = true;
    }

    if ( entries.empty() ) {
        engine.createSession(nullptr, TR("canvas.welcome").data(), true);
        return;
    }

    bool hasBeatmapSession = false;
    for ( const auto& entry : entries ) {
        if ( !entry.isLogoPlaceholder ) {
            hasBeatmapSession = true;
            break;
        }
    }

    if ( !hasBeatmapSession ) {
        if ( entries.size() == 1 && entries.front().isLogoPlaceholder ) {
            m_projectSwitchClosingCanvas.clear();
            Event::EventBus::instance().publish(
                Event::ProjectSwitchCompletedEvent{});
        }
        return;
    }

    if ( !m_projectSwitchClosingCanvas.empty() ) {
        auto* canvas =
            sourceManager->getCanvasView(m_projectSwitchClosingCanvas);
        if ( canvas && canvas->consumeCloseCancelled() ) {
            XINFO(
                "CanvasTabManager: Project switch cancelled while closing "
                "cameraId={}",
                m_projectSwitchClosingCanvas);
            m_projectSwitchClosingCanvas.clear();
            Event::EventBus::instance().publish(
                Event::ProjectSwitchCancelledEvent{});
        }
        return;
    }

    for ( const auto& entry : entries ) {
        if ( entry.isLogoPlaceholder ) {
            continue;
        }

        auto* canvas = sourceManager->getCanvasView(entry.cameraId);
        if ( !canvas ) {
            continue;
        }

        XINFO(
            "CanvasTabManager: Requesting canvas close before project switch "
            "cameraId={}",
            entry.cameraId);
        canvas->requestClose();
        m_projectSwitchClosingCanvas = entry.cameraId;
        return;
    }
}

/// @brief 消费逻辑层的画布聚焦请求并转发给对应 Basic2DCanvas。
void CanvasTabManager::focusPendingSessionCanvas(
    UIManager* sourceManager, const std::vector<Logic::SessionEntry>& entries)
{
    auto&   engine     = Logic::EditorEngine::instance();
    int32_t focusIndex = engine.consumePendingFocusSessionIndex();
    if ( focusIndex < 0 ||
         focusIndex >= static_cast<int32_t>(entries.size()) ) {
        return;
    }

    const auto& entry  = entries[static_cast<size_t>(focusIndex)];
    auto*       canvas = sourceManager->getCanvasView(entry.cameraId);
    if ( !canvas ) {
        engine.requestSessionFocus(focusIndex);
        return;
    }

    canvas->requestFocus();
}

void CanvasTabManager::update(UIManager* sourceManager)
{
    auto& engine  = Logic::EditorEngine::instance();
    auto  entries = engine.getSessionEntries();

    std::unordered_set<std::string> activeCameraIds;
    activeCameraIds.reserve(entries.size());
    for ( const auto& entry : entries ) {
        activeCameraIds.insert(entry.cameraId);
    }

    std::vector<std::string> staleCanvases;
    for ( const auto& cameraId : m_initializedCanvases ) {
        if ( !activeCameraIds.contains(cameraId) ) {
            staleCanvases.push_back(cameraId);
        }
    }
    for ( const auto& cameraId : staleCanvases ) {
        sourceManager->unregisterView(cameraId);
        m_initializedCanvases.erase(cameraId);
    }

    // 1. 同步：检查是否有新的 Session 需要创建 Canvas
    for ( const auto& entry : entries ) {
        if ( m_initializedCanvases.find(entry.cameraId) ==
             m_initializedCanvases.end() ) {
            // 创建新的 Canvas 视图
            XINFO("CanvasTabManager: Creating Basic2DCanvas for cameraId={}",
                  entry.cameraId);

            auto* canvasFactory = sourceManager->getCanvasViewFactory();
            if ( !canvasFactory ) {
                XERROR("CanvasTabManager: Canvas factory is not configured");
                continue;
            }
            auto newCanvas = canvasFactory->createBasic2DCanvas(
                entry.cameraId,
                200,
                200,
                engine.getSyncBuffer(entry.cameraId),
                entry.cameraId);
            if ( !entry.restoreDockFromWorkspace ) {
                if ( auto* canvas = newCanvas->asCanvasView() ) {
                    canvas->requestDockToCenter();
                }
            }

            sourceManager->registerView(entry.cameraId, std::move(newCanvas));
            m_initializedCanvases.insert(entry.cameraId);

            // 如果有合法的中央停靠区，进行停靠
            ImGuiID centerDockId = MainDockSpaceUI::getCenterDockId();
            if ( centerDockId != 0 && !entry.restoreDockFromWorkspace ) {
                XINFO("CanvasTabManager: Docking {} to center dock #{}",
                      entry.cameraId,
                      centerDockId);
                ImGui::DockBuilderDockWindow(entry.cameraId.c_str(),
                                             centerDockId);
            }
        }
    }

    focusPendingSessionCanvas(sourceManager, entries);

    // 2. 检查：是否有已初始化的 Canvas 被关闭了
    bool sessionClosed = false;
    for ( int32_t i = 0; i < static_cast<int32_t>(entries.size()); ++i ) {
        const auto& entry = entries[i];
        if ( m_initializedCanvases.find(entry.cameraId) !=
             m_initializedCanvases.end() ) {
            auto* canvas = sourceManager->getCanvasView(entry.cameraId);
            if ( !canvas ) {
                // Canvas 已经不存在（已被 UIManager
                // 垃圾回收/注销），说明用户关闭了该 Tab
                XINFO(
                    "CanvasTabManager: Detected closed tab for cameraId={}, "
                    "closing logic session #{}",
                    entry.cameraId,
                    i);

                bool isProjectSwitchClose =
                    m_projectSwitchClosingCanvas == entry.cameraId;
                m_initializedCanvases.erase(entry.cameraId);
                if ( isProjectSwitchClose ) {
                    m_projectSwitchClosingCanvas.clear();
                }
                engine.closeSession(i, !isProjectSwitchClose);

                // 如果所有画布都被关闭了，我们需要恢复默认的 Logo 占位画布
                if ( engine.getSessionCount() == 0 ) {
                    XINFO(
                        "CanvasTabManager: All sessions closed. Creating "
                        "initial Logo placeholder session.");
                    engine.createSession(
                        nullptr, TR("canvas.welcome").data(), true);
                }
                sessionClosed = true;
                break;  // 逻辑会话列表已变动，跳出并在下一帧继续处理
            }
        }
    }

    if ( sessionClosed ) {
        return;
    }

    handlePendingProjectSwitch(sourceManager, entries);
}

}  // namespace MMM::UI
