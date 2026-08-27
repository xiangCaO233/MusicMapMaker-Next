#pragma once

#include "mmm/Metadata.h"
#include "mmm/note/Note.h"
#include "mmm/sample/AudioSample.h"
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

}  // namespace MMM::Common::Render
