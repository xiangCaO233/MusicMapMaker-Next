#pragma once
#include "ui/workspace/IWorkspacePresentation.h"
#include <algorithm>
#include <imgui.h>

namespace MMM::Canvas
{
/// @brief 触屏辅助表格初次打开时使用外壳安全区域，保留常驻工具栏的输入区。
/// @details 只改变平台窗口几何，业务选择、编辑副本和关闭逻辑仍由原窗口持有。
/// 位置与尺寸均使用当前 UI 帧屏幕坐标，不能使用旧设备密度推导。
/// @warning 每帧只计算固定标量；Appearing 约束不会在拖动时把窗口拉回。
inline void prepareMobileAuxiliaryWindow()
{
    const auto  area = UI::workspaceFloatingWindowArea();
    const float font = ImGui::GetFontSize();
    // 限制宽高的同时为边框留出余量，不允许关闭按钮落到工具栏下面。
    // 字体越大所需空间越大，但任何尺寸仍不得超过外壳发布的安全区域。
    const ImVec2 maximum{ std::max(1.0f, area.m_size.x * .94f),
                          std::max(1.0f, area.m_size.y * .94f) };
    // 两轴独立夹取，极窄窗口仍有有限合法尺寸，不用缩小字号挤压触摸区。
    // 工作区边界来自外壳，因此不在这里复制工具栏宽度或抽屉布局公式。
    ImGui::SetNextWindowSizeConstraints(
        { std::min(maximum.x, font * 42), std::min(maximum.y, font * 24) },
        maximum);
    ImGui::SetNextWindowSize({ std::min(maximum.x, font * 70), maximum.y },
                             ImGuiCond_Appearing);
    // 旧的桌面位置即使仍处于主视口内，也可能被移动工具栏遮挡。
    // 每次显式打开都在安全区域居中，后续帧继续尊重用户拖动位置。
    ImGui::SetNextWindowPos({ area.m_origin.x + area.m_size.x * .5f,
                              area.m_origin.y + area.m_size.y * .5f },
                            ImGuiCond_Appearing,
                            { .5f, .5f });
}
}  // namespace MMM::Canvas
