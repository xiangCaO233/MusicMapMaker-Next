#include "ui/workspace/TouchReadPress.h"
#include "graphic/mobile/MobileTouchInput.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
namespace MMM::UI
{
/// @brief 滚动往返、长按后抬手和同帧多行都不能重复产生操作。
/// @warning UI 热路径固定次数访问已发布触点，不读取业务模型。
TouchReadPress::Result TouchReadPress::update(std::uint32_t id, bool originHit,
                                              bool eligible)
{
    const auto& touch = Graphic::mobileTouchSnapshot();
    // 零代际意味着平台尚未发布触点，不回退到合成鼠标猜测触摸。
    // 经典鼠标由上层平台分派保留，不能在本分类器内混用两套时钟。
    // 没有原始设备输入时保持无结果，桌面回退由宿主单独处理。
    // 不能从全局滚动层补交的鼠标 Up 反推一次触摸。
    if ( !touch.m_epoch ) return Result::None;
    const int frame = ImGui::GetFrameCount();
    if ( m_epoch != touch.m_epoch ) {
        // 同帧遍历多行时只重置一次，只有实际命中起点的行可以锁存。
        m_epoch   = touch.m_epoch;
        m_target  = 0;
        m_blocked = m_sent = false;
        m_started          = ImGui::GetTime();
        m_captureFrame     = frame;
    } else if ( m_frame && frame > m_frame + 1 ) {
        // 折叠、切页或隐藏后不能恢复旧的长按候选。
        m_blocked = true;
    }
    // 同帧不同资源行只参与归属判定，不会重启长按计时。
    // 列表使用稳定 ID，当前目标不可见时宿主应撤去许可。
    m_frame        = frame;
    const auto& io = ImGui::GetIO();
    // 位移峰值来自整代触点，而不是相邻两帧的差值。
    // 因此移出再回到起点仍是滑动，不能在最后松手时误读为轻点。
    // 容差随当前字体尺度变化，不依赖设备厂商的物理像素密度。
    const float tolerance = std::max(8.0f, ImGui::GetFontSize() * 0.45f);
    if ( touch.m_peak != 1 || touch.m_cancelled ||
         std::abs(touch.m_motionX * io.DisplaySize.x) > tolerance ||
         std::abs(touch.m_motionY * io.DisplaySize.y) > tolerance )
        m_blocked = true;
    if ( !m_target && originHit && eligible && m_captureFrame == frame )
        m_target = id;
    // 目标 ID 冻结后，列表排序或裁剪只会使其暂时缺席。
    // 不能用下一行或手指当前经过的行替代这个稳定身份。
    // 取消与交付标志不会因目标暂时缺席而被清空。
    // 一个长按最多打开一次菜单，最后抬手不能再打开控制器。
    // 锁存后其他行只返回无结果，不修改已选定的目标。
    if ( !id || id != m_target || m_blocked || m_sent ) return Result::None;
    if ( !eligible ) {
        m_blocked = true;
        return Result::None;
    }
    if ( !touch.m_count ) {
        // 快速 Down/Up 合帧也只交付一次轻点，滚动往返起点仍被位移峰值拒绝。
        m_sent = true;
        return Result::Tap;
    }
    // 长按只检查经过时间，不睡眠或阻塞页面滚动。
    // 发出一次结果后立即锁存，即使继续按住数秒也不重复打开菜单。
    // 正常 Up 与系统 Cancel 分开处理，后者永远没有轻点结果。
    if ( ImGui::GetTime() - m_started >= 0.5 ) {
        m_sent = true;
        return Result::Hold;
    }
    return Result::None;
}
/// @brief 明确隐藏时封锁当前原始触摸，保留其他视图各自的归属。
void TouchReadPress::cancel()
{
    // 只封锁本解释器，不修改公共触点，也不影响另一窗口的新手势。
    // 下次完整 Down 的新代际自然恢复，无需等待定时器。
    m_epoch        = Graphic::mobileTouchSnapshot().m_epoch;
    m_blocked      = true;
    m_target       = 0;
    m_captureFrame = -1;
}
/// @brief 归一化 Down 与矩形转换保持在同一 UI 逻辑像素空间。
/// @warning 每个可见行只执行常量几何计算，不保存 ImGui 指针。
bool TouchReadPress::originInside(ImVec2 min, ImVec2 max)
{
    const auto&  touch = Graphic::mobileTouchSnapshot();
    const auto&  io    = ImGui::GetIO();
    const ImVec2 p{ touch.m_originX * io.DisplaySize.x,
                    touch.m_originY * io.DisplaySize.y };
    return p.x >= min.x && p.x < max.x && p.y >= min.y && p.y < max.y;
}
}  // namespace MMM::UI
