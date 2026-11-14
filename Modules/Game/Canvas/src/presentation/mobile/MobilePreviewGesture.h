#pragma once
#include "graphic/mobile/MobileTouchInput.h"
#include "presentation/PreviewInteractionPresentation.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace MMM::Canvas
{
/// @brief 预览手势的离散输出，没有编辑或历史动作。
struct PreviewTouchDelta {
    bool  m_tap{}, m_pan{}, m_rail{}, m_finished{};  ///< 每个边沿只交付一次。
    float m_y{}, m_dy{}, m_anchor{},
        m_ratio{ 1 };  ///< 屏幕纵坐标、增量和范围倍率。
};
/// @brief 首触点决定预览内容或密度轴归属，多指只在内容区导航。
/// @details 不读取合成鼠标，轻点、单指平移与捏合互斥。
class MobilePreviewGesture
{
public:
    /// @brief 根据原始代际推进一帧；取消保留已显示位置，不补发落点。
    /// @warning 每帧固定数量触点计算，无分配、锁、排序或后台等待。
    // 这里消费已发布值而非 SDL 回调；平台事件可比 UI 帧更频繁。
    // 每次 update 的输出只含这一帧增量，调用者不能把它当作累计拖动距离。
    // 外壳负责窗口遮挡判断，本类只负责同一原始代际的唯一区域归属。
    // 轻点不设长按超时；停留本身不会发出任何定位或历史操作。
    // 窗口外区域不能被手指当前经过的位置重新认领。
    PreviewTouchDelta update(const Graphic::MobileTouchSnapshot& raw,
                             const PreviewInteractionFrame& frame, float width,
                             float height, float ratio, float slop)
    {
        PreviewTouchDelta out;
        if ( !raw.m_epoch ) return out;
        // 新代际也可能在旧导航尚未交付结束时到达。
        // 先释放旧 Scrub，下一 UI 帧才能以新配置建立下一笔基线。
        // 这种跨帧交接不阻塞输入线程，也不使用固定时长消抖窗口。
        // 范围基线只在起笔取配置；后续捏合不被逻辑回流重置。
        // 首触点已经决定是内容还是密度轴，滑过边界不能切换职责。
        if ( raw.m_epoch != m_epoch ) {
            // 快速换手可能没有全抬帧，先交付上一笔最终导航再接下一代际。
            if ( m_running ) {
                out.m_finished = true;
                m_running      = false;
                return out;
            }
            m_epoch = raw.m_epoch;
            m_taken = m_blocked = m_pair = m_moved = m_pairSeen = false;
            m_ratio       = std::clamp(ratio, 1.0f, 50.0f);
            m_origin      = frame.m_origin;
            m_size        = frame.m_size;
            const float x = raw.m_originX * width, y = raw.m_originY * height;
            // 只有首次观察这一代时允许归属；窗口外移入不构成新手势。
            m_rail  = inside(x, y, frame.m_railMin, frame.m_railMax);
            m_taken = frame.m_hovered && frame.m_available &&
                      (m_rail || inside(x,
                                        y,
                                        frame.m_origin,
                                        { frame.m_origin.x + frame.m_size.x,
                                          frame.m_origin.y + frame.m_size.y }));
            m_lastY = y;
        }
        out.m_ratio = m_ratio;
        // 窗口旋转、移动或缩放使旧坐标口径失效，不能继续沿旧轴修改视野。
        // 外部弹层、系统 Cancel 与三指同样封锁本代，残留抬手不能成为轻点。
        // 几何比较在每帧开始进行，避免布局变化后出现一次错误的大幅平移。
        // 内容起点或尺寸任一变化都会使本代际失效，恢复原值不解除封锁。
        // 显示宽高只用于归一化触点，不参与谱面滚速或时间范围计算。
        // 密度轴是否可用由上层提供的正面积矩形决定，不读取 UI 窗口树。
        // 方向和缩放都由有效触点计算，不借用合成鼠标拖动距离。
        const bool geometry = std::abs(frame.m_origin.x - m_origin.x) < .5f &&
                              std::abs(frame.m_origin.y - m_origin.y) < .5f &&
                              std::abs(frame.m_size.x - m_size.x) < .5f &&
                              std::abs(frame.m_size.y - m_size.y) < .5f;
        if ( !m_taken || m_blocked ) return out;
        if ( !frame.m_available || !geometry || raw.m_cancelled ||
             raw.m_peak > 2 || !std::isfinite(width) ||
             !std::isfinite(height) || width <= 0 || height <= 0 ||
             (m_rail && raw.m_peak > 1) ) {
            out.m_finished = m_running;
            m_running      = false;
            m_blocked      = true;
            return out;
        }
        // 最高触点数由原始跟踪器保留，不能只看当前剩余手指数。
        // 内容区允许从一指平移转成两指导航；密度轴则在多指到来时结束。
        // 两指成立后永远不回到本代际的单指分支。
        // 三指峰值此前已经被拒绝，不能任取其中两指继续缩放。
        // Preview 只浏览，不复用主画布的二指撤销或三指重做。
        if ( raw.m_peak == 2 ) {
            m_pairSeen = true;
            if ( raw.m_count != 2 ) {
                // 任一指抬起即结束双指导航，剩余指不能接回单指点击或拖动。
                out.m_finished = m_running;
                m_running      = false;
                m_blocked      = true;
                return out;
            }
            // 当前两指与初次配对必须属于同一个设备和手指身份。
            // 坐标换算分别使用宽高，非方形屏幕不能把归一化距离当作像素。
            // 第二指按下前已有的单指位移已经提交，不在配对时再计算一次。
            // 配对开始时只冻结基线，不主动产生定位或倍率变化。
            // 后续真正越过阈值才一次性交付累计位移。
            const auto&  a   = raw.m_contacts[0];
            const auto&  b   = raw.m_contacts[1];
            const auto   min = frame.m_origin;
            const ImVec2 max{ min.x + frame.m_size.x, min.y + frame.m_size.y };
            if ( !m_pair ) {
                // 第二指必须从同一预览内容落下；跨窗配对不允许接管首指。
                // 配对基准来自第二指加入的原始位置，不借首指更早的落点。
                const auto& sa = raw.m_pairStart[0];
                const auto& sb = raw.m_pairStart[1];
                if ( !inside(sa.m_x * width, sa.m_y * height, min, max) ||
                     !inside(sb.m_x * width, sb.m_y * height, min, max) ) {
                    out.m_finished = m_running;
                    m_running      = false;
                    m_blocked      = true;
                    return out;
                }
                m_span      = std::hypot((sa.m_x - sb.m_x) * width,
                                         (sa.m_y - sb.m_y) * height);
                m_lastY     = (sa.m_y + sb.m_y) * height * .5f;
                m_lastX     = (sa.m_x + sb.m_x) * width * .5f;
                m_pairRatio = m_ratio;
                m_aDevice   = a.m_device;
                m_aFinger   = a.m_finger;
                m_bDevice   = b.m_device;
                m_bFinger   = b.m_finger;
                m_pair      = true;
            }
            if ( a.m_device != m_aDevice || a.m_finger != m_aFinger ||
                 b.m_device != m_bDevice || b.m_finger != m_bFinger ) {
                // 换指不复用旧指距，避免突然跳变；下一笔完整代际才能重新开始。
                out.m_finished = m_running;
                m_running      = false;
                m_blocked      = true;
                return out;
            }
            const float x = (a.m_x + b.m_x) * width * .5f;
            const float y = (a.m_y + b.m_y) * height * .5f;
            const float span =
                std::hypot((a.m_x - b.m_x) * width, (a.m_y - b.m_y) * height);
            // 缩放死区独立于中点平移死区，轻微反向开合不触发缩放。
            // 以初始指距计算相对阈值，手机与平板的规则保持一致。
            // 捏合没有逐帧倍率相乘，张开后合回可恢复同一范围。
            // 初始重合的两指只允许平移，不把接近零的距离作为除数。
            // 达到倍率上下界后仍保留中点平移，不冻结整个导航。
            const float dead   = std::max(slop * 1.5f, m_span * .08f);
            const float change = span - m_span;
            if ( !m_moved && std::hypot(x - m_lastX, y - m_lastY) <= slop &&
                 std::abs(change) <= dead )
                return out;
            // 只映射捏合死区之外的距离，纯双指平移不会悄悄改变范围倍率。
            // 预览范围是视觉缩放的倒数，张开手指应减小可见时间跨度。
            const float pinch =
                std::copysign(std::max(std::abs(change) - dead, 0.0f), change);
            // 范围一和五十是既有预览设置允许的边界，不更改主画布缩放范围。
            // 回到初始指距时恢复原范围，减少边界反复捏合造成的漂移。
            // 同一帧只输出一个平移和缩放组合，保持中点锚点的数学一致性。
            // 无变化输出不意味着释放手势，输入仍保持独占直到真实结束。
            // 最终倍率与最后增量一起返回，不能在收尾时回退到初始配置。
            const float target =
                m_span <= slop ? m_ratio
                               : std::clamp(m_pairRatio * m_span /
                                                std::max(slop, m_span + pinch),
                                            1.0f,
                                            50.0f);
            out.m_pan    = std::abs(y - m_lastY) > .001f ||
                           std::abs(target - m_ratio) > .00001f;
            out.m_dy     = y - m_lastY;
            out.m_anchor = m_lastY;
            out.m_ratio  = target;
            m_lastY      = y;
            m_lastX      = x;
            m_ratio      = target;
            m_moved      = true;
            m_running |= out.m_pan;
            return out;
        }
        // 曾经双指的代际不能在剩余指抬起时再次交付单指结果。
        // 原始峰值跨帧保留，取消和普通释放都遵守同一锁存边界。
        if ( m_pairSeen ) return out;
        // 单指距离用整代峰值，移开再返回原位也不会退化成轻点。
        // 位移峰值保留移出又返回的全过程，不能在松手时重新归类轻点。
        // 横向拖动没有垂直时间位移，但仍否决后来可能出现的点击。
        // 密度轴的上下越界由时间映射夹取；内容浏览由正式 Seek 夹取。
        // 没有可见变化的帧不发布连续命令，静止长按不会制造忙循环。
        // 正常 Up 的位置独立保存，不能读取已移除触点的尾部残值。
        m_moved |=
            raw.m_motionX * width > slop || raw.m_motionY * height > slop;
        const float y = raw.m_count ? raw.m_contacts[0].m_y * height
                                    : raw.m_releaseY * height;
        if ( !raw.m_count ) {
            // Up 自身也携带最终坐标；快速拖动合帧时不能只结束而丢失整笔位移。
            out.m_tap    = !m_moved;
            out.m_y      = y;
            out.m_dy     = y - m_lastY;
            out.m_anchor = m_lastY;
            out.m_pan    = !m_rail && m_moved && std::abs(out.m_dy) > .001f;
            out.m_rail   = m_rail && (out.m_tap || std::abs(out.m_dy) > .001f);
            out.m_finished =
                m_running || out.m_pan || (out.m_rail && !out.m_tap);
            m_running = false;
            m_blocked = true;
            return out;
        }
        if ( !m_moved ) return out;
        // 横向浏览没有时间增量；只有纵向变化才开启连续 Scrub。
        out.m_dy     = y - m_lastY;
        out.m_anchor = m_lastY;
        out.m_y      = y;
        out.m_pan    = !m_rail && std::abs(out.m_dy) > .001f;
        out.m_rail   = m_rail && (!m_running || std::abs(out.m_dy) > .001f);
        m_running |= out.m_pan || out.m_rail;
        m_lastY = y;
        return out;
    }
    /// @brief 隐藏或切换方案时拒绝残留代际，调用者独立提交最后有效导航。
    // 显式取消也覆盖尚未越过阈值的候选轻点。
    // 下一次独立 Down 会建立新代际，不依赖人工补发 Up。
    // 最后显示值由调用方提交，本函数不直接访问业务或配置。
    // 没有待定值队列，因此窗口关闭不会留下异步的点击任务。
    // 旧手势封锁属于本视图，不影响另一个窗口的新手势。
    void cancel(std::uint64_t epoch)
    {
        m_epoch = epoch;
        m_taken = m_running = false;
        m_blocked           = true;
    }
    /// @brief 待定轻点与已开始导航均拥有输入；松手或取消后释放。
    // 这个查询用于操作方案守卫，不用于判断是否需要刷新谱面。
    // 待定轻点也占有输入，不能在 Up 前把它移交给经典鼠标方案。
    // 取消后保持封锁到下一代，而不是等待一个猜测的毫秒窗口。
    // 成员全为值，不拥有设备、窗口、会话或共享渲染快照。
    bool active() const { return m_taken && !m_blocked; }

private:
    /// @brief 半开矩形避免内容与相邻密度轴同时认领同一落点。
    static bool inside(float x, float y, ImVec2 min, ImVec2 max)
    {
        return x >= min.x && x < max.x && y >= min.y && y < max.y;
    }
    /// @brief 只保存几何值，不跨帧持有快照或 ImGui 窗口地址。
    ImVec2 m_origin{}, m_size{};
    /// @brief 触点身份与原始代际共同隔离换指和换窗。
    std::uint64_t m_epoch{}, m_aDevice{}, m_aFinger{}, m_bDevice{}, m_bFinger{};
    /// @brief 单指与双指的独立基准，不从异步快照回流重置增量。
    float m_lastX{}, m_lastY{}, m_span{}, m_pairRatio{ 1 }, m_ratio{ 1 };
    /// @brief 单向门闩阻止多指结束后的单指回放，不生成编辑历史。
    bool m_taken{}, m_blocked{}, m_pair{}, m_moved{}, m_pairSeen{}, m_rail{},
        m_running{};
};
}  // namespace MMM::Canvas
