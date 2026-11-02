#include "ui/workspace/MobileDockLayout.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <cmath>
#include <imgui_internal.h>
#include <string_view>

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
    // 菜单状态先于启动焦点的早退处理，关闭标签后也必须推进释放保护。
    // 释放帧也保持输入保护，直到下一个没有触点的完整帧才交还画布。
    if ( !ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0) )
        m_releaseGuard = false;
    // 原窗口可能被项目切换移除，不能让失效身份永久占用输入。
    if ( m_menuWindow ) {
        const auto* window = ImGui::FindWindowByID(m_menuWindow);
        if ( !window || !window->Active ) {
            m_menuWindow   = 0;
            m_releaseGuard = true;
        }
    }
    if ( !ImGui::IsMouseDown(0) ) m_holdWindow = 0;
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

/// @brief 用非阻塞长按打开标签操作菜单，关闭仍由业务窗口消费原有打开指针。
/// @details 命中原生标签实际矩形，并扣除滚动裁剪，不能把标签旁空白当作标签。
/// @param closeLabel 当前界面语言的关闭文案，不参与窗口身份计算。
/// @param scale 触摸容差和命中高度的内容倍率，不覆盖主题圆角。
/// @param canClose 业务关闭权限，无可用操作时不弹出空菜单。
/// @param enabled 是否允许开始新手势，不影响已经打开菜单的正常取消。
/// @warning UI 热路径：只读取当前 Dock 标签与触点标量，不保存窗口观察指针。
bool MobileDockLayout::tabCloseRequested(const char* closeLabel, float scale,
                                         bool canClose, bool enabled)
{
    auto* window = ImGui::GetCurrentWindow();
    auto* bar    = window->DockNode ? window->DockNode->TabBar : nullptr;
    auto* tab    = bar ? ImGui::TabBarFindTabByID(bar, window->TabId) : nullptr;
    // 初次 Begin 尚未建立标签时先不识别长按，等待下一帧真实几何。
    if ( !tab ) return false;
    // 跟随原生标签排序与滚动，不按标题文本宽度另建一份命中几何。
    // 当前预编译 ImGui 尚未导出 TabBarGetTabPos，按官方标签布局公式换算。
    const float scroll =
        tab->Flags & ImGuiTabItemFlags_SectionMask_ ? 0.0f : bar->ScrollingAnim;
    const ImVec2 position{ bar->BarRect.Min.x +
                               std::trunc(tab->Offset - scroll),
                           bar->BarRect.Min.y };
    ImRect rect(position, { position.x + tab->Width, bar->BarRect.Max.y });
    // 当前窗口的内容裁剪不包含标签栏，不能用 IsItemHovered 代替标签命中。
    rect.ClipWith(bar->BarRect);
    if ( !(tab->Flags & ImGuiTabItemFlags_SectionMask_) ) {
        // 普通标签随滚动裁剪，首尾露出部分以外的按下不属于这个标签。
        // 固定前后缀标签不参加滚动，因此只对中间区应用这一边界。
        rect.Min.x = std::max(rect.Min.x, bar->ScrollingRectMinX);
        rect.Max.x = std::min(rect.Max.x, bar->ScrollingRectMaxX);
    }
    const auto& io        = ImGui::GetIO();
    const bool  popupOpen = ImGui::IsPopupOpen(
        nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    // 只从标签内的新按下开始；从内容滑入、菜单遮挡及鼠标来源均不启动。
    if ( enabled && canClose && !popupOpen &&
         io.MouseSource == ImGuiMouseSource_TouchScreen &&
         ImGui::IsMouseClicked(0) && rect.Contains(io.MousePos) ) {
        m_holdWindow = window->ID;
        m_holdOrigin = io.MousePos;
        // 使用 ImGui 的单调帧时间，不以固定帧数判定，低帧率也有相同阈值。
        m_holdStarted = ImGui::GetTime();
    }
    if ( m_holdWindow == window->ID ) {
        const float dx = io.MousePos.x - m_holdOrigin.x;
        const float dy = io.MousePos.y - m_holdOrigin.y;
        // 容差按内容 DPI 换算为像素，手机与平板保留相同的手指位移语义。
        const float tolerance = 10 * scale;
        // 一旦开始滑动就取消整个触点，移回原处也不能重新累计长按。
        if ( !enabled || !canClose || popupOpen || !ImGui::IsMouseDown(0) ||
             !rect.Contains(io.MousePos) ||
             dx * dx + dy * dy > tolerance * tolerance )
            m_holdWindow = 0;
        else if ( ImGui::GetTime() - m_holdStarted >= .55 ) {
            // 时间只用于识别手势，不阻塞 UI，也不延迟原生标签的短按切换。
            // Popup ID 属于当前业务窗口，关闭菜单不会错误地改绑相邻标签。
            ImGui::OpenPopup("##MobileTabMenu");
            m_menuWindow = window->ID;
            // 手指仍按在标题上，菜单出现并不表示用户已选择其中的关闭项。
            m_waitMenuRelease = true;
            m_holdWindow      = 0;
        }
    }
    if ( m_menuWindow == window->ID &&
         !ImGui::IsPopupOpen("##MobileTabMenu") ) {
        // 点击外部或原生返回关闭同样拦截最后的触点释放。
        m_menuWindow   = 0;
        m_releaseGuard = true;
    }
    // 只返回本次显式命令，不把正在显示菜单当作关闭成功。
    bool close = false;
    if ( m_menuWindow == window->ID ) {
        // 菜单宽度服从安全工作区；关闭项保留完整手指命中高度。
        // 位置由普通 Popup 的安全区避让处理，不用固定屏幕坐标保存菜单。
        const auto* viewport = ImGui::GetMainViewport();
        const float width    = std::min(220 * scale, viewport->WorkSize.x);
        ImGui::SetNextWindowSizeConstraints({ width, 0 }, viewport->WorkSize);
        // 父窗口即使刚被切为后台标签，也保留本窗口作用域的菜单身份。
        if ( ImGui::BeginPopup("##MobileTabMenu") ) {
            if ( !ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0) )
                // 连释放帧都排除，必须由下一次独立触摸激活关闭。
                m_waitMenuRelease = false;
            // 权限在菜单打开期间变化时也必须移除关闭项，不留下空的禁用按钮。
            if ( !canClose )
                ImGui::CloseCurrentPopup();
            else {
                ImGui::BeginDisabled(m_waitMenuRelease);
                if ( FeedbackButton(
                         closeLabel,
                         { -1,
                           std::max(48 * scale, ImGui::GetFrameHeight()) }) ) {
                    close          = true;
                    m_releaseGuard = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
            }
            // 关闭权限由调用方实时提供，欢迎和设置不能充当额外主画布。
            ImGui::EndPopup();
        }
    }
    return close;
}

/// @brief 在原生 TabBar 排版前约束实际宽度，不截短窗口名称或更换窗口 ID。
/// @details 每个页签最多占 240 个逻辑像素，窄分区再限制为分区宽度的一半。
/// @param scale 当前内容倍率，旋转后重新结合节点宽度计算上限。
/// @warning 每帧访问少量已注册窗口；颜色快照复用容量，只在新标签加入时增长。
void MobileDockLayout::prepareTabCaptions(float scale)
{
    // 只复用存储容量，不复用上一帧颜色快照或窗口名观察指针。
    // 显隐重建之后旧叶节点 ID 可失效，颜色恢复按窗口稳定 ID 完成。
    m_tabCaptions.clear();
    for ( const auto id : { m_center, m_timing, m_preview } ) {
        auto* node = ImGui::DockBuilderGetNode(id);
        if ( !node ) continue;
        // 标签宽度和触摸高度分开处理，长名字不能扩大整个工作区。
        // DPI 只缩放逻辑上限；分区缩窄时不能让单页签独占整排。
        const float maximum =
            std::min(240 * scale, std::max(64 * scale, node->Size.x * .5f));
        for ( auto* window : node->Windows ) {
            // 暂时隐藏原生文字，背景、选中态、排序和滚动箭头仍由原生绘制。
            // 不覆盖标签背景，也不遍历顶点擦除文字，保持主题与批处理顺序。
            // 原生滚动箭头不使用这个窗口缓存色，因此仍保留正常可见反馈。
            auto& color =
                window->DockStyle.Colors[ImGuiWindowDockStyleCol_Text];
            m_tabCaptions.push_back({ window->ID, color });
            color &= ~IM_COL32_A_MASK;
            // 首次建树尚无 TabBar 时仍记录颜色，原生创建后由绘制阶段恢复。
            if ( !node->TabBar ) continue;
            auto* tab = ImGui::TabBarFindTabByID(node->TabBar, window->TabId);
            // 新窗口先登记宽度，再由本轮原生布局安排位置，避免先撑满再收缩。
            // 已登记的标签不可再次 AddTab，避免原生顺序与窗口身份重复。
            if ( !tab ) {
                ImGui::TabBarAddTab(node->TabBar,
                                    window->WindowClass.TabItemFlagsOverrideSet,
                                    window);
                tab = ImGui::TabBarFindTabByID(node->TabBar, window->TabId);
            }
            const auto* end = ImGui::FindRenderedTextEnd(window->Name);
            // 测量仍使用完整可见标题；仅提交显示宽度，不破坏谱名和脏标记。
            const float natural = ImGui::CalcTextSize(window->Name, end).x +
                                  ImGui::GetStyle().FramePadding.x * 2;
            // 原生宽度先变化，短按和长按都沿用这一份真实命中矩形。
            // 这里只改标签排版，不修改业务窗口尺寸或其完整标题字符串。
            if ( tab ) tab->RequestedWidth = std::min(maximum, natural);
        }
    }
}

/// @brief 在原生标签宿主层绘制完整标题，溢出时复用公共往复滚动文本。
/// @details 原生命中区完全保留，文字滚动不影响短按、拖动排序或长按菜单。
/// @warning 每帧只查找保存的窗口 ID，不持有跨帧窗口指针，不重新布局业务窗口。
void MobileDockLayout::drawTabCaptions()
{
    for ( const auto& caption : m_tabCaptions ) {
        // DockSpace 可能调整标签顺序，重新按 ID 找窗口而不依赖旧数组索引。
        auto* window = ImGui::FindWindowByID(caption.m_window);
        if ( !window ) continue;
        // 原生标签已经提交后立即恢复缓存色，不能把透明色传给下一次业务 Begin。
        // 即使原生节点在提交中消失，也须先恢复窗口颜色再检查标签是否仍存在。
        window->DockStyle.Colors[ImGuiWindowDockStyleCol_Text] =
            caption.m_color;
        auto* bar = window->DockNode ? window->DockNode->TabBar : nullptr;
        auto* tab =
            bar ? ImGui::TabBarFindTabByID(bar, window->TabId) : nullptr;
        if ( !tab || !bar->Window ) continue;
        const bool  central = !(tab->Flags & ImGuiTabItemFlags_SectionMask_);
        const float x = bar->BarRect.Min.x +
                        (central ? std::trunc(tab->Offset - bar->ScrollingAnim)
                                 : tab->Offset);
        // 标签栏自身横向滚动时，只画出原生命中范围内露出的部分。
        const float left =
            central ? bar->ScrollingRectMinX : bar->BarRect.Min.x;
        const float right =
            central ? bar->ScrollingRectMaxX : bar->BarRect.Max.x;
        // 必须提交到原生宿主层，后台业务窗口自己的 DrawList 不会显示。
        auto& draw = *bar->Window->DrawList;
        // 宿主当前内容裁剪可能排除标题栏，显式采用标签栏的原生边界。
        draw.PushClipRect(
            { left, bar->BarRect.Min.y }, { right, bar->BarRect.Max.y }, false);
        // ### 后缀只负责稳定身份，不能把内部相机 ID 当作可见标题滚出来。
        const auto* end = ImGui::FindRenderedTextEnd(window->Name);
        // 共享曲线包含两端停留；短标题保持静止，不依赖鼠标悬浮才展示信息。
        Utils::drawScrollingTextOnDrawList(
            draw,
            std::string_view(window->Name, end - window->Name),
            { x + bar->FramePadding.x, bar->BarRect.Min.y },
            std::max(0.0f, tab->Width - 2 * bar->FramePadding.x),
            bar->BarRect.GetHeight(),
            caption.m_color,
            true);
        // 独立宿主的裁剪栈到此恢复，后续分隔握柄和弹层不继承标签矩形。
        draw.PopClipRect();
    }
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
/// 分区宽度由原生分隔条连续调整，不允许拖出；关闭使用标签长按菜单。
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
    // 触屏释放后仍保留最后鼠标坐标，但并不存在真实悬浮，不能持续点亮整条缝隙。
    // 仅抑制无按下悬浮色；实际拖动仍沿用主题 SeparatorActive 反馈。
    ImGui::PushStyleColor(ImGuiCol_SeparatorHovered,
                          style.Colors[ImGuiCol_WindowBg]);
    prepareTabCaptions(scale);
    ImGui::DockSpace(m_root, {}, flags);
    drawTabCaptions();
    ImGui::PopStyleColor();
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
