#pragma once
#include "graphic/mobile/MobileTouchInput.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace MMM::Canvas
{
/// @brief 双指导航及轻触历史动作的值结果，不包含会话或 UI 控件。
struct MobileNavigationDelta {
    float m_x{}, m_y{}, m_anchorX{}, m_anchorY{},
        m_zoom{ 1.0f };  ///< 增量、旧中点及绝对缩放。
    bool m_changed{}, m_finished{}, m_undo{}, m_redo{};  ///< 各边沿只发布一次。
};
/// @brief 双指直接移动画布，捏合缩放；二指/三指轻触分别撤销和重做。
/// @details 手指身份变化重新定基，不把指间距离跳变误解释为捏合。
/// 已经导航过的手势永远不能退回历史轻触，移回原点同样不例外。
class MobileCanvasNavigation
{
public:
    /// @brief 从同线程原始快照生成导航增量，不依赖合成鼠标顺序。
    /// @param allowed 首次落点归本画布且没有外部弹层接管。
    /// @param width 平台窗口逻辑宽度，用于换算归一化触点。
    /// @param height 平台窗口逻辑高度，与传入锚点使用相同坐标空间。
    /// @param zoom 开始手势时的配置倍率，后续更新不读取滞后的逻辑快照。
    /// @param slop 已转换为逻辑像素的触摸防抖半径。
    /// @details 仅把本帧变化交给调用者，不保存尚待后台处理的命令队列。
    /// 同一原始代际不允许重启历史动作，全部抬起后的空帧也不例外。
    /// 归一化坐标必须按两个轴分别转换，不能把宽高比带入捏合距离。
    /// 缩放上限与工具栏一致，达到边界后仍继续发布真实中点平移。
    /// 返回的锚点属于增量发生前的位置，配合增量构成一次原子视图变换。
    /// 手指抬起后只结束导航，不回放其中任意一指的单指编辑。
    /// 原始峰值可能大于当前触点数，三指曾经参与的手势不能再当二指轻触。
    /// 快照里最远运动距离保留往返过程，短时移回原点也不是历史轻触。
    /// 轻触时限只用于分类，不暂停 UI 或逻辑线程等待判定。
    /// 容差之前没有导航副作用，跨过容差时完整保留起始位移。
    /// 开始缩放取配置值，连续缩放只读本地基线，避免快照延迟造成反馈环。
    /// 调用者负责权限和画布身份；本类不自行切换活动谱面。
    /// @warning 每帧固定规模数学；无分配、配置复制、锁或阻塞等待。
    MobileNavigationDelta update(const Graphic::MobileTouchSnapshot& raw,
                                 bool allowed, float width, float height,
                                 float zoom, float slop)
    {
        MobileNavigationDelta result;
        if ( raw.m_epoch == 0 ) return result;
        // 最小化窗口不产生新的导航；无效尺寸也不能让历史手势误判为轻触。
        allowed = allowed && std::isfinite(width) && std::isfinite(height) &&
                  width > 0 && height > 0;
        if ( !std::isfinite(zoom) ) zoom = 1.0f;
        if ( m_epoch != raw.m_epoch ) {
            // 先结束尚未交付的旧导航，再在下一帧采纳新代际。
            // 否则一次快速换手可能用新配置覆盖尚未发布的旧目标。
            if ( m_running ) {
                // 快速换手可能没有独立的全抬帧，先提交上一代际的视图变换。
                m_running         = false;
                result.m_finished = true;
                result.m_zoom     = m_zoom;
                return result;
            }
            m_epoch = raw.m_epoch;
            m_zoom  = std::clamp(zoom, 0.1f, 10.0f);
            m_pair = m_moved = m_disabled = m_historyDone = false;
        }
        result.m_zoom = m_zoom;
        // 外部接管后不允许旧触点恢复导航，直到下一次真实按下。
        m_disabled |= !allowed || raw.m_cancelled;
        const bool pairAvailable =
            raw.m_count == 2 && raw.m_peak == 2 && !m_disabled;
        if ( !pairAvailable ) {
            // 三指参与后不继续捏合，避免任意两指组合切换造成倍率跳变。
            // 但静止的三指仍保留重做资格，直到全部手指抬起统一决定。
            if ( m_running ) result.m_finished = true;
            m_pair = m_running = false;
            if ( raw.m_count == 0 && !m_historyDone ) {
                // 只在全抬起时决定历史动作，不能在第一指抬起时提前撤销。
                m_historyDone  = true;
                const bool tap = !m_disabled && !m_moved &&
                                 raw.m_updated - raw.m_started <= 0.3 &&
                                 raw.m_motionX * width <= slop &&
                                 raw.m_motionY * height <= slop;
                result.m_undo  = tap && raw.m_peak == 2;
                result.m_redo  = tap && raw.m_peak == 3;
            }
            return result;
        }
        // 两点身份由跟踪器保持，中点不依赖哪根手指是系统鼠标来源。
        // 距离使用像素度量，非方形窗口也采用相同触觉阈值。
        const auto& a = raw.m_contacts[0];
        const auto& b = raw.m_contacts[1];
        const float x = (a.m_x + b.m_x) * width * 0.5f;
        const float y = (a.m_y + b.m_y) * height * 0.5f;
        const float span =
            std::hypot((a.m_x - b.m_x) * width, (a.m_y - b.m_y) * height);
        if ( !m_pair || m_deviceA != a.m_device || m_fingerA != a.m_finger ||
             m_deviceB != b.m_device || m_fingerB != b.m_finger ) {
            // 设备 ID 也参与身份比较，不同设备的同号手指不能配成旧触点。
            // 已导航后的新配对只取当前坐标，不能再借用上一对的起始距离。
            // 第一对用第二指加入瞬间的快照定基，避免同帧 Motion 被吞掉。
            const auto& sa       = raw.m_pairStart[0];
            const auto& sb       = raw.m_pairStart[1];
            const bool  original = !m_moved && sa.m_device == a.m_device &&
                                   sa.m_finger == a.m_finger &&
                                   sb.m_device == b.m_device &&
                                   sb.m_finger == b.m_finger;
            m_lastX    = original ? (sa.m_x + sb.m_x) * width * .5f : x;
            m_lastY    = original ? (sa.m_y + sb.m_y) * height * .5f : y;
            m_span     = original ? std::hypot((sa.m_x - sb.m_x) * width,
                                               (sa.m_y - sb.m_y) * height)
                                  : span;
            m_baseZoom = m_zoom;
            m_deviceA  = a.m_device;
            m_fingerA  = a.m_finger;
            m_deviceB  = b.m_device;
            m_fingerB  = b.m_finger;
            m_pair     = true;
        }
        // 平移开始不能隐式解锁缩放，否则手指间的自然开合会持续改变倍率。
        // 缩放同时考虑逻辑像素与初始指距，手机和平板使用相同的触觉尺度。
        const float pinchSlop = std::max(slop * 1.5f, m_span * .08f);
        const float spanDelta = span - m_span;
        // 位移与有效捏合任一成立就导航，轻微开合本身不能提前启动。
        // 一旦开始便持续推进，回程不会重新成为历史轻触。
        if ( !m_running && std::hypot(x - m_lastX, y - m_lastY) <= slop &&
             std::abs(spanDelta) <= pinchSlop )
            return result;
        m_running = m_moved = true;
        // 只把超出死区的距离交给缩放，边界处从原倍率连续开始，不突然跳档。
        // 仍以同一配对的起始距离计算绝对倍率，回到起始区间就恢复原倍率。
        const float pinchDelta = std::copysign(
            std::max(std::abs(spanDelta) - pinchSlop, 0.0f), spanDelta);
        // 手指初始重合时只做平移，避免除以接近零的距离产生缩放爆跳。
        // 死区内直接返回基准值，避免浮点乘除令纯平移产生微小倍率差。
        const float target =
            m_span <= slop ? m_zoom
            : pinchDelta == 0
                ? m_baseZoom
                : std::clamp(
                      m_baseZoom * (m_span + pinchDelta) / m_span, .1f, 10.0f);
        result.m_x       = x - m_lastX;
        result.m_y       = y - m_lastY;
        result.m_anchorX = m_lastX;
        result.m_anchorY = m_lastY;
        result.m_zoom    = target;
        result.m_changed = std::abs(result.m_x) > .001f ||
                           std::abs(result.m_y) > .001f ||
                           std::abs(target - m_zoom) > .00001f;
        // 确认导航后才推进增量基线，阈值内积累的位移不能被逐帧吃掉。
        // 绝对倍率另由初始距离求得，中点基线更新不改变缩放参照。
        m_lastX = x;
        m_lastY = y;
        m_zoom  = target;
        return result;
    }
    /// @brief 释放配置尚未交付时仍持有输入，不在方案之间移交半次导航。
    bool active() const { return m_running; }

private:
    /// @details 只缓存设备及手指编号，不持有 SDL 指针或 Android 对象。
    /// 坐标基线只属于当前配对，生命周期不能跨越新的原始手势代际。
    /// m_moved 一旦置位就保持到代际结束，防止回程动作被误认作撤销。
    /// m_historyDone 允许空帧继续显示菜单，但不能重复提交历史操作。
    std::uint64_t m_epoch{}, m_deviceA{}, m_fingerA{}, m_deviceB{},
        m_fingerB{};  ///< 稳定触点身份。
    float m_lastX{}, m_lastY{}, m_span{}, m_baseZoom{},
        m_zoom{ 1 };  ///< 本手势坐标基线。
    bool m_pair{}, m_running{}, m_moved{}, m_disabled{},
        m_historyDone{};  ///< 单向仲裁门闩。
};
}  // namespace MMM::Canvas
