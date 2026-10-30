#include "ui/workspace/MobileDockLayout.h"
#include <algorithm>
#include <imgui_internal.h>

namespace MMM::UI
{
/// @brief 返回当前分区的叶节点，调用方不能把根分割节点用于业务停靠。
/// @details 节点 ID 在显隐切换时可能变化，业务窗口应每帧重新取得目标。
/// 隐藏分区返回零，避免业务窗口继续使用已经删除的辅助叶节点。
ImGuiID MobileDockLayout::node(MobileDockPanel panel) const
{
    switch ( panel ) {
    case MobileDockPanel::Timing: return m_timing;
    case MobileDockPanel::Preview: return m_preview;
    default: return m_center;
    }
}

/// @brief
/// 在一个根停靠区内组合三个等高叶节点，标签与内容边界由同一原生节点树计算。
/// @param position 已扣除系统安全区与外边距的宿主左上角。
/// @param size 宿主整体尺寸，工具栏由外壳在调用前另行扣除。
/// @param scale 内容倍率，分隔条与可见握柄使用同一触摸尺度。
/// @param timing 是否为时间线预留独立叶节点。
/// @param preview 是否为预览预留独立叶节点。
/// @details 不启用自动隐藏标签栏，单窗口叶节点也保留自己的标签。
/// 分区宽度由原生分隔条连续调整，业务窗口不提供拖出或关闭入口。
/// @warning 普通帧只读固定数量节点，不执行业务数据扫描或同步等待。
void MobileDockLayout::render(ImVec2 position, ImVec2 size, float scale,
                              bool timing, bool preview)
{
    // 加宽原生分隔条并绘制中心握柄，让触屏用户有明确的落指位置。
    // 三个分区共享宿主顶部和底部，标签栏使用同一帧字号与 FramePadding。
    ImGui::GetStyle().DockingSeparatorSize = 16.0f * scale;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{});
    // 宿主不产生额外内边距，实际内容内边距由每个业务窗口自己管理。
    // 根位置采用安全区坐标，不能依赖上次 ini 中保存的桌面窗口位置。
    ImGui::SetNextWindowPos(position);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("MobileDockHost",
                 nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    m_root = ImGui::GetID("MobileDockRoot");
    // 显隐组成两位键；单纯横竖屏切换保留原生树内的相对分割比例。
    // 所有 ID 都在宿主作用域取得，避免与业务视图的同名控件碰撞。
    const int  visible = (timing ? 1 : 0) | (preview ? 2 : 0);
    const auto flags   = ImGuiDockNodeFlags_NoDockingSplit |
                         ImGuiDockNodeFlags_NoUndocking |
                         ImGuiDockNodeFlags_NoWindowMenuButton |
                         ImGuiDockNodeFlags_NoCloseButton;
    if ( !ImGui::DockBuilderGetNode(m_root) || visible != m_visibility ) {
        // 先读取旧节点的实际尺寸，再进行任何删除；删除会使观察指针失效。
        if ( const auto* root = ImGui::DockBuilderGetNode(m_root);
             root && root->Size.x > 0 ) {
            if ( const auto* old = ImGui::DockBuilderGetNode(m_timing) )
                m_timingRatio =
                    std::clamp(old->Size.x / root->Size.x, .1f, .6f);
            if ( const auto* old = ImGui::DockBuilderGetNode(m_preview) )
                m_previewRatio =
                    std::clamp(old->Size.x / root->Size.x, .1f, .6f);
            if ( const auto* old = ImGui::DockBuilderGetNode(m_center) )
                m_restoreFocus = old->SelectedTabId;
            ImGui::DockBuilderRemoveNode(m_root);
        }
        // 只在拓扑变化时重建，不能逐帧用默认比例覆盖用户拖动结果。
        ImGui::DockBuilderAddNode(m_root, ImGuiDockNodeFlags_DockSpace | flags);
        ImGui::DockBuilderSetNodePos(m_root, position);
        ImGui::DockBuilderSetNodeSize(m_root, size);
        m_center = m_root;
        // 重建后先清除隐藏节点，不能将旧 ID 发布给后续业务 Begin。
        // 主区可直接是根叶节点，也可以在分割后成为最后剩余的左叶。
        m_timing = m_preview = 0;
        if ( preview )
            ImGui::DockBuilderSplitNode(m_center,
                                        ImGuiDir_Right,
                                        m_previewRatio,
                                        &m_preview,
                                        &m_center);
        if ( timing ) {
            // 第二次分割作用于扣除预览后的剩余宽度，换算比例避免重复扣除。
            const float remaining = preview ? 1.0f - m_previewRatio : 1.0f;
            ImGui::DockBuilderSplitNode(
                m_center,
                ImGuiDir_Right,
                std::clamp(m_timingRatio / remaining, .1f, .75f),
                &m_timing,
                &m_center);
        }
        ImGui::DockBuilderFinish(m_root);
        // 完成整棵树后再记录组合，下一帧即可走不重建的稳定路径。
        // 业务窗口稍后依次 Begin，并从当前布局获取各自所属叶节点。
        m_visibility = visible;
    }
    ImGui::DockSpace(m_root, {}, flags);
    // 使用原生分隔条承接拖动，握柄只提供视觉提示，不创建竞争命中区。
    // 握柄属于宿主绘制层，业务菜单和模态仍可正常覆盖它。
    for ( const auto id : { m_timing, m_preview } ) {
        // 每个右侧叶节点的左边界对应一个分割，隐藏叶节点不绘制握柄。
        // 读取当前几何使握柄随原生拖动实时移动，不保存另一份坐标。
        if ( const auto* leaf = ImGui::DockBuilderGetNode(id) ) {
            // 分隔条位于叶节点左侧间隙，图形中心与原生命中区域中心一致。
            // 根宿主高度统一，因此两个握柄都保持在同一水平线上。
            const float x =
                leaf->Pos.x - ImGui::GetStyle().DockingSeparatorSize * .5f;
            const float y = position.y + size.y * .5f;
            // DockSpace 创建自己的子宿主；画到该层才能覆盖分隔条底色。
            const auto* root = ImGui::DockBuilderGetNode(m_root);
            auto* draw = root && root->HostWindow ? root->HostWindow->DrawList
                                                  : ImGui::GetWindowDrawList();
            draw->AddRectFilled({ x - 2 * scale, y - 24 * scale },
                                { x + 2 * scale, y + 24 * scale },
                                ImGui::GetColorU32(ImGuiCol_Text),
                                2 * scale);
        }
    }
    // 所有标签栏交由同一个停靠宿主计算，不再独立拼接无标题浮窗。
    // 与宿主窗口和样式推入顺序配对，不能将零内边距泄漏给业务视图。
    ImGui::End();
    ImGui::PopStyleVar();
}

/// @brief 将共享业务窗口放入受控分区，窗口内容仍由原视图绘制。
/// @details 仅在重建后的匹配窗口恢复一次焦点，不逐帧抢走文本输入。
/// @param panel 本次业务窗口应进入的受控叶节点。
/// @param title 包含固定内部 ID 的标题，用于匹配先前活动标签。
/// @warning 每个窗口 Begin 前调用一次，只做固定 ID 设置与比较。
void MobileDockLayout::target(MobileDockPanel panel, const char* title)
{
    ImGui::SetNextWindowDockID(node(panel), ImGuiCond_Always);
    // 固定目标只约束窗口归属，不重新设置叶节点尺寸或比例。
    if ( m_restoreFocus && ImHashStr(title) == m_restoreFocus ) {
        // 标题使用 ### 时哈希仍取固定身份，语言变化不会丢失焦点目标。
        // 只消费一次恢复请求，之后由用户点击决定当前标签。
        ImGui::SetNextWindowFocus();
        m_restoreFocus = 0;
    }
}
}  // namespace MMM::UI
