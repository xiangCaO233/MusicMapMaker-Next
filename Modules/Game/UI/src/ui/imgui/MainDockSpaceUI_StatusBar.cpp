#include "config/skin/SkinConfig.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/session/context/SessionContext.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include <cmath>
#include <fmt/format.h>

namespace MMM::UI
{

void MainDockSpaceUI::renderStatusBar(UIManager* sourceManager,
                                      float statusBarHeight, float dpiScale)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // 设置状态栏位置：位于主视口底部
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x,
               viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, statusBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    // 窗口标志：无标题栏、禁止收缩、禁止调整大小、禁止移动、禁止置顶、禁止停靠、无滚动条
    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar;

    // 移除圆角和边框，以及内边距
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(4.0f * dpiScale, 0.0f));  // 左右留点边距，上下为0

    // 背景颜色 (同步为 MenuBarBg)
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg]);

    if ( ImGui::Begin("StatusBar", nullptr, window_flags) ) {
        // 在状态栏顶部画一根分隔线
        ImVec2 p1 = ImGui::GetWindowPos();
        ImVec2 p2 = ImVec2(p1.x + ImGui::GetWindowWidth(), p1.y);
        ImGui::GetWindowDrawList()->AddLine(
            p1, p2, ImGui::GetColorU32(ImGuiCol_Border), 1.0f);

        // 垂直居中处理
        float textHeight = ImGui::GetFontSize();
        float offsetY    = (statusBarHeight - textHeight) / 2.0f;
        ImGui::SetCursorPosY(offsetY);

        // 渲染状态栏内容
        std::string menuStatus = m_mainMenuview.getStatusMessage();
        if ( !menuStatus.empty() ) {
            ImGui::Text("%s", menuStatus.c_str());
        } else {
            ImGui::Text("%s", TR("ui.status.ready").data());
        }

        ImGui::SameLine();
        ImGui::SetCursorPosY(offsetY);
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        auto& engine     = Logic::EditorEngine::instance();
        auto  syncBuffer = engine.getSyncBuffer("Basic2DCanvas");
        if ( syncBuffer ) {
            auto snapshot = syncBuffer->getReadingSnapshot();
            if ( snapshot ) {
                auto formatTime = [](double seconds) {
                    int totalMillis =
                        static_cast<int>(std::round(seconds * 1000.0));
                    int ms = std::abs(totalMillis % 1000);
                    int s  = std::abs((totalMillis / 1000) % 60);
                    int m  = (totalMillis / 60000);
                    return fmt::format("{:02d}:{:02d}.{:03d}", m, s, ms);
                };

                // 判定线时间 (常驻)
                ImGui::SetCursorPosY(offsetY);
                ImGui::Text("%s: %s",
                            TR("ui.canvas.time").data(),
                            formatTime(snapshot->currentTime).c_str());

                // 鼠标位置时间 (仅在主画布悬浮时显示)
                if ( snapshot->isHoveringCanvas ) {
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::Text("%s: %s",
                                TR("ui.status.mouse_time").data(),
                                formatTime(snapshot->hoveredTime).c_str());
                }

                // 物件数量与最大连击数统计 (仅在谱面打开时显示)
                auto session = engine.getActiveSession();
                if ( session ) {
                    size_t totalPlayableNotes = 0;
                    size_t normalNotes        = 0;
                    size_t holds              = 0;
                    size_t flicks             = 0;

                    auto noteView =
                        session->getContext()
                            .noteRegistry.view<Logic::NoteComponent>();
                    for ( auto entity : noteView ) {
                        const auto& nc =
                            noteView.get<Logic::NoteComponent>(entity);
                        if ( nc.m_type == ::MMM::NoteType::POLYLINE ) {
                            for ( const auto& sub : nc.m_subNotes ) {
                                if ( sub.type == ::MMM::NoteType::NOTE )
                                    normalNotes++;
                                else if ( sub.type == ::MMM::NoteType::HOLD )
                                    holds++;
                                else if ( sub.type == ::MMM::NoteType::FLICK )
                                    flicks++;
                            }
                        } else if ( !nc.m_isSubNote ) {
                            if ( nc.m_type == ::MMM::NoteType::NOTE )
                                normalNotes++;
                            else if ( nc.m_type == ::MMM::NoteType::HOLD )
                                holds++;
                            else if ( nc.m_type == ::MMM::NoteType::FLICK )
                                flicks++;
                        }
                    }
                    totalPlayableNotes = normalNotes + holds + flicks;

                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::Text("%s: %zu",
                                TR("ui.status.note_count").data(),
                                totalPlayableNotes);

                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::Text(
                        "%s: %zu",
                        TR("ui.status.max_combo").data(),
                        totalPlayableNotes);  // Combo数目前展示为物件数量
                }

                // 在状态栏最右侧显示最后一次操作信息
                if ( !snapshot->lastActionMessage.empty() ) {
                    float textWidth =
                        ImGui::CalcTextSize(snapshot->lastActionMessage.c_str())
                            .x;
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - textWidth -
                                         8.0f * dpiScale);
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::TextDisabled("%s",
                                        snapshot->lastActionMessage.c_str());
                }
            }
        }

        ImGui::End();
    }

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

}  // namespace MMM::UI
