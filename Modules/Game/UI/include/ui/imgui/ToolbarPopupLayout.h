#pragma once

#include <imgui.h>

#include <algorithm>

namespace MMM::UI
{
/// @brief 工具弹层的位置、锚定枢轴及可滚动尺寸上限。
/// @details 弹层位于工具栏朝向工作区的一侧，始终保留按钮操作区域。
struct ToolbarPopupGeometry {
    /// @brief 传给 SetNextWindowPos 的屏幕锚点。
    ImVec2 m_position;
    /// @brief 底部弹层使用下边缘枢轴，实际高度变化不会覆盖工具栏。
    ImVec2 m_pivot;
    /// @brief 工具栏内侧到视口安全边缘的可用尺寸。
    ImVec2 m_maximumSize;
};

/// @brief 按工具栏所在边缘计算弹层位置和尺寸约束。
/// @param toolbarPosition 工具栏本帧屏幕左上角。
/// @param toolbarSize 工具栏本帧实际尺寸。
/// @param buttonPosition 触发按钮的屏幕左上角，横排需要同时记录 X 与 Y。
/// @param popupSize 弹层上次实际尺寸或首次估计值，只用于选择横向对齐边缘。
/// @param viewportPosition 主视口可用工作区起点。
/// @param viewportSize 主视口可用工作区尺寸。
/// @param edge 工具栏所在边缘，浮动工具栏由调用者根据可用空间选边。
/// @param gap 弹层与工具栏之间的间距。
/// @param padding 弹层与视口边缘之间的安全留白。
/// @return 无负尺寸的弹层几何；超高内容由窗口内部滚动承载。
/// @warning UI 热路径：每个可见弹层每帧执行，只计算固定数量的坐标。
inline ToolbarPopupGeometry calculateToolbarPopupGeometry(
    ImVec2 toolbarPosition, ImVec2 toolbarSize, ImVec2 buttonPosition,
    ImVec2 popupSize, ImVec2 viewportPosition, ImVec2 viewportSize,
    ImGuiDir edge, float gap, float padding)
{
    // 极小视口仍保留正的尺寸范围，避免 std::clamp 收到颠倒的上下限。
    ImVec2 minimum(viewportPosition.x + padding, viewportPosition.y + padding);
    ImVec2 maximum(
        std::max(minimum.x + 1, viewportPosition.x + viewportSize.x - padding),
        std::max(minimum.y + 1, viewportPosition.y + viewportSize.y - padding));
    ToolbarPopupGeometry result{ buttonPosition, ImVec2(0, 0), ImVec2(1, 1) };
    const bool horizontal = edge == ImGuiDir_Up || edge == ImGuiDir_Down;
    const int  axis       = horizontal ? 1 : 0;
    const bool leading    = edge == ImGuiDir_Up || edge == ImGuiDir_Left;
    // 只在展开轴扣除工具栏占位，不能再沿用竖排时向左挤的布局。
    // 起始边向下或向右展开，末尾边向上或向左展开。
    if ( leading ) {
        minimum[axis] =
            std::clamp(toolbarPosition[axis] + toolbarSize[axis] + gap,
                       minimum[axis],
                       maximum[axis] - 1);
        result.m_position[axis] = minimum[axis];
    } else {
        maximum[axis] = std::clamp(
            toolbarPosition[axis] - gap, minimum[axis] + 1, maximum[axis]);
        result.m_position[axis] = maximum[axis];
        result.m_pivot[axis]    = 1;
    }
    result.m_maximumSize = ImVec2(maximum.x - minimum.x, maximum.y - minimum.y);
    // 沿工具栏长轴对齐实际按钮；靠近右边的按钮改用右边缘枢轴。
    // 枢轴由 ImGui 在自动尺寸确定后应用，比先减去旧高度更稳定。
    const int crossAxis = 1 - axis;
    if ( buttonPosition[crossAxis] + popupSize[crossAxis] >
         maximum[crossAxis] ) {
        result.m_position[crossAxis] = maximum[crossAxis];
        result.m_pivot[crossAxis]    = 1;
    } else {
        result.m_position[crossAxis] =
            std::max(minimum[crossAxis], buttonPosition[crossAxis]);
        // 新展开内容可能超过旧尺寸；限制到锚点另一侧的剩余空间。
        // 这样首次内容增长也不会越界，后续帧可按实际尺寸重新选对齐边缘。
        result.m_maximumSize[crossAxis] =
            maximum[crossAxis] - result.m_position[crossAxis];
    }
    return result;
}

/// @brief 为下一次 Begin 提交工具弹层几何与内容尺寸上限。
/// @param geometry 已按本帧工具栏和主视口计算的几何。
/// @param viewportId 弹层所属主视口的稳定身份。
/// @warning UI 热路径：只提交 ImGui 布局请求，不改变工具栏或中心停靠区。
inline void prepareToolbarPopup(const ToolbarPopupGeometry& geometry,
                                ImGuiID                     viewportId)
{
    // AlwaysAutoResize 在上限以内仍按内容调整，超过上限时出现内部滚动条。
    // 用枢轴确定顶部或底部展开方向，不依赖上一帧高度来保持边缘贴合。
    ImGui::SetNextWindowViewport(viewportId);
    ImGui::SetNextWindowPos(
        geometry.m_position, ImGuiCond_Always, geometry.m_pivot);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), geometry.m_maximumSize);
}
}  // namespace MMM::UI
