#pragma once

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>

namespace MMM::UI
{
/// @brief 只调整工具栏的停靠归属，保留现有工作区的窗口与拆分树。
/// @param rootId 主工作区根节点，必须已经建立。
/// @param fixed 是否将工具栏固定到工作区外侧。
/// @param toolbarWidth 工具栏实际像素宽度，不是宿主的外部占位。
/// @param workspaceSize 切换模式后宿主的可用尺寸。
/// @return 可停靠工具栏所在节点；固定模式或根节点缺失时返回零。
/// @details 只在模式切换时调用，且必须先于本帧 DockSpace 提交。
/// 在现有根节点外侧增加窄栏会保留内部叶节点，设置页和并排画布无需重停靠。
/// 固定时仅移出工具栏，由 ImGui 合并空叶节点；同节点的其他窗口必须保留。
/// @warning 低频布局路径：禁止逐帧执行拆分，也不能跨拆分保存节点指针。
inline ImGuiID updateToolbarDockLayout(ImGuiID rootId, bool fixed,
                                       float toolbarWidth, ImVec2 workspaceSize)
{
    // 使用与 ToolbarView 的 ### 后缀一致的身份，避免操作一个同名虚拟窗口。
    constexpr const char* TOOLBAR_NAME = " ###Toolbar";
    if ( fixed ) {
        // 即使工具栏与设置页共用叶节点，也只移出工具栏这一张标签。
        // 不移除整个节点，更不能删除主根节点而重建其他窗口的默认布局。
        ImGui::DockBuilderDockWindow(TOOLBAR_NAME, 0);
        return 0;
    }
    if ( !ImGui::DockBuilderGetNode(rootId) ) return 0;

    // 项目恢复或重复请求时优先复用原节点，不额外叠加一层空窄栏。
    // 只接受主工作区内部的节点，浮动工具栏需要重新接入主宿主。
    ImGuiID existingId = 0;
    if ( auto* toolbar = ImGui::FindWindowByName(TOOLBAR_NAME) ) {
        existingId = toolbar->DockId;
    } else if ( auto* saved =
                    ImGui::FindWindowSettingsByID(ImHashStr(TOOLBAR_NAME)) ) {
        // 启动恢复 ini 时工具栏还没有 Begin，保存记录同样可以指向有效叶节点。
        // 如果只检查活跃窗口，会在每次启动时增加新的工具栏分栏。
        existingId = saved->DockId;
    }
    if ( auto* node = ImGui::DockBuilderGetNode(existingId);
         node && !node->IsSplitNode() &&
         ImGui::DockNodeGetRootNode(node)->ID == rootId ) {
        return existingId;
    }

    // 宿主变宽的同一帧旧根尺寸尚未更新，拆分前显式提供新的可用尺寸。
    // 比例扣除分隔条，窄栏与外部固定工具栏尽量使用相同的像素宽度。
    ImGui::DockBuilderSetNodeSize(rootId, workspaceSize);
    const float availableWidth = std::max(
        workspaceSize.x - ImGui::GetStyle().DockingSeparatorSize, 1.0f);
    const float ratio = std::clamp(toolbarWidth / availableWidth, 0.01f, 0.25f);
    const ImGuiID toolbarId = ImGui::DockBuilderSplitNode(
        rootId, ImGuiDir_Right, ratio, nullptr, nullptr);
    // 根节点拆分继承原子树，原画布/设置叶节点的身份和相互关系不变。
    // Finish 将工具栏加入新叶节点，不重新枚举或定位其他任何窗口。
    ImGui::DockBuilderDockWindow(TOOLBAR_NAME, toolbarId);
    ImGui::DockBuilderFinish(rootId);
    return toolbarId;
}
}  // namespace MMM::UI
