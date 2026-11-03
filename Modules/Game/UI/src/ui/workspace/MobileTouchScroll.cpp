#include "ui/workspace/MobileTouchScroll.h"
#include <algorithm>
#include <cmath>
#include <imgui_internal.h>
#include <utility>

namespace MMM::UI
{
/// @brief 释放仲裁状态，不清除已交付给业务组件的 ActiveID。
/// @details 最终释放保护独立保留，不能因清理候选而泄漏同一次触摸。
/// 点击补交状态也由调用方推进：清理滚动候选不能撤销已交付的按钮按下。
/// 输入快照必须活到 endFrame，因此这里不重置快照或过滤标记。
/// 数值拖动接管后的 ActiveID 属于原生组件，不能由本组件代为清除。
/// @warning UI 帧内低成本状态转换，不执行焦点请求、窗口销毁或资源访问。
void MobileTouchScroll::cancel()
{
    if ( m_activeId && ImGui::GetActiveID() == m_activeId )
        ImGui::ClearActiveID();
    // 触点是否还要拦截由释放保护负责，不能让身份清理隐含一次点击提交。
    // 所有目标同时失效，下一次按下必须重新解析窗口父链与滚动起点。
    m_captured = m_dragging = m_widgetDrag = m_cancelled = false;
    m_pressedWindow = m_xWindow = m_yWindow = m_activeId = m_item = 0;
    m_horizontal = m_vertical = false;
}

/// @brief 在 UI 提交期间统一过滤左键及其命名键别名，不能只屏蔽按钮 API。
/// @details 同时覆盖按下即触发的下拉框、折叠标题及释放触发的普通按钮。
/// 原始状态在 endFrame 恢复，下一次 NewFrame 仍从真实物理输入计算边沿。
/// @param down 业务组件本帧可见的按住状态，不表示改变平台设备状态。
/// @param clicked 是否补交已确认的轻点或水平拖动起点。
/// @param released 是否完成上一帧补交点击的原生配对释放。
/// @param count 原始触点的连点计数，避免文件双击打开退化成单击。
/// @warning 每次过滤只改左键标量，不改事件队列、不分配，也不合成离散滚轮事件。
void MobileTouchScroll::filter(bool down, bool clicked, bool released,
                               ImU16 count)
{
    auto& io                 = ImGui::GetIO();
    m_filtered               = true;
    io.MouseDown[0]          = down;
    io.MouseClicked[0]       = clicked;
    io.MouseReleased[0]      = released;
    io.MouseClickedCount[0]  = clicked ? count : 0;
    io.MouseDoubleClicked[0] = clicked && count == 2;
    // 原生 IsMouseClicked 还检查按住时长；只修改 MouseClicked 会漏掉下拉入口。
    // 待定手势用负时长屏蔽重复触发，已确认按下用零时长产生原生起始沿。
    // 释放采用上一帧零时长，使命名键别名同样产生一次有效释放。
    io.MouseDownDuration[0] = clicked ? 0.0f : down ? m_raw.m_duration : -1.0f;
    io.MouseDownDurationPrev[0] = released ? 0.0f : -1.0f;
    // 原生控件可能使用鼠标函数或命名键函数，两种入口必须一致。
    auto& key            = *ImGui::GetKeyData(ImGuiKey_MouseLeft);
    key.Down             = down;
    key.DownDuration     = io.MouseDownDuration[0];
    key.DownDurationPrev = io.MouseDownDurationPrev[0];
}

/// @brief 从原生内容窗口建立候选，不把父弹层入口误当作可滚动祖先。
/// @param origin 物理触点起始坐标，后续跨出窗口仍使用同一滚动目标。
/// @param count 该触点原始连点计数，不从延后的 UI 帧重新计时。
/// @param windowId 快速连点补交时使用已记录窗口；普通起手取本帧命中窗口。
/// @return 命中内容且至少有一个可滚动轴时返回 true。
/// 各轴独立选择最近祖先，表格横向滚动不能夺走父面板的纵向滚动。
/// 显示滚动条不是必要条件，只有实际 ScrollMax 决定内容能否滚动。
/// @warning 每次按下最多访问 32 层父链，不执行全局窗口遍历。
bool MobileTouchScroll::capture(ImVec2 origin, ImU16 count, ImGuiID windowId)
{
    auto& context = *ImGui::GetCurrentContext();
    auto* hit =
        windowId ? ImGui::FindWindowByID(windowId) : context.HoveredWindow;
    if ( !context.OpenPopupStack.empty() ) {
        auto* popup = context.OpenPopupStack.back().Window;
        if ( popup &&
             (!hit || !ImGui::IsWindowWithinBeginStackOf(hit, popup)) ) {
            // 收起与背景按下必须分开消费，否则再次点入口会在同帧重新打开。
            // 只收起最上层普通弹层；确认模态仍须由明确操作或返回关闭。
            m_pressedWindow = hit ? hit->ID : popup->ID;
            m_activeId = ImHashStr("##MobilePopupDismiss", 0, m_pressedWindow);
            m_captured = m_cancelled = true;
            if ( !(popup->Flags & ImGuiWindowFlags_Modal) )
                ImGui::ClosePopupToLevel(context.OpenPopupStack.Size - 1, true);
            return true;
        }
        if ( !popup ) return false;
    }
    // 原生滚动条、标题和停靠标签沿用自己的输入，不进入内容仲裁。
    if ( !hit || !hit->InnerClipRect.Contains(origin) ) return false;
    auto* window = hit;
    for ( int depth = 0; window && depth < 32;
          ++depth, window = window->ParentWindow ) {
        // 自定义编辑画布的拖动不属于标准滚动，也不委托给外层滚动窗口。
        if ( (window->Flags & ImGuiWindowFlags_NoScrollWithMouse) &&
             (window->Flags & ImGuiWindowFlags_NoScrollbar) )
            break;
        if ( !m_xWindow && window->ScrollMax.x > 0 ) m_xWindow = window->ID;
        if ( !m_yWindow && window->ScrollMax.y > 0 ) m_yWindow = window->ID;
        // Popup 的 ParentWindow 是打开它的视图，不是几何上的可滚动容器。
        // 在这里停止可避免拖动短菜单时带动背后的设置页。
        // 普通子窗口继续向上查找尚未找到的轴，但不会覆盖已选中的较近目标。
        if ( !(window->Flags & ImGuiWindowFlags_ChildWindow) ||
             (window->Flags & ImGuiWindowFlags_Popup) )
            break;
    }
    // 无滚动范围时不延后业务输入，例如工具栏普通高度下的播放按钮。
    // 也不为这种窗口保留一个空候选，后续移入邻窗不能凭空开始滚动。
    if ( !m_xWindow && !m_yWindow ) return false;
    // 每次只采样起点滚动量，拖动途中内容增减时仅夹取当前有效范围。
    // 身份从固定窗口 ID 派生，本地化标题和列表行排序都不会改变捕获归属。
    m_pressedWindow = hit->ID;
    m_activeId      = ImHashStr("##MobileContentScroll", 0, hit->ID);
    m_origin        = origin;
    m_clickCount    = count;
    if ( const auto* x = ImGui::FindWindowByID(m_xWindow) )
        m_scrollOrigin.x = x->Scroll.x;
    if ( const auto* y = ImGui::FindWindowByID(m_yWindow) )
        m_scrollOrigin.y = y->Scroll.y;
    m_captured = m_justPressed = true;
    return true;
}

/// @brief 先区分内容滚动、水平数值拖动和轻点，再向业务组件交付输入。
/// @details 不设置等待窗口；超过位移阈值立即滚动，抬手立即交付点击按下。
/// 普通按钮的配对释放在下一帧交付，沿用原生点击、双击和焦点逻辑。
/// @param scale 内容 DPI 倍率，仅用于手指位移容差，不改变字体或主题尺寸。
/// 过滤发生在 NewFrame 之后、业务视图之前；原生后端仍正常轮询所有触摸事件。
/// 不把整份 ImGuiIO 复制到堆上，快照仅保存本控制器会临时改动的字段。
/// 方向一旦确定便不反转归属，避免同一滑条手势中途变成内容滚动。
/// 系统取消只释放捕获，不把缺失的抬手事件猜测为用户点击。
/// @warning UI 热路径：只操作标量、窗口 ID 和原生滚动目标，无分配或阻塞。
void MobileTouchScroll::beginFrame(float scale)
{
    auto& io       = ImGui::GetIO();
    m_releaseGuard = m_justPressed = false;
    // 原始快照先于任何改写取得；平台下一帧的输入去重必须看到真实按住状态。
    // 鼠标兼容 API 与命名键 API 共享设备，但派生时长分别存储，必须同时保存。
    // 补交释放只临时借用旧坐标，帧末仍需归还当前物理位置与实际命中窗口。
    m_raw = { io.MouseDown[0],
              io.MouseClicked[0],
              io.MouseReleased[0],
              io.MouseDoubleClicked[0],
              io.MouseClickedCount[0],
              io.MouseDownDuration[0],
              io.MouseDownDurationPrev[0],
              *ImGui::GetKeyData(ImGuiKey_MouseLeft),
              io.MousePos,
              io.MouseDelta,
              ImGui::GetCurrentContext()->HoveredWindow
                  ? ImGui::GetCurrentContext()->HoveredWindow->ID
                  : 0 };
    // 桌面鼠标与笔保持原有路径，只有移动外壳主动调用本控制器。
    if ( io.MouseSource != ImGuiMouseSource_TouchScreen ) {
        cancel();
        m_tapRelease = m_deferredPress = false;
        return;
    }
    if ( std::exchange(m_tapRelease, false) ) {
        // 点击释放不能与下一物理按下合并，保留后者供下一轮候选消费。
        if ( m_raw.m_clicked ) {
            m_deferredPress  = true;
            m_deferredOrigin = io.MousePos;
            m_deferredCount  = m_raw.m_count;
            m_deferredWindow = m_raw.m_hoveredWindow;
        }
        // 后一触点可能已移到别处；前一点击仍在其原来抬手的位置完成释放。
        io.MousePos   = m_tapPosition;
        io.MouseDelta = {};
        ImGui::GetCurrentContext()->HoveredWindow =
            ImGui::FindWindowByID(m_tapWindow);
        filter(false, false, true);
        m_releaseGuard = true;
        return;
    }
    // 补交只按逻辑帧推进，没有定时等待；两次极快的触摸也保留各自的起点。
    // 新触点不能复用旧组件 ID，后一次业务命中由它自己的窗口布局决定。
    const bool deferred = std::exchange(m_deferredPress, false);
    if ( m_raw.m_clicked || deferred ) {
        cancel();
        if ( !capture(deferred ? m_deferredOrigin : io.MousePos,
                      deferred ? m_deferredCount : m_raw.m_count,
                      deferred ? m_deferredWindow : 0) )
            return;
    }
    if ( !m_captured && !m_widgetDrag ) return;
    // 水平组件已经收到唯一一次补交按下，后续直接使用真实事件而不重复注入。
    // 这样 Drag 控件的增量累积、编辑完成状态和保存时机继续由原生组件负责。
    if ( m_widgetDrag ) {
        // 已确定的水平拖动继续由原生组件处理，直到物理释放，不重新抢回滚动。
        if ( !m_raw.m_down ) {
            cancel();
            m_releaseGuard = true;
        }
        return;
    }
    const auto* hit = ImGui::FindWindowByID(m_pressedWindow);
    // 弹层消失、视图关闭或焦点丢失后，只吞掉余下触点，不补发一次点击。
    if ( !hit || (!hit->WasActive && !hit->Active) ) m_cancelled = true;
    if ( !ImGui::GetCurrentContext()->OpenPopupStack.empty() ) {
        auto* popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
        // 新模态接管后原背景只保留释放保护，不能继续滚动或补发点击。
        if ( !popup || !hit ||
             !ImGui::IsWindowWithinBeginStackOf(
                 ImGui::FindWindowByID(m_pressedWindow), popup) )
            m_cancelled = true;
    }
    const ImVec2 delta{ io.MousePos.x - m_origin.x,
                        io.MousePos.y - m_origin.y };
    // 允许轻点抖动，但超过阈值后即使回到原处也不再恢复成点击。
    // 原生最大位移保留之前采样的移动，避免反向滑回起点时误触发按钮。
    const float slop  = 8.0f * std::max(.5f, scale);
    const bool  moved = delta.x * delta.x + delta.y * delta.y > slop * slop ||
                        io.MouseDragMaxDistanceSqr[0] > slop * slop;
    if ( !m_cancelled && !m_dragging && moved ) {
        const bool widgetDirection =
            (m_horizontal && std::abs(delta.x) > std::abs(delta.y)) ||
            (m_vertical && std::abs(delta.y) > std::abs(delta.x));
        // 初始命中的控件决定主轴，移动途中不能改由滑过的其他控件接管。
        // EQ 纵向调值与横向浏览复用同一判定，不额外屏蔽父页的普通空白滚动。
        if ( widgetDirection && m_raw.m_down ) {
            const auto item = m_item;
            cancel();
            m_widgetDrag = true;
            // 方向确定后才允许改变值，纵向起手从未向滑条提交过按下。
            if ( item ) ImGui::SetKeyOwner(ImGuiKey_MouseLeft, item);
            filter(true, true, false, m_clickCount);
            return;
        }
        // 非数值组件的任何明显位移都取消点击，即使该方向已滚到边界。
        m_dragging = true;
    }
    if ( m_dragging && !m_cancelled ) {
        // 原点加绝对位移使合帧和反向拖动保持一致，不累计每帧浮点误差。
        if ( auto* x = ImGui::FindWindowByID(m_xWindow) )
            ImGui::SetScrollX(
                x,
                std::clamp(m_scrollOrigin.x - delta.x, 0.0f, x->ScrollMax.x));
        if ( auto* y = ImGui::FindWindowByID(m_yWindow) )
            ImGui::SetScrollY(
                y,
                std::clamp(m_scrollOrigin.y - delta.y, 0.0f, y->ScrollMax.y));
    }
    // 待定与滚动阶段都不向组件发送按下或释放，因此不需要回滚业务配置。
    // 在边界滑动也保持这份所有权，不能因没有更多可滚距离而突然触发按钮。
    filter(false, false, false);
    if ( m_raw.m_released ) {
        // 空白轻点没有业务目标，无须制造按钮事件；这同时避免多余的补交帧。
        // 极快连点可能已在补交期间抬手，其目标由即将提交的布局正常命中。
        const bool tap   = !m_dragging && !m_cancelled &&
                           (m_item != 0 || (deferred && m_justPressed));
        const auto item  = m_item;
        const auto count = m_clickCount;
        m_tapPosition    = io.MousePos;
        m_tapWindow      = m_pressedWindow;
        cancel();
        m_releaseGuard = true;
        if ( tap ) {
            // 只交付最初命中的组件，布局变化后不能激活同坐标的新按钮。
            // 极快连点的后一个目标还未提交
            // HoveredID，不继承前一个组件的键所有权。
            ImGui::SetKeyOwner(ImGuiKey_MouseLeft,
                               item ? item : ImGuiKeyOwner_NoOwner,
                               item ? ImGuiInputFlags_LockThisFrame : 0);
            filter(true, true, false, count);
            m_tapRelease = true;
        }
    } else if ( !m_raw.m_down ) {
        // 系统取消不等同于用户轻点，不能生成业务命令。
        cancel();
        m_releaseGuard = true;
    }
}

/// @brief 在候选按下帧记录真实数值组件的横向拖动能力。
/// @details 组合 Drag 控件可使用整体矩形；最终命中 ID 由本帧 HoveredID 提供。
/// 只在初始候选帧登记，滚动后经过另一滑条不能改变已经确定的手势归属。
/// 禁用组件不提供方向优先权，但仍可作为内容滚动的起手位置。
/// @warning 每个数值组件只进行常量矩形测试，不建立每帧组件索引或拥有型引用。
void MobileTouchScroll::registerHorizontalDragItem()
{
    registerDragItem(false);
}

/// @brief 竖滑条使用相同的延后按下和释放流程，仅交换主方向。
/// @warning 每个数值组件常量时间登记，不提前修改数值或触发业务命令。
void MobileTouchScroll::registerVerticalDragItem()
{
    registerDragItem(true);
}

/// @brief 在有效初始命中区登记方向，保证滚出裁剪区域的组件不能抢走手势。
/// @warning 只保存方向位，稳定 Item ID 仍由帧末统一捕获。
void MobileTouchScroll::registerDragItem(bool vertical)
{
    // 登记仅是一位方向语义，不改组件值，也不会让它提前收到按下。
    // 无可滚动祖先时直接为空操作，桌面和普通大小的参数弹层无需参与仲裁。
    if ( !m_justPressed || !m_captured ) return;
    const auto& context = *ImGui::GetCurrentContext();
    if ( context.CurrentWindow->ID != m_pressedWindow ||
         (context.CurrentItemFlags & ImGuiItemFlags_Disabled) )
        return;
    // 使用真实矩形与窗口裁剪，不让被裁掉的行或同坐标后台组件取得方向优先权。
    if ( context.LastItemData.Rect.Contains(m_origin) &&
         context.CurrentWindow->ClipRect.Contains(m_origin) ) {
        m_vertical   = vertical;
        m_horizontal = !vertical;
    }
}

/// @brief 冻结初始命中组件并恢复输入快照，原生后端的去重仍基于真实触摸。
/// @details 独立 ActiveID 阻止空白按下在帧末被解释为移动窗口。
/// 最初 HoveredID 在真实组件完成提交后取得，不根据文本宽度猜测点击对象。
/// 这里只恢复过滤字段，不恢复业务 ActiveID；原生组件必须完成自己的生命周期。
/// 关闭窗口后的身份重新解析为 nullptr，不跨帧保存可悬空的窗口指针。
/// @warning 每帧只查一个起点窗口，不保留业务窗口或组件指针。
void MobileTouchScroll::endFrame()
{
    if ( m_captured ) {
        auto* hit = ImGui::FindWindowByID(m_pressedWindow);
        if ( !hit || !hit->Active ) m_cancelled = true;
        // 首帧过滤只屏蔽点击，组件仍会提供 HoveredID；无需建立全局命中索引。
        // 后续帧 ActiveID 属于滚动捕获，不能再用当时的 HoveredID 覆盖最初目标。
        if ( m_justPressed ) m_item = ImGui::GetHoveredID();
        // 保留一个没有可见外观的原生输入身份，帧末不会把空白触摸解释为移动窗口。
        // 这与禁用窗口不同：文本布局、弹层生命周期和真实组件绘制仍正常推进。
        if ( hit ) {
            ImGui::SetActiveID(m_activeId, hit);
            ImGui::KeepAliveID(m_activeId);
        }
    }
    // 不参与仲裁的普通帧完全保留原生状态，不做无意义的恢复和焦点覆盖。
    // 所有临时字段成对恢复，不能让下一帧把真实按住误判为一次新的按下。
    if ( !std::exchange(m_filtered, false) ) return;
    auto& io                               = ImGui::GetIO();
    io.MouseDown[0]                        = m_raw.m_down;
    io.MouseClicked[0]                     = m_raw.m_clicked;
    io.MouseReleased[0]                    = m_raw.m_released;
    io.MouseDoubleClicked[0]               = m_raw.m_doubleClicked;
    io.MouseClickedCount[0]                = m_raw.m_count;
    io.MouseDownDuration[0]                = m_raw.m_duration;
    io.MouseDownDurationPrev[0]            = m_raw.m_previousDuration;
    *ImGui::GetKeyData(ImGuiKey_MouseLeft) = m_raw.m_key;
    // 位置与命中窗口一起恢复；只归还坐标会让下一层 UI 使用错误的窗口身份。
    // 这些赋值不重新命中窗口，也不会触发业务回调或改变已绘制的滚动位置。
    io.MousePos   = m_raw.m_position;
    io.MouseDelta = m_raw.m_delta;
    ImGui::GetCurrentContext()->HoveredWindow =
        ImGui::FindWindowByID(m_raw.m_hoveredWindow);
}
}  // namespace MMM::UI
