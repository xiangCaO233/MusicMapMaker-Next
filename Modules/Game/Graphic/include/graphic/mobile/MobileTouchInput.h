#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace MMM::Graphic
{
/// @brief 平台原始触点阶段，不向画布传播 SDL 或 Android 类型。
/// @details Cancel 与 Up 不同，系统抢占不能被识别为正常提交。
enum class TouchPhase { Down, Move, Up, Cancel };
/// @brief 设备与手指共同构成稳定身份，坐标相对整个窗口归一化。
/// @details 活跃数组可压缩，消费者必须比较身份而非记住元素地址。
struct TouchContact {
    std::uint64_t m_device{},
        m_finger{};  ///< 不以数组位置或主鼠标身份代替触点身份。
    /// @brief 起点只在 Down 写入，移动后返回原位也能识别曾经发生拖动。
    float m_startX{}, m_startY{};
    float m_x{}, m_y{};  ///< 窗口归一化坐标，由消费方转换为自身 UI 坐标。
};
/// @brief 同一 UI 线程发布的定长多触点快照，不持有任何编辑对象。
/// @details 已结束手势仍保留最高指头数和起点，避免快速多指轻点退化成单击。
struct MobileTouchSnapshot {
    /// @brief 手势内最大归一化位移，用于拒绝已经移动过的多指轻点。
    float m_motionX{}, m_motionY{};
    /// @brief 首次形成双指时冻结的落点；导航不从首指更早的绘制起点跳变。
    std::array<TouchContact, 2>  m_pairStart{};
    static constexpr std::size_t CAPACITY =
        10;  ///< 有界存储，事件洪泛不分配内存。
    std::array<TouchContact, CAPACITY>
                m_contacts{};  ///< 有效触点只占前 count 项。
    std::size_t m_count{},
        m_peak{};  ///< 当前数量与本次手势曾同时出现的最大数量。
    std::uint64_t m_epoch{},
        m_revision{};  ///< 新手势与事件修订分开，空闲帧不伪造变化。
    float m_originX{},
        m_originY{};  ///< 首触点落点，用于确定唯一画布或面板归属。
    /// @brief 最近一次正常 Up 的归一化位置，仅全抬起且单指峰值时作为最终落点。
    float  m_releaseX{}, m_releaseY{};
    double m_started{}, m_updated{};  ///< 平台单调时间，单位秒，不读取墙钟。
    bool   m_cancelled{};             ///< 取消后剩余手指不能重新成为单指编辑。
};
/// @brief 纯值触点跟踪器，测试和 SDL 入口使用同一身份与取消规则。
/// @details 不保存 ImGui 或窗口指针，旋转后可直接取消而不回收资源。
/// 最大触点数固定，失败时取消整段输入，不向业务发布不完整手势。
class MobileTouchTracker
{
public:
    /// @brief 推进一条平台事件，重复按下按位置更新处理。
    /// @warning 事件热路径：最多扫描十项，无分配、锁或后台任务。
    void update(TouchPhase phase, std::uint64_t device, std::uint64_t finger,
                float x, float y, double seconds);
    /// @brief 生命周期中断清除触点，平台不必再补发正常抬手。
    /// @warning 仅写本线程值，不访问渲染或会话状态。
    void cancel();
    /// @brief 借用当前快照，调用方不能跨线程或跨事件泵缓存引用。
    const MobileTouchSnapshot& snapshot() const { return m_state; }

private:
    MobileTouchSnapshot m_state;  ///< 只由原生事件泵或独立测试写入。
    std::size_t
        m_overflow{};  ///< 超容量手指仍须全部抬起，避免重新接管半次手势。
};
/// @brief 借用本 UI 线程原始多触点状态；桌面没有生产者时始终为空。
/// @warning 事件泵与 UI 在同一线程，不能在逻辑或音频线程读取该引用。
const MobileTouchSnapshot& mobileTouchSnapshot();
/// @brief 平台边界发布一条原始触摸，UI 不能用此入口合成编辑命令。
/// @warning 每条原生事件固定规模更新，无原子、互斥或动态分配。
void publishMobileTouch(TouchPhase phase, std::uint64_t device,
                        std::uint64_t finger, float x, float y, double seconds);
/// @brief 窗口失焦、旋转或退后台时取消原始手势，不能伪造正常松手。
void cancelMobileTouch();
}  // namespace MMM::Graphic
