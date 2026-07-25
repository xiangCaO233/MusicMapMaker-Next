#pragma once

#include <algorithm>

namespace MMM::Audio
{

/// @brief 单个音频帧的左右声道增益。
struct StereoGain {
    /// @brief 左声道增益。
    float left{ 1.0F };
    /// @brief 右声道增益。
    float right{ 1.0F };
};

/// @brief 单次音效从开始到结束的线性双声道增益包络。
struct StereoGainEnvelope {
    /// @brief 音效开始时的左声道增益。
    float startLeft{ 1.0F };
    /// @brief 音效开始时的右声道增益。
    float startRight{ 1.0F };
    /// @brief 音效结束时的左声道增益。
    float endLeft{ 1.0F };
    /// @brief 音效结束时的右声道增益。
    float endRight{ 1.0F };
};

/// @brief 计算线性包络在指定归一化进度处的左右声道增益。
/// @param envelope 双声道增益包络。
/// @param progress 音效播放进度，读取时限制到 0 到 1。
/// @return 插值后的左右声道增益。
[[nodiscard]] inline StereoGain stereoGainAtProgress(
    const StereoGainEnvelope& envelope, float progress)
{
    const float ratio = std::clamp(progress, 0.0F, 1.0F);
    return {
        envelope.startLeft + (envelope.endLeft - envelope.startLeft) * ratio,
        envelope.startRight + (envelope.endRight - envelope.startRight) * ratio,
    };
}

}  // namespace MMM::Audio
