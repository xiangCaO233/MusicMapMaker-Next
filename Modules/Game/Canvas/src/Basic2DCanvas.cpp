#include "canvas/Basic2DCanvas.h"
#include "canvas/Basic2DCanvasInteraction.h"
#include "config/AppConfig.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/utils/UIWidgetUtils.h"
#include <cmath>
#include <fmt/format.h>
#include <utility>

namespace MMM::Canvas
{
namespace
{
/// @brief 判断鼠标是否悬停在当前 ImGui 窗口的内容区域内。
/// @return 鼠标位于当前窗口内容区域时返回 true。
/// @warning UI 热路径：主画布每帧更新时调用，只读取当前 ImGui
/// 窗口几何与鼠标位置。
bool isMouseHoveringCurrentWindowContent()
{
    if ( !ImGui::IsWindowHovered(
             ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ) {
        return false;
    }

    const ImVec2 mousePos    = ImGui::GetMousePos();
    const ImVec2 contentPos  = ImGui::GetCursorScreenPos();
    const ImVec2 contentSize = ImGui::GetContentRegionAvail();
    if ( contentSize.x <= 0.0f || contentSize.y <= 0.0f ) {
        return false;
    }

    return mousePos.x >= contentPos.x &&
           mousePos.x <= contentPos.x + contentSize.x &&
           mousePos.y >= contentPos.y &&
           mousePos.y <= contentPos.y + contentSize.y;
}

/// @brief 判断当前主画布内容区是否正在接收带修饰键的滚轮操作。
/// @return Ctrl、Command、Alt 或组合修饰滚轮发生在当前窗口内容区时返回 true。
/// @warning UI 热路径：主画布每帧更新时调用；只读取 ImGui 输入状态和窗口几何。
bool isModifierWheelOverCurrentWindowContent()
{
    const auto& io = ImGui::GetIO();
    return std::abs(io.MouseWheel) > 0.01f &&
           (io.KeyCtrl || io.KeySuper || io.KeyAlt) &&
           !ImGui::IsAnyMouseDown() && isMouseHoveringCurrentWindowContent();
}

/// @brief 判断当前主画布 ImGui 窗口本帧是否真实可见。
/// @return 窗口内容区域可见且不是隐藏 Dock Tab 时返回 true。
/// @warning UI 热路径：主画布每帧更新时调用；只读取当前 ImGuiWindow 状态。
bool isCurrentCanvasWindowVisible()
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if ( !window || !window->WasActive || window->Hidden || window->Collapsed ||
         window->SkipItems ) {
        return false;
    }
    if ( window->DockIsActive && !window->DockTabIsVisible ) {
        return false;
    }

    const ImVec2 contentSize = ImGui::GetContentRegionAvail();
    return contentSize.x > 1.0f && contentSize.y > 1.0f;
}
}  // namespace

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
    m_backgroundVideoPlayer = std::make_unique<BackgroundVideoPlayer>();
}

Basic2DCanvas::~Basic2DCanvas() {}

/// @brief 更新画布 ImGui 窗口和交互状态。
/// @warning 热路径：主渲染线程每帧执行；背景纹理同步必须保持在路径变化分支内，
/// 后台画布 hover 滚轮只在滚轮输入发生时进入同主音轨判定路径；播放保持模式下的
/// 修饰键滚轮只走窄交互入口，其余修饰键滚轮仅在鼠标位于内容区时切换焦点。
void Basic2DCanvas::update(UI::UIManager* sourceManager)
{
    auto& engine           = Logic::EditorEngine::instance();
    auto  findSessionIndex = [this, &engine]() -> int32_t {
        for ( int32_t i = 0; i < engine.getSessionCount(); ++i ) {
            const auto* entry = engine.getSessionEntry(i);
            if ( entry && entry->cameraId == m_cameraId ) {
                return i;
            }
        }
        return -1;
    };

    // 1. 检查保存确认拦截
    if ( !m_isOpen ) {
        if ( !m_closeConfirmed && m_currentSnapshot &&
             m_currentSnapshot->isDirty ) {
            m_isOpen          = true;
            m_showSaveConfirm = true;
        } else if ( shouldKeepOpenForLastSessionReset() ) {
            if ( int32_t myIdx = findSessionIndex(); myIdx != -1 ) {
                engine.resetSessionToLogoPlaceholder(myIdx,
                                                     TR("canvas.welcome").pStr);
            }
            m_isOpen         = true;
            m_closeConfirmed = false;
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

    bool    showClose = false;
    int32_t myIndex   = findSessionIndex();
    if ( myIndex != -1 ) {
        const auto* entry = engine.getSessionEntry(myIndex);
        showClose         = entry && !entry->isLogoPlaceholder;
    }

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

    m_isCanvasVisible = isCurrentCanvasWindowVisible();
    engine.setSessionCanvasVisible(m_cameraId, m_isCanvasVisible);

    if ( m_isCanvasVisible ) {
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
            /// @brief
            /// 仅在快照路径或类型变化时同步背景资源，避免热路径
            /// 每帧访问文件系统。
            if ( m_currentSnapshot->backgroundPath != m_loadedBgPath ||
                 m_currentSnapshot->backgroundIsVideo !=
                     m_loadedBackgroundIsVideo ) {
                updateBackgroundTexture();
            }
            if ( m_currentSnapshot->backgroundIsVideo ) {
                updateBackgroundVideoFrame();
            }
        }

        // 仅当当前画布是活动画布时才处理完整交互，防止后台画布发送干扰指令
        bool isActiveCanvas = engine.getActiveCameraId() == m_cameraId;
        if ( !isActiveCanvas && isModifierWheelOverCurrentWindowContent() ) {
            const bool keepCurrentPlayback =
                !engine.getEditorConfig().settings.stopPlaybackOnScroll &&
                engine.isPlaybackPlaying();
            if ( keepCurrentPlayback ) {
                if ( m_currentSnapshot ) {
                    m_interaction->handleModifierWheel(m_currentSnapshot,
                                                       false);
                }
            } else if ( myIndex != -1 ) {
                ImGui::SetWindowFocus();
                engine.setActiveSessionIndex(myIndex);
                m_shouldFocusNextFrame = true;
                isActiveCanvas = engine.getActiveCameraId() == m_cameraId;
                XINFO(
                    "Basic2DCanvas: Modifier wheel switched active session to "
                    "index {} (cameraId={})",
                    myIndex,
                    m_cameraId);
            }
        }

        if ( isActiveCanvas ) {
            m_interaction->update(sourceManager,
                                  m_currentSnapshot,
                                  m_logicalWidth,
                                  m_logicalHeight);
        } else {
            /// @brief 本帧普通滚轮是否发生在当前后台主画布内容区。
            const bool isHoverWheelScroll =
                isMouseHoveringCurrentWindowContent() &&
                std::abs(ImGui::GetIO().MouseWheel) > 0.01f &&
                !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeySuper &&
                !ImGui::GetIO().KeyAlt;
            if ( isHoverWheelScroll &&
                 engine.canHoverScrollCamera(m_cameraId) ) {
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdScroll{ m_cameraId,
                                      -ImGui::GetIO().MouseWheel,
                                      ImGui::GetIO().KeyShift }));
            }
            m_interaction->updateTransientUi();
        }
    }

    // --- 渲染保存确认弹窗 ---
    if ( m_showSaveConfirm ) {
        ::MMM::UI::FeedbackOpenPopup("Save Confirmation###SaveConfirmModal");
    }

    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    ::MMM::UI::Utils::CenteredModalPopupScope modalScope(dpiScale);
    if ( modalScope.begin("Save Confirmation###SaveConfirmModal") ) {
        std::string mapName =
            m_currentSnapshot ? m_currentSnapshot->beatmapName : "Unknown";
        ImGui::Text("%s", TR_FMT("ui.exit.confirm_msg_fmt", mapName).c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if ( ::MMM::UI::FeedbackButton(TR("ui.file.save").data(),
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
        if ( ::MMM::UI::FeedbackButton(TR("ui.exit.dont_save").data(),
                                       ImVec2(120 * dpiScale, 0)) ) {
            m_closeConfirmed  = true;
            m_isOpen          = false;
            m_showSaveConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton(TR("ui.help.cancel").data(),
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
    return m_syncBuffer && m_isCanvasVisible &&
           (m_isOpen || m_showSaveConfirm ||
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
    return m_isCanvasVisible;
}

bool Basic2DCanvas::shouldRecordOffscreen() const
{
    return m_isCanvasVisible;
}

bool Basic2DCanvas::isOpen() const
{
    if ( shouldKeepOpenForLastSessionReset() ) {
        return true;
    }
    if ( !m_isOpen && !m_closeConfirmed && m_currentSnapshot &&
         m_currentSnapshot->isDirty ) {
        return true;
    }
    return m_isOpen;
}

bool Basic2DCanvas::shouldKeepOpenForLastSessionReset() const
{
    if ( m_isOpen ) {
        return false;
    }

    auto& engine = Logic::EditorEngine::instance();
    if ( engine.getSessionCount() != 1 ) {
        return false;
    }

    const auto* entry = engine.getSessionEntry(0);
    return entry && entry->cameraId == m_cameraId && !entry->isLogoPlaceholder;
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
    const auto& currentAsciiFont =
        Config::AppConfig::instance().getEditorSettings().preferredAsciiFont;
    if ( currentAsciiFont != m_loadedAsciiFontPreference ) {
        m_needReload = true;
    }
    return std::exchange(m_needReload, false);
}

}  // namespace MMM::Canvas
