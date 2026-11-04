#pragma once
#include <imgui.h>

namespace MMM::Canvas
{
/// @brief 移动画布在起笔之前区分轻点、拖动与长按，不撤销已执行的编辑。
/// @details 只有移动超过容差才交给连续工具；等待阶段不发送任何起笔命令。
/// 这层只判断意图，不解释当前工具，因此框选与画笔具有相同的安全边界。
/// 移动开始时仍回放最初落点，避免拖动抓取到经过的另一个物件。
/// 回放是非阻塞的逐帧推进，不等待逻辑线程确认。
/// 快照到达延迟不能影响本地手势分类，时钟只用于长按判定。
class MobileCanvasGesture
{
public:
    /// @brief 本帧输入呈现方式，Begin/Release 只在原控制器调用期间回放。
    enum class Output { Suppress, Pass, Begin, Release, Menu };
    /// @brief 从平台真实触点推进一次，不访问会话、窗口或逻辑线程。
    /// @warning 每帧常量时间值运算，无等待、分配或所有权复制。
    /// @param clicked 原生按下边沿，不能使用合成回放的边沿。
    /// @param down 原生触点是否仍按住。
    /// @param released 原生释放边沿，快速触摸也以此完成。
    /// @param point 当前屏幕坐标，只缓存本次手势所需的三个坐标。
    /// @param time 单调 UI 时间，由调用方提供便于确定性测试。
    /// @param tolerance 已转换为屏幕逻辑像素的移动容差。
    /// @param eligible 当前窗口、播放和遮罩允许新编辑意图。
    Output update(bool clicked, bool down, bool released, ImVec2 point,
                  double time, float tolerance, bool eligible)
    {
        // 轻点的起笔与收尾分属两个 UI 帧，使原工具完整看到两次边沿。
        // 同帧到达的新触点先锁存，不能因上一轻点的回放被吃掉。
        if ( m_releaseQueued ) {
            m_releaseQueued = false;
            m_replay        = m_end;
            if ( clicked && eligible ) begin(point, time);
            return Output::Release;
        }
        if ( m_editing ) {
            // 认定拖动后不再回到长按候选；回到起点也不会误开菜单。
            if ( released || !down ) m_editing = false;
            return Output::Pass;
        }
        if ( !m_pending ) {
            if ( clicked && eligible ) begin(point, time);
            return Output::Suppress;
        }
        if ( !eligible ) {
            // 切换谱面、播放、遮罩或失去窗口后丢弃候选，不能补发轻点。
            m_pending = false;
            return Output::Suppress;
        }
        const float dx = point.x - m_origin.x, dy = point.y - m_origin.y;
        if ( dx * dx + dy * dy > tolerance * tolerance || released || !down ) {
            // 回放起点而非当前位置，保持选中对象与长条起拍的原有语义。
            m_pending       = false;
            m_replay        = m_origin;
            m_editing       = down;
            m_releaseQueued = !down;
            m_end           = point;
            return Output::Begin;
        }
        if ( time - m_started >= 0.45 ) {
            // 长按只产生菜单意图，原编辑器从未看到按下边沿。
            m_pending = false;
            return Output::Menu;
        }
        return Output::Suppress;
    }
    /// @brief 外部弹层接管时取消尚未交给工具的候选。
    void cancelPending() { m_pending = false; }
    /// @brief 输入归属包括等待与回放阶段，摄像机不可抢占。
    bool active() const { return m_pending || m_editing || m_releaseQueued; }
    /// @brief 返回当前回放落点，生命周期仅为下一次 update 之前。
    ImVec2 replayPoint() const { return m_replay; }

private:
    /// @brief 新触点只锁存屏幕起点和时钟，等待移动或释放决定语义。
    void begin(ImVec2 point, double time)
    {
        m_pending = true;
        m_origin  = point;
        m_started = time;
    }
    ImVec2 m_origin{}, m_end{},
        m_replay{};      ///< 当前触点与一次轻点回放的值坐标。
    double m_started{};  ///< 单调 UI 时钟，不使用定时线程或 sleep。
    bool   m_pending{}, m_editing{}, m_releaseQueued{};  ///< 三个互斥推进阶段。
};
}  // namespace MMM::Canvas
