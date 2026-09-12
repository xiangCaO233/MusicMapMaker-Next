#pragma once

#include "imgui.h"
#include <string>

namespace MMM::UI
{

/// @brief WidgetSizeHelper 支持估算的基础 ImGui 控件类别。
enum class ImGuiWidget { Button, Slider, InputText, Checkbox, TextOnly };

/// @brief 根据当前字体和样式估算控件首选尺寸的无状态辅助类。
/// @note 结果用于布局建议，不替代 ImGui 在实际窗口中的最终尺寸裁决。
struct WidgetSizeHelper {
    /// @brief 计算控件在当前 ImGui 样式下的首选尺寸。
    /// @param type 待估算的控件类别。
    /// @param label 控件可见标签；无文本控件可以留空。
    /// @param imageSize 预留图片尺寸参数，当前基础控件估算不使用。
    /// @return 建议尺寸；未知类别返回零尺寸。
    /// @warning UI 热路径辅助函数：只读取当前 ImGui 样式和字体。
    static ImVec2 Calculate(ImGuiWidget type, const std::string& label = "",
                            ImVec2 imageSize = { 0, 0 })
    {
        // imageSize 为兼容既有调用保留，图片缩放统一由 ImageFit 处理。
        ImGuiStyle& style    = ImGui::GetStyle();
        float       fontSize = ImGui::GetFontSize();

        // 基础公式：高度 = 字体高度 + 上下边距 * 2
        float frameHeight = fontSize + style.FramePadding.y * 2.0f;

        switch ( type ) {
        case ImGuiWidget::Button: {
            // 按钮宽度 = 文字宽度 + 左右边距 * 2
            ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
            return ImVec2(textSize.x + style.FramePadding.x * 2.0f,
                          frameHeight);
        }

        case ImGuiWidget::Slider:
        case ImGuiWidget::InputText: {
            // Slider/Input 通常没有“固有宽度”，建议给个默认值或由父容器决定
            // 这里返回高度，宽度建议外部指定 (如 Sizing::Percent)
            return ImVec2(100.0f, frameHeight);
        }

        case ImGuiWidget::Checkbox: {
            // Checkbox 宽度 = 勾选框(等于高度) + 间隔 + 文字宽度
            ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
            return ImVec2(frameHeight + style.ItemInnerSpacing.x + textSize.x,
                          frameHeight);
        }

        case ImGuiWidget::TextOnly: {
            // 纯文本没有 FramePadding
            return ImGui::CalcTextSize(label.c_str());
        }

        default: return ImVec2(0, 0);
        }
    }

    /// @brief 按目标高度等比缩放图片尺寸。
    /// @param originalSize 图片原始宽高。
    /// @param targetHeight 目标显示高度。
    /// @return 保持原宽高比的目标尺寸。
    /// @pre originalSize.y 必须大于零。
    static ImVec2 ImageFit(ImVec2 originalSize, float targetHeight)
    {
        // 仅使用宽高比，不裁剪或限制目标宽度。
        float ratio = originalSize.x / originalSize.y;
        return ImVec2(targetHeight * ratio, targetHeight);
    }
};

}  // namespace MMM::UI
