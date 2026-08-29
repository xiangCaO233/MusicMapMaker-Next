#pragma once

#include "mmm/annotation/BeatmapAnnotation.h"
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <string>
#include <vector>

namespace MMM::Common::Render
{

/// @brief 主画布单个时间戳标记内的一条批注展示数据。
struct AnnotationRenderItem {
    /// @brief 批注稳定标识。
    std::string id;
    /// @brief 批注目标类型。
    ::MMM::BeatmapAnnotationTargetKind targetKind{
        ::MMM::BeatmapAnnotationTargetKind::TIMESTAMP
    };
    /// @brief 目标物件轨道；独立时间戳或目标丢失时为 -1。
    std::int32_t track{ -1 };
    /// @brief 目标物件在当前逻辑会话中的实体。
    entt::entity targetEntity{ entt::null };
    /// @brief Polyline 子物件索引；负值表示整个物件。
    std::int32_t targetSubIndex{ -1 };
    /// @brief 目标物件是否已经不存在。
    bool targetMissing{ false };
    /// @brief 批注作者。
    std::string author;
    /// @brief Markdown 正文。
    std::string content;
};

/// @brief 同一时间戳上合并显示的一组批注。
struct AnnotationRenderMarker {
    /// @brief 当前目标解析出的展示时间，单位秒。
    double timestamp{ 0.0 };
    /// @brief 使用主画布滚动缓存投影后的局部 Y 坐标。
    float canvasY{ 0.0F };
    /// @brief 该时间戳上的全部批注。
    std::vector<AnnotationRenderItem> items;
};

}  // namespace MMM::Common::Render
