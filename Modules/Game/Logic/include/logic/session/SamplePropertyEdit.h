#pragma once

#include "logic/ecs/components/SampleComponent.h"
#include "mmm/project/AudioResource.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace MMM::Logic
{

/// @brief 自动采样精确属性编辑的校验结果。
enum class SamplePropertyEditIssue {
    None,
    MissingResource,
    UnsupportedResourceType,
    InvalidPlayerTrackCount,
    InvalidBgmLane,
    InvalidVolume,
    AbsoluteTrackOverflow,
};

/// @brief 自动采样精确属性编辑结果。
struct SamplePropertyEditResult {
    /// @brief 校验失败原因；None 表示成功。
    SamplePropertyEditIssue m_issue{ SamplePropertyEditIssue::None };

    /// @brief 校验通过后的完整组件副本。
    std::optional<SampleComponent> m_sample;
};

/// @brief 校验并构建自动采样精确属性编辑结果。
/// @param current 当前自动采样组件。
/// @param playerTrackCount 当前玩家轨道数。
/// @param resource 已按项目资源表解析的 Main 或 Effect 资源。
/// @param bgmLane 相对玩家轨道区的零基 BGM 轨道索引。
/// @param offsetMs 相对锚点的有符号毫秒偏移。
/// @param volume 自动采样物件音量。
/// @return 校验结果；成功时保留原时间与扩展元数据，仅替换指定属性。
[[nodiscard]] inline SamplePropertyEditResult resolveSamplePropertyEdit(
    const SampleComponent& current, std::int32_t playerTrackCount,
    const ::MMM::AudioResource* resource, std::int32_t bgmLane,
    std::int64_t offsetMs, float volume)
{
    if ( !resource || resource->m_id.empty() ) {
        return { SamplePropertyEditIssue::MissingResource, std::nullopt };
    }

    switch ( resource->m_type ) {
    case ::MMM::AudioTrackType::Main:
    case ::MMM::AudioTrackType::Effect: break;
    default:
        return { SamplePropertyEditIssue::UnsupportedResourceType,
                 std::nullopt };
    }

    if ( playerTrackCount <= 0 ) {
        return { SamplePropertyEditIssue::InvalidPlayerTrackCount,
                 std::nullopt };
    }
    if ( bgmLane < 0 ) {
        return { SamplePropertyEditIssue::InvalidBgmLane, std::nullopt };
    }
    if ( !std::isfinite(volume) ) {
        return { SamplePropertyEditIssue::InvalidVolume, std::nullopt };
    }

    const std::uint64_t absoluteTrack =
        static_cast<std::uint64_t>(playerTrackCount) +
        static_cast<std::uint64_t>(bgmLane);
    if ( absoluteTrack > std::numeric_limits<std::uint32_t>::max() ) {
        return { SamplePropertyEditIssue::AbsoluteTrackOverflow, std::nullopt };
    }

    SampleComponent edited   = current;
    edited.m_audioResourceId = resource->m_id;
    edited.m_track           = static_cast<std::uint32_t>(absoluteTrack);
    edited.m_offsetMs        = offsetMs;
    edited.m_volume          = volume;
    return { SamplePropertyEditIssue::None, std::move(edited) };
}

}  // namespace MMM::Logic
