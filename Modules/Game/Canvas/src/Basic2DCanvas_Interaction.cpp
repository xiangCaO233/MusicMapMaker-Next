#include "canvas/Basic2DCanvas.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/UIManager.h"
#include "ui/imgui/SideBarUI.h"
#include <algorithm>
#include <filesystem>

namespace MMM::Canvas
{

void Basic2DCanvas::handleDrops(UI::UIManager* sourceManager)
{
    if ( m_pendingDrops.empty() ) return;

    bool isHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    if ( isHovered ) {
        for ( const auto& drop : m_pendingDrops ) {
            if ( !drop.paths.empty() ) {
                std::filesystem::path p(drop.paths[0]);
                std::filesystem::path projectPath =
                    std::filesystem::is_directory(p) ? p : p.parent_path();
                auto ext = p.extension().string();

                XINFO("File dropped on Canvas: {}, opening project: {}",
                      p.string(),
                      projectPath.string());

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
                if ( ext == ".osu" || ext == ".imd" || ext == ".mc" ) {
                    XINFO("Auto-loading beatmap from drop: {}",
                          p.filename().string());
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

void Basic2DCanvas::handleHotkeys()
{
    if ( !m_currentSnapshot ) return;

    if ( ImGui::IsKeyPressed(ImGuiKey_Space, false) ) {
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdSetPlayState{ !m_currentSnapshot->isPlaying }));
    }

    // --- 快捷键：工具切换 (V/E: Move, M/Q: Marquee, P/W: Draw, C/R: Cut) ---
    if ( ImGui::IsKeyPressed(ImGuiKey_V, false) ||
         ImGui::IsKeyPressed(ImGuiKey_E, false) ) {
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdChangeTool{ Logic::EditTool::Move }));
    } else if ( ImGui::IsKeyPressed(ImGuiKey_M, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Q, false) ) {
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdChangeTool{ Logic::EditTool::Marquee }));
    } else if ( ImGui::IsKeyPressed(ImGuiKey_P, false) ||
                ImGui::IsKeyPressed(ImGuiKey_W, false) ) {
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdChangeTool{ Logic::EditTool::Draw }));
    } else if ( ImGui::IsKeyPressed(ImGuiKey_C, false) ||
                ImGui::IsKeyPressed(ImGuiKey_R, false) ) {
        if ( !ImGui::GetIO().KeyCtrl ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdChangeTool{ Logic::EditTool::Cut }));
        }
    }

    // --- 快捷键：编辑操作 (Ctrl+C/X/V, Ctrl+Z/Y) ---
    if ( ImGui::GetIO().KeyCtrl ) {
        if ( ImGui::IsKeyPressed(ImGuiKey_C, false) ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdCopy{}));
        } else if ( ImGui::IsKeyPressed(ImGuiKey_X, false) ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdCut{}));
        } else if ( ImGui::IsKeyPressed(ImGuiKey_V, false) ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdPaste{}));
        } else if ( ImGui::IsKeyPressed(ImGuiKey_Z, false) ) {
            if ( ImGui::GetIO().KeyShift ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdRedo{}));
            } else {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdUndo{}));
            }
        } else if ( ImGui::IsKeyPressed(ImGuiKey_Y, false) ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdRedo{}));
        }
    }
}

void Basic2DCanvas::handleInteractions()
{
    if ( !m_currentSnapshot ) return;

    ImVec2 mousePos      = ImGui::GetMousePos();
    ImVec2 windowPos     = ImGui::GetCursorScreenPos();
    ImVec2 localMousePos = { mousePos.x - windowPos.x,
                             mousePos.y - windowPos.y };

    bool isHovered  = ImGui::IsWindowHovered();
    bool isDragging = ImGui::IsMouseDragging(0);

    Event::EventBus::instance().publish(
        Event::LogicCommandEvent(Logic::CmdSetMousePosition{ m_cameraId,
                                                             localMousePos.x,
                                                             localMousePos.y,
                                                             isHovered,
                                                             isDragging }));

    // --- 交互：显示精确时间戳工具提示 ---
    if ( isHovered && m_currentSnapshot->isHoveringCanvas &&
         !m_currentSnapshot->isPlaying ) {
        auto& visual = Config::AppConfig::instance().getVisualConfig();
        auto& layout = visual.trackLayout;

        float normX = localMousePos.x / m_targetWidth;
        float normY = localMousePos.y / m_targetHeight;

        bool isInTrackLayout = (normX >= layout.left && normX <= layout.right &&
                                normY >= layout.top && normY <= layout.bottom);

        if ( isInTrackLayout ) {
            bool isEditTool =
                (m_currentSnapshot->currentTool != Logic::EditTool::Move &&
                 m_currentSnapshot->currentTool != Logic::EditTool::Marquee);

            if ( m_currentSnapshot->isSnapped || isEditTool ||
                 m_currentSnapshot->hoveredNoteNumerator > 0 ) {
                ImGui::SetNextWindowPos(
                    ImVec2(mousePos.x + 15, mousePos.y + 15));
                ImGui::SetNextWindowBgAlpha(0.7f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    ImVec2(12, 12));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));

                ImGui::BeginTooltip();

                if ( m_currentSnapshot->hoveredNoteNumerator > 0 ) {
                    ImGui::TextColored(
                        ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                        "%s: %d/%d",
                        TR("ui.canvas.note_fraction").data(),
                        m_currentSnapshot->hoveredNoteNumerator,
                        m_currentSnapshot->hoveredNoteDenominator);
                    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                       "%s: %.3f s",
                                       TR("ui.canvas.note_time").data(),
                                       m_currentSnapshot->hoveredNoteTime);
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                }

                if ( m_currentSnapshot->isSnapped ) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                                       "%s: %.3f s",
                                       TR("ui.canvas.snap").data(),
                                       m_currentSnapshot->snappedTime);

                    if ( m_currentSnapshot->snappedNumerator == 1 &&
                         m_currentSnapshot->snappedDenominator == 1 ) {
                        ImGui::TextColored(
                            ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
                            "%s (1/1)",
                            TR("ui.canvas.beat_fraction").data());
                    } else {
                        ImGui::TextColored(
                            ImVec4(0.8f, 0.9f, 1.0f, 1.0f),
                            "%s (%d/%d)",
                            TR("ui.canvas.beat_fraction").data(),
                            m_currentSnapshot->snappedNumerator,
                            m_currentSnapshot->snappedDenominator);
                    }
                } else {
                    ImGui::Text("%s: %.3f s",
                                TR("ui.canvas.time").data(),
                                m_currentSnapshot->hoveredTime);
                }

                if ( m_currentSnapshot->hoveredBeatIndex > 0 ) {
                    ImGui::Text("%s: %d",
                                TR("ui.canvas.beat_index").data(),
                                m_currentSnapshot->hoveredBeatIndex);
                }

                ImGui::Text("%s: %d",
                            TR("ui.canvas.track").data(),
                            m_currentSnapshot->hoveredTrack + 1);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f),
                                   "%s: %d",
                                   TR("ui.canvas.beat_divisor").data(),
                                   m_currentSnapshot->currentBeatDivisor);

                ImGui::EndTooltip();
                ImGui::PopStyleVar(2);
            }
        }
    }

    entt::entity hoveredEntity   = entt::null;
    uint8_t      hoveredPart     = 0;
    int          hoveredSubIndex = -1;

    for ( auto it = m_currentSnapshot->hitboxes.rbegin();
          it != m_currentSnapshot->hitboxes.rend();
          ++it ) {
        if ( localMousePos.x >= it->x && localMousePos.x <= it->x + it->w &&
             localMousePos.y >= it->y && localMousePos.y <= it->y + it->h ) {
            hoveredEntity   = it->entity;
            hoveredPart     = static_cast<uint8_t>(it->part);
            hoveredSubIndex = it->subIndex;
            break;
        }
    }

    Event::EventBus::instance().publish(
        Event::LogicCommandEvent(Logic::CmdSetHoveredEntity{
            hoveredEntity, hoveredPart, hoveredSubIndex }));

    if ( ImGui::IsMouseClicked(0) ) {
        if ( isHovered ) {
            if ( m_currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdStartMarquee{
                        m_cameraId, localMousePos.x, localMousePos.y }));
            } else if ( hoveredEntity != entt::null ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdSelectEntity{
                        hoveredEntity, !ImGui::GetIO().KeyCtrl }));

                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdStartDrag{ hoveredEntity, m_cameraId }));
            } else {
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdSelectEntity{ entt::null, true }));
            }
        }
    }

    if ( ImGui::IsMouseDragging(0) ) {
        if ( m_currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdUpdateMarquee{ localMousePos.x, localMousePos.y }));
        } else {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdUpdateDrag{
                    m_cameraId, localMousePos.x, localMousePos.y }));
        }
    }

    if ( ImGui::IsMouseReleased(0) ) {
        if ( m_currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndMarquee{}));
        } else {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndDrag{ m_cameraId }));
        }
    }

    // --- 交互：鼠标滚轮控制时间跳转与属性修改 ---
    float wheel = ImGui::GetIO().MouseWheel;
    if ( isHovered && std::abs(wheel) > 0.01f ) {
        bool isCtrlPressed  = ImGui::GetIO().KeyCtrl;
        bool isAltPressed   = ImGui::GetIO().KeyAlt;
        bool isShiftPressed = ImGui::GetIO().KeyShift;

        if ( isCtrlPressed ) {
            auto  editorCfg = Logic::EditorEngine::instance().getEditorConfig();
            int   direction = editorCfg.settings.reverseScroll ? -1 : 1;
            float adjustedWheel = wheel * direction;
            float step          = 0.1f;
            if ( isShiftPressed )
                step *= editorCfg.settings.scrollSpeedMultiplier;
            editorCfg.visual.timelineZoom += adjustedWheel * step;
            editorCfg.visual.timelineZoom =
                std::clamp(editorCfg.visual.timelineZoom, 0.1f, 10.0f);
            Logic::EditorEngine::instance().setEditorConfig(editorCfg);
        } else if ( isAltPressed ) {
            auto  editorCfg = Logic::EditorEngine::instance().getEditorConfig();
            int   direction = editorCfg.settings.reverseScroll ? -1 : 1;
            float adjustedWheel = wheel * direction;

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
        } else {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdScroll{ m_cameraId, -wheel, isShiftPressed }));
        }
    }
}

}  // namespace MMM::Canvas
