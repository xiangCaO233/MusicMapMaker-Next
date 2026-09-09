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
    /// @brief 所有属性通过检查，结果携带完整组件副本。
    None,
    /// @brief 资源未解析成功，或没有可用于持久化的资源 ID。
    MissingResource,
    /// @brief 资源不是允许自动采样引用的主音轨或效果音。
    UnsupportedResourceType,
    /// @brief 玩家轨道数不能作为有效的 BGM 区起点。
    InvalidPlayerTrackCount,
    /// @brief BGM 相对轨道索引为负。
    InvalidBgmLane,
    /// @brief 音量包含 NaN 或无穷值。
    InvalidVolume,
    /// @brief 转换后的绝对轨道不能由组件字段表示。
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
/// @note 按资源、布局、音量的顺序返回首个失败原因，不收集多项诊断。
[[nodiscard]] inline SamplePropertyEditResult resolveSamplePropertyEdit(
    const SampleComponent& current, std::int32_t playerTrackCount,
    const ::MMM::AudioResource* resource, std::int32_t bgmLane,
    std::int64_t offsetMs, float volume)
{
    // 失败只返回原因，不就地修改 current，调用方可继续显示原属性。
    if ( !resource || resource->m_id.empty() ) {
        return { SamplePropertyEditIssue::MissingResource, std::nullopt };
    }

    // 接受可播放音频类别，不因资源 ID 存在就允许所有资源类型进入采样。
    switch ( resource->m_type ) {
    case ::MMM::AudioTrackType::Main:
    case ::MMM::AudioTrackType::Effect: break;
    default:
        return { SamplePropertyEditIssue::UnsupportedResourceType,
                 std::nullopt };
    }

    // 先检查有符号输入，再转换无符号轨道，避免负数变成巨大索引。
    if ( playerTrackCount <= 0 ) {
        return { SamplePropertyEditIssue::InvalidPlayerTrackCount,
                 std::nullopt };
    }
    if ( bgmLane < 0 ) {
        return { SamplePropertyEditIssue::InvalidBgmLane, std::nullopt };
    }
    // 这里只拒绝非有限音量，不额外钳制合法的有限增益。
    if ( !std::isfinite(volume) ) {
        return { SamplePropertyEditIssue::InvalidVolume, std::nullopt };
    }

    // 相对 BGM 索引从玩家区右侧开始，以宽整数计算后检查目标字段容量。
    const std::uint64_t absoluteTrack =
        static_cast<std::uint64_t>(playerTrackCount) +
        static_cast<std::uint64_t>(bgmLane);
    if ( absoluteTrack > std::numeric_limits<std::uint32_t>::max() ) {
        return { SamplePropertyEditIssue::AbsoluteTrackOverflow, std::nullopt };
    }

    // 以原组件为基底保留锚点、协作身份和扩展数据，只替换显式编辑的属性。
    SampleComponent edited   = current;
    edited.m_audioResourceId = resource->m_id;
    edited.m_track           = static_cast<std::uint32_t>(absoluteTrack);
    // 偏移保持有符号毫秒，不吸附或并入锚点时间，便于后续精确调整。
    edited.m_offsetMs = offsetMs;
    edited.m_volume   = volume;
    return { SamplePropertyEditIssue::None, std::move(edited) };
}

}  // namespace MMM::Logic
