#include "ui/walkthrough/WalkthroughPage.h"

#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "config/skin/translation/Translation.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/markdown/MarkdownImageCache.h"
#include "ui/imgui/markdown/MarkdownRenderer.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/walkthrough/WalkthroughModel.h"
#include "ui/walkthrough/WalkthroughService.h"
#include <algorithm>
#include <imgui.h>
#include <string>

/// @file WalkthroughPage.cpp
/// @brief 欢迎页演练主题正文、分支卡片和步骤操作的即时模式绘制。
/// @details 页面层遵循以下职责边界：
/// - 主题、章节和步骤内容来自已验证的 Walkthrough 模型；
/// - 完成状态、依赖可用性和持久化由 Walkthrough::Service 管理；
/// - Markdown 文本交给共享渲染器；
/// - 文档图片通过 UIManager 注册的 MarkdownImageCache 预热；
/// - 页面只保存当前主题、唯一展开分支和图片准备身份；
/// - 数据文件中的 action 只能调用服务预先注册的可信函数。
///
/// 绘制层级约定：
/// - 主题标题和整体说明位于正文顶部；
/// - 每个操作分支使用独立的自动高度卡片；
/// - 卡片标题行本身是透明的全宽反馈按钮；
/// - 左侧圆点表达分支完成状态；
/// - 右侧徽标表达已完成步骤数；
/// - 展开区域依次绘制步骤标题、Markdown 正文和操作按钮；
/// - 页面底部绘制目标完成提示、重置入口及服务错误。
///
/// 状态约定：
/// - 主题切换时默认展开第一分支；
/// - 再次点击已展开分支会折叠全部内容；
/// - 任一时刻至多展开一个分支；
/// - 已完成步骤不能重复手动确认；
/// - 自动与手动完成使用相同完成判定但展示不同来源；
/// - 有 action 的步骤必须同时满足依赖和注册条件才能执行；
/// - 重置只清理当前主题，不影响其他主题；
/// - 占位主题不渲染分支、进度或重置入口。
///
/// 性能约定：
/// - 每帧只遍历当前主题的分支和步骤；
/// - 不在绘制循环中读取教程文件；
/// - 图片准备只在主题或语言变化时提交；
/// - 进度文件只在确认、重置或业务事件改变状态时保存；
/// - 所有尺寸使用缓存的内容缩放比例；
/// - 长标题通过裁剪与悬停提示处理，不测量额外布局；
/// - ImGui 样式、字体、ID、缩进和子窗口必须严格成对恢复。

namespace MMM::UI
{
/// @brief 在欢迎页正文区域绘制一个演练主题及其步骤分支。
/// @param manager 提供演练服务和 Markdown 图片缓存的 UI 管理器。
/// @param topicIndex 当前主题在已验证目录中的索引。
/// @warning UI 热路径：欢迎页可见时每帧执行；只遍历当前主题分支和步骤。
/// @details 页面不持有学习进度，所有状态判断和持久化委托给 Service；
/// 本地只保存展开分支及最近准备图片的主题/语言身份。
void WalkthroughPage::render(UIManager* manager, std::size_t topicIndex)
{
    // 主题目录由服务统一验证，索引失效时不绘制过期内容。
    auto&       service = manager->walkthroughService();
    const auto& topics  = service.topics();
    if ( topicIndex >= topics.size() ) return;
    // 主题引用只在本帧使用，不跨越目录可能重建的生命周期。
    const auto& topic = topics[topicIndex];
    // 标题、正文和步骤内容均按当前编辑器语言即时选择。
    const auto& language =
        Config::AppConfig::instance().getEditorSettings().language;
    // 固定视觉间距随内容缩放，保持高 DPI 下相同比例。
    const float scale = Config::AppConfig::instance().getWindowContentScale();
    if ( m_currentTopic != topic.m_id ) {
        // 切换主题时默认展开第一分支，不沿用上一主题的索引。
        m_currentTopic   = topic.m_id;
        m_expandedBranch = 0;
    }
    // 图片缓存是可选注册视图，缺失时 Markdown 仍可渲染纯文本。
    auto* images = manager->getView<MarkdownImageCache>("WalkthroughImages");
    if ( images &&
         (m_preparedTopic != topic.m_id || m_preparedLanguage != language) ) {
        // 仅主题或语言变化时扫描文档图片，避免每帧重复准备资源。
        images->prepareDocument(topic.m_description.get(language));
        for ( const auto& branch : topic.m_branches )
            for ( const auto& step : branch.m_steps )
                // 分支步骤正文也可能引用图片，需与主题说明一并预热。
                images->prepareDocument(step.m_body.get(language));
        // 两个身份都在准备完成后更新，避免半完成状态被视为有效缓存。
        m_preparedTopic    = topic.m_id;
        m_preparedLanguage = language;
    }
    // 当前页面所有 Markdown 块共享同一个图片缓存选项。
    MarkdownRenderOptions markdownOptions;
    markdownOptions.images = images;
    // 进度引用由服务拥有，只在本帧查询，不由页面修改。
    const auto& progress = service.progress();
    // 主题 ID 隔离分支、步骤和重置弹窗的 ImGui 内部标识。
    ImGui::PushID(topic.m_id.c_str());
    ImGui::Dummy({ 0, 24.0F * scale });
    // 使用当前字体族放大主题标题，不引入独立字体资源依赖。
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.75F);
    ImGui::TextWrapped("%s", topic.m_title.get(language).c_str());
    ImGui::PopFont();
    if ( topic.m_placeholder ) {
        // 占位主题只展示统一说明，不渲染尚未定义的分支和进度。
        ImGui::Dummy({ 0, ImGui::GetStyle().WindowPadding.y });
        ImGui::TextWrapped("%s", TR("ui.welcome.placeholder_body").data());
        ImGui::PopID();
        return;
    }
    ImGui::Dummy({ 0, 16.0F * scale });
    // 非占位主题先显示整体目标，再展示可独立完成的操作分支。
    renderMarkdown(topic.m_description.get(language), markdownOptions);
    ImGui::Dummy({ 0, 28.0F * scale });
    // 主题正文与欢迎目录统一沿用设置项的淡背景和中性描边。
    const auto& style      = ImGui::GetStyle();
    auto        background = style.Colors[ImGuiCol_FrameBg];
    auto        border     = style.Colors[ImGuiCol_Border];
    background.w *= 0.35F;
    border.w *= 0.6F;
    // 完成分支数量用于最终判断 all-of 或 any-of 主题目标。
    std::size_t finishedBranches = 0;
    for ( std::size_t index = 0; index < topic.m_branches.size(); ++index ) {
        // 每个分支卡片按自身稳定 ID 建立独立 ImGui 状态。
        const auto& branch = topic.m_branches[index];
        std::size_t count  = 0;
        for ( const auto& step : branch.m_steps )
            // completed 会同时解释手动确认和业务事件自动完成记录。
            count += progress.completed(topic, step) ? 1 : 0;
        // 空分支按 vacuous truth 视为完成，与模型目标判定保持一致。
        const bool done = count == branch.m_steps.size();
        if ( done ) ++finishedBranches;
        // 页面只允许一个分支展开，减少长教程的纵向占用。
        const bool expanded = m_expandedBranch == index;
        ImGui::PushID(branch.m_id.c_str());
        // 卡片圆角沿用普通 Frame，背景和边框使用降低透明度的主题色。
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, style.FrameRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize,
                            style.ChildBorderSize);
        ImGui::PushStyleColor(ImGuiCol_Border, border);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
        if ( ImGui::BeginChild("BranchCard",
                               { 0, 0 },
                               ImGuiChildFlags_AutoResizeY |
                                   ImGuiChildFlags_Borders |
                                   ImGuiChildFlags_AlwaysUseWindowPadding,
                               ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse) ) {
            // 卡片由内容自动决定高度，内部不建立第二层滚动区域。
            const float line      = ImGui::GetTextLineHeight();
            const float rowHeight = line + 16.0F * scale;
            const auto  row       = ImGui::GetCursorScreenPos();
            // 透明整行按钮只提供统一反馈和命中区，视觉由 DrawList 绘制。
            ImGui::PushStyleColor(ImGuiCol_Button, { 0, 0, 0, 0 });
            const bool clicked =
                FeedbackButton("##BranchHeader",
                               { ImGui::GetContentRegionAvail().x, rowHeight });
            ImGui::PopStyleColor();
            // 读取按钮实际矩形后定位状态圆、标题裁剪区和完成计数。
            const auto   end  = ImGui::GetItemRectMax();
            auto*        draw = ImGui::GetWindowDrawList();
            const ImVec2 center{ row.x + 10.0F * scale,
                                 row.y + rowHeight * 0.5F };
            // 展开或完成使用强调色，未完成折叠分支使用禁用文字色。
            const auto accent = ImGui::GetColorU32(
                expanded || done ? ImGuiCol_CheckMark : ImGuiCol_TextDisabled);
            if ( done ) {
                // 完成状态绘制实心圆和简洁对勾，不依赖额外图标字体。
                draw->AddCircleFilled(center, 6.0F * scale, accent);
                const auto checkColor = ImGui::GetColorU32(ImGuiCol_WindowBg);
                draw->AddLine(
                    { center.x - 3.0F * scale, center.y },
                    { center.x - 1.0F * scale, center.y + 2.0F * scale },
                    checkColor,
                    1.5F * scale);
                draw->AddLine(
                    { center.x - 1.0F * scale, center.y + 2.0F * scale },
                    { center.x + 3.0F * scale, center.y - 2.0F * scale },
                    checkColor,
                    1.5F * scale);
            } else
                // 未完成状态保留空心圆，表达尚待操作。
                draw->AddCircle(center, 6.0F * scale, accent, 0, 1.0F * scale);
            // 计数按已完成步骤数/总步骤数展示。
            const auto badge = std::to_string(count) + "/" +
                               std::to_string(branch.m_steps.size());
            // 计数贴齐卡片右侧，标题区域在其左边单独裁剪。
            const float badgeX =
                end.x - ImGui::CalcTextSize(badge.c_str()).x - 6.0F * scale;
            draw->PushClipRect(
                { row.x + 28.0F * scale, row.y },
                { std::max(row.x + 28.0F * scale, badgeX - 10.0F * scale),
                  end.y },
                true);
            // 裁剪长标题，避免覆盖右侧完成计数。
            draw->AddText({ row.x + 28.0F * scale, row.y + 8.0F * scale },
                          ImGui::GetColorU32(ImGuiCol_Text),
                          branch.m_title.get(language).c_str());
            draw->PopClipRect();
            draw->AddText({ badgeX, row.y + 8.0F * scale },
                          ImGui::GetColorU32(ImGuiCol_TextDisabled),
                          badge.c_str());
            if ( ImGui::IsItemHovered() )
                // 悬停展示完整标题，补偿视觉裁剪造成的信息缺失。
                ImGui::SetTooltip("%s", branch.m_title.get(language).c_str());
            if ( clicked ) {
                if ( expanded )
                    // 点击已展开分支会折叠全部分支。
                    m_expandedBranch.reset();
                else
                    // 点击其他分支直接切换唯一展开索引。
                    m_expandedBranch = index;
            }
            if ( expanded ) {
                // 步骤内容缩进到状态圆之后，与分支标题文字对齐。
                ImGui::Indent(28.0F * scale);
                for ( const auto& step : branch.m_steps ) {
                    // 步骤 ID 隔离确认与执行业务按钮的内部状态。
                    ImGui::PushID(step.m_id.c_str());
                    ImGui::Spacing();
                    ImGui::TextWrapped("%s",
                                       step.m_title.get(language).c_str());
                    // 步骤正文允许包含格式化说明和预热过的本地图片。
                    renderMarkdown(step.m_body.get(language), markdownOptions);
                    ImGui::Spacing();
                    // 已完成步骤禁用再次确认，来源标签仍可解释完成方式。
                    const bool completed = progress.completed(topic, step);
                    const auto acknowledge =
                        std::string(ICON_MMM_CHECK) + "  " +
                        TR("ui.walkthrough.acknowledge").toString();
                    // 确认按钮采用透明背景和主题勾选色，融入教程正文。
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4{ 0, 0, 0, 0 });
                    ImGui::PushStyleColor(
                        ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
                    ImGui::BeginDisabled(completed);
                    if ( FeedbackButton(acknowledge.c_str()) )
                        // 手动确认由服务更新依赖进度并立即持久化。
                        service.acknowledge(topic, step);
                    ImGui::EndDisabled();
                    ImGui::PopStyleColor(2);
                    if ( completed ) {
                        // 完成来源区分业务事件自动达成与用户手动确认。
                        ImGui::SameLine();
                        ImGui::TextDisabled(
                            "%s",
                            TR(progress.source(topic, step) == "automatic"
                                   ? "ui.walkthrough.automatic"
                                   : "ui.walkthrough.manual")
                                .data());
                    }
                    if ( !step.m_action.empty() ) {
                        // 操作同时受步骤依赖可用性和可信 C++ 注册表限制。
                        ImGui::BeginDisabled(!progress.available(topic, step) ||
                                             !service.hasAction(step.m_action));
                        if ( FeedbackButton(
                                 TR("ui.walkthrough.perform").data()) )
                            // 数据文件只提供操作 ID，真正函数来自服务注册表。
                            service.execute(step.m_action);
                        ImGui::EndDisabled();
                    }
                    ImGui::Dummy({ 0, 14.0F * scale });
                    ImGui::PopID();
                }
                // 与进入步骤列表时的缩进严格配对。
                ImGui::Unindent(28.0F * scale);
            }
        }
        // BeginChild 无论返回值如何都必须结束，并恢复四项局部样式。
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        ImGui::PopID();
        ImGui::Dummy({ 0, 6.0F * scale });
    }
    ImGui::Dummy({ 0, 12.0F * scale });
    // anyBranch 主题完成任一分支即可，否则要求所有分支完成。
    if ( topic.m_anyBranch ? finishedBranches > 0
                           : finishedBranches == topic.m_branches.size() )
        ImGui::TextWrapped("%s", TR("ui.walkthrough.goal_complete").data());
    ImGui::Dummy({ 0, 8.0F * scale });
    // 重置入口与步骤确认按钮使用一致的轻量文本样式。
    const auto resetLabel = std::string(ICON_MMM_REDO) + "  " +
                            TR("ui.walkthrough.reset").toString();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0, 0, 0, 0 });
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
    const bool resetClicked = FeedbackButton(resetLabel.c_str());
    ImGui::PopStyleColor(2);
    if ( resetClicked ) FeedbackOpenPopup("ResetWalkthrough");
    if ( ImGui::BeginPopup("ResetWalkthrough") ) {
        // 二次确认避免误删当前主题的全部学习记录。
        ImGui::TextWrapped("%s", TR("ui.walkthrough.reset_confirm").data());
        if ( FeedbackButton(TR("ui.common.confirm").data()) ) {
            service.reset(topic);
            // 重置完成后关闭弹窗，正文下一帧读取更新后的进度。
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( FeedbackButton(TR("ui.common.cancel").data()) )
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // 加载、保存或自定义主题校验错误显示在当前页面底部。
    if ( !service.error().empty() )
        ImGui::TextWrapped("%s", service.error().c_str());
    // 恢复主题级 ImGui ID 作用域。
    ImGui::PopID();
}
}  // namespace MMM::UI
