#pragma once

#include "mmm/sample/AudioSample.h"

#include <cstdint>
#include <string>

namespace MMM::Logic
{

/// @brief ECS 中的自动采样物件，内部时间统一使用秒。
struct SampleComponent {
    /// @brief 采样锚点时间，单位秒。
    double m_timestamp{ 0.0 };

    /// @brief 相对锚点的有符号播放偏移，单位毫秒。
    std::int64_t m_offsetMs{ 0 };

    /// @brief 玩家区与 BGM 区统一空间中的绝对轨道索引。
    std::uint32_t m_track{ 0 };

    /// @brief 项目音频资源 ID；空字符串表示尚未绑定资源的静音草稿。
    std::string m_audioResourceId;

    /// @brief 采样物件播放音量。
    float m_volume{ 1.0F };

    /// @brief 来源格式无法映射到通用字段的元数据。
    ::MMM::SampleMetadata m_metadata;

    /// @brief 采样物件在保存与协作同步间保持稳定的逻辑标识。
    std::string m_collaborationId;

    /// @brief 获取应用偏移后的实际播放时间。
    /// @return 实际播放时间，单位秒。
    [[nodiscard]] double effectiveTime() const
    {
        return m_timestamp + static_cast<double>(m_offsetMs) / 1000.0;
    }

    /// @brief 从 BeatMap 领域对象构造 ECS 数据。
    /// @param sample 自动采样领域对象。
    /// @return 时间已从毫秒转换为秒的 ECS 组件。
    [[nodiscard]] static SampleComponent fromAudioSample(
        const ::MMM::AudioSampleEvent& sample)
    {
        return {
            .m_timestamp       = sample.m_timestamp / 1000.0,
            .m_offsetMs        = sample.m_offsetMs,
            .m_track           = sample.m_track,
            .m_audioResourceId = sample.m_audioResourceId,
            .m_volume          = sample.m_volume,
            .m_metadata        = sample.m_metadata,
            .m_collaborationId = sample.m_collaborationId,
        };
    }

    /// @brief 转换为 BeatMap 领域对象。
    /// @return 时间已从秒转换为毫秒的自动采样领域对象。
    [[nodiscard]] ::MMM::AudioSampleEvent toAudioSample() const
    {
        return {
            .m_timestamp       = m_timestamp * 1000.0,
            .m_offsetMs        = m_offsetMs,
            .m_track           = m_track,
            .m_audioResourceId = m_audioResourceId,
            .m_volume          = m_volume,
            .m_metadata        = m_metadata,
            .m_collaborationId = m_collaborationId,
        };
    }
};

}  // namespace MMM::Logic
