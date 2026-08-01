#pragma once

#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"

#include <algorithm>

namespace MMM::Common
{

/// @brief 单文件兼容流程无法表示谱面音频时间线的原因。
enum class SingleAudioTimelineIssue {
    /// @brief 时间线可无损表示为单个零点音频文件。
    None,

    /// @brief 时间线没有自动采样。
    MissingSample,

    /// @brief 时间线含多个可独立播放的自动采样。
    CompositeTimeline,

    /// @brief 唯一采样并非从谱面零点触发。
    NonZeroStart,

    /// @brief 唯一采样引用的项目音频资源不存在。
    MissingResource,
};

/// @brief 单文件兼容流程解析出的零点自动采样及项目资源。
struct SingleAudioTimelineSource {
    /// @brief 兼容性检查结果。
    SingleAudioTimelineIssue m_issue{ SingleAudioTimelineIssue::MissingSample };

    /// @brief 唯一零点自动采样；检查失败时为空。
    const AudioSampleEvent* m_sample{ nullptr };

    /// @brief 自动采样引用的项目音频资源；检查失败时为空。
    const AudioResource* m_resource{ nullptr };

    /// @brief 判断当前结果是否可供单文件流程使用。
    /// @return 同时解析出采样和资源时返回 true。
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return m_issue == SingleAudioTimelineIssue::None &&
               m_sample != nullptr && m_resource != nullptr;
    }
};

/// @brief 检查谱面音频时间线能否无损退化为单个零点音频文件。
/// @param beatmap 待检查谱面。
/// @param project 提供音频资源 ID 解析的项目。
/// @return 兼容结果及成功时的稳定资源引用。
[[nodiscard]] inline SingleAudioTimelineSource
resolveSingleZeroPointAudioTimeline(const BeatMap& beatmap,
                                    const Project& project) noexcept
{
    if ( beatmap.m_audioSamples.empty() ) {
        return { SingleAudioTimelineIssue::MissingSample };
    }
    if ( beatmap.m_audioSamples.size() != 1U ) {
        return { SingleAudioTimelineIssue::CompositeTimeline };
    }

    const AudioSampleEvent& sample = beatmap.m_audioSamples.front();
    if ( sample.m_timestamp != 0.0 || sample.m_offsetMs != 0 ) {
        return { SingleAudioTimelineIssue::NonZeroStart };
    }

    const auto resource =
        std::find_if(project.m_audioResources.begin(),
                     project.m_audioResources.end(),
                     [&](const AudioResource& candidate) {
                         return candidate.m_id == sample.m_audioResourceId;
                     });
    if ( resource == project.m_audioResources.end() ) {
        return { SingleAudioTimelineIssue::MissingResource };
    }

    return {
        SingleAudioTimelineIssue::None,
        &sample,
        &*resource,
    };
}

}  // namespace MMM::Common
