#pragma once

#include "graphic/mobile/MobileTouchInput.h"
#include <cmath>
#include <cstdint>
#include <imgui.h>

namespace MMM::Canvas
{
/// @brief 元素编辑只接收单指，按下捕获与实际修改分别报告。
/// @details m_pressed 只冻结身份，不能因命中带扩大而直接重写边界。
/// m_moved 由整代触点最大位移决定，移开后回原位仍属于拖动。
/// m_finished 与 m_moved 可以同时成立，表示正常 Up 携带最后合法位置。
/// 取消只报告 finished；宿主不能读取取消位置来完成一次修改。
/// @details 此值不拥有布局；取消只结束输入，宿主保留最后合法配置。
struct LayoutTouchResult {
    ImVec2 m_point{};  ///< 屏幕逻辑坐标，首次报告原点供宿主冻结几何。
    bool   m_pressed{}, m_moved{}, m_finished{};  ///< 没有移动不允许写布局。
};
/// @brief 一次布局编辑的投影和身份，均来自正在显示的画布。
/// @details origin 和 size 使用逻辑像素，不使用离屏纹理的物理分辨率。
/// display 只将平台归一化坐标转换到同一逻辑像素空间。
/// instance 区分同路径重新载入，keyCount 防止旧指针修改新的键数配置。
/// generation 属于操作方案，不能以窗口标题代替输入身份。
struct LayoutTouchFrame {
    ImVec2 m_origin{}, m_size{}, m_display{};  ///< 起点命中及显示尺寸门禁。
    /// @brief 稳定窗口与停靠身份，允许同节点随配置发布采纳尺寸。
    ImGuiID       m_window{}, m_dock{};
    std::uint64_t m_instance{}, m_generation{};  ///< 换谱与操作方案的门禁。
    int   m_frame{}, m_keyCount{};  ///< 隐藏缺帧和键数变化使原捕获失效。
    float m_slop{};                 ///< 逻辑像素容差，静止轻点不产生配置修改。
    bool  m_allowed{};              ///< 仅新代际采样的实际窗口命中结果。
};
/// @brief 不依赖合成鼠标或长按计时的布局单指输入。
/// @details 双指不在此工具导航，系统接管后余指也不能继续修改。
/// 快速拖动先报告起点，下一帧补交真实终点，不等待逻辑快照回流。
/// 本类没有谱面或预设引用，目标命中与边界约束由原布局处理器执行。
/// 每个画布拥有独立对象，外部落点也必须记录为封锁的代际。
/// 隐藏后漏掉一帧就拒绝旧投影，恢复显示不能从旧坐标继续编辑。
class LayoutTouchInput
{
public:
    /// @brief 工具退出或外部弹窗接管时封锁旧代际。
    void suspend(std::uint64_t epoch)
    {
        m_epoch   = epoch;
        m_blocked = true;
    }
    /// @brief 新代际才需要宿主按原始落点查询窗口遮挡。
    /// @warning 每帧只读本线程标量，不访问平台。
    std::uint64_t epoch() const { return m_epoch; }
    /// @brief 捕获期间阻止相机及普通画布手势接管。
    /// @warning 每帧固定标量读取，无共享所有权或同步。
    bool active() const { return m_epoch && !m_blocked; }
    /// @brief 将原始单指推进为布局边沿，不写配置或产生反馈音效。
    /// @param raw 同 UI 线程借用的固定容量快照，结束后仍保留整代峰值。
    /// @param frame 当前可见的画布投影与键数身份，不能使用后台旧窗口。
    /// @return 捕获、合法移动和结束分别报告；宿主按此顺序执行原布局算法。
    /// @warning 每帧固定数量计算，无分配、锁、计时等待或文件操作。
    LayoutTouchResult update(const Graphic::MobileTouchSnapshot& raw,
                             const LayoutTouchFrame&             frame)
    {
        LayoutTouchResult result;
        if ( !raw.m_epoch ) return result;
        const ImVec2 origin{ raw.m_originX * frame.m_display.x,
                             raw.m_originY * frame.m_display.y };
        if ( raw.m_epoch != m_epoch ) {
            // 外部落点从一开始封锁，途中移入不能重新认领。
            m_epoch   = raw.m_epoch;
            m_blocked = !frame.m_allowed || origin.x < frame.m_origin.x ||
                        origin.y < frame.m_origin.y ||
                        origin.x >= frame.m_origin.x + frame.m_size.x ||
                        origin.y >= frame.m_origin.y + frame.m_size.y;
            m_frame   = frame;
            m_moved   = false;
            m_device  = raw.m_contacts[0].m_device;
            m_finger  = raw.m_contacts[0].m_finger;
            result.m_pressed = !m_blocked;
        }
        // 已封锁的代际只能由下一次真实 Down 解开；普通空闲帧不会复活它。
        // 不清除平台快照，其他 UI 仍可独立读取按钮和系统手势。
        if ( m_blocked ) return result;
        // 布局发布会让同一停靠节点重新采纳 padding 和工作台分割尺寸。
        // 不能按几个像素的自发内容变化取消刚开始的拖动；原业务仍用当前投影。
        // 旋转看显示尺寸，换窗口／停靠节点看稳定 ID，隐藏看连续帧门禁。
        const bool geometry = frame.m_window == m_frame.m_window &&
                              frame.m_dock == m_frame.m_dock &&
                              frame.m_display.x == m_frame.m_display.x &&
                              frame.m_display.y == m_frame.m_display.y;
        if ( raw.m_cancelled || raw.m_peak != 1 || !geometry ||
             frame.m_frame > m_frame.m_frame + 1 ||
             frame.m_instance != m_frame.m_instance ||
             frame.m_generation != m_frame.m_generation ||
             frame.m_keyCount != m_frame.m_keyCount || frame.m_size.x <= 0 ||
             frame.m_size.y <= 0 ||
             (raw.m_count && (raw.m_contacts[0].m_device != m_device ||
                              raw.m_contacts[0].m_finger != m_finger)) ) {
            // 不交付系统取消坐标，也不将双指结束误当成单指释放。
            m_blocked = true;
            return { .m_finished = true };
        }
        // 只推进连续帧编号，原始几何和资源身份始终维持捕获时的值。
        // 若每帧覆盖整份 Frame，旋转后就无法检查前后投影是否一致。
        m_frame.m_frame = frame.m_frame;
        if ( result.m_pressed ) {
            // 即使 Down/Move/Up 合帧，也先冻结命中目标再处理终点。
            result.m_point = origin;
            return result;
        }
        // 静止按住不按时间自动进入编辑，也不会弹出通用选择菜单。
        // 使用累计最大位移使原路返回不能伪装成一次无移动的轻点。
        m_moved = m_moved ||
                  std::abs(raw.m_motionX * frame.m_display.x) > frame.m_slop ||
                  std::abs(raw.m_motionY * frame.m_display.y) > frame.m_slop;
        // 活跃触点用稳定身份检查过的元素；全抬起则只采正常释放坐标。
        // 数组被平台压缩后的残留元素不作为终点使用。
        const auto& contact = raw.m_contacts[0];
        result.m_point      = raw.m_count
                                  ? ImVec2(contact.m_x * frame.m_display.x,
                                           contact.m_y * frame.m_display.y)
                                  : ImVec2(raw.m_releaseX * frame.m_display.x,
                                           raw.m_releaseY * frame.m_display.y);
        result.m_moved      = m_moved;
        // 正常 Up 可交付最后位移；之后的空闲帧不再次保存或播放释放音。
        result.m_finished = raw.m_count == 0;
        if ( result.m_finished ) m_blocked = true;
        return result;
    }

private:
    LayoutTouchFrame m_frame;  ///< 冻结投影及最近连续帧，不持有窗口。
    std::uint64_t    m_epoch{}, m_device{}, m_finger{};  ///< 稳定物理身份。
    bool m_blocked{}, m_moved{};  ///< 单向封锁，只有新的 Down 可解开。
};
}  // namespace MMM::Canvas
