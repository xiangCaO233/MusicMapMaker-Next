#include "graphic/mobile/MobileTouchInput.h"
#include <algorithm>
#include <cmath>

namespace MMM::Graphic
{
namespace
{
/// @brief Android 事件泵和 ImGui 共用 UI 线程，状态不通过其他线程发布。
MobileTouchTracker tracker;
}  // namespace
/// @brief 维护稳定身份、手势代际和取消门闩，坐标只接受有限输入。
/// @param phase 平台阶段，取消独立于普通抬手。
/// @param device 触摸设备身份，不能用鼠标编号代替。
/// @param finger 当前设备分配的触点身份，不要求连续编号。
/// @param x 窗口归一化横坐标，可暂时越出窗口边界。
/// @param y 窗口归一化纵坐标，不在平台层进行轨道投影。
/// @param seconds 平台单调时间，只用于手势时长，不作为同步等待。
/// @details 单帧内的按下与抬手仍保留同一代际及 peak，供消费方区分轻点。
/// @warning 高频事件最多查找十个触点，不访问文件系统或业务对象。
void MobileTouchTracker::update(TouchPhase phase, std::uint64_t device,
                                std::uint64_t finger, float x, float y,
                                double seconds)
{
    // 非有限位置不能进入距离运算；将本手势取消而不是传播 NaN。
    if ( !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(seconds) ) {
        cancel();
        return;
    }
    // 只搜索当前有效前缀，已抬起的旧数组项不能再次命中。
    // 两个设备可复用相同 finger 值，复合身份避免把双指误合并成单指。
    auto       begin = m_state.m_contacts.begin();
    auto       end   = begin + m_state.m_count;
    const auto found = std::find_if(begin, end, [&](const TouchContact& p) {
        return p.m_device == device && p.m_finger == finger;
    });
    if ( phase == TouchPhase::Down && found == end ) {
        if ( m_state.m_count == 0 && m_overflow == 0 ) {
            // 快照覆盖只发生在真正的新手势；抬手后保留旧 peak 供 UI 消费。
            // 保留事件修订的单调性，消费方可跳过没有新事件的重复快照。
            // 新代际清理取消标记，避免上一个系统中断永久禁用触屏。
            const auto epoch    = m_state.m_epoch + 1;
            const auto revision = m_state.m_revision;
            m_state             = {};
            m_state.m_epoch     = epoch;
            m_state.m_revision  = revision;
            m_state.m_originX   = x;
            m_state.m_originY   = y;
            m_state.m_started   = seconds;
        }
        if ( m_state.m_count == MobileTouchSnapshot::CAPACITY ) {
            // 超出容量时整次手势失效，但仍统计额外抬手，不能提前重开输入。
            // 不扩容，也不把丢失身份的触点交给编辑层；整段都保持取消状态。
            // 额外计数在抬手时扣除，已知触点抬完不代表屏幕已完全释放。
            ++m_overflow;
            m_state.m_cancelled = true;
        } else {
            // 按首次出现顺序保存接触点；后续移动只更新坐标，不重新排序。
            // 连续 UI 帧因此能够稳定跟踪同一对手指，不产生配对跳变。
            m_state.m_contacts[m_state.m_count++] = {
                device, finger, x, y, x, y
            };
            if ( m_state.m_count == 2 && m_state.m_peak < 2 ) {
                // 冻结第二指加入瞬间的两点，后续 UI 帧不读取已经移动后的起点。
                m_state.m_pairStart = { m_state.m_contacts[0],
                                        m_state.m_contacts[1] };
            }
            m_state.m_peak = std::max(m_state.m_peak, m_state.m_count);
        }
    } else if ( found != end ) {
        // 最大位移在 Up 之前采集，快速拖动只占一帧也不能伪装成撤销轻点。
        // 分开保存两个轴，消费方使用当前窗口尺寸换算真实像素容差。
        m_state.m_motionX =
            std::max(m_state.m_motionX, std::abs(x - found->m_startX));
        m_state.m_motionY =
            std::max(m_state.m_motionY, std::abs(y - found->m_startY));
        if ( phase == TouchPhase::Up || phase == TouchPhase::Cancel ) {
            // 保持存活触点的原来顺序；一根手指抬起不改变另一根的真实身份。
            std::move(found + 1, end, found);
            // 只压缩有效前缀，尾部残值从未对消费者声明为活跃。
            // peak 故意不随 count 减少，剩余手指仍属于原来的多指手势。
            --m_state.m_count;
            if ( phase == TouchPhase::Cancel ) m_state.m_cancelled = true;
        } else {
            // 重复 Down 不创建第二个相同手指，未知 Move 也不能凭空起手。
            found->m_x = x;
            found->m_y = y;
        }
    } else if ( phase == TouchPhase::Up || phase == TouchPhase::Cancel ) {
        // 平台可以在溢出或系统抢占后发送未知 Up，不能让计数下溢。
        // 未知 Move 不创建身份，防止从系统窗口返回后恢复半次旧手势。
        if ( m_overflow > 0 ) --m_overflow;
        if ( phase == TouchPhase::Cancel ) m_state.m_cancelled = true;
    } else {
        return;
    }
    // 每条有效事件推进一次修订；空闲 UI 帧不调用本函数，也不修改时间。
    // 时间和坐标作为同一个本线程值发布，不引入跨线程可见性协议。
    m_state.m_updated = seconds;
    ++m_state.m_revision;
}
/// @brief 生命周期取消不要求平台补发所有旧触点的释放事件。
void MobileTouchTracker::cancel()
{
    // 失焦后平台可能不再派发旧手指抬起，必须同时清除溢出计数。
    // 原代际仍标为取消；下一次真实 Down 才能建立可编辑的新代际。
    m_state.m_count = m_overflow = 0;
    m_state.m_cancelled          = true;
    ++m_state.m_revision;
}
/// @brief 返回同线程稳定值引用，空闲期间不复制整个定长数组。
const MobileTouchSnapshot& mobileTouchSnapshot()
{
    return tracker.snapshot();
}
/// @brief SDL 类型在调用方转换，本实现和公共头不引用平台 SDK。
void publishMobileTouch(TouchPhase phase, std::uint64_t device,
                        std::uint64_t finger, float x, float y, double seconds)
{
    tracker.update(phase, device, finger, x, y, seconds);
}
/// @brief 失焦或旋转统一走取消路径，不能把中断解释为一次点击。
void cancelMobileTouch()
{
    tracker.cancel();
}
}  // namespace MMM::Graphic
