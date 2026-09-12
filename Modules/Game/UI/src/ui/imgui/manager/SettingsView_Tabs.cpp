#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"

#include "imgui.h"

/// @file SettingsView_Tabs.cpp
/// @brief 设置页复用的分组、标准双列行和自动换行单选项布局实现。
/// @details 辅助函数从对象池取得 CLayBox 容器，通过递增索引保证同一帧节点
/// 身份稳定；实际 ImGui 控件由布局完成后的回调绘制。

namespace MMM::UI
{

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
/// @warning UI 热路径：设置页可见时每帧构造布局，回调不得阻塞或访问文件。
void SettingsView::addSettingItem(CLayVBox& parent, size_t& rowIndex,
                                  const char* label, float labelWidth,
                                  CLayBox::DrawFunc widget, bool dangerLabel,
                                  bool decorated)
{
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
    row.addElement(labelId + "_wgt",
                   Sizing::Grow(),
                   Sizing::Grow(),
                   [widget](Clay_BoundingBox r, bool h) {
                       // 控件回调接收完整横向区域和 Clay 悬停状态。
                       float widgetH = ImGui::GetFrameHeight();
                       float offset  = (r.height - widgetH) * 0.5f;
                       ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                       widget(r, h);
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
/// @warning UI 热路径：每帧按可用宽度重新分行，选项集合应保持小规模稳定。
void SettingsView::addRadioSetting(
    CLayVBox& parent, size_t& rowIndex, size_t& sectionIndex, const char* label,
    float labelWidth, const std::vector<std::pair<std::string, int>>& options,
    int& current, bool& changed, bool decorated)
{
    // 使用当前 ImGui 内容区宽度决定单选项换行位置。
    float totalWidth = ImGui::GetContentRegionAvail().x;

    // 扣除外层、分组、行内边距、列间距及滚动条缓冲后得到控件区宽度。
    float widgetAvailW = totalWidth - labelWidth - 72.0f;
    if ( widgetAvailW < 150.0f ) {
        // 极窄窗口仍保留可绘制单选按钮的最小区域。
        widgetAvailW = 150.0f;
    }

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
    row.addElement(labelId + "_lbl",
                   Sizing::Fixed(labelWidth),
                   Sizing::Grow(),
                   [label](Clay_BoundingBox r, bool) {
                       // 标签在 Clay 分配行高中按实际文本高度居中。
                       float textH  = ImGui::CalcTextSize(label).y;
                       float offset = (r.height - textH) * 0.5f;
                       ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                       ImGui::Text("%s", label);
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

    for ( size_t i = 0; i < options.size(); ++i ) {
        const auto& [optLabel, optValue] = options[i];
        // 文本宽度外预留 Radio 圆点和内部间距。
        float itemW = ImGui::CalcTextSize(optLabel.c_str()).x + 36.0f;

        if ( !currentLineRow || (currentLineW + itemW > widgetAvailW) ) {
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
            Sizing::Fixed(ImGui::GetFrameHeight()),
            [optLabel = optLabel,
             optValue = optValue,
             optionId = optId,
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
                }
                ImGui::PopID();
            });

        // 加入下一项前计入统一十二像素项间距。
        currentLineW += itemW + 12.0f;
    }

    // 控件组使用计算后的固定宽度，高度按换行数量适配。
    row.addLayout((labelId + "_group").c_str(),
                  containerVBox,
                  Sizing::Fixed(widgetAvailW),
                  Sizing::Fit());

    // 整个设置行高度由标签与多行选项中较高的一方决定。
    parent.addLayout(
        (labelId + "_row").c_str(), row, Sizing::Grow(), Sizing::Fit());
}

}  // namespace MMM::UI
