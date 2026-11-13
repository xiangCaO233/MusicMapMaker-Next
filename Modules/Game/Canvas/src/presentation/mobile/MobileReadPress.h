#pragma once
#include "graphic/mobile/MobileTouchInput.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <imgui.h>

namespace MMM::Canvas
{
/// @brief 只读列表与批注入口的轻点／长按分类，不与页面滚动争用鼠标回放。
/// @details 原始触点拥有一代输入；移动、多指、系统取消后直到下一代才恢复。
/// 目标使用稳定控件身份，不保存行指针或上一帧快照。
class MobileReadPress
{
public:
    /// @brief 选择只在真实抬手出现，长按只在静止时出现一次。
    enum class Result { None, Tap, Hold };
    /// @brief 为当前可见目标采样，未命中起点的行不能接管途中移入的手指。
    /// @param id 当前窗口内稳定目标身份，零表示没有可阅读目标。
    /// @param originHit 原始起点属于该目标且未被其它弹层覆盖。
    /// @param eligible 当前目标仍可见且属于原谱面实例。
    /// @warning 每帧固定标量运算，不分配、不等待、不修改谱面或 ImGui 输入。
    Result update(std::uint32_t id, bool originHit, bool eligible)
    {
        const auto& touch = Graphic::mobileTouchSnapshot();
        // 零代际意味着平台尚未发布触点，不回退到合成鼠标猜测触摸。
        // 经典鼠标由上层平台分派保留，不能在本分类器内混用两套时钟。
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
    /// @brief 隐藏或切谱时吞掉当前代际，不能在重开后接收残留抬手。
    void cancel()
    {
        m_epoch        = Graphic::mobileTouchSnapshot().m_epoch;
        m_blocked      = true;
        m_target       = 0;
        m_captureFrame = -1;
    }
    /// @brief 命中原始归一化 Down，而非合成鼠标或当前指尖位置。
    /// @warning 每个可见目标固定几何判断，不查询窗口树或设备状态。
    static bool originInside(ImVec2 min, ImVec2 max)
    {
        const auto&  touch = Graphic::mobileTouchSnapshot();
        const auto&  io    = ImGui::GetIO();
        const ImVec2 p{ touch.m_originX * io.DisplaySize.x,
                        touch.m_originY * io.DisplaySize.y };
        return p.x >= min.x && p.x < max.x && p.y >= min.y && p.y < max.y;
    }

private:
    /// @brief 同一物理手势不能向两个目标交付。
    std::uint64_t m_epoch{};
    /// @brief 起笔目标控件身份，行重排不改写它。
    std::uint32_t m_target{};
    /// @brief UI 单调时钟只作非阻塞长按判定。
    double m_started{};
    /// @brief 起点仅在首次观察代际的帧内允许归属。
    int m_captureFrame{ -1 };
    /// @brief 检测视图隐藏，禁止剩余指头重新激活旧候选。
    int m_frame{};
    /// @brief 取消与已交付均为单向门闩。
    bool m_blocked{}, m_sent{};
};
}  // namespace MMM::Canvas
