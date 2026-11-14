#pragma once
#include "MobileCanvasGesture.h"
#include <algorithm>
#include <cfloat>
#include <imgui.h>

namespace MMM::Canvas
{
/// @brief 原控制器调用期间借用一份左键快照，离开作用域立即恢复真实触点。
/// @details 不修改全局事件队列，也不让菜单或其他窗口看到合成边沿。
/// @par 输入字段的成组约束
/// - Down 与 Clicked 共同描述起笔，单独保留任一个都可能漏掉事务。
/// - Released 独立描述收尾，不能在同一次调用里同时起笔又结束。
/// - ClickedPos 只在起笔回放时改变，维持原工具的拖动参照点。
/// - MousePos 在等待阶段保持真实坐标，确保吸附线即时跟随。
/// - 坐标回到原点时清零 MouseDelta，不制造不存在的摄像机位移。
/// - 回放边沿清零累计拖动量，不借用另一物理触点的距离。
/// - 持续时间局部改变，不影响后续按钮的按住动画。
/// - 双击次数保留后端判断，不用触摸计时重新合并点击。
/// - 所有字段在析构恢复，其他画布和浮层仍读取真实设备输入。
/// @note 不调用事件队列，不会制造下一帧残留的合成输入。
/// @note 只能围绕原控制器使用，不能跨过弹层或业务按钮绘制。
/// @note 发布命令后直接返回，不等待逻辑线程应用或回传快照。
/// @note 等待阶段从未向编辑器起笔，因此不是事后撤销事务。
/// @note 多画布独占各自策略，状态不放到全局静态变量中共享。
class MobilePointerInputScope
{
public:
    /// @brief 保存物理状态后呈现本帧已经确定的编辑意图。
    /// @param mode 等待阶段隐藏左键，起笔与收尾分别呈现唯一边沿。
    /// @param point 已锁存的原点或终点，不能使用当前菜单指针代替。
    /// @param count 方案确认的点击次数，选择工具将连续轻点保持为单次选择。
    /// @param continuing 起笔后同帧补交真实位移，不产生第二次按下或快捷键。
    /// @param hidePointer 全部抬起或多指接管时局部清除悬浮，不影响其他控件。
    /// @details 所有修改严格局限于 routeInput 的同步调用栈。
    /// 不调用 AddMouseButtonEvent，因为入队事件要到下一轮 NewFrame 才生效。
    /// 也不直接调用编辑命令，否则会绕过权限、投影和当前工具的事务规则。
    /// 合成 Down 与 Clicked 配对，避免画笔把一个无按住的边沿解释为取消。
    /// Release 独立成帧，让单帧快速触摸也经过正式的起笔及提交生命周期。
    /// 等待阶段 MousePos 保持真实值，吸附线与只读位置反馈仍即时更新。
    /// 双击沿用后端给出的次数，不因 450ms 手势阈值改变桌面双击时限。
    /// 该作用域不执行可见按钮，后续弹层按原始触点处理释放保护。
    /// @warning 每帧栈上构造，仅保存固定大小字段，无堆分配或线程同步。
    MobilePointerInputScope(MobileCanvasGesture::Output mode, ImVec2 point,
                            unsigned short count, bool hidePointer = false,
                            bool continuing = false)
        : m_io(ImGui::GetIO())
        , m_pos(m_io.MousePos)
        , m_delta(m_io.MouseDelta)
        , m_clickedPos(m_io.MouseClickedPos[0])
        , m_down(m_io.MouseDown[0])
        , m_clicked(m_io.MouseClicked[0])
        , m_released(m_io.MouseReleased[0])
        , m_double(m_io.MouseDoubleClicked[0])
        , m_count(m_io.MouseClickedCount[0])
        , m_duration(m_io.MouseDownDuration[0])
        , m_previous(m_io.MouseDownDurationPrev[0])
        , m_distance(m_io.MouseDragMaxDistanceSqr[0])
    {
        using O = MobileCanvasGesture::Output;
        if ( continuing ) {
            // 原点 Begin
            // 已经派发；本次只回放跨过触控容差的实际坐标，不重复按下。
            // 若舍弃此点，下一帧已经转向时会把首个拐角错误合成对角线。
            // 持续样本保留原手势的 ClickedPos，不能把当前位置变成第二个起点。
            // 累计位移由这两个点重算，不借用上一物理触点的拖动距离残留。
            // 快速抬手也先补交位移，已经排队的 Release 随后负责唯一一次结束。
            // 内层析构先恢复外层 Begin，外层结束后再恢复原始
            // IO，期间不绘制按钮。
            const float dx                  = point.x - m_clickedPos.x,
                        dy                  = point.y - m_clickedPos.y;
            m_io.MouseDown[0]               = true;
            m_io.MouseClicked[0]            = m_io.MouseReleased[0] =
                m_io.MouseDoubleClicked[0]  = false;
            m_io.MouseClickedCount[0]       = 0;
            m_io.MousePos                   = point;
            m_io.MouseDelta                 = { dx, dy };
            m_io.MouseDownDuration[0]       = m_io.DeltaTime;
            m_io.MouseDownDurationPrev[0]   = 0;
            m_io.MouseDragMaxDistanceSqr[0] = dx * dx + dy * dy;
            return;
        }
        // Pass 完全保留连续拖动。等待阶段连 Down 都屏蔽，画笔与框选均无法起笔。
        if ( mode == O::Pass ) return;
        m_io.MouseDown[0]          = mode == O::Begin;
        m_io.MouseClicked[0]       = mode == O::Begin;
        m_io.MouseReleased[0]      = mode == O::Release;
        m_io.MouseDoubleClicked[0] = mode == O::Begin && count == 2;
        m_io.MouseClickedCount[0] =
            mode == O::Begin ? std::max<unsigned short>(1, count) : 0;
        m_io.MouseDownDuration[0]       = mode == O::Begin ? 0.0f : -1.0f;
        m_io.MouseDownDurationPrev[0]   = mode == O::Release ? 0.0f : -1.0f;
        m_io.MouseDragMaxDistanceSqr[0] = 0;
        if ( mode == O::Suppress && hidePointer ) {
            // 触摸没有离开后继续悬浮的语义，不能留下桌面 tooltip 和待绘制虚影。
            // 仅在画布调用栈内隐藏；恢复真实 IO 后按钮仍收到正常释放边沿。
            m_io.MousePos   = { -FLT_MAX, -FLT_MAX };
            m_io.MouseDelta = {};
        }
        if ( mode == O::Begin || mode == O::Release ) {
            // 起笔必须位于最初触点，释放必须位于最后触点，不能跳到菜单位置。
            m_io.MousePos   = point;
            m_io.MouseDelta = {};
            if ( mode == O::Begin ) m_io.MouseClickedPos[0] = point;
        }
    }
    /// @brief 恢复原始帧；即使工具提前返回也不会污染后续 UI。
    /// @details 坐标、累计拖动量和持续时间必须一起恢复。
    /// 只恢复 MouseDown 会使之后窗口误判拖动或双击，造成输入穿透。
    /// 原始释放帧仍留给菜单保护使用，不由画布回放消耗系统事件。
    /// 没有后台任务或延迟析构，作用域结束后不存在指向 IO 的新观察者。
    /// @warning 每帧仅恢复固定数量字段，不写事件队列或等待后端。
    ~MobilePointerInputScope()
    {
        m_io.MousePos                   = m_pos;
        m_io.MouseDelta                 = m_delta;
        m_io.MouseClickedPos[0]         = m_clickedPos;
        m_io.MouseDown[0]               = m_down;
        m_io.MouseClicked[0]            = m_clicked;
        m_io.MouseReleased[0]           = m_released;
        m_io.MouseDoubleClicked[0]      = m_double;
        m_io.MouseClickedCount[0]       = m_count;
        m_io.MouseDownDuration[0]       = m_duration;
        m_io.MouseDownDurationPrev[0]   = m_previous;
        m_io.MouseDragMaxDistanceSqr[0] = m_distance;
    }

private:
    ImGuiIO& m_io;  ///< 只借用 UI 线程当前上下文，不拥有或跨帧保存。
    ImVec2   m_pos, m_delta, m_clickedPos;  ///< 精确还原合成输入前的坐标。
    bool m_down, m_clicked, m_released, m_double;  ///< 左键全部边沿成对保存。
    unsigned short m_count;  ///< 双击计数仍由原生输入层维护。
    float          m_duration, m_previous,
        m_distance;  ///< 拖动阈值和持续时间不泄漏给后续控件。
};
}  // namespace MMM::Canvas
