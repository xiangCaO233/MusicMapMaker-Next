#include "common/render/RenderSnapshotBuffer.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/EditorEngine.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/utils/CanvasContentVisibility.h"
#include "ui/utils/TimeFormatUtils.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

namespace MMM::UI
{
namespace
{
/// @brief 计算状态栏当前应显示的动画时间。
/// @param snapshot 当前活动画布快照。
/// @return 已按 UI 当前帧补偿后的显示时间，单位秒。
/// @warning UI 热路径：每帧状态栏绘制调用；只做常量级时间计算。
double resolveStatusBarAnimateTime(
    const Common::Render::RenderSnapshot& snapshot)
{
    const double now = std::chrono::duration<double>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    return snapshot.resolveCurrentTimeAt(now);
}

/// @brief 取得项目加载阶段对应的翻译键。
/// @param stage 当前项目加载阶段。
/// @return 状态栏阶段文本的翻译键。
const char* projectOpenProgressTranslationKey(
    Event::ProjectOpenProgressStage stage)
{
    using enum Event::ProjectOpenProgressStage;
    switch ( stage ) {
    case Validating: return "ui.status.project_loading.validating";
    case ExtractingPackage:
        return "ui.status.project_loading.extracting_package";
    case ClosingCurrentProject:
        return "ui.status.project_loading.closing_current_project";
    case ScanningDirectory:
        return "ui.status.project_loading.scanning_directory";
    case BuildingResources:
        return "ui.status.project_loading.building_resources";
    case LoadingConfiguration:
        return "ui.status.project_loading.loading_configuration";
    case MigratingConfiguration:
        return "ui.status.project_loading.migrating_configuration";
    case SavingConfiguration:
        return "ui.status.project_loading.saving_configuration";
    case PreparingAudio: return "ui.status.project_loading.preparing_audio";
    case LoadingBeatmaps: return "ui.status.project_loading.loading_beatmaps";
    case Finalizing: return "ui.status.project_loading.finalizing";
    }
    return "ui.status.project_loading.validating";
}

/// @brief 在状态栏右侧绘制项目加载阶段、当前对象和总进度。
/// @param progress UI 线程持有的项目加载进度快照。
/// @param statusBarHeight 状态栏高度。
/// @param dpiScale 当前 DPI 缩放。
/// @warning UI 热路径：仅在打开项目期间每帧调用，只读取 UI 本地快照并绘制
/// 固定数量控件。
void renderProjectOpenProgress(const ProjectOpenProgressState& progress,
                               float statusBarHeight, float dpiScale)
{
    const auto stageText =
        TR(projectOpenProgressTranslationKey(progress.stage));
    const float stageWidth = ImGui::CalcTextSize(stageText.data()).x;
    const float detailWidth =
        progress.detail.empty()
            ? 0.0F
            : ImGui::CalcTextSize(progress.detail.c_str()).x;
    const float separatorWidth =
        progress.detail.empty() ? 0.0F : ImGui::CalcTextSize(": ").x;
    const float textWidth    = stageWidth + separatorWidth + detailWidth;
    const float gap          = 8.0F * dpiScale;
    const float barWidth     = 176.0F * dpiScale;
    const float rightPadding = 8.0F * dpiScale;
    const float barX       = ImGui::GetWindowWidth() - rightPadding - barWidth;
    const float textRightX = barX - gap;
    const float minimumTextX = ImGui::GetCursorPosX();
    const float textX        = std::max(minimumTextX, textRightX - textWidth);
    const float textHeight   = ImGui::GetFontSize();
    const float textY        = (statusBarHeight - textHeight) * 0.5F;

    ImGui::SetCursorPos(ImVec2(textX, textY));
    const ImVec2 clipMinimum(ImGui::GetWindowPos().x + minimumTextX,
                             ImGui::GetWindowPos().y);
    const ImVec2 clipMaximum(ImGui::GetWindowPos().x + textRightX,
                             ImGui::GetWindowPos().y + statusBarHeight);
    ImGui::PushClipRect(clipMinimum, clipMaximum, true);
    if ( progress.detail.empty() ) {
        ImGui::TextUnformatted(stageText.data());
    } else {
        ImGui::Text("%s: %s", stageText.data(), progress.detail.c_str());
    }
    ImGui::PopClipRect();

    const float progressHeight = std::max(
        4.0F * dpiScale,
        std::min(ImGui::GetFrameHeight(), statusBarHeight - 4.0F * dpiScale));
    ImGui::SameLine();
    ImGui::SetCursorPos(
        ImVec2(barX, (statusBarHeight - progressHeight) * 0.5F));
    ImGui::ProgressBar(std::clamp(progress.fraction, 0.0F, 1.0F),
                       ImVec2(barWidth, progressHeight));
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

        const ProjectOpenProgressState* projectOpenProgress =
            sourceManager ? &sourceManager->getProjectOpenProgress() : nullptr;
        const bool projectTransitionInProgress =
            sourceManager && sourceManager->isProjectTransitionInProgress();
        std::shared_ptr<Common::Render::RenderSnapshotBuffer> syncBuffer;
        if ( projectOpenProgress &&
             (projectOpenProgress->active || projectTransitionInProgress) ) {
            const ProjectOpenProgressState initialProgress;
            const auto& displayedProgress = projectOpenProgress->active
                                                ? *projectOpenProgress
                                                : initialProgress;
            renderProjectOpenProgress(
                displayedProgress, statusBarHeight, dpiScale);
        } else {
            auto&       engine         = Logic::EditorEngine::instance();
            std::string activeCameraId = engine.getActiveCameraId();
            syncBuffer                 = engine.getSyncBuffer(
                activeCameraId.empty() ? "Basic2DCanvas" : activeCameraId);
        }
        if ( syncBuffer ) {
            auto snapshot = syncBuffer->getReadingSnapshot();
            if ( snapshot && MMM::UI::Utils::shouldShowBeatmapDetails(
                                 snapshot->hasBeatmap) ) {
                // 判定线时间 (常驻)
                const double displayedTime =
                    resolveStatusBarAnimateTime(*snapshot);
                const auto currentTimeText =
                    MMM::UI::Utils::formatCanvasTime(displayedTime, snapshot);
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
                    const auto hoveredTimeText =
                        MMM::UI::Utils::formatCanvasTime(snapshot->hoveredTime,
                                                         snapshot);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::Text("%s: %s",
                                TR("ui.status.mouse_time").data(),
                                hoveredTimeText.c_str());
                }

                // 物件数量与最大连击数统计由逻辑线程写入快照，避免 UI
                // 每帧访问 Session 锁和 ECS。
                if ( snapshot->hasBeatmap ) {
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::Text("BPM: %.3f", snapshot->currentBpm);

                    if ( snapshot->currentBeatIndex > 0 ) {
                        ImGui::SameLine();
                        ImGui::SetCursorPosY(offsetY);
                        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                        ImGui::SameLine();
                        ImGui::SetCursorPosY(offsetY);
                        ImGui::Text("%s: %d",
                                    TR("ui.canvas.beat_index").data(),
                                    snapshot->currentBeatIndex);
                    }

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
