#include "ui/UIManager.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/walkthrough/WalkthroughSpotlight.h"
#include "ui/workspace/IWorkspacePresentation.h"

#include "imgui.h"
#include <algorithm>

/// @file SettingsView_Tabs.cpp
/// @brief 设置页复用的分组、标准双列行和自动换行单选项布局实现。
/// @details 辅助函数从对象池取得 CLayBox 容器，通过递增索引保证同一帧节点
/// 身份稳定；实际 ImGui 控件由布局完成后的回调绘制。

namespace MMM::UI
{
namespace
{
/// @brief 将设置项引导框限制在当前设置内容 Child 的可见区域。
/// @param bounds Clay 返回的屏幕空间布局矩形。
/// @param minimum 接收可见部分的左上角。
/// @param maximum 接收可见部分的右下角。
/// @return 至少有一部分可见时返回 true。
/// @warning UI 热路径：只做固定次数的几何计算，不查询或修改设置状态。
bool visibleGuideRect(Clay_BoundingBox bounds, ImVec2& minimum, ImVec2& maximum)
{
    // Clay 仍会回调滚出 Child 的行；这些行不能成为当前引导锚点。
    const ImVec2 windowPos  = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    // 当前回调始终在 SettingsContent Child 中，窗口边界即滚动裁剪边界。
    minimum = { std::max(bounds.x, windowPos.x),
                std::max(bounds.y, windowPos.y) };
    maximum = { std::min(bounds.x + bounds.width, windowPos.x + windowSize.x),
                std::min(bounds.y + bounds.height,
                         windowPos.y + windowSize.y) };
    // 仅上报实际可见的交集，避免孔洞覆盖侧栏或视口外区域。
    return maximum.x > minimum.x && maximum.y > minimum.y;
}
}  // namespace

/// @brief 在新引导步骤首次绘制时定位离屏设置项。
/// @param targetId 当前设置行的稳定语义 ID。
/// @param bounds Clay 布局返回的屏幕空间矩形。
/// @warning UI 热路径：日常帧只比较令牌，滚动请求仅在新步骤首次出现时执行。
void SettingsView::scrollGuideRowIntoView(const char*      targetId,
                                          Clay_BoundingBox bounds)
{
    // 普通设置行不触发滚动查询，只有已注册的引导控件参与定位。
    if ( !targetId || !m_sourceManager ) return;
    auto& spotlight = m_sourceManager->walkthroughSpotlight();
    if ( !spotlight.awaitingTarget(targetId) ||
         spotlight.stepToken() == m_lastGuideScrollToken )
        return;
    // 新步骤只定位一次，随后允许用户自行滚动浏览其它设置。
    // 即使该行本来可见也记下令牌，防止稍后手动滚走时突然拉回。
    m_lastGuideScrollToken  = spotlight.stepToken();
    const ImVec2 windowPos  = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    const float  margin     = ImGui::GetFrameHeight();
    if ( bounds.y >= windowPos.y + margin &&
         bounds.y + bounds.height <= windowPos.y + windowSize.y - margin )
        return;
    // 整组高于 Child 时从顶部展示，避免居中后首尾同时被截断。
    // 可容纳时仍按中心定位，让标题和整组六项尽量同时可见。
    const float availableHeight = std::max(0.0f, windowSize.y - margin * 2.0f);
    const float targetCenter    = bounds.height > availableHeight
                                      ? bounds.y + availableHeight * 0.5f
                                      : bounds.y + bounds.height * 0.5f;
    // 仅修改当前内容 Child 的纵向滚动，不移动设置窗口或侧栏。
    const float viewCenter = windowPos.y + windowSize.y * 0.5f;
    ImGui::SetScrollY(
        std::max(0.0f, ImGui::GetScrollY() + targetCenter - viewCenter));
}

/// @brief 创建一个共享边框的关联设置项容器。
/// @param parent 接收新分组的父级纵向布局。
/// @param sectionIndex 分组对象池索引，成功取得分组后递增。
/// @param id 当前布局树内使用的稳定分组 ID。
/// @return 已清理并加入父布局的分组容器引用。
CLayVBox& SettingsView::addSettingGroup(CLayVBox& parent, size_t& sectionIndex,
                                        const char* id)
{
    // 分组对象由 SettingsView 池复用，索引在一次布局构造中单调递增。
    auto& group = getSection(sectionIndex++);
    // 统一装饰、间距与内边距，避免各设置页自行复制样式参数。
    group.setDecorated(true).setSpacing(4).setPadding(8, 8, 6, 6);
    // 宽度占满父级，高度由分组内容决定。
    parent.addLayout(id, group, Sizing::Grow(), Sizing::Fit());
    return group;
}

/// @brief 添加一行标准设置项。
/// @param parent 接收该行的父级纵向布局。
/// @param rowIndex 当前布局行索引，会在添加过程中递增。
/// @param label 设置项标签文本。
/// @param labelWidth 标签列固定宽度。
/// @param widget 设置项右侧控件绘制回调。
/// @param dangerLabel 是否使用危险色绘制标签。
/// @param decorated 是否为整行绘制背景、边框和较大内边距。
/// @param walkthroughTarget 可选引导目标；控件回调执行后报告真实值列矩形。
/// @warning UI 热路径：设置页可见时每帧构造布局，回调不得阻塞或访问文件。
void SettingsView::addSettingItem(CLayVBox& parent, size_t& rowIndex,
                                  const char* label, float labelWidth,
                                  CLayBox::DrawFunc widget, bool dangerLabel,
                                  bool decorated, const char* walkthroughTarget)
{
    // 布局构造时冻结行身份；绘制回调执行时当前分组已可能改变。
    const bool searchTarget = isSettingsSearchTarget(label);
    // 主行来自对象池，装饰状态决定内边距与最终行高。
    auto& row = getRow(rowIndex++);
    row.setDecorated(decorated)
        .setPadding(decorated ? 8 : 0,
                    decorated ? 8 : 0,
                    decorated ? 6 : 2,
                    decorated ? 6 : 2)
        .setSpacing(8)
        .setAlignment(Alignment::Center());

    // ID 同时包含递增索引和标签，防止本地化相同文本造成节点冲突。
    std::string labelId = "R" + std::to_string(rowIndex) + "_L_" + label;

    // 左列由文本和弹簧组成，使标签保持靠左且垂直居中。
    auto& leftBox = getRow(rowIndex++);
    // 复用容器必须先清除上一帧节点，再应用本行布局参数。
    leftBox.clear();
    leftBox.setPadding(0, 0, 0, 0)
        .setSpacing(0)
        .setAlignment(Alignment::Center());

    // 标签本身按内容宽度 Fit，高度随左列增长。
    leftBox.addElement(
        labelId + "_lbl",
        Sizing::Fit(),
        Sizing::Grow(),
        [label, dangerLabel](Clay_BoundingBox r, bool) {
            // 使用实际字体高度计算垂直居中偏移。
            float textH  = ImGui::CalcTextSize(label).y;
            float offset = (r.height - textH) * 0.5f;
            ImGui::SetCursorScreenPos({ r.x, r.y + offset });
            if ( dangerLabel ) {
                // 破坏性设置使用统一危险语义色。
                ImGui::TextColored(
                    Utils::UIThemeUtils::getDangerColor(), "%s", label);
            } else {
                ImGui::Text("%s", label);
            }
        });

    // 剩余横向空间由无绘制弹簧吸收。
    leftBox.addElement(
        labelId + "_lbl_spring", Sizing::Grow(), Sizing::Grow(), nullptr);

    // 左列固定为当前标签页预先测得的最大标签宽度。
    row.addLayout((labelId + "_left").c_str(),
                  leftBox,
                  Sizing::Fixed(labelWidth),
                  Sizing::Grow());

    // 右列占据剩余宽度，并在回调中按 ImGui 标准帧高垂直居中。
    row.addElement(
        labelId + "_wgt",
        Sizing::Grow(),
        Sizing::Grow(),
        [this, widget, walkthroughTarget, searchTarget, labelWidth](
            Clay_BoundingBox r, bool h) {
            // 控件回调接收完整横向区域和 Clay 悬停状态。
            float widgetH = ImGui::GetFrameHeight();
            float offset  = (r.height - widgetH) * 0.5f;
            ImGui::SetCursorScreenPos({ r.x, r.y + offset });
            widget(r, h);
            // 整行边界同时覆盖名称和控件，高亮不依赖翻译长度或屏幕行号。
            if ( searchTarget )
                reportSettingsSearchRow({ r.x - labelWidth - 8.0f,
                                          r.y,
                                          r.width + labelWidth + 8.0f,
                                          r.height });
            // Clay 已给出真实值列位置，避免用翻译文本或行号猜测高亮区。
            // 原控件先完成绘制，再用相同布局矩形定位引导遮罩和描边。
            if ( walkthroughTarget && m_sourceManager ) {
                scrollGuideRowIntoView(walkthroughTarget, r);
                ImVec2 minimum, maximum;
                if ( visibleGuideRect(r, minimum, maximum) )
                    m_sourceManager->walkthroughSpotlight().reportTarget(
                        walkthroughTarget, minimum, maximum);
            }
        });

    // 装饰行额外包含上下各六像素内边距，普通行仅保留轻量间隔。
    float rowH = ImGui::GetFrameHeight() + (decorated ? 12.0f : 4.0f);
    // 主行宽度随父级增长，高度固定以稳定相邻设置项节奏。
    parent.addLayout(
        (labelId + "_row").c_str(), row, Sizing::Grow(), Sizing::Fixed(rowH));
}

/// @brief 添加一个可自动换行的单选设置项。
/// @param parent 接收该行的父级纵向布局。
/// @param rowIndex 当前布局行索引，会在添加过程中递增。
/// @param sectionIndex 当前段落索引，会在生成控件容器时递增。
/// @param label 设置项标签文本。
/// @param labelWidth 标签列固定宽度。
/// @param options 单选项文本和值列表。
/// @param current 当前选中值。
/// @param changed 设置发生变化时写入 true。
/// @param decorated 是否为整行绘制装饰背景与内边距。
/// @param walkthroughTarget 可选引导目标，分别报告各实际单选按钮。
/// @warning UI 热路径：每帧按可用宽度重新分行，选项集合应保持小规模稳定。
/// 测量与实际 RadioButton 使用相同字号和样式，不能用逻辑像素常量代替。
/// DPI、语言或主题变化后直接使用新尺寸，不保留跨帧的过期组件宽度。
/// 移动端单项超出值列时纵向组合；桌面保留既有名称和值列布局。
/// 响应式排版只改变几何，原配置字段、枚举值和点击处理保持不变。
/// 搜索导航必须包含名称和全部选项行，不能用旧单行高度定位。
void SettingsView::addRadioSetting(
    CLayVBox& parent, size_t& rowIndex, size_t& sectionIndex, const char* label,
    float labelWidth, const std::vector<std::pair<std::string, int>>& options,
    int& current, bool& changed, bool decorated, const char* walkthroughTarget)
{
    // 单选组同样通过原设置行导航，不能只定位到某一个选项按钮。
    const bool searchTarget = isSettingsSearchTarget(label);
    // 使用当前 ImGui 内容区宽度决定单选项换行位置。
    float totalWidth = ImGui::GetContentRegionAvail().x;

    // 扣除外层、分组、行内边距、列间距及滚动条缓冲后得到控件区宽度。
    float       widgetAvailW = std::max(1.0f, totalWidth - labelWidth - 72.0f);
    const auto& style        = ImGui::GetStyle();
    // 原生 RadioButton 的圆点区域占完整帧高；固定预留 36 像素会在大字号下重叠。
    // 帧高已经包含当前 DPI 和字号，不再额外乘倍率，否则会再次高估或重复缩放。
    // ItemInnerSpacing 只连接圆点和文字，外部选项间距仍由各行容器单独提供。
    const float radioWidth = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x;
    float       widestOption = 0;
    for ( const auto& option : options )
        widestOption = std::max(
            widestOption,
            ImGui::CalcTextSize(option.first.c_str(), nullptr, true).x +
                radioWidth);
    // 触摸窄区不能靠强制最小值列宽撑出视口，单项放不下时改为名称在上、选项在下。
    const bool stacked =
        !workspaceAllowsHoverFeedback() && widestOption > widgetAvailW;
    // 全宽预算扣除相同嵌套边距，纵向组合不增加额外装饰或外边距。
    // 不以强制最小宽度撑大父窗口，仍由同一个原生内容区承载滚动。
    if ( stacked ) widgetAvailW = std::max(1.0f, totalWidth - 72.0f);

    // 主行与标准设置项使用相同的装饰和间距规则。
    auto& row = getRow(rowIndex++);
    row.setDecorated(decorated)
        .setPadding(decorated ? 8 : 0,
                    decorated ? 8 : 0,
                    decorated ? 6 : 2,
                    decorated ? 6 : 2)
        .setSpacing(8)
        .setAlignment(Alignment::Center());

    // sectionIndex 和 rowIndex 共同构成跨分组不冲突的稳定前缀。
    std::string labelId = "S" + std::to_string(sectionIndex) + "_R" +
                          std::to_string(rowIndex) + "_L_" + label;

    // 标签列固定宽度，使同一标签页的控件起点保持对齐。
    if ( !stacked )
        row.addElement(
            labelId + "_lbl",
            Sizing::Fixed(labelWidth),
            Sizing::Grow(),
            [this, label, searchTarget, widgetAvailW](Clay_BoundingBox r,
                                                      bool) {
                // 标签在 Clay 分配行高中按实际文本高度居中。
                float textH  = ImGui::CalcTextSize(label).y;
                float offset = (r.height - textH) * 0.5f;
                ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                ImGui::Text("%s", label);
                // 标签列高度与整个自动换行行一致，保留完整高亮范围。
                if ( searchTarget )
                    reportSettingsSearchRow(
                        { r.x, r.y, r.width + widgetAvailW + 8.0f, r.height });
            });

    // 选项容器按需要建立多行 HBox，自身高度由所有行内容适配。
    auto& containerVBox = getSection(sectionIndex++);
    // 对象池容器每帧复用，必须先清空旧选项节点。
    containerVBox.clear();
    containerVBox.setSpacing(4).setPadding(0, 0, 0, 0);

    // 累计当前行占用宽度，空指针表示尚未创建首行。
    float     currentLineW   = 0;
    CLayHBox* currentLineRow = nullptr;
    int       lineCount      = 0;
    float     optionHeight = 0, currentLineHeight = 0;

    for ( size_t i = 0; i < options.size(); ++i ) {
        const auto& [optLabel, optValue] = options[i];
        // 与原生控件的字体、帧高、文本内间距一致，避免后项圆点覆盖前项文字。
        // 隐藏 ID 后缀不参与可见宽度；显式换行则参与高度，与原生组件规则一致。
        // 即使某项只有圆点也要保留完整帧高，不能因文本为空缩小触摸区域。
        const auto textSize =
            ImGui::CalcTextSize(optLabel.c_str(), nullptr, true);
        const float itemW =
            ImGui::GetFrameHeight() +
            (textSize.x > 0 ? style.ItemInnerSpacing.x + textSize.x : 0.0f);
        const float itemH = std::max(ImGui::GetFrameHeight(),
                                     textSize.y + 2 * style.FramePadding.y);

        if ( !currentLineRow || (currentLineW + itemW > widgetAvailW) ) {
            // 累计各行真实最高项，翻译自带换行时下一行也不能覆盖当前文字。
            if ( currentLineRow ) optionHeight += currentLineHeight + 4;
            currentLineHeight = 0;
            // 首项或超出控件区宽度时，从行对象池取得新横向容器。
            currentLineRow = &getRow(rowIndex++);
            currentLineRow->clear();
            currentLineRow->setPadding(0, 0, 0, 0)
                .setSpacing(12)
                // 每行单选按钮按共同帧高垂直居中。
                .setAlignment(Alignment::Center());

            // 行 ID 使用局部分行序号，顺序随选项与宽度确定。
            std::string lineId =
                labelId + "_line_" + std::to_string(lineCount++);
            containerVBox.addLayout(
                lineId.c_str(), *currentLineRow, Sizing::Grow(), Sizing::Fit());
            // 新行从零重新累计已用宽度。
            currentLineW = 0;
        }

        // 每个选项拥有独立 Clay ID 和 ImGui ID 栈作用域。
        std::string optId = labelId + "_opt_" + std::to_string(i);
        currentLineRow->addElement(
            optId.c_str(),
            Sizing::Fixed(itemW),
            Sizing::Fixed(itemH),
            [this,
             optLabel = optLabel,
             optValue = optValue,
             optionId = optId,
             walkthroughTarget,
             &current,
             &changed](Clay_BoundingBox r, bool) {
                // Clay 决定按钮位置，ImGui 负责输入和最终绘制。
                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImGui::PushID(optionId.c_str());
                if ( ::MMM::UI::FeedbackRadioButton(optLabel.c_str(),
                                                    current == optValue) ) {
                    // 点击同时写入选项值和页面级变化标记。
                    current = optValue;
                    changed = true;
                    if ( walkthroughTarget && m_sourceManager )
                        m_sourceManager->walkthroughSpotlight().completeTarget(
                            walkthroughTarget, true);
                }
                ImGui::PopID();
                if ( walkthroughTarget && m_sourceManager ) {
                    scrollGuideRowIntoView(walkthroughTarget, r);
                    ImVec2 minimum, maximum;
                    // 多个单选按钮只合并当前可见的部分。
                    if ( visibleGuideRect(r, minimum, maximum) )
                        m_sourceManager->walkthroughSpotlight().reportTarget(
                            walkthroughTarget, minimum, maximum);
                }
            });

        // 加入下一项前计入统一十二像素项间距。
        currentLineW += itemW + 12.0f;
        currentLineHeight = std::max(currentLineHeight, itemH);
    }
    // 最后一行没有后续换行分支，必须在循环后计入一次，且不添加尾部行间距。
    optionHeight += currentLineHeight;

    // 换行预算保守扣除嵌套边距，实际值列仍随父行增长，不强行撑宽窄分区。
    row.addLayout((labelId + "_group").c_str(),
                  containerVBox,
                  Sizing::Grow(),
                  Sizing::Fit());

    if ( stacked ) {
        // 包装容器来自现有对象池，生命周期覆盖本帧布局执行，不保存父布局指针。
        // 选项沿用原前缀，点击命中不依赖文字长度或当前换行序号。
        auto& stack = getSection(sectionIndex++);
        stack.clear();
        stack.setPadding(0, 0, 0, 0).setSpacing(4);
        // 名称换行与选项分别占据真实高度，搜索高亮必须包含整个纵向组合。
        const float labelHeight =
            ImGui::CalcTextSize(label, nullptr, false, widgetAvailW).y;
        const float groupHeight = optionHeight + (decorated ? 12.0f : 4.0f);
        stack.addElement(
            labelId + "_stack_label",
            Sizing::Grow(),
            Sizing::Fixed(labelHeight),
            [this, label, searchTarget, groupHeight](Clay_BoundingBox r, bool) {
                // 换行只作用于本名称，不改变整个 Child 的文本裁剪区。
                // 名称单独占行，不再垂直居中到多行选项的总高度。
                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + r.width);
                ImGui::TextUnformatted(label);
                ImGui::PopTextWrapPos();
                // 搜索高亮覆盖名称、组内间距和全部选项，不能只露出组标题。
                if ( searchTarget )
                    reportSettingsSearchRow(
                        { r.x, r.y, r.width, r.height + 4 + groupHeight });
            });
        stack.addLayout(
            (labelId + "_row").c_str(), row, Sizing::Grow(), Sizing::Fit());
        parent.addLayout(
            (labelId + "_stack").c_str(), stack, Sizing::Grow(), Sizing::Fit());
        // 同一行仅挂入一条布局路径，避免控件重复提交相同 ImGui ID。
        return;
    }
    // 整个设置行高度由标签与多行选项中较高的一方决定。
    parent.addLayout(
        (labelId + "_row").c_str(), row, Sizing::Grow(), Sizing::Fit());
}

}  // namespace MMM::UI
