#include "canvas/TimeFormatUtils.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace MMM::UI
{
namespace
{
/// @brief 计算状态栏当前应显示的动画时间。
/// @param snapshot 当前活动画布快照。
/// @return 已按 UI 当前帧补偿后的显示时间，单位秒。
/// @warning UI 热路径：每帧状态栏绘制调用；只做常量级时间计算。
double resolveStatusBarAnimateTime(const Logic::RenderSnapshot& snapshot)
{
    double animateTime = snapshot.currentTime;
    if ( !snapshot.isPlaying || snapshot.snapshotSysTime <= 0.0 ||
         !std::isfinite(snapshot.playbackSpeed) ) {
        return animateTime;
    }

    const double now = std::chrono::duration<double>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    const double dt  = now - snapshot.snapshotSysTime;
    if ( dt <= 0.0 || dt >= 0.1 ) {
        return animateTime;
    }

    animateTime += dt * snapshot.playbackSpeed;
    if ( !std::isfinite(animateTime) ) {
        return snapshot.currentTime;
    }
    return animateTime;
}
}  // namespace

/// @brief 渲染主窗口底部状态栏。
/// @warning UI 热路径：每帧调用；只读取当前活动画布快照和轻量状态。
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
    const ImVec4 statusTextColor = ImGui::GetStyle().Colors[ImGuiCol_TextLink];
    ImGui::PushStyleColor(ImGuiCol_Text, statusTextColor);
    ImGui::PushStyleColor(
        ImGuiCol_TextDisabled,
        ImVec4(
            statusTextColor.x, statusTextColor.y, statusTextColor.z, 0.6200f));
    ImGui::PushStyleColor(
        ImGuiCol_Separator,
        ImVec4(
            statusTextColor.x, statusTextColor.y, statusTextColor.z, 0.3600f));
    ImGui::PushStyleColor(
        ImGuiCol_Border,
        ImVec4(
            statusTextColor.x, statusTextColor.y, statusTextColor.z, 0.3000f));

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
        const std::string_view statusMessage =
            m_statusMessageService.getStatusMessage();
        if ( !statusMessage.empty() ) {
            ImGui::TextUnformatted(statusMessage.data(),
                                   statusMessage.data() + statusMessage.size());
        } else {
            ImGui::Text("%s", TR("ui.status.ready").data());
        }

        ImGui::SameLine();
        ImGui::SetCursorPosY(offsetY);
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        auto&       engine         = Logic::EditorEngine::instance();
        std::string activeCameraId = engine.getActiveCameraId();
        auto        syncBuffer     = engine.getSyncBuffer(
            activeCameraId.empty() ? "Basic2DCanvas" : activeCameraId);
        if ( syncBuffer ) {
            auto snapshot = syncBuffer->getReadingSnapshot();
            if ( snapshot ) {
                // 判定线时间 (常驻)
                const double displayedTime =
                    resolveStatusBarAnimateTime(*snapshot);
                const auto currentTimeText =
                    Canvas::formatCanvasTime(displayedTime, snapshot);
                ImGui::SetCursorPosY(offsetY);
                ImGui::Text("%s: %s",
                            TR("ui.canvas.time").data(),
                            currentTimeText.c_str());

                // 鼠标位置时间 (仅在主画布悬浮时显示)
                const double visibleStart = std::min(snapshot->visibleTimeStart,
                                                     snapshot->visibleTimeEnd);
                const double visibleEnd   = std::max(snapshot->visibleTimeStart,
                                                     snapshot->visibleTimeEnd);
                const bool   hasValidHoveredTime =
                    std::isfinite(snapshot->hoveredTime) &&
                    (!std::isfinite(visibleStart) ||
                     snapshot->hoveredTime >= visibleStart - 1.0) &&
                    (!std::isfinite(visibleEnd) ||
                     snapshot->hoveredTime <= visibleEnd + 1.0);
                if ( snapshot->isHoveringCanvas && hasValidHoveredTime ) {
                    const auto hoveredTimeText = Canvas::formatCanvasTime(
                        snapshot->hoveredTime, snapshot);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::Text("%s: %s",
                                TR("ui.status.mouse_time").data(),
                                hoveredTimeText.c_str());
                }

                // 物件数量与最大连击数统计由逻辑线程写入快照，避免 UI 每帧访问
                // Session 锁和 ECS。
                if ( snapshot->hasBeatmap ) {
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::Text("BPM: %.3f", snapshot->currentBpm);

                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::Text("SV: %.4f", snapshot->currentSv);

                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::Text("%s: %zu",
                                TR("ui.status.note_count").data(),
                                snapshot->noteCount);

                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::Text("%s: %zu",
                                TR("ui.status.max_combo").data(),
                                snapshot->maxCombo);
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

    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(3);
}

}  // namespace MMM::UI
