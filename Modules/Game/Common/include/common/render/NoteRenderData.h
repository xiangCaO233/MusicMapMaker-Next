#pragma once

#include "mmm/metadata/NoteMetadata.h"
#include "mmm/note/NoteType.h"
#include "mmm/sample/AudioSample.h"
#include <cmath>
#include <cstdint>
#include <glm/vec4.hpp>
#include <optional>
#include <string>

namespace MMM::Common::Render
{

/// @brief 渲染与编辑预览共享的音符局部颜色覆盖。
struct NoteColorOverrides {
    /// @brief 普通 Note 本体颜色。
    std::optional<glm::vec4> tap;
    /// @brief Hold、Flick 或 Polyline 头部颜色。
    std::optional<glm::vec4> head;
    /// @brief Hold、Flick 或 Polyline 连接体颜色。
    std::optional<glm::vec4> hold;
    /// @brief Hold、Flick 或 Polyline 尾部颜色。
    std::optional<glm::vec4> end;
    /// @brief Flick 或 Polyline 滑键箭头颜色。
    std::optional<glm::vec4> flickArrow;
    /// @brief Polyline 中间节点颜色。
    std::optional<glm::vec4> node;
};

/// @brief 渲染与编辑预览共享的 Polyline 子物件数据。
struct PolylineSubNote {
    /// @brief 子物件类型。
    ::MMM::NoteType type{ ::MMM::NoteType::NOTE };
    /// @brief 子物件时间戳。
    double timestamp{ 0.0 };
    /// @brief 子物件持续时间。
    double duration{ 0.0 };
    /// @brief 子物件轨道索引。
    int trackIndex{ 0 };
    /// @brief Flick 横向轨道偏移。
    int dtrack{ 0 };
    /// @brief 子物件元数据。
    ::MMM::NoteMetadata metadata;
    /// @brief 子物件编辑器注释。
    std::string annotation;
    /// @brief 子物件采样绑定。
    std::optional<::MMM::AudioSampleBinding> sampleBinding;
    /// @brief 子物件颜色覆盖。
    NoteColorOverrides customColors;
    /// @brief 协作会话内稳定逻辑标识。
    std::string collaborationId;
};

/// @brief 判断后继连接能否无损拆成两个合法子段。
/// @warning 渲染与离散命令共用的常量判断，不修改原节点或分配资源。
inline bool canSplitPolylineConnection(const PolylineSubNote& a,
                                       const PolylineSubNote& b)
{
    // 仅拆分已存在的连接；零长度和不连续的异常段不猜测修复。
    const double dt = b.timestamp - a.timestamp;
    if ( !std::isfinite(dt) ) return false;
    if ( a.type == ::MMM::NoteType::NOTE ) return dt > 0.000002;
    if ( a.type == ::MMM::NoteType::HOLD )
        return dt > 0.000002 && a.trackIndex == b.trackIndex &&
               std::abs(a.duration - dt) < 1e-6;
    const std::int64_t delta = std::int64_t(b.trackIndex) - a.trackIndex;
    // 横向 Flick 只能在同一时间的整数轨道之间拆分。
    return a.type == ::MMM::NoteType::FLICK && std::abs(dt) < 1e-7 &&
           (delta >= 2 || delta <= -2) && delta == a.dtrack;
}
}  // namespace MMM::Common::Render
