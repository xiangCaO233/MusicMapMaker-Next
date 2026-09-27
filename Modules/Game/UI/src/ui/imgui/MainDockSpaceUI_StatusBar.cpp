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
/// @brief 本文件把异步工程状态和活动画布快照投影到底部状态栏。
///
/// UI 只能消费已发布的快照，不得为展示统计信息回访 BeatmapSession、ECS
/// 或音频线程。工程切换阶段优先占用状态栏右侧，避免同时呈现旧工程的
/// 画布指标；普通状态下则以活动 cameraId 选择同步缓冲区。

/// @brief 计算状态栏当前应显示的动画时间。
/// @param snapshot 当前活动画布快照。
/// @return 已按 UI 当前帧补偿后的显示时间，单位秒。
/// @warning UI 热路径：每帧状态栏绘制调用；只做常量级时间计算。
double resolveStatusBarAnimateTime(
    const Common::Render::RenderSnapshot& snapshot)
{
    // steady_clock 与快照时间使用同一单调时钟域，不受系统时间校准影响。
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
    // 阶段枚举跨后台流程与 UI 传递，此映射只返回稳定翻译键。
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
    // 防御未来新增枚举值：缺少专用文案时仍显示可理解的加载状态。
    return "ui.status.project_loading.validating";
}

/// @brief 在状态栏右侧绘制项目加载阶段、当前对象和总进度。
/// @param progress UI 线程持有的项目加载进度快照。
/// @param statusBarHeight 状态栏高度。
/// @param dpiScale 当前 DPI 缩放。
///
/// 布局约束：
/// - 进度条固定在右侧并保持稳定宽度，避免阶段变化导致抖动；
/// - 阶段和 detail 文本右对齐到进度条左侧；
/// - 文本过长时仅裁剪文本，不压缩或移动进度条；
/// - 左侧裁剪边界取当前游标，保留通用状态消息和分隔线；
/// - fraction 只作为展示值，UI 不据此推断后台流程是否完成。
/// @warning UI 热路径：仅在打开项目期间每帧调用，只读取 UI 本地快照并绘制
/// 固定数量控件。
void renderProjectOpenProgress(const ProjectOpenProgressState& progress,
                               float statusBarHeight, float dpiScale)
{
    // 阶段文本始终存在，detail 用于显示当前文件或具体处理对象。
    const auto stageText =
        TR(projectOpenProgressTranslationKey(progress.stage));
    const float stageWidth = ImGui::CalcTextSize(stageText.data()).x;
    const float detailWidth =
        progress.detail.empty()
            ? 0.0F
            : ImGui::CalcTextSize(progress.detail.c_str()).x;
    const float separatorWidth =
        progress.detail.empty() ? 0.0F : ImGui::CalcTextSize(": ").x;
    // 先计算完整文本宽度，随后把其右边缘锚定到进度条左侧。
    const float textWidth    = stageWidth + separatorWidth + detailWidth;
    const float gap          = 8.0F * dpiScale;
    const float barWidth     = 176.0F * dpiScale;
    const float rightPadding = 8.0F * dpiScale;
    // 进度条固定靠右，阶段文本使用左侧剩余空间并接受裁剪。
    const float barX       = ImGui::GetWindowWidth() - rightPadding - barWidth;
    const float textRightX = barX - gap;
    const float minimumTextX = ImGui::GetCursorPosX();
    const float textX        = std::max(minimumTextX, textRightX - textWidth);
    const float textHeight   = ImGui::GetFontSize();
    const float textY        = (statusBarHeight - textHeight) * 0.5F;

    // clipMinimum 保留前方状态消息空间，避免长文件名覆盖左侧内容。
    ImGui::SetCursorPos(ImVec2(textX, textY));
    const ImVec2 clipMinimum(ImGui::GetWindowPos().x + minimumTextX,
                             ImGui::GetWindowPos().y);
    const ImVec2 clipMaximum(ImGui::GetWindowPos().x + textRightX,
                             ImGui::GetWindowPos().y + statusBarHeight);
    ImGui::PushClipRect(clipMinimum, clipMaximum, true);
    // detail 为空时不输出多余冒号，使仅阶段状态保持简洁。
    if ( progress.detail.empty() ) {
        ImGui::TextUnformatted(stageText.data());
    } else {
        ImGui::Text("%s: %s", stageText.data(), progress.detail.c_str());
    }
    ImGui::PopClipRect();

    // 进度条高度受可用空间约束，同时保持最低可见厚度。
    const float progressHeight = std::max(
        4.0F * dpiScale,
        std::min(ImGui::GetFrameHeight(), statusBarHeight - 4.0F * dpiScale));
    ImGui::SameLine();
    ImGui::SetCursorPos(
        ImVec2(barX, (statusBarHeight - progressHeight) * 0.5F));
    // 在 UI 边界再次钳制后台值，防止异常进度破坏 ImGui 几何。
    ImGui::ProgressBar(std::clamp(progress.fraction, 0.0F, 1.0F),
                       ImVec2(barWidth, progressHeight));
}
}  // namespace

/// @brief 渲染主窗口底部状态栏。
/// @param sourceManager UI 管理器，用于读取工程切换状态和加载进度。
/// @param statusBarHeight 状态栏在当前 DPI 下的像素高度。
/// @param dpiScale 当前窗口内容缩放。
///
/// 显示优先级从高到低为工程加载进度、活动画布统计和普通就绪状态。
/// 工程切换期间不会读取旧画布快照；无活动画布或快照尚未发布时仅保留左侧
/// 状态消息。所有画布派生值均来自同一份 RenderSnapshot，保证时间、BPM、
/// SV、物件数量和操作提示在一帧内彼此一致。
///
/// 状态栏数据契约：
/// - statusMessageService 提供左侧通用反馈，其生命周期覆盖本帧绘制；
/// - ProjectOpenProgressState 是 UI 线程快照，不直接引用后台任务对象；
/// - RenderSnapshotBuffer 只在非切换状态下按活动画布查找；
/// - RenderSnapshot 在本帧内保持只读，所有格式化均基于同一个对象；
/// - hoveredTime 必须同时通过有限值和视野邻域检查；
/// - lastActionMessage 可以覆盖中部空白，但不参与布局回流。
/// - 状态栏不持有会话所有权，缓冲区缺失时允许本帧不显示详情；
/// - 所有跨线程值必须先进入 RenderSnapshot，再由本函数读取；
/// - 新增统计字段不得在 UI 热路径遍历谱面或实体集合。
/// @warning UI 热路径：每帧调用；只读取当前活动画布快照和轻量状态。
void MainDockSpaceUI::renderStatusBar(UIManager* sourceManager,
                                      float statusBarHeight, float dpiScale)
{
    // 主视口工作区是固定状态栏的坐标基准，不使用当前窗口位置。
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // 状态栏贴合工作区底边，完整覆盖主视口宽度。
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x,
               viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, statusBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    // 固定宿主不响应移动、缩放、停靠或导航，也不产生滚动条。
    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar;

    // 去除圆角和边框，由手动画线提供与菜单栏一致的顶部边界。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(4.0f * dpiScale, 0.0f));  // 仅保留横向安全边距。

    // 背景与顶部菜单栏统一，形成包围中央工作区的固定框架。
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg]);
    const ImVec4 statusTextColor = ImGui::GetStyle().Colors[ImGuiCol_TextLink];
    ImGui::PushStyleColor(ImGuiCol_Text, statusTextColor);
    // 次要操作提示降低透明度，避免与实时谱面指标争夺焦点。
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

    // 样式栈约定：
    // - 三项 StyleVar 控制固定窗口外形和内边距；
    // - 五项 StyleColor 控制背景、主文本、弱文本、分隔线与顶边；
    // - 函数体内不再压入同类样式，因此末尾可成组恢复；
    // - Begin 返回 false 时不绘制内容，但仍执行 End 之外的样式恢复。
    if ( ImGui::Begin("StatusBar", nullptr, window_flags) ) {
        // 顶部分隔线使用 Border 颜色，横跨当前实际窗口宽度。
        ImVec2 p1 = ImGui::GetWindowPos();
        ImVec2 p2 = ImVec2(p1.x + ImGui::GetWindowWidth(), p1.y);
        ImGui::GetWindowDrawList()->AddLine(
            p1, p2, ImGui::GetColorU32(ImGuiCol_Border), 1.0f);

        // 所有单行文本复用同一 offsetY，保证不同片段基线一致。
        float textHeight = ImGui::GetFontSize();
        float offsetY    = (statusBarHeight - textHeight) / 2.0f;
        ImGui::SetCursorPosY(offsetY);

        // 左侧优先显示最近状态消息；没有消息时显示本地化就绪文本。
        const std::string_view statusMessage =
            m_statusMessageService.getStatusMessage();
        if ( !statusMessage.empty() ) {
            // string_view 可能不是零结尾，显式传结束指针避免越界读取。
            ImGui::TextUnformatted(statusMessage.data(),
                                   statusMessage.data() + statusMessage.size());
        } else {
            ImGui::Text("%s", TR("ui.status.ready").data());
        }

        // 首个竖分隔线把通用消息与工程或画布状态区分开。
        ImGui::SameLine();
        ImGui::SetCursorPosY(offsetY);
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        const ProjectOpenProgressState* projectOpenProgress =
            sourceManager ? &sourceManager->getProjectOpenProgress() : nullptr;
        // transition 标志覆盖进度对象尚未激活的短暂切换窗口。
        const bool projectTransitionInProgress =
            sourceManager && sourceManager->isProjectTransitionInProgress();
        std::shared_ptr<Common::Render::RenderSnapshotBuffer> syncBuffer;
        // 工程切换状态优先，防止状态栏展示即将失效的旧工程数据。
        if ( projectOpenProgress &&
             (projectOpenProgress->active || projectTransitionInProgress) ) {
            // 两种进入条件的含义不同：
            // - active 表示已收到包含阶段和进度的后台事件；
            // - transition 表示工程边界已开始切换但事件可能尚未到达。
            // 后一种情况不能继续显示旧快照，否则用户会误认旧工程仍可编辑。
            const ProjectOpenProgressState initialProgress;
            // 首个进度事件到达前，以零进度的默认验证阶段填补空档。
            const auto& displayedProgress = projectOpenProgress->active
                                                ? *projectOpenProgress
                                                : initialProgress;
            renderProjectOpenProgress(
                displayedProgress, statusBarHeight, dpiScale);
        } else {
            // 没有活动 cameraId 时回退基础画布，兼容启动和单画布阶段。
            auto&       engine         = Logic::EditorEngine::instance();
            std::string activeCameraId = engine.getActiveCameraId();
            syncBuffer                 = engine.getSyncBuffer(
                activeCameraId.empty() ? "Basic2DCanvas" : activeCameraId);
        }
        if ( syncBuffer ) {
            // reading snapshot 是跨线程发布的只读对象，本帧持有以稳定生命周期。
            auto snapshot = syncBuffer->getReadingSnapshot();
            // 状态栏遵循与画布相同的无谱面内容可见性策略。
            if ( snapshot && MMM::UI::Utils::shouldShowBeatmapDetails(
                                 snapshot->hasBeatmap) ) {
                // 画布统计区的不变量：
                // - 当前时间是唯一常驻字段；
                // - 悬停时间仅在指针位于画布并通过边界校验后出现；
                // - 谱面统计仅在 hasBeatmap 为真时出现；
                // - 最后操作消息独立右对齐，不改变前述字段的计算；
                // - 所有字段只做常量级格式化和 ImGui 提交。
                // 字段来源约定：
                // - resolveCurrentTimeAt 根据播放状态外推当前判定线时间；
                // - visibleTimeStart/End 定义悬停时间的可信视野；
                // - currentBpm 与 currentSv 对应判定线所在时间点；
                // - currentBeatIndex 小于等于零表示暂无可展示节拍；
                // - noteCount 与 maxCombo 是逻辑线程预先汇总的只读统计；
                // - lastActionMessage 是逻辑层生成的本地化或业务提示文本。
                // 新增字段时应继续由快照携带，不能在此处查询业务模型。
                // 判定线时间用当前单调时钟外推，播放时仍保持平滑更新。
                const double displayedTime =
                    resolveStatusBarAnimateTime(*snapshot);
                const auto currentTimeText =
                    MMM::UI::Utils::formatCanvasTime(displayedTime, snapshot);
                // 统一格式化入口同时处理谱面时长和当前显示格式。
                ImGui::SetCursorPosY(offsetY);
                ImGui::Text("%s: %s",
                            TR("ui.canvas.time").data(),
                            currentTimeText.c_str());

                // 悬停时间只在主画布悬浮且数值落在可见范围附近时显示。
                const double visibleStart = std::min(snapshot->visibleTimeStart,
                                                     snapshot->visibleTimeEnd);
                const double visibleEnd   = std::max(snapshot->visibleTimeStart,
                                                     snapshot->visibleTimeEnd);
                // 一秒容差覆盖边界插值误差；非有限边界不施加对应限制。
                const bool hasValidHoveredTime =
                    std::isfinite(snapshot->hoveredTime) &&
                    (!std::isfinite(visibleStart) ||
                     snapshot->hoveredTime >= visibleStart - 1.0) &&
                    (!std::isfinite(visibleEnd) ||
                     snapshot->hoveredTime <= visibleEnd + 1.0);
                if ( snapshot->isHoveringCanvas && hasValidHoveredTime ) {
                    // 悬停值与当前时间使用相同格式，便于直接比较定位。
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

                // 物件数量与最大连击数由逻辑线程写入快照。
                // UI 只读取同一版本统计，不在每帧访问 Session 锁或 ECS。
                if ( snapshot->hasBeatmap ) {
                    // 以下指标共享同一快照版本，保证画面与统计信息一致。
                    // 每个字段前用竖线分隔，保持紧凑且可快速扫读。
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::Text("BPM: %.3f", snapshot->currentBpm);

                    if ( snapshot->currentBeatIndex > 0 ) {
                        // 零值表示当前节拍不可用，不用占位文本误导用户。
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

                    // noteCount 是谱面对象总数，不在 UI 线程重新遍历实体。
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::Text("%s: %zu",
                                TR("ui.status.note_count").data(),
                                snapshot->noteCount);

                    // maxCombo 已由逻辑层按谱面规则计算，状态栏只负责展示。
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(offsetY);
                    ImGui::Text("%s: %zu",
                                TR("ui.status.max_combo").data(),
                                snapshot->maxCombo);
                }

                // 最后一次操作信息锚定最右侧，作为弱化显示的瞬时反馈。
                if ( !snapshot->lastActionMessage.empty() ) {
                    // 先测量文字宽度再反推起点，保证右侧安全边距固定。
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

        // Begin 成功后在同一分支 End，避免提交窗口内容到错误上下文。
        ImGui::End();
    }

    // 五项颜色与三项尺寸样式成组恢复，避免污染下一窗口。
    ImGui::PopStyleColor(5);
    // StyleVar 的恢复顺序与类型无关，但数量必须与入口压栈严格一致。
    ImGui::PopStyleVar(3);
}

}  // namespace MMM::UI
