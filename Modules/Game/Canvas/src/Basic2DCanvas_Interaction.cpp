#include "audio/AudioManager.h"
#include "canvas/Basic2DCanvasInteraction.h"
#include "canvas/TimeFormatUtils.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/input/glfw/GLFWDropEvent.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/UIManager.h"
#include "ui/imgui/SideBarUI.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>

namespace MMM::Canvas
{

Basic2DCanvasInteraction::Basic2DCanvasInteraction(
    const std::string& canvasName, const std::string& cameraId)
    : m_canvasName(canvasName), m_cameraId(cameraId)
{
    m_dropSubId = Event::EventBus::instance().subscribe<Event::GLFWDropEvent>(
        [this](const Event::GLFWDropEvent& e) {
            XINFO("CanvasInteraction received GLFWDropEvent with {} paths",
                  e.paths.size());
            m_pendingDrops.push_back({ e.paths, e.pos });
        });
}

Basic2DCanvasInteraction::~Basic2DCanvasInteraction()
{
    Event::EventBus::instance().unsubscribe<Event::GLFWDropEvent>(m_dropSubId);
}

void Basic2DCanvasInteraction::update(
    UI::UIManager* sourceManager, const Logic::RenderSnapshot* currentSnapshot,
    float targetWidth, float targetHeight)
{
    handleDrops(sourceManager);

    if ( currentSnapshot ) {
        handleHotkeys(currentSnapshot);
        handleInteractions(currentSnapshot, targetWidth, targetHeight);
    }

    if ( m_speedTooltipTimer > 0.0f ) {
        m_speedTooltipTimer -= ImGui::GetIO().DeltaTime;

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2         mousePos = ImGui::GetMousePos();

        // 始终跟随鼠标，并根据屏幕位置自动调整对齐方式（边缘翻转）
        ImVec2 pivot = ImVec2(0.0f, 0.0f);
        if ( mousePos.x > viewport->WorkPos.x + viewport->WorkSize.x * 0.7f )
            pivot.x = 1.0f;
        if ( mousePos.y > viewport->WorkPos.y + viewport->WorkSize.y * 0.7f )
            pivot.y = 1.0f;

        float offsetX = (pivot.x == 0.0f) ? 20.0f : -20.0f;
        float offsetY = (pivot.y == 0.0f) ? 20.0f : -20.0f;

        ImGui::SetNextWindowPos(
            ImVec2(mousePos.x + offsetX, mousePos.y + offsetY),
            ImGuiCond_Always,
            pivot);
        ImGui::SetNextWindowBgAlpha(0.7f);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 10));
        if ( ImGui::Begin("##SpeedTooltip", nullptr, flags) ) {
            ImFont* font = Config::SkinManager::instance().getFont("content");
            if ( font ) ImGui::PushFont(font);
            ImGui::Text("Playback Speed: %.2fx", m_speedTooltipValue);
            if ( font ) ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}

void Basic2DCanvasInteraction::handleDrops(UI::UIManager* sourceManager)
{
    if ( m_pendingDrops.empty() ) return;

    bool isHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    if ( isHovered ) {
        for ( const auto& drop : m_pendingDrops ) {
            if ( !drop.paths.empty() ) {
                std::filesystem::path p = Config::utf8ToPath(drop.paths[0]);
                std::filesystem::path projectPath =
                    std::filesystem::is_directory(p) ? p : p.parent_path();
                auto ext = Config::pathToUtf8(p.extension());

                XINFO("File dropped on Canvas: {}, opening project: {}",
                      Config::pathToUtf8(p),
                      Config::pathToUtf8(projectPath));

                // 1. 打开项目
                Event::OpenProjectEvent ev;
                ev.m_projectPath = projectPath;
                Event::EventBus::instance().publish(ev);

                // 2. 跳转到谱面管理器
                Event::UISubViewToggleEvent evt;
                evt.sourceUiName           = m_canvasName;
                evt.uiManager              = sourceManager;
                evt.targetFloatManagerName = "SideBarManager";
                evt.subViewId =
                    UI::TabToSubViewId(UI::SideBarTab::BeatMapExplorer);
                evt.showSubView = true;
                Event::EventBus::instance().publish(evt);

                // 3. 如果是谱面文件，直接加载
                if ( ext == ".osu" || ext == ".imd" || ext == ".mc" ||
                     ext == ".mmm" ) {
                    XINFO("Auto-loading beatmap from drop: {}",
                          Config::pathToUtf8(p.filename()));
                    try {
                        auto loadedBeatmap = std::make_shared<MMM::BeatMap>(
                            MMM::BeatMap::loadFromFile(p));
                        Logic::EditorEngine::instance().pushCommand(
                            Logic::CmdLoadBeatmap{ loadedBeatmap });
                    } catch ( const std::exception& e ) {
                        XERROR("Failed to load dropped beatmap: {}", e.what());
                    }
                }
            }
        }
    }
    m_pendingDrops.clear();
}

void Basic2DCanvasInteraction::handleHotkeys(
    const Logic::RenderSnapshot* currentSnapshot)
{
    auto& io = ImGui::GetIO();

    // 如果 ImGui 当前处于文本输入状态，跳过画布快捷键处理 (如 Delete 键、1/2/3
    // 工具切换键)
    if ( io.WantTextInput ) return;

    // --- 快捷键：工具切换 (1: Move, 2: Marquee, 3: Draw) ---
    // 这些是画布特有的，保留在这里
    if ( ImGui::IsKeyPressed(ImGuiKey_1, false) ) {
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdChangeTool{ Logic::EditTool::Move }));
    } else if ( ImGui::IsKeyPressed(ImGuiKey_2, false) ) {
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdChangeTool{ Logic::EditTool::Marquee }));
    } else if ( ImGui::IsKeyPressed(ImGuiKey_3, false) ) {
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdChangeTool{ Logic::EditTool::Draw }));
    }

    // --- 快捷键：删除操作 ---
    // 目前菜单栏没有 Delete，保留在这里
    if ( !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper ) {
        if ( ImGui::IsKeyPressed(ImGuiKey_Delete, false) ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdDeleteSelected{}));
        }
    }

    // 注意：Ctrl+C/V/X/Z/Y 和 Space (播放/暂停) 已由全局 MainMenuView 处理，
    // 在此处移除以防止重复触发。
}

void Basic2DCanvasInteraction::handleInteractions(
    const Logic::RenderSnapshot* currentSnapshot, float targetWidth,
    float targetHeight)
{
    ImVec2 mousePos      = ImGui::GetMousePos();
    ImVec2 windowPos     = ImGui::GetCursorScreenPos();
    ImVec2 localMousePos = { mousePos.x - windowPos.x,
                             mousePos.y - windowPos.y };

    bool isHovered  = ImGui::IsWindowHovered();
    bool isDragging = ImGui::IsMouseDragging(0);

    constexpr float mouseEpsilon = 0.1f;
    bool            shouldSendMouse =
        !m_lastMouseCommand.valid ||
        std::abs(m_lastMouseCommand.pos.x - localMousePos.x) > mouseEpsilon ||
        std::abs(m_lastMouseCommand.pos.y - localMousePos.y) > mouseEpsilon ||
        std::abs(m_lastMouseCommand.viewportWidth - targetWidth) >
            mouseEpsilon ||
        std::abs(m_lastMouseCommand.viewportHeight - targetHeight) >
            mouseEpsilon ||
        m_lastMouseCommand.isHovering != isHovered ||
        m_lastMouseCommand.isDragging != isDragging;

    if ( shouldSendMouse ) {
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdSetMousePosition{ .cameraId       = m_cameraId,
                                        .mouseX         = localMousePos.x,
                                        .mouseY         = localMousePos.y,
                                        .viewportWidth  = targetWidth,
                                        .viewportHeight = targetHeight,
                                        .isHovering     = isHovered,
                                        .isDragging     = isDragging }));
        m_lastMouseCommand.valid         = true;
        m_lastMouseCommand.pos           = { localMousePos.x, localMousePos.y };
        m_lastMouseCommand.viewportWidth = targetWidth;
        m_lastMouseCommand.viewportHeight = targetHeight;
        m_lastMouseCommand.isHovering     = isHovered;
        m_lastMouseCommand.isDragging     = isDragging;
    }

    // --- 交互：显示精确时间戳工具提示 ---
    if ( isHovered && currentSnapshot->isHoveringCanvas &&
         !currentSnapshot->isPlaying ) {
        auto& visual = Config::AppConfig::instance().getVisualConfig();
        auto& layout = visual.trackLayout;

        float normX = localMousePos.x / targetWidth;
        float normY = localMousePos.y / targetHeight;

        bool isInTrackLayout = (normX >= layout.left && normX <= layout.right &&
                                normY >= layout.top && normY <= layout.bottom);

        if ( isInTrackLayout ) {
            bool isEditTool =
                (currentSnapshot->currentTool != Logic::EditTool::Move &&
                 currentSnapshot->currentTool != Logic::EditTool::Marquee);

            if ( currentSnapshot->isSnapped || isEditTool ||
                 currentSnapshot->hoverInspect.show ) {
                ImGui::SetNextWindowPos(
                    ImVec2(mousePos.x + 15, mousePos.y + 15));
                ImGui::SetNextWindowBgAlpha(0.7f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    ImVec2(12, 12));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));

                ImGui::BeginTooltip();

                if ( currentSnapshot->hoverInspect.show ) {
                    const auto& inspect = currentSnapshot->hoverInspect;
                    auto drawPoint = [currentSnapshot](
                                         const char*                  labelKey,
                                         const Logic::HoverBeatPoint& point,
                                         bool showTrack) {
                        if ( !point.show ) return;
                        const auto label = TR(labelKey);
                        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                           "%s %s: %d + %d/%d",
                                           label.data(),
                                           TR("ui.canvas.note_fraction").data(),
                                           point.beatIndex,
                                           point.numerator,
                                           point.denominator);
                        const auto timeText =
                            formatCanvasTime(point.time, currentSnapshot);
                        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                           "%s %s: %s",
                                           label.data(),
                                           TR("ui.canvas.note_time").data(),
                                           timeText.c_str());
                        if ( showTrack ) {
                            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                               "%s %s: %d",
                                               label.data(),
                                               TR("ui.canvas.track").data(),
                                               point.track + 1);
                        }
                    };

                    switch ( inspect.kind ) {
                    case Logic::HoverInspectKind::Note:
                        drawPoint("ui.canvas.hover.note",
                                  inspect.head,
                                  inspect.showTrack);
                        break;
                    case Logic::HoverInspectKind::HoldHead:
                        drawPoint("ui.canvas.hover.head", inspect.head, true);
                        break;
                    case Logic::HoverInspectKind::HoldEnd:
                    case Logic::HoverInspectKind::PolylineHoldEnd:
                        drawPoint(
                            "ui.canvas.hover.hold_end", inspect.end, true);
                        break;
                    case Logic::HoverInspectKind::FlickHead:
                        drawPoint(
                            "ui.canvas.hover.flick_head", inspect.head, true);
                        break;
                    case Logic::HoverInspectKind::FlickBody:
                    case Logic::HoverInspectKind::PolylineFlickBody:
                        drawPoint(
                            "ui.canvas.hover.flick_body", inspect.body, false);
                        break;
                    case Logic::HoverInspectKind::FlickEnd:
                    case Logic::HoverInspectKind::PolylineFlickEnd:
                        drawPoint(
                            "ui.canvas.hover.flick_end", inspect.end, true);
                        break;
                    case Logic::HoverInspectKind::PolylineHead:
                        drawPoint("ui.canvas.hover.polyline_head",
                                  inspect.body,
                                  inspect.showTrack);
                        break;
                    case Logic::HoverInspectKind::PolylineNode:
                        drawPoint("ui.canvas.hover.polyline_node",
                                  inspect.body,
                                  inspect.showTrack);
                        break;
                    case Logic::HoverInspectKind::HoldBody:
                    case Logic::HoverInspectKind::PolylineHoldBody:
                    case Logic::HoverInspectKind::None: break;
                    }

                    if ( inspect.showDuration ) {
                        const auto durationText =
                            formatCanvasDuration(inspect.duration);
                        ImGui::TextColored(
                            ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                            "%s: %s",
                            TR("ui.canvas.hover.duration").data(),
                            durationText.c_str());
                    }
                    if ( inspect.showDtrack ) {
                        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                           "%s: %d",
                                           TR("ui.canvas.hover.dtrack").data(),
                                           inspect.dtrack);
                    }
                    if ( inspect.showTrack &&
                         (inspect.kind == Logic::HoverInspectKind::HoldBody ||
                          inspect.kind ==
                              Logic::HoverInspectKind::PolylineHoldBody) ) {
                        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                           "%s: %d",
                                           TR("ui.canvas.track").data(),
                                           inspect.track + 1);
                    }

                    // 计算鼠标当前所在的唯一物件包围框个数
                    std::vector<entt::entity> hoveredEntities;
                    bool                      includeBodyHitboxes =
                        inspect.kind == Logic::HoverInspectKind::HoldBody ||
                        inspect.kind ==
                            Logic::HoverInspectKind::PolylineHoldBody ||
                        inspect.kind == Logic::HoverInspectKind::FlickBody ||
                        inspect.kind ==
                            Logic::HoverInspectKind::PolylineFlickBody;
                    for ( const auto& hb : currentSnapshot->hitboxes ) {
                        if ( hb.entity != entt::null ) {
                            if ( !includeBodyHitboxes &&
                                 hb.part == Logic::HoverPart::HoldBody ) {
                                continue;
                            }
                            if ( localMousePos.x >= hb.x &&
                                 localMousePos.x <= hb.x + hb.w &&
                                 localMousePos.y >= hb.y &&
                                 localMousePos.y <= hb.y + hb.h ) {
                                if ( std::find(hoveredEntities.begin(),
                                               hoveredEntities.end(),
                                               hb.entity) ==
                                     hoveredEntities.end() ) {
                                    hoveredEntities.push_back(hb.entity);
                                }
                            }
                        }
                    }
                    int overlappingCount =
                        static_cast<int>(hoveredEntities.size());

                    if ( overlappingCount > 1 ) {
                        ImGui::TextColored(
                            ImVec4(1.0f, 0.2f, 0.2f, 1.0f),
                            "%s: %d%s",
                            TR("ui.canvas.overlapping_hitboxes").data(),
                            overlappingCount,
                            TR("ui.canvas.overlapping_warning").data());
                    } else {
                        ImGui::TextColored(
                            ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                            "%s: %d",
                            TR("ui.canvas.overlapping_hitboxes").data(),
                            overlappingCount);
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                }

                if ( currentSnapshot->isSnapped ) {
                    const auto timeText = formatCanvasTime(
                        currentSnapshot->snappedTime, currentSnapshot);
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                                       "%s: %s",
                                       TR("ui.canvas.snap").data(),
                                       timeText.c_str());

                    if ( currentSnapshot->snappedNumerator == 1 &&
                         currentSnapshot->snappedDenominator == 1 ) {
                        ImGui::TextColored(
                            ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
                            "%s (1/1)",
                            TR("ui.canvas.beat_fraction").data());
                    } else {
                        ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f),
                                           "%s (%d/%d)",
                                           TR("ui.canvas.beat_fraction").data(),
                                           currentSnapshot->snappedNumerator,
                                           currentSnapshot->snappedDenominator);
                    }
                } else {
                    const auto timeText = formatCanvasTime(
                        currentSnapshot->hoveredTime, currentSnapshot);
                    ImGui::Text("%s: %s",
                                TR("ui.canvas.time").data(),
                                timeText.c_str());
                }

                if ( currentSnapshot->hoveredBeatIndex > 0 ) {
                    ImGui::Text("%s: %d",
                                TR("ui.canvas.beat_index").data(),
                                currentSnapshot->hoveredBeatIndex);
                }

                ImGui::Text("%s: %d",
                            TR("ui.canvas.track").data(),
                            currentSnapshot->hoveredTrack + 1);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f),
                                   "%s: %d",
                                   TR("ui.canvas.beat_divisor").data(),
                                   currentSnapshot->currentBeatDivisor);
                if ( m_hoverLayerCount > 1 ) {
                    ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f),
                                       "%s: %d/%d  %s",
                                       TR("ui.canvas.hover.layer").data(),
                                       m_hoverLayerIndex + 1,
                                       m_hoverLayerCount,
                                       TR("ui.canvas.hover.layer_hint").data());
                }

                ImGui::EndTooltip();
                ImGui::PopStyleVar(2);
            }
        }
    }

    entt::entity hoveredEntity   = entt::null;
    uint8_t      hoveredPart     = 0;
    int          hoveredSubIndex = -1;

    struct HoverCandidate {
        entt::entity     entity{ entt::null };
        Logic::HoverPart part{ Logic::HoverPart::None };
        int              subIndex{ -1 };
    };

    std::vector<HoverCandidate> candidates;
    std::string                 layerSignature;
    if ( isHovered ) {
        for ( auto it = currentSnapshot->hitboxes.rbegin();
              it != currentSnapshot->hitboxes.rend();
              ++it ) {
            if ( localMousePos.x >= it->x && localMousePos.x <= it->x + it->w &&
                 localMousePos.y >= it->y &&
                 localMousePos.y <= it->y + it->h ) {
                candidates.push_back({ it->entity, it->part, it->subIndex });
                layerSignature +=
                    std::to_string(
                        static_cast<uint32_t>(entt::to_integral(it->entity))) +
                    ":" + std::to_string(static_cast<uint32_t>(it->part)) +
                    ":" + std::to_string(it->subIndex) + ";";
            }
        }
    }

    if ( layerSignature != m_hoverLayerSignature ) {
        m_hoverLayerSignature = layerSignature;
        m_hoverLayerIndex     = 0;
    }

    m_hoverLayerCount = static_cast<int>(candidates.size());
    if ( candidates.empty() ) {
        m_hoverLayerIndex = 0;
    } else {
        if ( m_hoverLayerIndex >= m_hoverLayerCount ) {
            m_hoverLayerIndex = m_hoverLayerCount - 1;
        }

        if ( m_hoverLayerCount > 1 && isHovered &&
             !ImGui::GetIO().WantTextInput ) {
            if ( ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
                 ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ) {
                m_hoverLayerIndex = (m_hoverLayerIndex + 1) % m_hoverLayerCount;
            } else if ( ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ) {
                m_hoverLayerIndex =
                    (m_hoverLayerIndex + m_hoverLayerCount - 1) %
                    m_hoverLayerCount;
            }
        }

        const auto& candidate = candidates[m_hoverLayerIndex];
        hoveredEntity         = candidate.entity;
        hoveredPart           = static_cast<uint8_t>(candidate.part);
        hoveredSubIndex       = candidate.subIndex;
    }

    bool shouldSendHover = !m_hasLastHovered ||
                           m_lastHoveredEntity != hoveredEntity ||
                           m_lastHoveredPart != hoveredPart ||
                           m_lastHoveredSubIndex != hoveredSubIndex;
    if ( shouldSendHover ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdSetHoveredEntity{
                hoveredEntity, hoveredPart, hoveredSubIndex }));
        m_hasLastHovered      = true;
        m_lastHoveredEntity   = hoveredEntity;
        m_lastHoveredPart     = hoveredPart;
        m_lastHoveredSubIndex = hoveredSubIndex;
    }

    if ( ImGui::IsMouseClicked(0) ) {
        if ( isHovered ) {
            if ( currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
                if ( hoveredEntity != entt::null ) {
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(Logic::CmdSelectEntity{
                            hoveredEntity, !ImGui::GetIO().KeyCtrl }));
                } else {
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdStartMarquee{ m_cameraId,
                                                    localMousePos.x,
                                                    localMousePos.y,
                                                    ImGui::GetIO().KeyCtrl }));
                }
            } else if ( currentSnapshot->currentTool ==
                        Logic::EditTool::Move ) {
                if ( !currentSnapshot->isPlaying &&
                     hoveredEntity != entt::null ) {
                    // 抓取工具不再负责选中，只负责发起拖拽
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdStartDrag{ hoveredEntity,
                                                 m_cameraId,
                                                 ImGui::GetIO().KeyCtrl }));
                } else {
                    // 抓取工具点击空白处不再清除选中（只有框选工具可以管理选中）
                }
            } else if ( currentSnapshot->currentTool ==
                        Logic::EditTool::Draw ) {
                if ( !currentSnapshot->isPlaying ) {
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdStartBrush{ m_cameraId,
                                                  localMousePos.x,
                                                  localMousePos.y,
                                                  ImGui::GetIO().KeyShift,
                                                  ImGui::GetIO().KeyCtrl }));
                }
            }
        }
    }

    if ( ImGui::IsMouseDragging(0) ) {
        if ( currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdUpdateMarquee{ localMousePos.x, localMousePos.y }));
        } else if ( !currentSnapshot->isPlaying &&
                    currentSnapshot->currentTool == Logic::EditTool::Draw ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdUpdateBrush{ m_cameraId,
                                       localMousePos.x,
                                       localMousePos.y,
                                       ImGui::GetIO().KeyShift,
                                       ImGui::GetIO().KeyCtrl }));
        } else if ( !currentSnapshot->isPlaying ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdUpdateDrag{ m_cameraId,
                                      localMousePos.x,
                                      localMousePos.y,
                                      ImGui::GetIO().KeyCtrl }));
        }
    }

    if ( ImGui::IsMouseReleased(0) ) {
        if ( currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndMarquee{}));
        } else if ( currentSnapshot->currentTool == Logic::EditTool::Draw ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndBrush{ m_cameraId }));
        } else {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndDrag{ m_cameraId }));
        }
    }

    // --- 右键交互：画笔工具下为擦除 ---
    if ( currentSnapshot->currentTool == Logic::EditTool::Draw &&
         !currentSnapshot->isPlaying ) {
        if ( ImGui::IsMouseClicked(1) && isHovered ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdStartErase{ m_cameraId, ImGui::GetIO().KeyShift }));
        }
        if ( ImGui::IsMouseDragging(1) ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdUpdateErase{ m_cameraId,
                                       localMousePos.x,
                                       localMousePos.y,
                                       ImGui::GetIO().KeyShift }));
        }
        if ( ImGui::IsMouseReleased(1) ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndErase{ m_cameraId }));
        }
    }

    // --- Ctrl+右键：移除框选框（全局可用） ---
    if ( ImGui::IsMouseClicked(1) && ImGui::GetIO().KeyCtrl ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdRemoveMarqueeAt{
                m_cameraId, localMousePos.x, localMousePos.y }));
    }

    // --- 交互：鼠标滚轮控制时间跳转与属性修改 ---
    float wheel = ImGui::GetIO().MouseWheel;
    if ( isHovered && std::abs(wheel) > 0.01f ) {
        bool isCtrlPressed  = ImGui::GetIO().KeyCtrl;
        bool isAltPressed   = ImGui::GetIO().KeyAlt;
        bool isShiftPressed = ImGui::GetIO().KeyShift;

        if ( isCtrlPressed && isAltPressed ) {
            const std::vector<double> presets = { 0.25, 0.50, 0.75, 1.0 };
            double                    currentSpeed =
                Audio::AudioManager::instance().getPlaybackSpeed();

            size_t bestIdx = 0;
            double minDiff = std::abs(currentSpeed - presets[0]);
            for ( size_t i = 1; i < presets.size(); ++i ) {
                double diff = std::abs(currentSpeed - presets[i]);
                if ( diff < minDiff ) {
                    minDiff = diff;
                    bestIdx = i;
                }
            }

            if ( wheel > 0.01f ) {
                if ( bestIdx < presets.size() - 1 ) bestIdx++;
            } else if ( wheel < -0.01f ) {
                if ( bestIdx > 0 ) bestIdx--;
            }

            double newSpeed = presets[bestIdx];
            if ( std::abs(newSpeed - currentSpeed) > 1e-4 ) {
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdSetPlaybackSpeed{ newSpeed }));
                m_speedTooltipValue = static_cast<float>(newSpeed);
                m_speedTooltipTimer = 2.0f;
            }
        } else if ( isCtrlPressed ) {
            if ( currentSnapshot->currentTool == Logic::EditTool::Marquee &&
                 currentSnapshot->isSelecting ) {
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdScroll{ m_cameraId, -wheel, isShiftPressed }));
            } else {
                auto editorCfg =
                    Logic::EditorEngine::instance().getEditorConfig();
                float adjustedWheel = wheel;
                float step          = 0.1f;
                if ( isShiftPressed )
                    step *= editorCfg.settings.scrollSpeedMultiplier;
                editorCfg.visual.timelineZoom += adjustedWheel * step;
                editorCfg.visual.timelineZoom =
                    std::clamp(editorCfg.visual.timelineZoom, 0.1f, 10.0f);
                Logic::EditorEngine::instance().setEditorConfig(editorCfg);
            }
        } else if ( isAltPressed ) {
            auto  editorCfg = Logic::EditorEngine::instance().getEditorConfig();
            float adjustedWheel = wheel;

            static std::unordered_map<std::string, float> wheelAccumulator;
            float& acc = wheelAccumulator[m_cameraId];
            acc += adjustedWheel;

            int steps = 0;
            if ( acc >= 1.0f ) {
                steps = static_cast<int>(acc);
                acc -= steps;
            } else if ( acc <= -1.0f ) {
                steps = static_cast<int>(acc);
                acc -= steps;
            }

            if ( steps != 0 ) {
                if ( isShiftPressed ) {
                    const std::vector<int> presets = {
                        1, 2, 3, 4, 6, 8, 12, 16
                    };
                    int current = editorCfg.settings.beatDivisor;

                    if ( steps > 0 ) {
                        for ( int i = 0; i < steps; ++i ) {
                            auto it = std::upper_bound(
                                presets.begin(), presets.end(), current);
                            if ( it != presets.end() ) {
                                current = *it;
                            } else {
                                current = presets.back();
                            }
                        }
                    } else {
                        for ( int i = 0; i < -steps; ++i ) {
                            auto it = std::lower_bound(
                                presets.begin(), presets.end(), current);
                            if ( it != presets.begin() ) {
                                current = *(--it);
                            } else {
                                current = presets.front();
                            }
                        }
                    }
                    editorCfg.settings.beatDivisor = current;
                } else {
                    editorCfg.settings.beatDivisor += steps;
                }
                editorCfg.settings.beatDivisor =
                    std::clamp(editorCfg.settings.beatDivisor, 1, 64);
                Logic::EditorEngine::instance().setEditorConfig(editorCfg);
            }
        } else if ( !isCtrlPressed && !isAltPressed ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdScroll{ m_cameraId, -wheel, isShiftPressed }));

            if ( !currentSnapshot->isPlaying &&
                 currentSnapshot->currentTool == Logic::EditTool::Draw &&
                 ImGui::IsMouseDown(0) ) {
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdUpdateBrush{ m_cameraId,
                                           localMousePos.x,
                                           localMousePos.y,
                                           ImGui::GetIO().KeyShift,
                                           ImGui::GetIO().KeyCtrl }));
            }
        }
    }
}

}  // namespace MMM::Canvas
