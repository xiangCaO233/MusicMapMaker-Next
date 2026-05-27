#include "ui/imgui/CanvasTabManager.h"
#include "canvas/Basic2DCanvas.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"

namespace MMM::UI
{

CanvasTabManager::CanvasTabManager(const std::string& name) : IUIView(name) {}

void CanvasTabManager::update(UIManager* sourceManager)
{
    auto& engine  = Logic::EditorEngine::instance();
    auto  entries = engine.getSessionEntries();

    // 1. 同步：检查是否有新的 Session 需要创建 Canvas
    for ( const auto& entry : entries ) {
        if ( m_initializedCanvases.find(entry.cameraId) ==
             m_initializedCanvases.end() ) {
            // 创建新的 Canvas 视图
            XINFO("CanvasTabManager: Creating Basic2DCanvas for cameraId={}",
                  entry.cameraId);

            auto newCanvas = std::make_unique<Canvas::Basic2DCanvas>(
                entry.cameraId,
                200,
                200,
                engine.getSyncBuffer(entry.cameraId),
                entry.cameraId);

            sourceManager->registerView(entry.cameraId, std::move(newCanvas));
            m_initializedCanvases.insert(entry.cameraId);

            // 如果有合法的中央停靠区，进行停靠
            ImGuiID centerDockId = MainDockSpaceUI::getCenterDockId();
            if ( centerDockId != 0 ) {
                XINFO("CanvasTabManager: Docking {} to center dock #{}",
                      entry.cameraId,
                      centerDockId);
                ImGui::DockBuilderDockWindow(entry.cameraId.c_str(),
                                             centerDockId);
            }
        }
    }

    // 2. 检查：是否有已初始化的 Canvas 被关闭了
    for ( int32_t i = 0; i < static_cast<int32_t>(entries.size()); ++i ) {
        const auto& entry = entries[i];
        if ( m_initializedCanvases.find(entry.cameraId) !=
             m_initializedCanvases.end() ) {
            auto* canvas =
                sourceManager->getView<Canvas::Basic2DCanvas>(entry.cameraId);
            if ( !canvas ) {
                // Canvas 已经不存在（已被 UIManager
                // 垃圾回收/注销），说明用户关闭了该 Tab
                XINFO(
                    "CanvasTabManager: Detected closed tab for cameraId={}, "
                    "closing logic session #{}",
                    entry.cameraId,
                    i);

                m_initializedCanvases.erase(entry.cameraId);
                engine.closeSession(i);

                // 如果所有画布都被关闭了，我们需要恢复默认的 Logo 占位画布
                if ( engine.getSessionCount() == 0 ) {
                    XINFO(
                        "CanvasTabManager: All sessions closed. Creating "
                        "initial Logo placeholder session.");
                    engine.createSession(
                        nullptr, TR("canvas.welcome").pStr, true);
                }
                break;  // 逻辑会话列表已变动，跳出并在下一帧继续处理
            }
        }
    }
}

}  // namespace MMM::UI
