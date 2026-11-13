#include "ui/workspace/AudioPlotTouch.h"
#include "common/LogicCommands.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "ui/interaction/EditorInteraction.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
namespace MMM::UI
{
namespace
{
/// @brief 半开区域使相邻声道不会同时认领边界点。
bool inside(ImVec2 p, ImVec2 min, ImVec2 max)
{
    return p.x >= min.x && p.x < max.x && p.y >= min.y && p.y < max.y;
}
}  // namespace
/// @brief 图表跟随真实播放；活动浏览则立即显示本地已经提交的中心。
/// @warning 每帧固定标量读取，不使用超时、睡眠或快照等待。
double AudioPlotTouch::begin(double time, std::uintptr_t instance)
{
    // 方案代数与谱面实例分别核对，同名方案被重置也会结束旧手势。
    // 帧号检测隐藏缺口，不用猜测的时间窗口延迟导航。
    // 上次收尾仍带旧实例，后台切谱后由正式控制器拒绝。
    // 新实例只在收尾之后写入，不能把旧最终定位发给新谱面。
    const auto generation = Interaction::profileGeneration();
    const int  frame      = ImGui::GetFrameCount();
    if ( !Interaction::activeProfile().touchGestures() ||
         instance != m_instance || generation != m_generation ||
         (m_frame && frame > m_frame + 1) )
        suspend();
    m_instance   = instance;
    m_generation = generation;
    m_frame      = frame;
    // 没有连续导航时跟随正式播放时钟，不把历史点击变成永久本地视野覆盖。
    // 活动拖动的每个增量则先更新本地中心，不等后台重新发布快照。
    if ( !m_running ) m_center = time;
    return m_center;
}
/// @brief 命令带实例约束，排队期间切谱也不能定位新的活动谱面。
void AudioPlotTouch::seek(double time, bool scrubbing)
{
    if ( !m_instance || !std::isfinite(time) ) return;
    // 本地只缓存已通过有限值检查的最后目标。
    // 连续预览可以合并联机发送，结束命令则立即提交最终状态。
    // 主画布物件历史不参与 Seek，不会清空既有重做栈。
    m_lastSeek = time;
    Interaction::dispatchCommand(Event::LogicCommandEvent(
        Logic::CmdSeek{ .time              = time,
                        .isScrubbing       = scrubbing,
                        .beatmapInstanceId = m_instance }));
}
/// @brief 正常释放和取消共用最后有效位置，绝不使用取消坐标重新计算。
void AudioPlotTouch::finish()
{
    // 静止按住没有进入 Scrub，不能因为关闭窗口而补一次定位。
    // 连续拖动则必须在窗口不可见之前释放正式 Scrub 状态。
    // 先发送最后目标再清门闩，重复结束没有业务副作用。
    if ( m_running ) seek(m_lastSeek, false);
    m_running = false;
}
/// @brief 外部关闭只收尾已经开始的定位，待定轻点直接丢弃。
void AudioPlotTouch::suspend()
{
    finish();
    // 同一代际恢复显示时仍拒绝旧 Up，新的 Down 才能重新建立归属。
    // 不清理全局原始触点，避免取消本窗口时影响其他窗口的新输入。
    // 结束只保留最后有效导航，不使用 Undo 回滚已显示的浏览。
    m_epoch   = Graphic::mobileTouchSnapshot().m_epoch;
    m_owner   = -1;
    m_blocked = true;
}
/// @brief 触点归属在声道间共享，单指纵向意图交回页面滚动。
/// @warning UI 热路径不扫描音轨或窗口树，不申请所有权或进入会话锁。
void AudioPlotTouch::update(const AudioPlotTouchFrame& f, float& zoom)
{
    if ( !Interaction::activeProfile().touchGestures() ) return;
    const auto& raw = Graphic::mobileTouchSnapshot();
    const auto& io  = ImGui::GetIO();
    // 归一化坐标分别乘两轴逻辑尺寸，不使用 framebuffer DPI。
    // 两轴单位一致之后才能求指距，长屏手机不能直接用归一化欧氏距离。
    // 同一触点的设备和指针身份在后续配对检查中保持独立。
    /// @brief 按当前 UI 逻辑像素将固定大小的触点坐标转为空间点。
    const auto pixel = [&](float x, float y) {
        return ImVec2{ x * io.DisplaySize.x, y * io.DisplaySize.y };
    };
    const ImVec2 origin = pixel(raw.m_originX, raw.m_originY);
    const int    now    = ImGui::GetFrameCount();
    if ( !raw.m_epoch ) return;
    if ( raw.m_epoch != m_epoch ) {
        finish();
        // 新代际先结束上一笔，避免快速换手让两个声道同时拥有 Scrub。
        // 归属只开放本帧，其他窗口开始的输入以后经过图表也不能认领。
        m_epoch        = raw.m_epoch;
        m_captureFrame = now;
        m_owner        = -1;
        m_blocked = m_moved = m_pair = false;
    }
    // 只在首次观察这一代的帧内允许两个声道依次检查原始起点。
    // 外部起点不能在后续移入图表时被认领，第二个声道也不能抢走首个所有者。
    if ( m_owner < 0 && !m_blocked && m_captureFrame == now && f.m_hovered &&
         f.m_available && inside(origin, f.m_min, f.m_max) ) {
        // 所有者在整代际固定，另一声道后提交也不能覆盖它。
        // 标题和外部控件不在绘图区，按下滑块不会被认成图表轻点。
        // 中心从这一帧实际显示的数据起步，不能借用音轨的旧载入位置。
        m_owner  = f.m_channel;
        m_min    = f.m_min;
        m_max    = f.m_max;
        m_lastX  = origin.x;
        m_lastY  = origin.y;
        m_center = f.m_time;
    }
    // 同一帧每个声道都会经过这里，只有已经冻结的所有者可交付增量。
    // 后提交的另一声道不能因为指尖当前位于其上而切换目标。
    // 所有者在完整代际结束前保持不变，与音轨左右声道数据刷新无关。
    if ( m_owner != f.m_channel || m_blocked ) return;
    // 冻结的是绘图区，不包含声道标题、坐标轴标签和控制条。
    // 窗口重排会改变投影口径，即使手指位置不变也必须中断。
    // 几何只存值，不跨帧借用 ImGui 窗口或绘图上下文。
    const float width    = f.m_max.x - f.m_min.x;
    const bool  geometry = std::abs(m_min.x - f.m_min.x) < .5f &&
                           std::abs(m_min.y - f.m_min.y) < .5f &&
                           std::abs(m_max.x - f.m_max.x) < .5f &&
                           std::abs(m_max.y - f.m_max.y) < .5f;
    // Cancel、第三指、跨实例以及窗口重排都只结束已有浏览。
    // 未成立的轻点没有最终 Seek，剩余指和抬手也不能重新激活。
    if ( !f.m_available || f.m_instance != m_instance || !geometry ||
         raw.m_cancelled || raw.m_peak > 2 || width <= 1 ||
         !std::isfinite(width) || !std::isfinite(zoom) || zoom < .1f ||
         zoom > 10 || !std::isfinite(f.m_time) || !std::isfinite(f.m_offset) ||
         !std::isfinite(f.m_duration) || f.m_duration <= 0 ) {
        finish();
        m_blocked = true;
        return;
    }
    // 容差跟随字体缩放，保留手机上的触摸命中面积。
    // 时间窗口缩放只改变每像素秒数，不修改频率或振幅轴。
    // 只有实际增量才发布命令，静止手指不会产生忙循环。
    const float slop = std::max(8.0f, ImGui::GetFontSize() * .5f);
    float       x{}, anchor = m_lastX, next = zoom;
    if ( raw.m_peak == 2 ) {
        // 双指任意一指结束后，余下触点只能等待全抬，不能变回单指点击。
        // 这里不提供二指撤销或三指重做，导航不能修改编辑历史。
        if ( raw.m_count != 2 ) {
            finish();
            m_blocked = true;
            return;
        }
        const auto& a = raw.m_contacts[0];
        const auto& b = raw.m_contacts[1];
        // 首次配对来自第二指加入时的两点，不能复用第一指更早的 Down。
        // 两点都属于同一声道，跨上下声道的手势整体拒绝。
        // 旧单指增量已应用到中心，配对不额外补发一次平移。
        if ( !m_pair ) {
            const auto& sa = raw.m_pairStart[0];
            const auto& sb = raw.m_pairStart[1];
            const auto  pa = pixel(sa.m_x, sa.m_y), pb = pixel(sb.m_x, sb.m_y);
            if ( !inside(pa, m_min, m_max) || !inside(pb, m_min, m_max) ) {
                finish();
                m_blocked = true;
                return;
            }
            // 配对开始时不跳转；第一指此前的单指增量已经提交。
            m_span     = std::hypot(pa.x - pb.x, pa.y - pb.y);
            m_pairZoom = zoom;
            m_lastX    = (pa.x + pb.x) * .5f;
            m_lastY    = (pa.y + pb.y) * .5f;
            m_deviceA  = a.m_device;
            m_fingerA  = a.m_finger;
            m_deviceB  = b.m_device;
            m_fingerB  = b.m_finger;
            m_pair     = true;
        }
        // 多设备或换指不能复用旧指距基线，固定身份使输出可逆。
        // 触点数组顺序由原始跟踪器保持，不能按当前坐标重新排序。
        if ( a.m_device != m_deviceA || a.m_finger != m_fingerA ||
             b.m_device != m_deviceB || b.m_finger != m_fingerB ) {
            finish();
            m_blocked = true;
            return;
        }
        const auto  pa = pixel(a.m_x, a.m_y), pb = pixel(b.m_x, b.m_y);
        const float span   = std::hypot(pa.x - pb.x, pa.y - pb.y),
                    change = span - m_span;
        const float dead   = std::max(1.5f * slop, m_span * .08f);
        // 死区只从累计跨度变化中扣除一次，不能每帧反复缩小倍率。
        // 达到原有最小或最大半窗后保持边界，反向捏合仍从同一基线计算。
        // 横向中点平移与跨度变化分别计算，纯平移不能悄悄改变半窗。
        const float pinch =
            std::copysign(std::max(std::abs(change) - dead, 0.0f), change);
        // 张开缩小时间窗；死区以初始指距为基准，回合不会逐帧累积误差。
        if ( m_span > slop )
            next =
                std::clamp(m_pairZoom * m_span / std::max(slop, m_span + pinch),
                           .1f,
                           10.0f);
        x       = (pa.x + pb.x) * .5f;
        anchor  = m_lastX;
        m_moved = true;
    } else {
        const float dx = raw.m_motionX * io.DisplaySize.x,
                    dy = raw.m_motionY * io.DisplaySize.y;
        // 先判定意图：纵向滑动留给父页面，图表不得顺便改变播放位置。
        // 一旦开始横向浏览，即使后来方向改变也不重新判成轻点。
        if ( !m_moved && std::max(dx, dy) > slop ) {
            if ( dy > dx ) {
                finish();
                m_blocked = true;
                return;
            }
            m_moved = true;
        }
        // 正常 Up 的坐标独立于已移除触点数组，快速合帧仍能计算尾增量。
        // Cancel 在前面已经拒绝，不能把系统接管坐标当作合法落点。
        x = raw.m_count ? raw.m_contacts[0].m_x * io.DisplaySize.x
                        : raw.m_releaseX * io.DisplaySize.x;
        // 只有未超死区且正常全抬才是轻点，停留时间不改变定位语义。
        // 多指峰值已经进入独立分支，不能在这里只看剩余手指数。
        // 点击音频时间由实际显示窗反投影，不依赖上一帧 Hover 或资源路径。
        if ( !raw.m_count && !m_moved ) {
            const double target =
                f.m_time +
                (std::clamp((x - f.m_min.x) / width, 0.0f, 1.0f) - .5) * 2 *
                    zoom;
            // 音频边界与视觉偏移分开处理，采样专用偏移不能进入 Seek。
            // 轻点没有预览阶段，只提交一次最终目标。
            // 业务控制器继续负责音频、播放偏好及协作同步。
            seek(std::clamp(target - f.m_offset, 0.0, f.m_duration), false);
            m_blocked = true;
            return;
        }
        if ( !m_moved ) return;
    }
    // 位移只保留本帧尚未消费的部分，不能把整段累计距离重复积分。
    // 双指配对时已重设上一中点，所以第二指加入不会产生跳变。
    // 移动到边界之外也只进行时间夹取，不把横向越界转成频率缩放。
    const float delta = x - m_lastX;
    // 保持旧中点所指时间，再按新尺度应用横向位移，横移不改变频率轴。
    // 只改本图表半窗和正式播放时间，不改主画布倍率、物件或历史栈。
    if ( std::abs(delta) > .001f || std::abs(next - zoom) > .00001f ) {
        // 锚点以增量前的中点表达，先缩放再按新尺度平移。
        // 同一笔中心在 UI 本地累计，不反复覆盖为滞后的逻辑快照。
        // 曲头曲尾夹取使用有效音轨长度，超出边界不会形成无穷时间。
        // 此处只做浏览投影，不能遍历物件、重算 FFT 或重建整段波形。
        const double at = (anchor - f.m_min.x) / width - .5;
        m_center += at * 2 * (zoom - next) - delta / width * 2 * next;
        // 本地中心先夹取，画面反馈不能等待音频线程纠正越界目标。
        // 最终命令继续走正式入口，界面不直接调用音频设备。
        // 缩放值只归本图表所有，不广播完整的全局编辑配置。
        m_center = std::clamp(m_center, f.m_offset, f.m_offset + f.m_duration);
        zoom     = next;
        seek(m_center - f.m_offset, true);
        m_running = true;
    }
    m_lastX = x;
    // Up 自身的尾坐标先提交，再结束；合帧快速拖动也不会丢掉最终位置。
    if ( !raw.m_count ) {
        finish();
        m_blocked = true;
    }
}
}  // namespace MMM::UI
