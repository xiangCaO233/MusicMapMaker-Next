#include "ui/workspace/MobileTouchScroll.h"
#include <algorithm>
#include <imgui_internal.h>

namespace MMM::UI
{
/// @brief 仅释放自己持有的交互 ID，弹层接管时不清除它的输入状态。
/// @details 取消不回滚已经显示的滚动位置，也不制造一次释放点击。
/// @warning 只访问当前 ImGui 上下文，禁止从平台回调或工作线程调用。
void MobileTouchScroll::cancel()
{
    // 释放保护由调用方决定，取消捕获时不能顺带让最终释放落到背景。
    // 只有自己的 ID 可以被清除；其他控件接管正是本次取消的原因之一。
    if ( m_captured && ImGui::GetActiveID() == m_activeId )
        ImGui::ClearActiveID();
    m_captured      = false;
    m_dragging      = false;
    m_pressedWindow = m_xWindow = m_yWindow = m_activeId = 0;
    // 原点无需清空：所有目标 ID 已归零，下一次捕获会重新采样原点。
}

/// @brief 采样触摸并在窗口布局前设置滚动，避免拖动时内容落后一轮更新。
/// @param scale 外壳提供的内容倍率，不使用帧缓冲比例代替触摸密度。
/// @details 鼠标与笔不走本路径，移动设备外接鼠标仍使用原有控件行为。
/// 目标在按下时固定，经过另一个窗口或拖出边界后也不会中途换目标。
/// @warning 每帧只做窗口 ID 查询、常量级数学和滚动目标写入。
void MobileTouchScroll::beginFrame(float scale)
{
    const auto& io = ImGui::GetIO();
    m_releaseGuard = false;
    // 保护仅覆盖释放当帧，下一次采样必须允许独立的编辑手势。
    if ( io.MouseSource != ImGuiMouseSource_TouchScreen ) {
        cancel();
        return;
    }
    if ( ImGui::IsMouseClicked(0) ) {
        // 新按下总是结束旧候选，不能把两个触点之间的距离当成滚动量。
        cancel();
        auto* window = ImGui::GetCurrentContext()->HoveredWindow;
        // 标题、边框和滚动条不属于空白内容；控件是否占用要等本帧提交后确认。
        if ( window && window->InnerClipRect.Contains(io.MousePos) ) {
            // InnerClipRect 排除窗口边框，停靠分隔条继续由原生布局处理。
            // 保存 ID 而非指针，窗口关闭不会产生悬空借用。
            m_pressedWindow = window->ID;
            m_origin        = io.MousePos;
        }
        return;
    }
    if ( !m_captured ) return;
    // 真正捕获必须等控件完成输入判断，按下第一帧只保存候选。
    // 触点取消或其他控件接管后立即结束，不能等待一个永远不来的释放沿。
    if ( ImGui::GetActiveID() != m_activeId ||
         (!ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0)) ) {
        m_releaseGuard = m_dragging;
        cancel();
        return;
    }
    ImGui::KeepAliveID(m_activeId);
    // 独立 ID 没有可见 Item，必须显式延长到下一帧，持续拖动才不会失焦。
    const ImVec2 delta{ io.MousePos.x - m_origin.x,
                        io.MousePos.y - m_origin.y };
    // 阈值以字体对应的内容倍率估计，普通轻点抖动不改变滚动位置。
    const float slop = 8.0f * std::max(.5f, scale);
    m_dragging =
        m_dragging || delta.x * delta.x + delta.y * delta.y > slop * slop;
    if ( m_dragging ) {
        // 范围随本帧内容夹取，资源列表变短时也不会滚到内容之外。
        // 不通过滚轮合成事件，避免离散行数和浮点像素移动互相转换。
        // 初始滚动量加绝对位移，不累计帧差，事件合并不会改变移动距离。
        // 每轴选择最近可滚动祖先，横向表格与纵向父窗口可以分别接收位移。
        // ID 每帧重新解析，窗口销毁不会留下悬空观察指针。
        if ( auto* x = ImGui::FindWindowByID(m_xWindow);
             x && (x->WasActive || x->Active) )
            ImGui::SetScrollX(
                x,
                std::clamp(m_scrollOrigin.x - delta.x, 0.0f, x->ScrollMax.x));
        if ( auto* y = ImGui::FindWindowByID(m_yWindow);
             y && (y->WasActive || y->Active) )
            ImGui::SetScrollY(
                y,
                std::clamp(m_scrollOrigin.y - delta.y, 0.0f, y->ScrollMax.y));
    }
    if ( ImGui::IsMouseReleased(0) ) {
        // 松手当帧提交最终位置，同时阻挡同一次释放落到背景画布。
        m_releaseGuard = m_dragging;
        cancel();
    }
}

/// @brief 在控件取得 ActiveID 后判断本次按下是否确实发生在空白处。
/// @details 不覆盖任何按钮、滑条、文本框、滚动条或原生停靠分隔条。
/// 捕获后用独立 ActiveID 阻止 ImGui 将弹层空白按下解释为移动整个窗口。
/// 只确认一次起点，手指移动到按钮上不重新触发该按钮。
/// 窗口 ID 的生命周期由 ImGui 管理，本组件不拥有任何窗口资源。
/// @warning 父链深度有固定上限，不分配临时窗口列表，也不扫描全局窗口。
void MobileTouchScroll::endFrame()
{
    // 当前帧控件均已提交，此时才能区分空白、滑条和文本输入框。
    if ( m_captured ) {
        // 窗口实际 Begin 后 Active 才可靠，这里检查本帧最终存活状态。
        ImGui::KeepAliveID(m_activeId);
        // 窗口关闭时提前释放，避免下一帧沿用失效的层级关系。
        const auto* hit = ImGui::FindWindowByID(m_pressedWindow);
        if ( !hit || !hit->Active ) cancel();
        return;
    }
    if ( !m_pressedWindow ) return;
    auto* hit = ImGui::FindWindowByID(m_pressedWindow);
    // HoveredID 和 ActiveID 来自本帧真实控件，不能只依赖上一帧悬浮。
    // 只在按下帧确认一次；触摸移到另一控件上不能重新建立捕获。
    if ( !ImGui::IsMouseClicked(0) || !ImGui::IsMouseDown(0) || !hit ||
         !hit->Active || ImGui::GetActiveID() != 0 ||
         ImGui::IsAnyItemHovered() ) {
        cancel();
        return;
    }
    const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
    if ( !popups.empty() ) {
        // 弹层外的触摸留给 ImGui 恢复焦点并收起，不能建立背景滚动的 ActiveID。
        // 再点入口也属于此情况；同一次触摸仍由弹层阻挡，不激活入口或背景控件。
        // 只检查最上层，父弹层空白不会抢走关闭子弹层的点击。
        auto* popup = popups.back().Window;
        if ( !popup || !ImGui::IsWindowWithinBeginStackOf(hit, popup) ) {
            cancel();
            return;
        }
        // 弹层自己的空白与滚动子窗口继续使用原有拖动路径。
        // 尚未 Begin 的弹层没有有效窗口，不能借用上一帧的背景身份。
    }
    auto* window = hit;
    // 两轴可由不同层级承载，未找到某一轴时才继续向祖先查找。
    // 已找到的最近目标不被更外层的窗口覆盖，避免内容突然跳动。
    for ( int depth = 0; window && depth < 32; ++depth ) {
        // 显式禁用滚轮且隐藏滚动条的画布不由标准滚动接管，保留绘制手势。
        const bool customCanvas =
            (window->Flags & ImGuiWindowFlags_NoScrollWithMouse) &&
            (window->Flags & ImGuiWindowFlags_NoScrollbar);
        if ( !customCanvas ) {
            if ( !m_xWindow && window->ScrollMax.x > 0 ) m_xWindow = window->ID;
            if ( !m_yWindow && window->ScrollMax.y > 0 ) m_yWindow = window->ID;
        }
        // Popup 的 ParentWindow 是打开它的窗口，不是可以继续滚动的内容祖先。
        if ( !(window->Flags & ImGuiWindowFlags_ChildWindow) ||
             (window->Flags & ImGuiWindowFlags_Popup) )
            break;
        window = window->ParentWindow;
    }
    if ( !m_xWindow && !m_yWindow ) {
        // 不能滚动的空白继续归原视图，例如画布绘制或抓取。
        cancel();
        return;
    }
    if ( const auto* x = ImGui::FindWindowByID(m_xWindow) )
        m_scrollOrigin.x = x->Scroll.x;
    if ( const auto* y = ImGui::FindWindowByID(m_yWindow) )
        m_scrollOrigin.y = y->Scroll.y;
    // 交互身份与最初命中窗口绑定，不依赖本地化标题或绘制顺序。
    m_activeId = ImHashStr("##MobileBlankScroll", 0, hit->ID);
    // 原点与 ID 只在捕获时建立一次；标题本地化不参与跨帧归属判断。
    m_captured = true;
    ImGui::SetActiveID(m_activeId, hit);
    // 捕获后阻止 EndFrame 将弹层空白按下解释为移动整个弹层。
    ImGui::KeepAliveID(m_activeId);
}
}  // namespace MMM::UI
