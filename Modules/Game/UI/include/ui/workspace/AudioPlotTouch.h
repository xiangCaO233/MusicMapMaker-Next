#pragma once
#include <cstdint>
#include <imgui.h>
namespace MMM::UI
{
/// @brief 声道图表的实际时间窗与输入许可，不保存快照或音轨指针。
struct AudioPlotTouchFrame {
    ImVec2 m_min{}, m_max{};  ///< 绘图区，排除标题、坐标轴和顶部滑块。
    double m_time{}, m_duration{},
        m_offset{};               ///< 显示中心、音轨时长及通用偏移。
    std::uintptr_t m_instance{};  ///< 正式谱面实例令牌。
    int            m_channel{};   ///< 同一图表内两个声道共享唯一手势。
    bool           m_hovered{}, m_available{};  ///< 起点归属与持续许可分开。
};
/// @brief 音频图表触控导航，桌面输入由宿主保留原实现。
/// @details 状态全部按值保存，无 PCM、纹理或会话所有权。
class AudioPlotTouch
{
public:
    /// @brief 每帧解析本地浏览中心；隐藏、换谱或方案变化结束旧导航。
    /// @warning 高频入口只访问标量，不等待逻辑快照或重建音频缓存。
    double begin(double visualTime, std::uintptr_t instance);
    /// @brief 单指轻点定位、横向浏览、双指平移与锚点缩放。
    /// @param zoom 输出图表左右半窗秒数，沿用原 0.1～10 秒范围。
    /// @warning 每个声道每帧固定触点运算；命令只在有效增量或结束边沿发出。
    void update(const AudioPlotTouchFrame& frame, float& zoom);
    /// @brief 关闭、换项目或重新计算时提交最后有效目标，拒绝残留抬手。
    void suspend();

private:
    /// @brief 一次结束只发一条最终 Seek，不发送鼠标或编辑历史命令。
    void finish();
    /// @brief 使用唯一实例和有限时间发布正式定位命令。
    void seek(double audioTime, bool scrubbing);
    /// @brief 原始触摸代际、方案代数和谱面实例共同隔离输入。
    std::uint64_t  m_epoch{}, m_generation{};
    std::uintptr_t m_instance{};
    /// @brief 声道仅在首次观察代际的帧内认领。
    int m_owner{ -1 }, m_captureFrame{}, m_frame{};
    /// @brief 冻结实际几何，重排或缩放窗口不能沿用旧坐标。
    ImVec2 m_min{}, m_max{};
    /// @brief 连续浏览累计中心；取消不读取取消事件坐标。
    double m_center{}, m_lastSeek{};
    /// @brief 单指增量与双指配对基线分别记录。
    float         m_lastX{}, m_lastY{}, m_span{}, m_pairZoom{};
    std::uint64_t m_deviceA{}, m_fingerA{}, m_deviceB{}, m_fingerB{};
    /// @brief 一代触点只能由轻点、单指或双指其中一条路径结束。
    bool m_blocked{}, m_moved{}, m_pair{}, m_running{};
};
}  // namespace MMM::UI
