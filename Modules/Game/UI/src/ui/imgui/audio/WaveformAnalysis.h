#pragma once

#include "audio/OfflineAnalysisEQ.h"

#include <cstdint>
#include <future>
#include <memory>
#include <stop_token>
#include <vector>

namespace ice
{
class AudioTrack;
class ThreadPool;
}  // namespace ice

namespace MMM::UI
{

/// @brief 一段固定时长内用于双声道绘图的四个极值。
struct WaveformBin {
    float minL{ 0.0F };  ///< 左声道下界，静音基线为零。
    float maxL{ 0.0F };  ///< 左声道上界，静音基线为零。
    float minR{ 0.0F };  ///< 右声道下界，静音基线为零。
    float maxR{ 0.0F };  ///< 右声道上界，静音基线为零。
};

/// @brief 在 UI 线程观察同一份波形请求，后台仅处理独立拥有的输入快照。
/// 关闭和来源变化丢弃待交付句柄并发出停止请求，不等待任务或解码线程退出。
class WaveformAnalysis
{
public:
    /// @brief 视野取样与离线统计使用同一时间桶密度。
    static constexpr double POINTS_PER_SECOND{ 100.0 };

    /// @brief 销毁观察端前使当前请求失效，不阻塞后台解码。
    ~WaveformAnalysis();

    /// @brief 提交一次完整包络重算，保留同一音轨已接收的包络。
    /// @param track 调用方持有的活动音轨，只在提交时为后台复制拥有权。
    /// @param duration 工程的音频时长，单位为秒。
    /// @param eq 为本次任务获取的整组 EQ 参数。
    /// @param eqRevision 与参数快照匹配的控制线程代次。
    /// @param filePool 处理全曲桶统计的文件线程池。
    /// @param decodePool 解码池，须与文件池分离且后于文件池关闭。
    void start(const std::shared_ptr<ice::AudioTrack>& track, double duration,
               Audio::OfflineAnalysisEQSettings eq, std::uint64_t eqRevision,
               ice::ThreadPool& filePool, ice::ThreadPool& decodePool);

    /// @brief 按当前音轨、时长及 EQ 代次检查已完成结果，未完成时立即返回。
    /// @warning UI 每帧调用；只轮询 future，任务未就绪时禁止等待。
    void poll(const std::shared_ptr<ice::AudioTrack>& track, double duration,
              std::uint64_t eqRevision);

    /// @brief 关闭窗口时取消未完成请求，保留此前接收的同音轨包络。
    void cancel();

    /// @brief 工程或音轨丢失时同时清除请求和包络。
    void reset();

    /// @brief 提示窗口是否仍有有效待交付请求。
    /// @warning 每帧读取，只查询 UI 独占的 future 句柄。
    [[nodiscard]] bool isRunning() const { return m_future.valid(); }

    /// @brief 借用本帧的只读包络，用于抽取可见时间窗。
    /// @warning 每帧读取，引用不可跨越下一次轮询或重置。
    [[nodiscard]] const std::vector<WaveformBin>& bins() const
    {
        return m_bins;
    }

private:
    /// @brief 音轨所有权标识或时间线长度变化时丢弃旧来源的数据。
    /// @warning UI 每帧调用；仅比较控制块，不锁定弱引用或持有音轨。
    void retarget(const std::shared_ptr<ice::AudioTrack>& track,
                  double                                  duration);

    /// @brief 活动来源的所有权标识，防止音轨销毁并在同地址创建后误命中。
    std::weak_ptr<ice::AudioTrack> m_track;
    /// @brief 统计与显示共同使用的工程音频时长。
    double m_duration{ 0.0 };
    /// @brief 当前请求读取的 EQ 代次，仅由 UI 线程管理。
    std::uint64_t m_eqRevision{ 0U };
    /// @brief 仅保留最新有效的交付句柄，丢弃旧句柄后结果不可回写到视图。
    std::future<std::vector<WaveformBin>> m_future;
    /// @brief 停止令牌随请求传入后台，全曲扫描按块检查取消。
    std::stop_source m_stopSource{ std::nostopstate };
    /// @brief 等宽时间桶，只有完整且匹配当前来源的结果能替换此处。
    std::vector<WaveformBin> m_bins;
};

}  // namespace MMM::UI
