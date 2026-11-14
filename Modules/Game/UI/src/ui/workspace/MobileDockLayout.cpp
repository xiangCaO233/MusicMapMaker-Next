#include "ui/workspace/MobileDockLayout.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <imgui_internal.h>
#include <string_view>
#include <utility>

namespace MMM::UI
{
/// @brief 波形和频谱归入主区标签，避免移动端出现无法操作的桌面浮窗。
/// @details 后缀与业务视图的 ### 身份一致，语言切换不改变其归属。
/// @warning 每帧常量级文本比较，不拥有窗口或业务会话。
bool MobileDockLayout::isAudioAnalysisWindow(std::string_view title)
{
    // 只接管明确支持的分析页，项目向导等独立弹窗继续由原有路径管理。
    // BPM 测量与音频图表同属中央页签，不能成为被主画布盖住的普通浮窗。
    // 固定后缀避免语言切换时创建第二个工具或误接管自动测量确认弹窗。
    return title.ends_with("###AudioWaveformViewGlobal") ||
           title.ends_with("###AudioSpectrumViewGlobal") ||
           title.ends_with("###BpmMeasurementTool");
}

/// @brief 从独立移动配置恢复首次分割参数，下一次提交按当前屏幕建树。
/// @details 活动标签待匹配窗口出现时消费，不持有窗口或节点指针。
void MobileDockLayout::restore(float timingRatio, float previewRatio,
                               ImGuiID selectedTab, float workbenchRatio)
{
    m_timingRatio    = timingRatio;
    m_previewRatio   = previewRatio;
    m_workbenchRatio = workbenchRatio;
    m_restoreFocus   = selectedTab;
    m_startupFocus   = selectedTab;
    m_visibility     = -1;
}

/// @brief 冻结用户实际调整结果，不能用最初默认比例冒充当前停靠宽度。
/// @details 隐藏叶节点没有几何，继续使用隐藏前缓存的比例。
/// 此接口只保存兼容旧版配置的缺省比例和选中身份，不用比例重建用户拓扑。
/// 窗口合并、上下分割和浮动位置继续由 ImGui 原生 ini 负责序列化。
/// 桌面项目工作区与移动外壳的配置入口保持分离，不在此生成项目 ini。
/// @warning 交互提交时只访问三叶一根，不遍历整个 ImGui 窗口集合。
void MobileDockLayout::capture(float& timingRatio, float& previewRatio,
                               ImGuiID& selectedTab,
                               float*   workbenchRatio) const
{
    timingRatio  = m_timingRatio;
    previewRatio = m_previewRatio;
    // 可选输出兼容旧调用，生产外壳同时保存工作台独立宽度。
    if ( workbenchRatio ) *workbenchRatio = m_workbenchRatio;
    // 尚未显示的树不能生成有效快照，保留调用方先前保存的标签身份。
    const auto* root = ImGui::DockBuilderGetNode(m_root);
    if ( !root || root->Size.x <= 0 ) return;
    if ( const auto* leaf = ImGui::DockBuilderGetNode(m_timing) )
        timingRatio = leaf->Size.x / root->Size.x;
    if ( const auto* leaf = ImGui::DockBuilderGetNode(m_preview) )
        previewRatio = leaf->Size.x / root->Size.x;
    if ( const auto* leaf = ImGui::DockBuilderGetNode(m_workbench);
         leaf && workbenchRatio )
        *workbenchRatio = leaf->Size.x / root->Size.x;
    if ( const auto* leaf = ImGui::DockBuilderGetNode(m_center);
         leaf && leaf->VisibleWindow )
        // 保存窗口 ID；原生 TabId 是从窗口 ID 派生的另一身份，不能用于 Begin
        // 匹配。
        // 工作台不是文档身份：保存其关联文档，避免启动时等待尚未创建的工具页。
        // 原生 ini 继续保存工具标签自身的排序和停靠位置。
        selectedTab = std::string_view(leaf->VisibleWindow->Name)
                                  .ends_with("###MobileWorkbench") &&
                              m_lastMainSelection
                          ? m_lastMainSelection
                          : leaf->VisibleWindow->ID;
}

/// @brief 在所有业务窗口提交后处理启动焦点，兼容惰性窗口的连续聚焦请求。
/// @details 用户第一次按下立即结束恢复保护，后续完全交还普通导航规则。
/// 弹层始终更优先，不能为了恢复中心标签夺走确认或文件选择的输入。
/// @warning 每帧只查找一个固定窗口 ID，无遍历、资源创建或同步等待。
void MobileDockLayout::finishFrame()
{
    // 先结束输入所有权再处理启动焦点，原生拖动和菜单的收尾不能互相跳过。
    // 移动窗口的观察状态不保存到配置，只有原生停靠结果参与布局持久化。
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
    if ( !ImGui::IsMouseDown(0) ) {
        m_holdWindow = 0;
        if ( m_dragWindow ) {
            m_dragWindow = 0;
            // 拖动释放后仍阻挡画布一帧，停靠落点不能变成绘制起笔。
            m_releaseGuard = true;
        }
    }
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
    // 浮动窗口使用自己的标题栏；仍支持长按关闭和继续拖回停靠区。
    // 停靠首帧尚未建立标签时不能借标题矩形启动错误的手势。
    if ( !tab && window->DockNode ) return false;
    // 跟随原生标签排序与滚动，不按标题文本宽度另建一份命中几何。
    // 当前预编译 ImGui 尚未导出 TabBarGetTabPos，按官方标签布局公式换算。
    ImRect rect = window->TitleBarRect();
    if ( tab ) {
        const float  scroll = tab->Flags & ImGuiTabItemFlags_SectionMask_
                                  ? 0.0f
                                  : bar->ScrollingAnim;
        const ImVec2 position{ bar->BarRect.Min.x +
                                   std::trunc(tab->Offset - scroll),
                               bar->BarRect.Min.y };
        rect =
            ImRect(position, { position.x + tab->Width, bar->BarRect.Max.y });
        rect.ClipWith(bar->BarRect);
        // 原生中间标签随滚动裁剪，前后固定标签不扣除滚动区边缘。
        if ( !(tab->Flags & ImGuiTabItemFlags_SectionMask_) ) {
            rect.Min.x = std::max(rect.Min.x, bar->ScrollingRectMinX);
            rect.Max.x = std::min(rect.Max.x, bar->ScrollingRectMaxX);
        }
    }
    const auto& io        = ImGui::GetIO();
    const bool  popupOpen = ImGui::IsPopupOpen(
        nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    // 只从标签内的新按下开始；从内容滑入、菜单遮挡及鼠标来源均不启动。
    if ( enabled && !popupOpen &&
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
        if ( !enabled || popupOpen || !ImGui::IsMouseDown(0) )
            m_holdWindow = 0;
        else if ( dx * dx + dy * dy > tolerance * tolerance ) {
            // 拖动优先于长按菜单；最后一个主画布也可以拖动，仅关闭受保护。
            m_dragWindow = window->ID;
            m_holdWindow = 0;
        } else if ( canClose && rect.Contains(io.MousePos) &&
                    ImGui::GetTime() - m_holdStarted >= .55 ) {
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
/// @warning 每帧访问已登记 UI 窗口；颜色快照复用容量，只在新标签加入时增长。
void MobileDockLayout::prepareTabCaptions(float scale)
{
    // 只复用存储容量，不复用上一帧颜色快照或窗口名观察指针。
    // 显隐重建之后旧叶节点 ID 可失效，颜色恢复按窗口稳定 ID 完成。
    m_tabCaptions.clear();
    for ( const auto& binding : m_windows ) {
        auto* window = ImGui::FindWindowByID(binding.m_window);
        auto* node   = window ? window->DockNode : nullptr;
        if ( !node || !window->WasActive ) continue;
        // 每个窗口只登记一次；多个类别合并到同一叶后不能重复隐藏和恢复文字。
        const float maximum =
            std::min(240 * scale, std::max(64 * scale, node->Size.x * .5f));
        // 暂时隐藏原生文字，背景、选中态、排序和滚动箭头仍由原生绘制。
        // 不覆盖标签背景，也不遍历顶点擦除文字，保持主题与批处理顺序。
        // 原生滚动箭头不使用这个窗口缓存色，因此仍保留正常可见反馈。
        auto& color = window->DockStyle.Colors[ImGuiWindowDockStyleCol_Text];
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
    case MobileDockPanel::Timing: return (m_visibility & 1) ? m_timing : 0;
    case MobileDockPanel::Preview: return (m_visibility & 2) ? m_preview : 0;
    case MobileDockPanel::Workbench:
        return (m_visibility & 4) ? m_workbench : 0;
    default: return m_center;
    }
}

/// @brief 可见主画布才提供触摸编辑工作台，欢迎、设置和音频页不占此区域。
/// @details 使用原生叶节点的当前选中窗口，工具窗口抢焦点不影响主区标签身份。
/// @warning 每帧固定节点查询，不分配字符串或访问后台会话。
bool MobileDockLayout::isMainCanvasSelected() const
{
    const auto* node = ImGui::DockBuilderGetNode(m_center);
    // 重建首帧尚未结算原生可见页，先使用待恢复身份，避免显隐条件反向触发重建。
    const auto* window = m_startupFocus ? ImGui::FindWindowByID(m_startupFocus)
                         : node         ? node->VisibleWindow
                                        : nullptr;
    // 浮动文档也可成为当前编辑目标，不能仅从原默认中心叶判断内容。
    // 工具或时间线取得焦点时仍沿用文档标签，不把辅助窗口当作主文档。
    const auto* focused = ImGui::GetCurrentContext()->NavWindow;
    focused             = focused ? focused->RootWindow : nullptr;
    for ( const auto& binding : m_windows ) {
        if ( !focused || focused->ID != binding.m_window ) continue;
        if ( binding.m_panel == MobileDockPanel::Main ) window = focused;
        if ( binding.m_panel == MobileDockPanel::Workbench )
            window = ImGui::FindWindowByID(m_lastMainSelection);
    }
    // 工作台与文档合并到同一标签栏时，选中工具页不改变原文档上下文。
    // 独立欢迎或设置页真正成为主文档后，刷新阶段会更新该身份并收起工作台。
    if ( window &&
         std::string_view(window->Name).ends_with("###MobileWorkbench") )
        window = ImGui::FindWindowByID(m_lastMainSelection);
    if ( !window ) return false;
    const std::string_view name(window->Name);
    // 标题随谱名和语言变化，不能以可见文字判断编辑上下文。
    const auto separator = name.rfind("###");
    const auto id =
        separator == std::string_view::npos ? name : name.substr(separator + 3);
    // 显示标题可包含谱名和脏标记，只有稳定内部身份能够区分真正的编辑画布。
    return id.starts_with("Canvas_") || id == "Basic2DCanvas";
}

/// @brief
/// 在一个根停靠区内组合主区与可选的等高辅助叶节点，标签与内容边界由同一原生节点树计算。
/// @param position 已扣除系统安全区与外边距的宿主左上角。
/// @param size 宿主整体尺寸，工具栏由外壳在调用前另行扣除。
/// @param scale 内容倍率，分隔条与可见握柄使用同一触摸尺度。
/// @param timing 是否为时间线预留独立叶节点。
/// @param preview 是否为预览预留独立叶节点。
/// @details 不启用自动隐藏标签栏，单窗口叶节点也保留自己的标签。
/// 原生标签允许拖出、合并和上下左右分割；关闭仍使用标签长按菜单。
/// @warning 普通帧只读固定数量节点，不执行业务数据扫描或同步等待。
void MobileDockLayout::render(ImVec2 position, ImVec2 size, float scale,
                              bool timing, bool preview, bool workbench)
{
    // 与 ImGui 窗口的整数像素几何一致，避免非整数 UI
    // 倍率让重建和最终宿主差一像素。
    // 所有比例换算都基于这份实际尺寸，反复重启才不会累积取整误差。
    position = { std::floor(position.x), std::floor(position.y) };
    size     = { std::max(1.0f, std::floor(size.x)),
                 std::max(1.0f, std::floor(size.y)) };
    // 分隔条可见尺寸由调用方美化样式提供，不为触摸命中额外画粗。
    // 所有分区共享宿主顶部和底部，标签栏使用同一帧字号与 FramePadding。
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
    // 显隐组成三位键；单纯横竖屏切换保留原生树内的相对分割比例。
    // 所有 ID 都在宿主作用域取得，避免与业务视图的同名控件碰撞。
    // 此位表示实际占用空间，用户开关由外壳独立保存。
    const int visible =
        (timing ? 1 : 0) | (preview ? 2 : 0) | (workbench ? 4 : 0);
    const auto flags = ImGuiDockNodeFlags_NoWindowMenuButton |
                       ImGuiDockNodeFlags_NoCloseButton;
    if ( !ImGui::DockBuilderGetNode(m_root) ) {
        // 没有已保存的移动根时才创建默认布局，不能因显隐或旋转销毁用户树。
        ImGui::DockBuilderAddNode(m_root, ImGuiDockNodeFlags_DockSpace | flags);
        ImGui::DockBuilderSetNodePos(m_root, position);
        ImGui::DockBuilderSetNodeSize(m_root, size);
        m_center = m_root;
        // 重建后先清除隐藏节点，不能将旧 ID 发布给后续业务 Begin。
        // 左侧预览和右侧时间线、工作台分别切出，剩余叶始终属于主区。
        m_timing = m_preview = m_workbench = 0;
        // 保存值相对于根宽度；原生 split 的比例却相对于扣除缝隙后的父宽度。
        // 先恢复目标像素再换算，不能在每次重启时重复扣除一次缝隙。
        const auto splitSide =
            [&](float ratio, ImGuiDir direction, ImGuiID& leaf) {
                const auto* parent = ImGui::DockBuilderGetNode(m_center);
                const float available =
                    std::max(1.0f, parent->Size.x - style.DockingSeparatorSize);
                // 左叶直接截断、右叶取余数，两侧偏置相反以恢复同一个整数目标宽度。
                // 像素取整只发生在重建，不影响原生持续拖动的即时几何反馈。
                const float desired = std::round(size.x * ratio);
                ImGui::DockBuilderSplitNode(
                    m_center,
                    direction,
                    std::clamp(
                        (desired + (direction == ImGuiDir_Left ? .5f : -.5f)) /
                            available,
                        .01f,
                        .99f),
                    &leaf,
                    &m_center);
            };
        // 从两侧向内切出辅助区，最终为预览、主区、时间线、工作台。
        // 恢复仍沿用各分区自身宽度比例，不把旧版保存的左右位置当作新布局依据。
        // 最右工作台先切出，随后时间线仅使用剩余空间，不会越过工作台。
        if ( workbench )
            splitSide(m_workbenchRatio, ImGuiDir_Right, m_workbench);
        if ( preview ) splitSide(m_previewRatio, ImGuiDir_Left, m_preview);
        if ( timing ) splitSide(m_timingRatio, ImGuiDir_Right, m_timing);
        ImGui::DockBuilderFinish(m_root);
        // 完成整棵树后再记录组合，下一帧即可走不重建的稳定路径。
        // 业务窗口稍后依次 Begin，并从当前布局获取各自所属叶节点。
        m_visibility = visible;
    }
    // 恢复顺序先让默认树存在，再解析保存的窗口归属，最后补缺失的辅助叶。
    // 不能把窗口尚未 Begin 的启动阶段误判为用户删除了全部停靠节点。
    // 已保存树由 ImGui 启动加载；恢复窗口身份后沿用真实节点，不消费桌面根。
    // 显隐切换只改变业务窗口是否提交，原生隐藏叶会释放占用几何。
    refreshNodes();
    if ( workbench )
        ensurePanel(MobileDockPanel::Workbench, m_workbenchRatio, false);
    if ( preview ) ensurePanel(MobileDockPanel::Preview, m_previewRatio, true);
    if ( timing ) ensurePanel(MobileDockPanel::Timing, m_timingRatio, false);
    // 可见性只决定当前业务窗口是否提交，缓存节点仍供下次打开复用。
    // 关闭工作台后不能清除用户合并到其它标签栏的原生设置。
    m_visibility = visible;
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
    // 用户可以创建横向或纵向分割；握柄从实际父子几何推导，不能再按面板类别猜左右。
    // 同一个父节点只绘制一次，合并了多个类别的标签不会生成重复握柄。
    // 绘制不创建额外 InvisibleButton，连续拖动始终由同一个原生分割器拥有。
    // 触摸透明命中区与可见握柄分离，不能因分割方向变化额外加粗主题缝隙。
    std::array<ImGuiDockNode*, 128> dividers{};
    std::size_t                     dividerCount = 0;
    if ( auto* root = ImGui::DockBuilderGetNode(m_root) )
        dividers[dividerCount++] = root;
    while ( dividerCount ) {
        const auto* split  = dividers[--dividerCount];
        const auto* first  = split->ChildNodes[0];
        const auto* second = split->ChildNodes[1];
        if ( !first || !second ) continue;
        for ( auto* child : split->ChildNodes )
            if ( child && dividerCount < dividers.size() )
                dividers[dividerCount++] = child;
        // 隐藏子树不占几何，不在原位置画一条悬空的假分隔握柄。
        // 只有两侧都可见时，原生分割器才提供这一条实际可拖动边界。
        if ( !first->IsVisible || !second->IsVisible ) continue;
        const auto* root = ImGui::DockBuilderGetNode(m_root);
        auto* draw = root && root->HostWindow ? root->HostWindow->DrawList
                                              : ImGui::GetWindowDrawList();
        const float half = std::max(.5f, style.DockingSeparatorSize * .15f);
        if ( split->SplitAxis == ImGuiAxis_X ) {
            const float x =
                (first->Pos.x + first->Size.x + second->Pos.x) * .5f;
            const float y = split->Pos.y + split->Size.y * .5f;
            draw->AddRectFilled({ x - half, y - 24 * scale },
                                { x + half, y + 24 * scale },
                                ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                style.GrabRounding);
        } else {
            // 上下分割使用横向握柄；粗细、颜色和圆角与竖向握柄共用主题。
            const float x = split->Pos.x + split->Size.x * .5f;
            const float y =
                (first->Pos.y + first->Size.y + second->Pos.y) * .5f;
            draw->AddRectFilled({ x - 24 * scale, y - half },
                                { x + 24 * scale, y + half },
                                ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                style.GrabRounding);
        }
    }
    // 原生窗口的拖动和停靠标志在这里只作用于移动根，不改全局 Docking 配置。
    // 业务浮层和系统选择器仍遵循各自的模态生命周期，不参与文档重排。
    // 所有标签栏交由同一个停靠宿主计算，不再独立拼接无标题浮窗。
    // 与宿主窗口和样式推入顺序配对，不能将零内边距泄漏给业务视图。
    ImGui::End();
    ImGui::PopStyleVar(2);
}

/// @brief 按稳定窗口身份恢复锚点，并清理旧版禁止拖动的原生标志。
/// @details 节点归 ImGui 所有；只保存 ID，拆分和合并后重新查询。
/// 隐藏窗口仍保留其原生位置；只有节点真正失效时才补新的缺省落点。
/// 首帧可从设置读取身份，后续运行期窗口的实际 DockId 优先。
/// 原生焦点仅用于选择文档上下文，不以工具窗口焦点覆盖文档类型。
/// @warning 每帧只访问 UI 登记和有限停靠树，不读取会话或持有互斥锁。
void MobileDockLayout::refreshNodes()
{
    const auto resolve = [](ImGuiID id) -> ImGuiID {
        const auto*   window = ImGui::FindWindowByID(id);
        const auto*   saved  = ImGui::FindWindowSettingsByID(id);
        const ImGuiID dock   = window  ? window->DockId
                               : saved ? saved->DockId
                                       : 0;
        return ImGui::DockBuilderGetNode(dock) ? dock : 0;
    };
    // 已有业务窗口更新自己的锚点，合并到别的分区后不能继续使用旧叶。
    // 首帧尚无运行期登记时，从稳定辅助窗口身份取得原生保存的位置。
    // 没有保存位置时保留刚创建的默认叶，不能每帧重新分割。
    for ( auto [slot, id] :
          { std::pair{ &m_timing, ImHashStr("###Timeline") },
            std::pair{ &m_preview, ImHashStr("###PreviewWindow") },
            std::pair{ &m_workbench, ImHashStr("###MobileWorkbench") } } ) {
        if ( const auto saved = resolve(id) )
            *slot = saved;
        else if ( !ImGui::DockBuilderGetNode(*slot) )
            *slot = 0;
    }
    auto* root = ImGui::DockBuilderGetNode(m_root);
    if ( !root ) return;
    const auto* main = ImGui::DockBuilderGetNode(m_center);
    if ( !main || main->IsSplitNode() ) {
        // 中央节点缺失时取当前树的普通叶，后续主区 Begin 再按可见标签细化。
        // 首次恢复优先使用原生中央叶，左侧预览叶不能冒充文档默认目标。
        auto* leaf = ImGui::DockBuilderGetCentralNode(m_root);
        if ( !leaf ) leaf = root;
        while ( leaf->IsSplitNode() )
            leaf =
                leaf->ChildNodes[0] ? leaf->ChildNodes[0] : leaf->ChildNodes[1];
        m_center = leaf->ID;
    }
    for ( const auto& binding : m_windows ) {
        const auto* window = ImGui::FindWindowByID(binding.m_window);
        if ( !window || !window->DockNode ) continue;
        switch ( binding.m_panel ) {
        case MobileDockPanel::Timing: m_timing = window->DockId; break;
        case MobileDockPanel::Preview: m_preview = window->DockId; break;
        case MobileDockPanel::Workbench: m_workbench = window->DockId; break;
        case MobileDockPanel::Main:
            // 多个文档分开可见时不能按遍历顺序覆盖最近编辑目标。
            // 只在还没有文档上下文时用可见页初始化，之后以实际聚焦为准。
            if ( !m_lastMainSelection &&
                 window->DockNode->VisibleWindow == window )
                m_lastMainSelection = window->ID;
            if ( m_lastMainSelection == window->ID ) m_center = window->DockId;
            break;
        }
    }
    // 多个文档分开显示时，明确聚焦的那个优先于登记遍历顺序。
    // 浮动窗口没有 DockNode，但仍需保留其文档身份供工作台继续使用。
    const auto* focused = ImGui::GetCurrentContext()->NavWindow;
    focused             = focused ? focused->RootWindow : nullptr;
    for ( const auto& binding : m_windows )
        if ( focused && binding.m_panel == MobileDockPanel::Main &&
             focused->ID == binding.m_window ) {
            m_lastMainSelection = focused->ID;
            if ( focused->DockNode ) m_center = focused->DockId;
        }
    // 清除旧版本根和叶保存的约束，工作台也可与文档合并或重新分割。
    // 栈只存节点观察指针且只在本次同步调用内有效，不建立新的节点所有权。
    std::array<ImGuiDockNode*, 128> pending{};
    std::size_t                     count = 0;
    pending[count++]                      = root;
    while ( count ) {
        auto* node = pending[--count];
        node->LocalFlags &=
            ~(static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoDocking) |
              ImGuiDockNodeFlags_NoUndocking);
        node->SharedFlags &=
            ~(static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoDocking) |
              ImGuiDockNodeFlags_NoUndocking);
        node->LocalFlags |= ImGuiDockNodeFlags_NoWindowMenuButton |
                            ImGuiDockNodeFlags_NoCloseButton;
        // 旧版本工作台隐藏了标签；迁移后所有停靠窗口统一显示原生标签。
        node->LocalFlags &=
            ~(ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_HiddenTabBar);
        for ( auto* child : node->ChildNodes )
            if ( child && count < pending.size() ) pending[count++] = child;
    }
}

/// @brief 新面板没有原生保存位置时，只在当前主叶旁增量创建默认落点。
/// @param panel 待显示的辅助面板；已合并或拆分的用户位置优先。
/// @param ratio 旧配置中的根宽度比例，仅用于首次位置估算。
/// @param left 预览默认靠左，时间线与工作台默认靠右。
/// @warning 只有首次打开或原生节点已失效时调用分割，不执行整树重建。
void MobileDockLayout::ensurePanel(MobileDockPanel panel, float ratio,
                                   bool left)
{
    ImGuiID& slot = panel == MobileDockPanel::Timing    ? m_timing
                    : panel == MobileDockPanel::Preview ? m_preview
                                                        : m_workbench;
    // 同一节点可同时承载主画布与辅助标签，这是用户合并后的合法状态。
    // 不再要求各类别占用独立 ID，也不为了恢复默认顺序拆散它们。
    if ( ImGui::DockBuilderGetNode(slot) ) return;
    const auto*   root = ImGui::DockBuilderGetNode(m_root);
    const ImGuiID source =
        panel == MobileDockPanel::Workbench ? m_root : m_center;
    // 工作台首次出现时仍默认放在宿主最右侧，之后移动位置完全由用户决定。
    // 时间线和预览首次出现则在当前主叶附近补充，保留其它自定义区域。
    const auto* main = ImGui::DockBuilderGetNode(source);
    if ( !main || !root ||
         (panel != MobileDockPanel::Workbench && main->IsSplitNode()) )
        return;
    // 根宽度换算为当前叶的比例，避免已调整主区被再次按默认比例均分。
    const float part = std::clamp(
        root->Size.x * ratio / std::max(1.0f, main->Size.x), .05f, .65f);
    ImGui::DockBuilderSplitNode(
        source, left ? ImGuiDir_Left : ImGuiDir_Right, part, &slot, &m_center);
    ImGui::DockBuilderFinish(m_root);
}

/// @brief 将共享业务窗口放入受控分区，窗口内容仍由原视图绘制。
/// @details 仅在重建后的匹配窗口恢复一次焦点，不逐帧抢走文本输入。
/// @param panel 本次业务窗口应进入的受控叶节点。
/// @param title 包含固定内部 ID 的标题，用于匹配先前活动标签。
/// @warning 每个窗口 Begin 前调用一次，只查询已有 UI 身份与登记。
/// @details 标题显示部分允许变化，ImHashStr 仍以 ### 后缀生成稳定身份。
/// 登记对象不拥有视图，因此工具销毁不会被布局缓存延迟。
/// 返回时不能保留原生窗口观察指针，下一帧拆分可能更换节点。
void MobileDockLayout::target(MobileDockPanel panel, const char* title)
{
    const auto id    = ImHashStr(title);
    auto       found = std::find_if(
        m_windows.begin(), m_windows.end(), [id](const WindowBinding& item) {
            return item.m_window == id;
        });
    // 业务视图可能先于移动宿主首帧提交，不能把“根尚未就绪”当作已完成默认停靠。
    // 暂不登记窗口，让下一次拥有有效根时仍能提供首次缺省位置。
    const bool ready = ImGui::DockBuilderGetNode(m_root) != nullptr;
    const bool first = found == m_windows.end();
    // 注册增长只在新 UI 窗口首次出现时发生，普通绘制复用已分配容量。
    // 关闭再打开同一稳定身份也不新建记录，标题变化不能生成重复绑定。
    if ( first && ready ) m_windows.push_back({ id, panel });
    const auto* window = ImGui::FindWindowByID(id);
    const auto* saved  = ImGui::FindWindowSettingsByID(id);
    const auto  dockId = window && window->LastFrameActive >= 0 ? window->DockId
                         : saved                                ? saved->DockId
                                                                : 0;
    const auto* existing = ImGui::DockBuilderGetNode(dockId);
    // 原生窗口已经存在时优先读取运行期
    // DockId，不能用旧磁盘位置盖掉刚完成的拖动。
    // 尚未创建的惰性窗口才读取设置条目，恢复期间无需扫描业务注册表。
    // 原生 ini 中 DockId 为零表示用户保存的浮动窗口，而非布局读取失败。
    // 只有非零节点失效或从未保存过的窗口才回到默认落点。
    // 有效的保存位置或已交给用户的浮动窗口不再被每帧拉回默认叶。
    // 首次打开且没有有效位置时才给出默认位置；工作台与文档遵循相同规则。
    // ImGuiCond_Always 仅用于这个一次性缺省落点，不作为每帧布局约束。
    // 后续原生排序、拖出、合并和分割都不再被外壳覆盖。
    // 登记不是 Begin 完成：适配链重复请求同一新窗口时仍需保留首次落点。
    // 只有实际绘制过的浮动窗口才代表用户选择，不能把尚未创建的窗口当作浮动。
    const bool awaitingFirstBegin = !window || window->LastFrameActive < 0;
    if ( ready && (first || awaitingFirstBegin) && !existing &&
         (!saved || dockId != 0) )
        ImGui::SetNextWindowDockID(node(panel), ImGuiCond_Always);
    else
        // 覆盖共享业务入口预设的 Always
        // 条件，但不改动已经恢复或拖动后的原生归属。 FirstUseEver
        // 在已有设置和已显示窗口上不会生效，浮动 DockId=0 同样得到保留。
        ImGui::SetNextWindowDockID(dockId, ImGuiCond_FirstUseEver);
    if ( m_restoreFocus && ImHashStr(title) == m_restoreFocus ) {
        // 标题使用 ### 时哈希仍取固定身份，语言变化不会丢失焦点目标。
        // 只消费一次恢复请求，之后由用户点击决定当前标签。
        ImGui::SetNextWindowFocus();
        m_restoreFocus = 0;
    }
}
}  // namespace MMM::UI
