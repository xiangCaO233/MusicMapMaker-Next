#include "canvas/Basic2DCanvasInteraction.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
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
#include "audio/AudioManager.h"
#include "config/skin/SkinConfig.h"
#include <algorithm>
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
        ImGui::SetNextWindowPos(
            ImVec2(viewport->Size.x * 0.5f, viewport->Size.y * 0.8f),
            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.7f);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
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
                if ( ext == ".osu" || ext == ".imd" || ext == ".mc" ||
                     ext == ".mmm" ) {
                    auto        u8 = p.filename().u8string();
                    std::string u8_filename(
                        reinterpret_cast<const char*>(u8.c_str()), u8.size());
                    XINFO("Auto-loading beatmap from drop: {}", u8_filename);
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
    if ( ImGui::IsKeyPressed(ImGuiKey_Space, false) && !io.KeyCtrl &&
         !io.KeyShift && !io.KeyAlt && !io.KeySuper ) {
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdSetPlayState{ !currentSnapshot->isPlaying }));
    }

    // --- 快捷键：工具切换 (1: Move, 2: Marquee, 3: Draw) ---
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
    } else {
        if ( ImGui::IsKeyPressed(ImGuiKey_Delete, false) ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdDeleteSelected{}));
        }
    }
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

    Event::EventBus::instance().publish(
        Event::LogicCommandEvent(Logic::CmdSetMousePosition{ m_cameraId,
                                                             localMousePos.x,
                                                             localMousePos.y,
                                                             isHovered,
                                                             isDragging }));

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
                 currentSnapshot->hoveredNoteNumerator > 0 ) {
                ImGui::SetNextWindowPos(
                    ImVec2(mousePos.x + 15, mousePos.y + 15));
                ImGui::SetNextWindowBgAlpha(0.7f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    ImVec2(12, 12));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));

                ImGui::BeginTooltip();

                if ( currentSnapshot->hoveredNoteNumerator > 0 ) {
                    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                       "%s: %d + %d/%d",
                                       TR("ui.canvas.note_fraction").data(),
                                       currentSnapshot->hoveredNoteBeatIndex,
                                       currentSnapshot->hoveredNoteNumerator,
                                       currentSnapshot->hoveredNoteDenominator);
                    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                       "%s: %.3f s",
                                       TR("ui.canvas.note_time").data(),
                                       currentSnapshot->hoveredNoteTime);
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                }

                if ( currentSnapshot->isSnapped ) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                                       "%s: %.3f s",
                                       TR("ui.canvas.snap").data(),
                                       currentSnapshot->snappedTime);

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
                    ImGui::Text("%s: %.3f s",
                                TR("ui.canvas.time").data(),
                                currentSnapshot->hoveredTime);
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

                ImGui::EndTooltip();
                ImGui::PopStyleVar(2);
            }
        }
    }

    entt::entity hoveredEntity   = entt::null;
    uint8_t      hoveredPart     = 0;
    int          hoveredSubIndex = -1;

    for ( auto it = currentSnapshot->hitboxes.rbegin();
          it != currentSnapshot->hitboxes.rend();
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
                if ( hoveredEntity != entt::null ) {
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
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdStartBrush{ m_cameraId,
                                          localMousePos.x,
                                          localMousePos.y,
                                          ImGui::GetIO().KeyShift,
                                          ImGui::GetIO().KeyCtrl }));
            }
        }
    }

    if ( ImGui::IsMouseDragging(0) ) {
        if ( currentSnapshot->currentTool == Logic::EditTool::Marquee ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdUpdateMarquee{ localMousePos.x, localMousePos.y }));
        } else if ( currentSnapshot->currentTool == Logic::EditTool::Draw ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdUpdateBrush{ m_cameraId,
                                       localMousePos.x,
                                       localMousePos.y,
                                       ImGui::GetIO().KeyShift,
                                       ImGui::GetIO().KeyCtrl }));
        } else {
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
    if ( currentSnapshot->currentTool == Logic::EditTool::Draw ) {
        if ( ImGui::IsMouseClicked(1) && isHovered ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdStartErase{ m_cameraId }));
        }
        if ( ImGui::IsMouseDragging(1) ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdUpdateErase{
                    m_cameraId, localMousePos.x, localMousePos.y }));
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
            double currentSpeed = Audio::AudioManager::instance().getPlaybackSpeed();

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
