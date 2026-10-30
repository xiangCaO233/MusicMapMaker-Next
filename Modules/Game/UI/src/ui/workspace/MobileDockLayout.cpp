#include "ui/workspace/MobileDockLayout.h"
#include <algorithm>
#include <cmath>
#include <imgui_internal.h>

namespace MMM::UI
{
/// @brief 从独立移动配置恢复首次分割参数，下一次提交按当前屏幕建树。
/// @details 活动标签待匹配窗口出现时消费，不持有窗口或节点指针。
void MobileDockLayout::restore(float timingRatio, float previewRatio,
                               ImGuiID selectedTab)
{
    m_timingRatio  = timingRatio;
    m_previewRatio = previewRatio;
    m_restoreFocus = selectedTab;
    m_startupFocus = selectedTab;
    m_visibility   = -1;
}

/// @brief 冻结用户实际调整结果，不能用最初默认比例冒充当前停靠宽度。
/// @details 隐藏叶节点没有几何，继续使用隐藏前缓存的比例。
/// @warning 交互提交时只访问三叶一根，不遍历整个 ImGui 窗口集合。
void MobileDockLayout::capture(float& timingRatio, float& previewRatio,
                               ImGuiID& selectedTab) const
{
    timingRatio  = m_timingRatio;
    previewRatio = m_previewRatio;
    // 尚未显示的树不能生成有效快照，保留调用方先前保存的标签身份。
    const auto* root = ImGui::DockBuilderGetNode(m_root);
    if ( !root || root->Size.x <= 0 ) return;
    if ( const auto* leaf = ImGui::DockBuilderGetNode(m_timing) )
        timingRatio = leaf->Size.x / root->Size.x;
    if ( const auto* leaf = ImGui::DockBuilderGetNode(m_preview) )
        previewRatio = leaf->Size.x / root->Size.x;
    if ( const auto* leaf = ImGui::DockBuilderGetNode(m_center);
         leaf && leaf->VisibleWindow )
        // 保存窗口 ID；原生 TabId 是从窗口 ID 派生的另一身份，不能用于 Begin
        // 匹配。
        selectedTab = leaf->VisibleWindow->ID;
}

/// @brief 在所有业务窗口提交后处理启动焦点，兼容惰性窗口的连续聚焦请求。
/// @details 用户第一次按下立即结束恢复保护，后续完全交还普通导航规则。
/// 弹层始终更优先，不能为了恢复中心标签夺走确认或文件选择的输入。
/// @warning 每帧只查找一个固定窗口 ID，无遍历、资源创建或同步等待。
void MobileDockLayout::finishFrame()
{
    if ( !m_startupFocus ) return;
    if ( ImGui::IsMouseClicked(0) ) {
        m_startupFocus = m_restoreFocus = 0;
        return;
    }
    if ( ImGui::IsPopupOpen(
             nullptr,
             ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) )
        return;
    // 未出现的窗口留待下一帧，无需猜测初始化耗时或设定等待帧数。
    if ( auto* window = ImGui::FindWindowByID(m_startupFocus);
         window && window->Active )
        ImGui::FocusWindow(window);
}

/// @brief 保留主题横向留白，并仅为标签交互提供最小触摸高度。
/// @details 宿主生成标签和业务 Begin 计算标题高度时需要同一值。
/// 内容控件不继承这一扩展，避免固定高度布局被标签尺寸撑坏。
/// @warning 每个停靠窗口每帧调用，只有固定规模标量运算。
ImVec2 MobileDockLayout::tabPadding(float scale)
{
    const auto& style = ImGui::GetStyle();
    return { style.FramePadding.x,
             std::max(style.FramePadding.y,
                      (48.0f * scale - ImGui::GetFontSize()) * .5f) };
}

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
    // 与 ImGui 窗口的整数像素几何一致，避免非整数 UI
    // 倍率让重建和最终宿主差一像素。
    // 所有比例换算都基于这份实际尺寸，反复重启才不会累积取整误差。
    position = { std::floor(position.x), std::floor(position.y) };
    size     = { std::max(1.0f, std::floor(size.x)),
                 std::max(1.0f, std::floor(size.y)) };
    // 分隔条可见尺寸由调用方美化样式提供，不为触摸命中额外画粗。
    // 三个分区共享宿主顶部和底部，标签栏使用同一帧字号与 FramePadding。
    const auto& style = ImGui::GetStyle();
    // 只在停靠树提交期间扩展标签高度，不能挤坏共享弹窗的固定高度控件。
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, tabPadding(scale));
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
                    std::clamp(old->Size.x / root->Size.x, .02f, .95f);
            if ( const auto* old = ImGui::DockBuilderGetNode(m_preview) )
                m_previewRatio =
                    std::clamp(old->Size.x / root->Size.x, .02f, .95f);
            if ( const auto* old = ImGui::DockBuilderGetNode(m_center);
                 old && old->VisibleWindow )
                m_restoreFocus = old->VisibleWindow->ID;
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
        // 保存值相对于根宽度；原生 split 的比例却相对于扣除缝隙后的父宽度。
        // 先恢复目标像素再换算，不能在每次重启时重复扣除一次缝隙。
        const auto splitRight = [&](float ratio, ImGuiID& leaf) {
            const auto* parent = ImGui::DockBuilderGetNode(m_center);
            const float available =
                std::max(1.0f, parent->Size.x - style.DockingSeparatorSize);
            // 原生先截断左子区，右子区取余数；半像素偏置保证整数目标不会向上漂移。
            // 像素取整只发生在重建，不影响原生持续拖动的即时几何反馈。
            const float desired = std::round(size.x * ratio);
            ImGui::DockBuilderSplitNode(
                m_center,
                ImGuiDir_Right,
                std::clamp((desired - .5f) / available, .01f, .99f),
                &leaf,
                &m_center);
        };
        if ( preview ) splitRight(m_previewRatio, m_preview);
        if ( timing ) splitRight(m_timingRatio, m_timing);
        ImGui::DockBuilderFinish(m_root);
        // 完成整棵树后再记录组合，下一帧即可走不重建的稳定路径。
        // 业务窗口稍后依次 Begin，并从当前布局获取各自所属叶节点。
        m_visibility = visible;
    }
    // 原生分割器支持独立的边缘命中扩展，透明范围不影响可见样式。
    auto& borderPadding = ImGui::GetCurrentContext()->WindowsBorderHoverPadding;
    const float previousPadding = borderPadding;
    // 修改仅包围这次原生 DockSpace，之后恢复，避免扩大其他窗口边界。
    borderPadding = std::max(borderPadding, 16.0f * scale);
    ImGui::DockSpace(m_root, {}, flags);
    borderPadding = previousPadding;
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
            const float halfWidth =
                std::max(0.5f, style.DockingSeparatorSize * .15f);
            // 握柄跟随分隔缝隙缩窄；颜色和端部圆角同样使用当前主题。
            // DockSpace 创建自己的子宿主；画到该层才能覆盖分隔条底色。
            const auto* root = ImGui::DockBuilderGetNode(m_root);
            auto* draw = root && root->HostWindow ? root->HostWindow->DrawList
                                                  : ImGui::GetWindowDrawList();
            draw->AddRectFilled({ x - halfWidth, y - 24 * scale },
                                { x + halfWidth, y + 24 * scale },
                                ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                style.GrabRounding);
        }
    }
    // 所有标签栏交由同一个停靠宿主计算，不再独立拼接无标题浮窗。
    // 与宿主窗口和样式推入顺序配对，不能将零内边距泄漏给业务视图。
    ImGui::End();
    ImGui::PopStyleVar(2);
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
