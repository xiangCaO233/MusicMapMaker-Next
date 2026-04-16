#pragma once

#include "mmm/Metadata.h"
#include <vector>

namespace MMM
{
enum class TimingEffect {
    // bpm效果
    BPM,
    // scroll效果
    SCROLL,
};

class Timing
{
public:
    Timing()                         = default;
    Timing(Timing&&)                 = default;
    Timing(const Timing&)            = default;
    Timing& operator=(Timing&&)      = default;
    Timing& operator=(const Timing&) = default;
    ~Timing()                        = default;

    /// @brief 时间线所处时间戳
    double m_timestamp{ 0 };

    /// @brief bpm或继承的bpm
    double m_bpm{ 0.0 };

    /// @brief 拍长
    double m_beat_length{ 0.0 };

    /// @brief 时间线效果类型
    TimingEffect m_timingEffect{ TimingEffect::BPM };

    /// @brief 时间线效果参数
    double m_timingEffectParameter{ 0.0 };

    /// @brief 所有时间线元数据
    TimingMetadata m_metadata;

    /// @brief 从osu的字符串读取
    void from_osu_description(std::vector<std::string>& description);

    /// @brief 转换为osu的字符串
    std::string to_osu_description();
};


}  // namespace MMM
