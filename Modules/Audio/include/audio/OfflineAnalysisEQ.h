#pragma once

#include <ice/core/effect/filter/BiquadFilter.hpp>

#include <cstdint>
#include <numbers>
#include <vector>

namespace ice
{
class AudioBuffer;
}

namespace MMM::Audio
{
class AudioManager;

/// @brief 一次分析任务中单个 EQ 频段的设置。
struct OfflineAnalysisEQBand {
    /// @brief 峰值均衡器中心频率，单位为 Hz。
    float frequency{ 0.0F };
    /// @brief 峰值提升或衰减，单位为 dB。
    float gain{ 0.0F };
    /// @brief 品质因数，必须为正；默认值与引擎一致。
    float q{ std::numbers::sqrt2_v<float> };
};

/// @brief 按音轨均衡器顺序保存整组频段；空数组表示直接分析原始 PCM。
using OfflineAnalysisEQSettings = std::vector<OfflineAnalysisEQBand>;

/// @brief 在控制线程捕获一次完整的主轨均衡器参数。
/// @param audio 当前编辑器的音频管理器。
/// @return 自包含的参数快照，可按值传给分析工作线程。
/// @warning 低频任务入口调用，逐频段读取控制侧参数，不得从普通绘制分支调用。
OfflineAnalysisEQSettings captureOfflineAnalysisEQ(const AudioManager& audio);

/// @brief 带有独立声道历史的离线 EQ；一个实例仅由一个工作者串行使用。
/// 用 ICE 的峰值滤波器保持与播放 EQ 相同的处理顺序和递推精度。
class OfflineAnalysisEQ
{
public:
    /// @brief 按一份参数快照一次性配置滤波链，各声道分别保留零初值历史。
    /// @param settings 从任务入口捕获的有序频段快照。
    /// @param sampleRate 实际待处理 PCM 的采样率。
    /// @param channels 实际待处理 PCM 的声道数。
    /// @warning 构造会分配内存，仅在开始离线分析时执行。
    OfflineAnalysisEQ(const OfflineAnalysisEQSettings& settings,
                      double sampleRate, std::uint16_t channels);

    /// @brief 原位滤波并保留跨块历史。禁用 EQ 时保留 PCM。
    /// @param buffer 声道数须与构造一致，帧数为本次待处理的有效帧数。
    /// @warning 每个离线工作块调用，不能分配内存或获取控制锁。
    void process(ice::AudioBuffer& buffer);

private:
    /// @brief 每个声道独立的滤波器链，系数在构造后固定。
    std::vector<std::vector<ice::BiquadFilter>> m_channelChains;
};
}  // namespace MMM::Audio
