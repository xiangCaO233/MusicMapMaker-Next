#include "canvas/Basic2DCanvas.h"
#include "canvas/Basic2DCanvasInteraction.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/system/ScrollCache.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <utility>

namespace MMM::Canvas
{

Basic2DCanvas::Basic2DCanvas(
    const std::string& name, uint32_t w, uint32_t h,
    std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer,
    const std::string&                        cameraId)
    : IUIView(name)
    , IRenderableView(name)
    , m_canvasName(name)
    , m_cameraId(cameraId.empty() ? name : cameraId)
    , m_syncBuffer(std::move(syncBuffer))
{
    m_targetWidth  = w;
    m_targetHeight = h;

    m_interaction =
        std::make_unique<Basic2DCanvasInteraction>(m_canvasName, m_cameraId);
}

Basic2DCanvas::~Basic2DCanvas() {}

/// @brief 更新画布 ImGui 窗口和交互状态。
/// @warning 热路径：主渲染线程每帧执行；背景纹理同步必须保持在路径变化分支内。
void Basic2DCanvas::update(UI::UIManager* sourceManager)
{
    // 1. 检查保存确认拦截
    if ( !m_isOpen ) {
        if ( !m_closeConfirmed && m_currentSnapshot &&
             m_currentSnapshot->isDirty ) {
            m_isOpen          = true;
            m_showSaveConfirm = true;
        }
    }

    std::string title = TR("canvas.editor").pStr;
    if ( m_currentSnapshot && m_currentSnapshot->hasBeatmap &&
         !m_currentSnapshot->beatmapName.empty() ) {
        title = m_currentSnapshot->beatmapName;
        if ( m_currentSnapshot->isDirty ) {
            title += " *";
        }
    }

    auto& engine           = Logic::EditorEngine::instance();
    bool  showClose        = engine.getSessionCount() > 1;
    auto  findSessionIndex = [this, &engine]() -> int32_t {
        for ( int32_t i = 0; i < engine.getSessionCount(); ++i ) {
            const auto* entry = engine.getSessionEntry(i);
            if ( entry && entry->cameraId == m_cameraId ) {
                return i;
            }
        }
        return -1;
    };

    ImGuiID dockId =
        m_shouldDockToCenter ? UI::MainDockSpaceUI::getCenterDockId() : 0;
    std::string windowName = fmt::format("{}###{}", title, m_canvasName);
    if ( m_shouldFocusNextFrame ) {
        ImGui::SetNextWindowFocus();
        m_shouldFocusNextFrame = false;
    }
    UI::LayoutContext lctx(m_layoutCtx,
                           windowName,
                           false,
                           ImGuiWindowFlags_NoTitleBar,
                           showClose ? &m_isOpen : nullptr,
                           dockId,
                           ImGuiCond_Always);
    m_lastDockId = ImGui::IsWindowDocked() ? ImGui::GetWindowDockID() : 0;
    if ( m_shouldDockToCenter && ImGui::IsWindowDocked() ) {
        m_shouldDockToCenter = false;
    }
    RenderContext rctx(
        this, m_canvasName.c_str(), m_targetWidth, m_targetHeight, nullptr);

    // 检查是否聚焦该窗口，若是，同步给 EditorEngine 设为活动 Session
    if ( ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ) {
        int32_t activeIdx = engine.getActiveSessionIndex();
        int32_t myIdx     = findSessionIndex();
        if ( myIdx != -1 && myIdx != activeIdx ) {
            engine.setActiveSessionIndex(myIdx);
            XINFO(
                "Basic2DCanvas: Focus switched active session to index {} "
                "(cameraId={})",
                myIdx,
                m_cameraId);
        }
    }

    if ( m_currentSnapshot ) {
        // 更新背景纹理
        /// @brief 仅在快照路径变化时同步背景纹理，避免热路径每帧访问文件系统。
        if ( m_currentSnapshot->backgroundPath != m_loadedBgPath ) {
            updateBackgroundTexture();
        }
    }

    // 仅当当前画布是活动画布时才处理交互，防止后台画布发送干扰指令
    if ( engine.getActiveCameraId() == m_cameraId ) {
        m_interaction->update(
            sourceManager, m_currentSnapshot, m_logicalWidth, m_logicalHeight);
    }

    // --- 渲染保存确认弹窗 ---
    if ( m_showSaveConfirm ) {
        ImGui::OpenPopup("Save Confirmation###SaveConfirmModal");
    }

    // 强制设置弹窗显示位置在屏幕中央
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if ( ImGui::BeginPopupModal("Save Confirmation###SaveConfirmModal",
                                nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize) ) {
        std::string mapName =
            m_currentSnapshot ? m_currentSnapshot->beatmapName : "Unknown";
        ImGui::Text("%s", TR_FMT("ui.exit.confirm_msg_fmt", mapName).c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float dpiScale = Config::AppConfig::instance().getWindowContentScale();

        if ( ImGui::Button(TR("ui.file.save").data(),
                           ImVec2(120 * dpiScale, 0)) ) {
            int32_t myIdx = findSessionIndex();
            if ( myIdx != -1 ) {
                engine.setActiveSessionIndex(myIdx);
            }
            // 保存谱面
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdSaveBeatmap{}));
            m_closeConfirmed  = true;
            m_isOpen          = false;
            m_showSaveConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.exit.dont_save").data(),
                           ImVec2(120 * dpiScale, 0)) ) {
            m_closeConfirmed  = true;
            m_isOpen          = false;
            m_showSaveConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.help.cancel").data(),
                           ImVec2(120 * dpiScale, 0)) ) {
            m_isOpen          = true;
            m_showSaveConfirm = false;
            m_closeCancelled  = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Basic2DCanvas::requestClose()
{
    m_closeCancelled = false;
    m_closeConfirmed = false;
    m_isOpen         = false;
}

bool Basic2DCanvas::consumeCloseCancelled()
{
    return std::exchange(m_closeCancelled, false);
}

void Basic2DCanvas::requestDockToCenter()
{
    m_shouldDockToCenter = true;
}

/// @brief 请求下一次更新时将画布窗口聚焦到前台。
void Basic2DCanvas::requestFocus()
{
    m_shouldFocusNextFrame = true;
}

/// @brief 获取画布当前所在的 ImGui Dock 节点。
/// @return 当前窗口停靠节点 ID；未停靠时返回 0。
ImGuiID Basic2DCanvas::getDockId() const
{
    return m_lastDockId;
}

/// @brief 判断当前帧是否需要准备画布快照。
/// @param snapshot 当前帧 UI 快照。
/// @return 需要准备时返回 true。
bool Basic2DCanvas::needsParallelUiPrepare(
    const UI::UiFrameSnapshot& snapshot) const
{
    (void)snapshot;
    return m_syncBuffer && (m_isOpen || m_showSaveConfirm ||
                            (m_currentSnapshot && m_currentSnapshot->isDirty));
}

/// @brief 在线程池中拉取并准备画布渲染快照。
/// @param snapshot 当前帧 UI 快照。
void Basic2DCanvas::prepareUiFrameData(const UI::UiFrameSnapshot& snapshot)
{
    (void)snapshot;
    m_preparedSnapshot = prepareCanvasSnapshot(
        m_syncBuffer.get(), m_lastOffsetSnapshot, m_lastAppliedYOffset, false);
    m_hasPreparedSnapshot = true;
}

/// @brief 将准备好的画布快照切换到主线程可见状态。
void Basic2DCanvas::swapPreparedUiFrameData()
{
    if ( !m_hasPreparedSnapshot ) {
        return;
    }

    m_currentSnapshot     = m_preparedSnapshot.snapshot;
    m_lastOffsetSnapshot  = m_preparedSnapshot.offsetSnapshot;
    m_lastAppliedYOffset  = m_preparedSnapshot.appliedYOffset;
    m_hasPreparedSnapshot = false;

    if ( !m_currentSnapshot ) {
        m_lastOffsetSnapshot = nullptr;
        m_lastAppliedYOffset = 0.0f;
    }
}

bool Basic2DCanvas::isDirty() const
{
    return true;
}

bool Basic2DCanvas::isOpen() const
{
    if ( !m_isOpen && !m_closeConfirmed && m_currentSnapshot &&
         m_currentSnapshot->isDirty ) {
        return true;
    }
    return m_isOpen;
}

void Basic2DCanvas::resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                               uint32_t h) const
{
    Event::CanvasResizeEvent e;
    e.canvasName = m_cameraId;
    e.lastSize   = { oldW, oldH };
    e.newSize    = { w, h };
    Event::EventBus::instance().publish(e);
}

bool Basic2DCanvas::needReload()
{
    return std::exchange(m_needReload, false);
}

}  // namespace MMM::Canvas
