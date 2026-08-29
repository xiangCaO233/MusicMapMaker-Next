#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"

#include "imgui.h"

namespace MMM::UI
{

/// @brief 添加一行标准设置项。
/// @param parent 接收该行的父级纵向布局。
/// @param rowIndex 当前布局行索引，会在添加过程中递增。
/// @param label 设置项标签文本。
/// @param labelWidth 标签列固定宽度。
/// @param widget 设置项右侧控件绘制回调。
/// @param dangerLabel 是否使用危险色绘制标签。
void SettingsView::addSettingItem(CLayVBox& parent, size_t& rowIndex,
                                  const char* label, float labelWidth,
                                  CLayBox::DrawFunc widget, bool dangerLabel)
{
    auto& row = getRow(rowIndex++);
    row.setPadding(8, 8, 0, 0).setSpacing(8).setAlignment(Alignment::Center());

    std::string labelId = "R" + std::to_string(rowIndex) + "_L_" + label;

    // A. Left Box: 【说明标签，弹簧】
    auto& leftBox = getRow(rowIndex++);
    leftBox.clear();
    leftBox.setPadding(0, 0, 0, 0)
        .setSpacing(0)
        .setAlignment(Alignment::Center());

    // 1. 说明标签 (采用 Fit 自动匹配内容宽度)
    leftBox.addElement(
        labelId + "_lbl",
        Sizing::Fit(),
        Sizing::Grow(),
        [label, dangerLabel](Clay_BoundingBox r, bool) {
            float textH  = ImGui::CalcTextSize(label).y;
            float offset = (r.height - textH) * 0.5f;
            ImGui::SetCursorScreenPos({ r.x, r.y + offset });
            if ( dangerLabel ) {
                ImGui::TextColored(
                    Utils::UIThemeUtils::getDangerColor(), "%s", label);
            } else {
                ImGui::Text("%s", label);
            }
        });

    // 2. 弹簧 spacer
    leftBox.addElement(
        labelId + "_lbl_spring", Sizing::Grow(), Sizing::Grow(), nullptr);

    // 将 Left Box 作为一个具有固定宽度的子 HBox 加入主行
    row.addLayout((labelId + "_left").c_str(),
                  leftBox,
                  Sizing::Fixed(labelWidth),
                  Sizing::Grow());

    // B. Right Box: 【控件或标签】直接 Grow()
    row.addElement(labelId + "_wgt",
                   Sizing::Grow(),
                   Sizing::Grow(),
                   [widget](Clay_BoundingBox r, bool h) {
                       float widgetH = ImGui::GetFrameHeight();
                       float offset  = (r.height - widgetH) * 0.5f;
                       ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                       widget(r, h);
                   });

    float rowH = ImGui::GetFrameHeight() + 8.0f;
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
void SettingsView::addRadioSetting(
    CLayVBox& parent, size_t& rowIndex, size_t& sectionIndex, const char* label,
    float labelWidth, const std::vector<std::pair<std::string, int>>& options,
    int& current, bool& changed)
{
    // 获取当前面板的实际可用宽度，并将剩余空间全部分配给控件
    float totalWidth = ImGui::GetContentRegionAvail().x;

    // 扣除 CLay 布局的多层 Padding (外层 VBox 8x2, 装饰 Section 8x2, Row 8x2,
    // 元素间距 8) 以及额外预留滚动条/边缘的缓冲宽度
    // (16)，算出控件可用的实际宽度
    float widgetAvailW = totalWidth - labelWidth - 72.0f;
    if ( widgetAvailW < 150.0f ) {
        widgetAvailW = 150.0f;  // 保证极端情况下的最小可用度，防崩溃
    }

    auto& row = getRow(rowIndex++);
    row.setPadding(8, 8, 0, 0).setSpacing(8).setAlignment(Alignment::Center());

    std::string labelId = "S" + std::to_string(sectionIndex) + "_R" +
                          std::to_string(rowIndex) + "_L_" + label;

    // 1. 标签 (固定宽度为 labelWidth)
    row.addElement(labelId + "_lbl",
                   Sizing::Fixed(labelWidth),
                   Sizing::Grow(),
                   [label](Clay_BoundingBox r, bool) {
                       float textH  = ImGui::CalcTextSize(label).y;
                       float offset = (r.height - textH) * 0.5f;
                       ImGui::SetCursorScreenPos({ r.x, r.y + offset });
                       ImGui::Text("%s", label);
                   });

    // 2. 控件容器组 (动态计算和包含所有 RadioButtons)
    auto& containerVBox = getSection(sectionIndex++);
    containerVBox.clear();
    containerVBox.setSpacing(4).setPadding(0, 0, 0, 0);

    float     currentLineW   = 0;
    CLayHBox* currentLineRow = nullptr;
    int       lineCount      = 0;

    for ( size_t i = 0; i < options.size(); ++i ) {
        const auto& [optLabel, optValue] = options[i];
        float itemW = ImGui::CalcTextSize(optLabel.c_str()).x + 36.0f;

        if ( !currentLineRow || (currentLineW + itemW > widgetAvailW) ) {
            currentLineRow = &getRow(rowIndex++);
            currentLineRow->clear();
            currentLineRow->setPadding(0, 0, 0, 0)
                .setSpacing(12)
                .setAlignment(
                    Alignment::Center());  // 垂直居中对齐每一行 RadioButtons！

            std::string lineId =
                labelId + "_line_" + std::to_string(lineCount++);
            containerVBox.addLayout(
                lineId.c_str(), *currentLineRow, Sizing::Grow(), Sizing::Fit());
            currentLineW = 0;
        }

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
                ImGui::SetCursorScreenPos({ r.x, r.y });
                ImGui::PushID(optionId.c_str());
                if ( ::MMM::UI::FeedbackRadioButton(optLabel.c_str(),
                                                    current == optValue) ) {
                    current = optValue;
                    changed = true;
                }
                ImGui::PopID();
            });

        currentLineW += itemW + 12.0f;
    }

    // 将整个控件组作为撑满剩余可用空间 (widgetAvailW) 的 HBox 加入主行
    row.addLayout((labelId + "_group").c_str(),
                  containerVBox,
                  Sizing::Fixed(widgetAvailW),
                  Sizing::Fit());

    parent.addLayout(
        (labelId + "_row").c_str(), row, Sizing::Grow(), Sizing::Fit());
}

}  // namespace MMM::UI
