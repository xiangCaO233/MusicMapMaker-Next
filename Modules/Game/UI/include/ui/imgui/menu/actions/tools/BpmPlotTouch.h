#pragma once

#include "graphic/mobile/MobileTouchInput.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace MMM::UI
{
/// @brief BPM 图表一帧的冻结投影和许可，不拥有音轨、窗口或编辑历史。
/// @details 投影必须来自正在绘制的数据区域，不能使用整个 ImGui 窗口。
/// 波形的坐标轴与频谱的标题不参与手势归属。
/// center 与 zoom 使用画布秒，音频偏移由宿主统一转换。
/// owner 在一个工具窗口内唯一，不代表某个持久化资源。
/// generation 来自活动操作方案，用于阻止切换方案后的输入串用。
/// hovered 只授权起点，已经捕获的手势允许移动到矩形外。
/// display 使用当前窗口尺寸，旋转后必须重新投影并结束旧手势。
struct BpmPlotTouchFrame {
    ImVec2 m_min{}, m_max{}, m_display{};  ///< 图表内部矩形和窗口逻辑像素。
    double m_center{}, m_zoom{}, m_duration{};  ///< 秒域中心、半窗及画布时长。
    float  m_slop{ 8 };            ///< 区分静止轻点与拖动，跟随实际 UI 缩放。
    int    m_owner{}, m_frame{};   ///< 波形和频谱共用代际，禁止跨区域抢占。
    std::uint64_t m_generation{};  ///< 操作方案变化不能继承旧触摸。
    bool m_hovered{}, m_edit{};    ///< 图层裁决起点；拍点模式禁止双指编辑。
};
/// @brief 图表触摸输出，只报告合法增量，不直接发送任何播放命令。
struct BpmPlotTouchResult {
    ImVec2 m_point{};             ///< 有效单指位置；取消时不使用系统取消坐标。
    double m_center{}, m_zoom{};  ///< 导航模式输出的下一帧共享视野。
    /// @brief 原始输入的各个边沿分别报告，取消不交付移动或轻点。
    bool m_pressed{}, m_down{}, m_moved{}, m_tapped{}, m_navigation{},
        m_finished{};
    // 起点和结束边沿分开，快速抬手不能变成另一张图表的新按下。
    // 编辑宿主在 finished 后释放自己的拍点捕获，保留最后合法结果。
};
/// @brief BPM 专用有界触摸状态机，播放路由和 Timing 语义仍归工具所有。
/// @details 单指浏览、轻点定位与双指锚点缩放共享唯一归属；拍点拖动显式启用。
/// @note 不使用系统触摸超时，静止按住不会因经过某个毫秒阈值自动提交。
/// 宿主的关闭和切轨均为显式边沿，不依赖后台偏好保存完成。
/// 此控制器没有编辑器实例令牌，不能独立跨项目继续手势。
/// 因而资源身份变化必须调用 suspend，而不是仅替换 Frame 中的时长。
/// 正常释放与平台取消在 Graphic 层即已区分，不能用鼠标 Up 代替 Cancel。
class BpmPlotTouch
{
public:
    /// @brief 关闭、切音轨或分页时拒绝旧代际的剩余触点。
    /// @param epoch 当前已发布的触点代际，不修改全局跟踪器。
    /// @details 下一次 update 即使遇到旧 Up 也只返回空输出。
    /// 宿主负责在调用前后结束自己的编辑历史，不由导航器回滚业务值。
    void suspend(std::uint64_t epoch)
    {
        m_epoch   = epoch;
        m_owner   = 0;
        m_blocked = true;
    }
    /// @brief 宿主在漏过抬手帧的快速换手边沿，先提交上一笔合法编辑。
    /// @warning UI 热路径只读取本线程代际标量，不分配或查询原始设备。
    std::uint64_t epoch() const { return m_epoch; }
    /// @brief 当前手势阻止播放跟随，也使一笔拍点修改合并为一条历史。
    /// @warning 每帧只检查本地归属和封锁位，不读取会话或发布命令。
    bool active() const { return m_owner && !m_blocked; }
    /// @brief 按实际图表投影消费原始触点，桌面不调用本入口。
    /// @param raw UI 线程的固定数组快照，借用仅持续本次调用。
    /// @param f 当前显示的视野；调用方须立即采纳有效导航结果。
    /// @return 捕获、移动、轻点和结束分别报告，不把结束合成为一次点击。
    /// @details 两张图表可以在同一帧依次调用，共享一个所有者。
    /// 数据结构不持有 PCM 或纹理，也不知道主谱面还是独立试听。
    /// 图表隐藏期间不调用此入口，帧号缺口将在恢复时终止旧投影。
    /// 编辑模式只输出指尖，不在这里计算 BPM 或操作 Timing 列表。
    /// @warning 每帧固定触点运算，无任务、分配、锁或编辑器命令。
    BpmPlotTouchResult update(const Graphic::MobileTouchSnapshot& raw,
                              const BpmPlotTouchFrame&            f)
    {
        BpmPlotTouchResult out;
        out.m_center = f.m_center;
        out.m_zoom   = f.m_zoom;
        /// @brief 坐标只使用 UI 逻辑像素，不混用 framebuffer 密度。
        const auto pixel = [&](float x, float y) {
            return ImVec2(x * f.m_display.x, y * f.m_display.y);
        };
        /// @brief 半开区域避免相邻图表同时命中边界。
        const auto inside = [&](ImVec2 p) {
            return p.x >= f.m_min.x && p.x < f.m_max.x && p.y >= f.m_min.y &&
                   p.y < f.m_max.y;
        };
        // 启动前没有任何真实 Down 时，零代际不能建立默认捕获。
        // 空输出仍保留调用方中心与半窗，宿主只在 navigation 时采纳。
        if ( !raw.m_epoch ) return out;
        if ( raw.m_epoch != m_epoch ) {
            // 首次看到代际的这一帧开放两个图表认领，之后不能从外部移入。
            // 事件泵可能在两个 UI 帧之间处理完整轻点，仍按原始起点认领。
            // 当前是否还有触点只决定后续输出，不决定代际是否合法。
            m_epoch   = raw.m_epoch;
            m_owner   = 0;
            m_blocked = false;
            m_capture = f.m_frame;
            m_moved = m_pair = false;
        }
        if ( !m_owner && !m_blocked && m_capture == f.m_frame && f.m_hovered &&
             inside(pixel(raw.m_originX, raw.m_originY)) ) {
            // 几何一经捕获便冻结，后续以最新绘图区比较而非静默重设。
            // 记录设备与指针两部分，另一个设备的同号手指不能替换它。
            // 第一帧只提供 pressed，不在这里把微小偏差当成拖动。
            m_owner       = f.m_owner;
            m_min         = f.m_min;
            m_max         = f.m_max;
            m_last        = pixel(raw.m_originX, raw.m_originY);
            m_generation  = f.m_generation;
            m_edit        = f.m_edit;
            m_frame       = f.m_frame;
            m_device      = raw.m_contacts[0].m_device;
            m_finger      = raw.m_contacts[0].m_finger;
            out.m_pressed = true;
        }
        // 外部起点在 capture 帧错过两张图后，不能下一帧移入再认领。
        // 即使另一图表的 hovered 为真，也不覆盖已经取得的 owner。
        if ( m_owner != f.m_owner || m_blocked ) return out;
        // 半像素容差仅吸收布局浮点舍入，不允许移动窗口后继续旧投影。
        // 横竖两个边界都检查，纵向重新排布同样会改变用户命中的图表。
        const bool geometry = std::abs(m_min.x - f.m_min.x) < .5f &&
                              std::abs(m_min.y - f.m_min.y) < .5f &&
                              std::abs(m_max.x - f.m_max.x) < .5f &&
                              std::abs(m_max.y - f.m_max.y) < .5f;
        // 几何、方案、模式或可见帧不连续时只收尾，不将新坐标用于旧投影。
        // Cancel 与第三指均封锁余下触点，绝不退化为轻点或首拍设置。
        if ( !geometry || f.m_generation != m_generation ||
             f.m_edit != m_edit || f.m_frame > m_frame + 1 || raw.m_cancelled ||
             raw.m_peak > 2 || (f.m_edit && raw.m_peak > 1) ||
             f.m_duration <= 0 || f.m_max.x <= f.m_min.x ||
             !std::isfinite(f.m_duration) || !std::isfinite(f.m_center) ||
             !std::isfinite(f.m_zoom) || f.m_zoom <= 0 ) {
            m_blocked      = true;
            out.m_finished = true;
            return out;
        }
        // 有效帧更新连续性基线；无效投影不会再获得后续编辑机会。
        // 宿主传入的时长和半窗已通过有限值检查，后续除法只使用有效宽度。
        m_frame            = f.m_frame;
        const double width = f.m_max.x - f.m_min.x;
        ImVec2       point = m_last;
        if ( raw.m_peak == 2 ) {
            // 任意一指抬起便结束双指导航，余指不能继续编辑或触发一次轻点。
            if ( raw.m_count != 2 ) {
                m_blocked      = true;
                out.m_finished = true;
                return out;
            }
            // 双指只消费当前快照的两项，不保留数组元素的地址。
            // 下一帧触点压缩或交换顺序仍需要核对同一对稳定身份。
            const auto& a  = raw.m_contacts[0];
            const auto& b  = raw.m_contacts[1];
            const auto  pa = pixel(a.m_x, a.m_y), pb = pixel(b.m_x, b.m_y);
            if ( !m_pair ) {
                // pairStart 是第二指加入时冻结的两点，不是两指各自 Down。
                // 两个落点都需位于已取得归属的图表，跨波形／频谱组合不成立。
                const auto sa =
                    pixel(raw.m_pairStart[0].m_x, raw.m_pairStart[0].m_y);
                const auto sb =
                    pixel(raw.m_pairStart[1].m_x, raw.m_pairStart[1].m_y);
                if ( !inside(sa) || !inside(sb) ) {
                    m_blocked      = true;
                    out.m_finished = true;
                    return out;
                }
                // 第二指加入以配对起点为基线，第一指此前的平移已经生效。
                m_last        = { (sa.x + sb.x) * .5f, (sa.y + sb.y) * .5f };
                m_span        = std::hypot(sa.x - sb.x, sa.y - sb.y);
                m_pairDevice  = a.m_device;
                m_pairFinger  = a.m_finger;
                m_otherDevice = b.m_device;
                m_otherFinger = b.m_finger;
                m_pair        = true;
            }
            // 原始数组顺序不是稳定身份；顺序颠倒仍表示同一双指配对。
            // 换成第三个指针则立即结束，不对新的指距做一次大幅缩放。
            const bool same =
                (a.m_device == m_pairDevice && a.m_finger == m_pairFinger &&
                 b.m_device == m_otherDevice && b.m_finger == m_otherFinger);
            const bool swapped =
                (b.m_device == m_pairDevice && b.m_finger == m_pairFinger &&
                 a.m_device == m_otherDevice && a.m_finger == m_otherFinger);
            if ( !same && !swapped ) {
                m_blocked      = true;
                out.m_finished = true;
                return out;
            }
            point = { (pa.x + pb.x) * .5f, (pa.y + pb.y) * .5f };
            // 指距在逻辑像素空间计算，横屏长宽比例不会造成缩放畸变。
            // 极近两指保持原缩放，避免除以零或由微小噪声放大到极限。
            const float span = std::hypot(pa.x - pb.x, pa.y - pb.y);
            // 首尾裁切后，实际可见跨度可能小于名义两倍半窗。
            // 指距倍率作用于实际跨度，避免在零秒附近捏合却只改数字、不缩放图像。
            // 完全静止的双指不归一化半窗，参数与视野都保持原值。
            if ( span > 1 && m_span > 1 &&
                 (span != m_span || point.x != m_last.x) ) {
                const double range =
                    std::min(f.m_duration, f.m_center + f.m_zoom) -
                    std::max(0., f.m_center - f.m_zoom);
                out.m_zoom = std::clamp(range * .5 * m_span / span,
                                        .01,
                                        std::max(.01, f.m_duration * .5));
            }
            // 增量倍率以最后一帧为基线，触达缩放上限后反向移动立即响应。
            // 不保留越界累计值，避免用户需要反向拖很久才能退出上限。
            m_span  = span;
            m_moved = true;
        } else {
            // 稳定身份不依赖数组顺序；正常 Up 的位置单独保存于快照。
            if ( raw.m_count ) {
                const auto& c = raw.m_contacts[0];
                if ( c.m_device != m_device || c.m_finger != m_finger ) {
                    m_blocked      = true;
                    out.m_finished = true;
                    return out;
                }
                point = pixel(c.m_x, c.m_y);
            } else
                point = pixel(raw.m_releaseX, raw.m_releaseY);
            // 使用整个代际最大位移，移开再返回原点仍然是一笔拖动。
            // 轻微抖动不修改视野；超过阈值的第一帧从最初落点开始计算。
            m_moved = m_moved ||
                      std::abs(raw.m_motionX * f.m_display.x) > f.m_slop ||
                      std::abs(raw.m_motionY * f.m_display.y) > f.m_slop;
        }
        // 按下、正常抬手与取消已在前面分流，结果不包含任何取消位置。
        // 拍点宿主可在正常 Up 采纳最后坐标，再立即清除自身捕获。
        out.m_point = point;
        out.m_moved = m_moved;
        out.m_down  = raw.m_count > 0;
        if ( !f.m_edit && m_moved ) {
            // 锚点使用实际裁切的时间窗，靠近音频首尾也沿用相同秒域。
            const double start = std::max(0., f.m_center - f.m_zoom);
            const double end   = std::min(f.m_duration, f.m_center + f.m_zoom);
            const double ratio =
                std::clamp((m_last.x - f.m_min.x) / width, 0., 1.);
            const double anchor = start + ratio * (end - start);
            const double nextRatio =
                std::clamp((point.x - f.m_min.x) / width, 0., 1.);
            if ( m_pair ) {
                // 新半窗由实际跨度生成，直接按指下时间反解可见区间起点。
                // 起点钳制后两端均留在音轨内，放大时不再陷入裁切跨度不变的歧义。
                // 只做固定数量标量运算，不求迭代解、不读取音频或后台时钟。
                const double range = out.m_zoom * 2;
                const double left =
                    std::clamp(anchor - nextRatio * range,
                               0.,
                               std::max(0., f.m_duration - range));
                out.m_center = left + out.m_zoom;
            } else {
                // 单指保持当前半窗，用上一有效位置的像素差即时平移。
                out.m_center = std::clamp(
                    f.m_center - (point.x - m_last.x) / width * (end - start),
                    0.,
                    f.m_duration);
            }
            out.m_navigation = point.x != m_last.x || out.m_zoom != f.m_zoom;
        }
        // 未达到阈值的抖动不积累为平移，也不让拍线在按下帧跳到指尖。
        if ( m_moved || m_pair ) m_last = point;
        // 只有全抬才允许单指轻点；多指已经在前面进入独立结束分支。
        // finished 后保留 epoch 门闩，空闲帧不会重复输出相同轻点。
        if ( !raw.m_count ) {
            out.m_tapped   = !m_moved && !m_pair;
            out.m_finished = true;
            m_blocked      = true;
        }
        return out;
    }

private:
    /// @brief 冻结图表身份和原始代际；外部起点不能延迟取得所有权。
    /// @details 所有状态由 UI 线程写入，跨线程访问会破坏代际边沿契约。
    /// epoch 只跟踪输入，不代表资源身份；切音轨必须由宿主 suspend。
    /// 帧号只用于判断隐藏缺口，不按墙钟时间猜测用户是否完成操作。
    std::uint64_t m_epoch{}, m_generation{}, m_device{}, m_finger{};
    /// @brief 区域归属、起点认领帧和最近有效帧共同防止隐藏后续接。
    int m_owner{}, m_capture{}, m_frame{};
    /// @brief 双指身份与上一有效指距按值保存，不借用原始数组元素。
    std::uint64_t m_pairDevice{}, m_pairFinger{}, m_otherDevice{},
        m_otherFinger{};
    /// @brief 几何冻结至全抬，最后有效点用于计算即时增量。
    ImVec2 m_min{}, m_max{}, m_last{};
    /// @brief 上一有效双指距离，只在稳定身份配对内更新。
    float m_span{};
    /// @brief 一代触点只走一个模式；被取消之后必须等全新 Down。
    bool m_blocked{}, m_moved{}, m_pair{}, m_edit{};
};
}  // namespace MMM::UI
