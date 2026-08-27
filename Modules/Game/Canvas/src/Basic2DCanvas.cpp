#include "canvas/Basic2DCanvas.h"
#include "canvas/Basic2DCanvasInteraction.h"
#include "canvas/CanvasTabTitle.h"
#include "canvas/CollaborationPeerColor.h"
#include "canvas/CollaborationViewportProjection.h"
#include "common/render/RenderSnapshotBuffer.h"
#include "config/AppConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/imguivk/VKTexture.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/session/CanvasCamera.h"
#include "network/collaboration/CollaborationRoom.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fmt/format.h>
#include <iterator>
#include <limits>
#include <string_view>
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

/// @brief 从渲染快照估算指定视觉时间对应的绝对 Y。
/// @param snapshot 当前主画布快照。
/// @param time 远端视口边界的视觉时间。
/// @return 与当前滚动分段一致的绝对 Y。
/// @warning UI 热路径：每个远端用户最多调用两次，只执行一次二分查找。
double collaborationAbsYAtTime(const Common::Render::RenderSnapshot& snapshot,
                               double                                time)
{
    const auto it = std::upper_bound(
        snapshot.scrollSegments.begin(),
        snapshot.scrollSegments.end(),
        time,
        [](double value, const Common::Render::ScrollSegment& segment) {
            return value < segment.time;
        });
    const auto& segment = it == snapshot.scrollSegments.begin()
                              ? snapshot.scrollSegments.front()
                              : *std::prev(it);
    return segment.absY + (time - segment.time) * segment.speed;
}

/// @brief 将协作视觉时间投影到当前主画布本地 Y 坐标。
/// @param snapshot 当前主画布快照。
/// @param time 待投影视觉时间。
/// @param canvasHeight 当前画布高度。
/// @return 以主画布左上角为原点的 Y 坐标。
/// @warning UI 热路径：只执行常量数值计算和两次滚动分段二分查找。
float collaborationTimeToCanvasY(const Common::Render::RenderSnapshot& snapshot,
                                 double time, float canvasHeight)
{
    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    const float judgmentLineY =
        canvasHeight * visual.judgmentLinePositionForKeyCount(
                           std::max(snapshot.trackCount, 1));
    if ( snapshot.scrollSegments.empty() ) {
        return projectCollaborationViewportTime(time,
                                                snapshot.currentTime,
                                                snapshot.visibleTimeStart,
                                                snapshot.visibleTimeEnd,
                                                judgmentLineY,
                                                canvasHeight)
            .value_or(judgmentLineY);
    }
    const double currentAbsY =
        collaborationAbsYAtTime(snapshot, snapshot.currentTime);
    const double targetAbsY = collaborationAbsYAtTime(snapshot, time);
    const double scale      = std::isfinite(snapshot.renderScaleY) &&
                                      std::abs(snapshot.renderScaleY) > 1e-6F
                                  ? static_cast<double>(snapshot.renderScaleY)
                                  : 1.0;
    return judgmentLineY -
           static_cast<float>((targetAbsY - currentAbsY) * scale);
}

}  // namespace

Basic2DCanvas::Basic2DCanvas(
    const std::string& name, uint32_t w, uint32_t h,
    std::shared_ptr<Common::Render::RenderSnapshotBuffer> syncBuffer,
    const std::string&                                    cameraId)
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
                engine.resetSessionToLogoPlaceholder(
                    myIdx, TR("canvas.welcome").data());
            }
            m_isOpen         = true;
            m_closeConfirmed = false;
        }
    }

    bool    showClose         = false;
    bool    isLogoPlaceholder = false;
    int32_t myIndex           = findSessionIndex();
    if ( myIndex != -1 ) {
        const auto* entry = engine.getSessionEntry(myIndex);
        isLogoPlaceholder = entry && entry->isLogoPlaceholder;
        showClose         = entry && !isLogoPlaceholder;
    }

    auto* collaborationRoom =
        sourceManager ? sourceManager->getCollaborationRoom() : nullptr;
    const bool roomLifecycleActive =
        collaborationRoom &&
        collaborationRoom->state() !=
            Network::Collaboration::CollaborationRoomState::Idle;
    m_isCollaborationCanvas = resolveCollaborationCanvasState(
        m_isCollaborationCanvas,
        isLogoPlaceholder,
        m_wasLogoPlaceholder,
        roomLifecycleActive,
        m_wasCollaborationRoomLifecycleActive,
        myIndex == engine.getActiveSessionIndex());
    m_wasLogoPlaceholder                  = isLogoPlaceholder;
    m_wasCollaborationRoomLifecycleActive = roomLifecycleActive;

    std::string_view collaborationStatusLabel;
    if ( m_isCollaborationCanvas ) {
        const auto state =
            collaborationRoom
                ? collaborationRoom->state()
                : Network::Collaboration::CollaborationRoomState::Idle;
        const bool online =
            state == Network::Collaboration::CollaborationRoomState::Hosting ||
            state == Network::Collaboration::CollaborationRoomState::Connected;
        collaborationStatusLabel = TR(online ? "canvas.collaboration.online"
                                             : "canvas.collaboration.offline")
                                       .data();
    }

    const std::string title = makeCanvasTabTitle(
        TR("canvas.editor").data(),
        m_currentSnapshot && m_currentSnapshot->hasBeatmap,
        m_currentSnapshot ? m_currentSnapshot->beatmapName : std::string_view{},
        m_currentSnapshot && m_currentSnapshot->isDirty,
        collaborationStatusLabel);

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

        // 先提交离屏纹理，后续主画布物件控制按钮才能位于纹理之上并接收输入。
        const ImVec2 canvasScreenPosition = ImGui::GetCursorScreenPos();
        const ImVec2 canvasSize           = rctx.getRenderSize();
        rctx.renderSurface();

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
            updateCollaborationViewports(
                sourceManager, canvasScreenPosition, canvasSize);
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

void Basic2DCanvas::updateCollaborationViewports(
    UI::UIManager* sourceManager, const ImVec2& canvasScreenPosition,
    const ImVec2& canvasSize)
{
    if ( !sourceManager || !m_currentSnapshot ||
         !m_currentSnapshot->hasBeatmap || canvasSize.x <= 1.0F ||
         canvasSize.y <= 1.0F ) {
        return;
    }
    auto* room = sourceManager->getCollaborationRoom();
    if ( !room || !room->isActive() || room->localPeerId() == 0 ) {
        m_lastFollowedPeerId           = 0;
        m_lastFollowedViewportSequence = 0;
        return;
    }

    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    const auto  viewportRenderMode = Config::AppConfig::instance()
                                         .getEditorSettings()
                                         .collaborationViewportRenderMode;
    const auto& layout =
        visual.trackLayoutForKeyCount(m_currentSnapshot->trackCount);

    Network::Collaboration::ParticipantViewport localViewport;
    localViewport.playbackTime     = m_currentSnapshot->playbackTime;
    localViewport.visualTime       = m_currentSnapshot->currentTime;
    localViewport.visibleTimeStart = m_currentSnapshot->visibleTimeStart;
    localViewport.visibleTimeEnd   = m_currentSnapshot->visibleTimeEnd;
    if ( std::isfinite(layout.top) && std::isfinite(layout.bottom) &&
         layout.top < layout.bottom ) {
        const float judgmentLineY =
            canvasSize.y * visual.judgmentLinePositionForKeyCount(
                               std::max(m_currentSnapshot->trackCount, 1));
        const float trackTopY =
            canvasSize.y * std::clamp(layout.top, 0.0F, 1.0F);
        const float trackBottomY =
            canvasSize.y * std::clamp(layout.bottom, 0.0F, 1.0F);
        const auto trackBottomTime = unprojectCollaborationViewportTime(
            trackBottomY,
            m_currentSnapshot->currentTime,
            m_currentSnapshot->visibleTimeStart,
            m_currentSnapshot->visibleTimeEnd,
            judgmentLineY,
            canvasSize.y);
        const auto trackTopTime = unprojectCollaborationViewportTime(
            trackTopY,
            m_currentSnapshot->currentTime,
            m_currentSnapshot->visibleTimeStart,
            m_currentSnapshot->visibleTimeEnd,
            judgmentLineY,
            canvasSize.y);
        if ( trackBottomTime && trackTopTime ) {
            localViewport.visibleTimeStart = *trackBottomTime;
            localViewport.visibleTimeEnd   = *trackTopTime;
        }
    }
    localViewport.horizontalOffsetRatio =
        static_cast<double>(m_currentSnapshot->canvasHorizontalOffsetX) /
        static_cast<double>(canvasSize.x);
    room->publishLocalViewport(localViewport,
                               m_currentSnapshot->isSeekScrubbing);

    const auto followedPeerId = room->followedPeerId();
    if ( followedPeerId != m_lastFollowedPeerId ) {
        m_lastFollowedPeerId           = followedPeerId;
        m_lastFollowedViewportSequence = 0;
    }
    const auto& viewports = room->participantViewports();
    const auto  jumpToViewport =
        [this, &canvasSize](
            const Network::Collaboration::ParticipantViewport& viewport) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSeek{ viewport.playbackTime }));
            const float desiredHorizontalOffset =
                static_cast<float>(viewport.horizontalOffsetRatio *
                                   static_cast<double>(canvasSize.x));
            const float horizontalDelta =
                desiredHorizontalOffset -
                m_currentSnapshot->canvasHorizontalOffsetX;
            if ( std::isfinite(horizontalDelta) &&
                 std::abs(horizontalDelta) > 0.05F ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdPanCanvas{
                        .cameraId       = m_cameraId,
                        .deltaX         = horizontalDelta,
                        .deltaY         = 0.0F,
                        .viewportWidth  = canvasSize.x,
                        .viewportHeight = canvasSize.y,
                        .renderScaleY   = m_currentSnapshot->renderScaleY,
                    }));
            }
        };
    if ( followedPeerId != 0 ) {
        const auto followed = viewports.find(followedPeerId);
        if ( followed != viewports.end() &&
             followed->second.sequence != m_lastFollowedViewportSequence ) {
            const auto& viewport = followed->second;
            jumpToViewport(viewport);
            m_lastFollowedViewportSequence = viewport.sequence;
        }
    }

    const auto&  participants = room->participants();
    const auto   localId      = room->localPeerId();
    const double localMinimum = std::min(m_currentSnapshot->visibleTimeStart,
                                         m_currentSnapshot->visibleTimeEnd);
    const double localMaximum = std::max(m_currentSnapshot->visibleTimeStart,
                                         m_currentSnapshot->visibleTimeEnd);
    if ( !std::isfinite(localMinimum) || !std::isfinite(localMaximum) ) return;

    const auto localLaneProjection = Logic::calculateCanvasLaneProjection(
        canvasSize.x,
        m_currentSnapshot->trackCount,
        m_currentSnapshot->bgmTrackCount,
        layout.left,
        layout.right,
        m_currentSnapshot->canvasHorizontalOffsetX,
        true,
        m_currentSnapshot->bmsEditingEnabled,
        m_currentSnapshot->draftLanesEnabled);
    if ( !localLaneProjection.valid ) return;
    const auto horizontalRange = projectCollaborationViewportHorizontalRange(
        localLaneProjection.player.leftX,
        localLaneProjection.bgmRightX,
        canvasSize.x);
    if ( !horizontalRange ) return;

    /// @brief 判断远端视口是否需要绘制上方或下方离屏提示。
    /// @return 上方提示返回 true，下方提示返回 false，不需要提示则返回空值。
    const auto classifyOffscreenIndicator =
        [&viewports, &participants, localId, localMinimum, localMaximum, this](
            Network::Collaboration::PeerId peerId) -> std::optional<bool> {
        if ( peerId == localId || !participants.contains(peerId) ) {
            return std::nullopt;
        }
        const auto viewportIt = viewports.find(peerId);
        if ( viewportIt == viewports.end() ) return std::nullopt;
        const auto&  viewport = viewportIt->second;
        const double remoteMinimum =
            std::min(viewport.visibleTimeStart, viewport.visibleTimeEnd);
        const double remoteMaximum =
            std::max(viewport.visibleTimeStart, viewport.visibleTimeEnd);
        if ( !std::isfinite(remoteMinimum) || !std::isfinite(remoteMaximum) ||
             !std::isfinite(viewport.visualTime) ||
             (remoteMaximum >= localMinimum &&
              remoteMinimum <= localMaximum) ) {
            return std::nullopt;
        }
        return viewport.visualTime > m_currentSnapshot->currentTime;
    };

    std::array<std::size_t, 2> indicatorCounts{};
    for ( const auto& [peerId, viewport] : viewports ) {
        (void)viewport;
        const auto direction = classifyOffscreenIndicator(peerId);
        if ( direction ) {
            ++indicatorCounts[*direction ? 1U : 0U];
        }
    }

    ImDrawList*  drawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasMaximum{
        canvasScreenPosition.x + canvasSize.x,
        canvasScreenPosition.y + canvasSize.y,
    };
    drawList->PushClipRect(canvasScreenPosition, canvasMaximum, true);

    for ( const auto& [peerId, viewport] : viewports ) {
        if ( peerId == localId ) continue;
        const auto participant = participants.find(peerId);
        if ( participant == participants.end() ) continue;

        const double remoteMinimum =
            std::min(viewport.visibleTimeStart, viewport.visibleTimeEnd);
        const double remoteMaximum =
            std::max(viewport.visibleTimeStart, viewport.visibleTimeEnd);
        if ( !std::isfinite(remoteMinimum) || !std::isfinite(remoteMaximum) ) {
            continue;
        }

        const float leftX   = horizontalRange->leftX;
        const float rightX  = horizontalRange->rightX;
        const float centerX = (leftX + rightX) * 0.5F;
        const ImU32 color =
            collaborationPeerColor(participant->second.participantId, 255);
        const bool following = followedPeerId == peerId;

        if ( remoteMaximum >= localMinimum && remoteMinimum <= localMaximum ) {
            float firstY = collaborationTimeToCanvasY(
                *m_currentSnapshot, viewport.visibleTimeStart, canvasSize.y);
            float secondY = collaborationTimeToCanvasY(
                *m_currentSnapshot, viewport.visibleTimeEnd, canvasSize.y);
            float topY = std::clamp(
                std::min(firstY, secondY), 1.0F, canvasSize.y - 1.0F);
            float bottomY = std::clamp(
                std::max(firstY, secondY), 1.0F, canvasSize.y - 1.0F);
            if ( bottomY - topY < 3.0F ) bottomY = topY + 3.0F;
            const ImVec2 rectangleMinimum{
                canvasScreenPosition.x + leftX,
                canvasScreenPosition.y + topY,
            };
            const ImVec2 rectangleMaximum{
                canvasScreenPosition.x + rightX,
                canvasScreenPosition.y + std::min(bottomY, canvasSize.y - 1.0F),
            };
            const float outlineThickness = following ? 3.0F : 2.0F;
            if ( viewportRenderMode ==
                 Config::CollaborationViewportRenderMode::Filled ) {
                drawList->AddRectFilled(
                    rectangleMinimum,
                    rectangleMaximum,
                    collaborationPeerColor(participant->second.participantId,
                                           24));
            }
            if ( viewportRenderMode ==
                 Config::CollaborationViewportRenderMode::TrackEdge ) {
                constexpr float BRACKET_CAP_WIDTH = 12.0F;
                const float     bracketRight      = std::min(
                    rectangleMinimum.x + BRACKET_CAP_WIDTH, rectangleMaximum.x);
                drawList->AddLine(rectangleMinimum,
                                  { rectangleMinimum.x, rectangleMaximum.y },
                                  color,
                                  outlineThickness);
                drawList->AddLine(rectangleMinimum,
                                  { bracketRight, rectangleMinimum.y },
                                  color,
                                  outlineThickness);
                drawList->AddLine({ rectangleMinimum.x, rectangleMaximum.y },
                                  { bracketRight, rectangleMaximum.y },
                                  color,
                                  outlineThickness);
            } else {
                drawList->AddRect(rectangleMinimum,
                                  rectangleMaximum,
                                  color,
                                  0.0F,
                                  0,
                                  outlineThickness);
            }

            const ImVec2 textSize =
                ImGui::CalcTextSize(participant->second.creator.c_str());
            const float labelHeight = textSize.y + 6.0F;
            float       labelY      = rectangleMinimum.y - labelHeight;
            if ( labelY < canvasScreenPosition.y ) {
                labelY = rectangleMinimum.y + 1.0F;
            }
            const float labelX =
                std::clamp(rectangleMinimum.x,
                           canvasScreenPosition.x,
                           std::max(canvasScreenPosition.x,
                                    canvasMaximum.x - textSize.x - 10.0F));
            const ImVec2 labelMinimum{ labelX, labelY };
            const ImVec2 labelMaximum{ labelX + textSize.x + 10.0F,
                                       labelY + labelHeight };
            drawList->AddRectFilled(
                labelMinimum,
                labelMaximum,
                collaborationPeerColor(participant->second.participantId, 220),
                3.0F);
            drawList->AddText({ labelX + 5.0F, labelY + 3.0F },
                              IM_COL32(255, 255, 255, 255),
                              participant->second.creator.c_str());
            continue;
        }

        const bool remoteAhead =
            viewport.visualTime > m_currentSnapshot->currentTime;
        const std::size_t directionIndex = remoteAhead ? 1U : 0U;
        std::size_t       indicatorSlot  = 0;
        for ( const auto& [candidatePeerId, candidateViewport] : viewports ) {
            (void)candidateViewport;
            if ( candidatePeerId >= peerId ) continue;
            const auto candidateDirection =
                classifyOffscreenIndicator(candidatePeerId);
            if ( candidateDirection && *candidateDirection == remoteAhead ) {
                ++indicatorSlot;
            }
        }
        const float tipY        = remoteAhead ? canvasScreenPosition.y + 7.0F
                                              : canvasMaximum.y - 7.0F;
        const float baseY       = remoteAhead ? tipY + 13.0F : tipY - 13.0F;
        const float arrowLocalX = layoutCollaborationViewportIndicatorX(
                                      leftX,
                                      rightX,
                                      canvasSize.x,
                                      indicatorSlot,
                                      indicatorCounts[directionIndex])
                                      .value_or(centerX);
        const float arrowX      = canvasScreenPosition.x + arrowLocalX;
        drawList->AddTriangleFilled({ arrowX, tipY },
                                    { arrowX - 8.0F, baseY },
                                    { arrowX + 8.0F, baseY },
                                    color);
        const ImVec2 textSize =
            ImGui::CalcTextSize(participant->second.creator.c_str());
        const float textX =
            std::clamp(arrowX - textSize.x * 0.5F,
                       canvasScreenPosition.x + 2.0F,
                       std::max(canvasScreenPosition.x + 2.0F,
                                canvasMaximum.x - textSize.x - 2.0F));
        const float textY =
            remoteAhead ? baseY + 2.0F : baseY - textSize.y - 2.0F;
        drawList->AddText(
            { textX, textY }, color, participant->second.creator.c_str());

        const ImVec2 savedCursorPosition = ImGui::GetCursorScreenPos();
        const ImVec2 hitMinimum{
            std::max(canvasScreenPosition.x,
                     std::min(arrowX - 11.0F, textX - 4.0F)),
            std::max(canvasScreenPosition.y,
                     std::min({ tipY, baseY, textY }) - 4.0F),
        };
        const ImVec2 hitMaximum{
            std::min(canvasMaximum.x,
                     std::max(arrowX + 11.0F, textX + textSize.x + 4.0F)),
            std::min(canvasMaximum.y,
                     std::max({ tipY, baseY, textY + textSize.y }) + 4.0F),
        };
        if ( hitMaximum.x > hitMinimum.x && hitMaximum.y > hitMinimum.y ) {
            ImGui::PushID(participant->second.participantId.c_str());
            ImGui::SetCursorScreenPos(hitMinimum);
            if ( ImGui::InvisibleButton("##CollaborationViewportJump",
                                        { hitMaximum.x - hitMinimum.x,
                                          hitMaximum.y - hitMinimum.y }) ) {
                jumpToViewport(viewport);
            }
            if ( ImGui::IsItemHovered() ) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            ImGui::PopID();
            ImGui::SetCursorScreenPos(savedCursorPosition);
        }
    }
    drawList->PopClipRect();
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

float Basic2DCanvas::currentFontRasterScale()
{
    const float scale = ImGui::GetIO().DisplayFramebufferScale.y;
    return std::isfinite(scale) && scale > 0.0F ? scale : 1.0F;
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
    const auto& settings = Config::AppConfig::instance().getEditorSettings();
    const auto& currentAsciiFont = settings.preferredAsciiFont;
    const auto& currentCjkFont   = settings.preferredCjkFont;
    if ( currentAsciiFont != m_loadedAsciiFontPreference ) {
        m_needReload = true;
    }
    if ( currentCjkFont != m_loadedCjkFontPreference ) {
        m_needReload = true;
    }
    if ( std::abs(currentFontRasterScale() - m_loadedFontRasterScale) >
         1e-3F ) {
        m_needReload = true;
    }
    if ( m_currentSnapshot ) {
        // 逻辑线程只回报当前可见标签真正缺失的码点；收到新码点后才触发
        // 低频图集重建，避免每帧扫描整个项目资源表。
        for ( std::size_t index = 0U;
              index < m_currentSnapshot->requestedUnicodeGlyphCount;
              ++index ) {
            const auto codepoint =
                m_currentSnapshot->requestedUnicodeGlyphs[index];
            if ( m_unicodeFontMetrics.glyph(codepoint) ||
                 std::find(m_requestedUnicodeCodepoints.begin(),
                           m_requestedUnicodeCodepoints.end(),
                           codepoint) != m_requestedUnicodeCodepoints.end() ) {
                continue;
            }
            m_requestedUnicodeCodepoints.push_back(codepoint);
            m_needReload = true;
        }
    }
    return std::exchange(m_needReload, false);
}

}  // namespace MMM::Canvas
