#pragma once

#include "imgui.h"
#include <string>

namespace MMM::Graphic::UI
{

enum class ImGuiWidget { Button, Slider, InputText, Checkbox, TextOnly };

struct WidgetSizeHelper {
    /**
     * @brief 计算控件在当前 ImGui 样式下的最佳尺寸 (Preferred Size)
     */
    static ImVec2 Calculate(ImGuiWidget type, const std::string& label = "",
                            ImVec2 imageSize = { 0, 0 })
    {
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

    // 专门处理图片，保持比例
    static ImVec2 ImageFit(ImVec2 originalSize, float targetHeight)
    {
        float ratio = originalSize.x / originalSize.y;
        return ImVec2(targetHeight * ratio, targetHeight);
    }
};

}  // namespace MMM::Graphic::UI
