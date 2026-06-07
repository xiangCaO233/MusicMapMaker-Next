#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace MMM::UI
{

/// @brief BPM 自动测量结果。
struct BpmAutoTimingResult {
    /// @brief 经过常见 BPM 栅格吸附后的 BPM。
    double bpm{ 0.0 };

    /// @brief 原始估算 BPM。
    double rawBpm{ 0.0 };

    /// @brief 原始 BPM 不确定度。
    double rawBpmUncertainty{ 0.0 };

    /// @brief 节奏峰值相对最终 BPM/offset 网格的加权 RMS 不准确度，单位为毫秒。
    double alignmentInaccuracyMs{ 0.0 };

    /// @brief 估计的小节拍数。
    uint32_t signature{ 1 };

    /// @brief 估计的拍内细分数。
    uint32_t division{ 1 };

    /// @brief 检测到的首拍相位，单位为毫秒；负值表示首拍略早于音频 0 点。
    double offsetMs{ 0.0 };
};

/// @brief 从已解码的单声道音频中自动估算 BPM 和首拍相位。
class BpmAutoDetector final
{
public:
    /// @brief 执行自动 BPM/offset 检测。
    /// @param monoSamples 单声道浮点音频采样。
    /// @param sampleRate 采样率。
    /// @return 成功时返回检测结果，否则返回空。
    /// @warning 后台耗时路径：会完整预处理音频并执行自相关 FFT，禁止在
    /// UI、渲染或逻辑热路径中调用。
    static std::optional<BpmAutoTimingResult> detect(
        const std::vector<float>& monoSamples, uint32_t sampleRate);
};

}  // namespace MMM::UI
