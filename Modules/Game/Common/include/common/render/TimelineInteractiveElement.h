#pragma once

#include "mmm/timing/TimingInterpolation.h"
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <optional>

namespace MMM::Common::Render
{
/// @brief 时间线上的 BPM、Scroll、Jump 与 HS 交互元素快照。
struct TimelineInteractiveElement {
    /// @brief 表格中的段落定义；普通独立时间点为空。
    std::optional<TimingInterpolation> interpolation;
    /// @brief 单个 Timing marker 的快照几何范围。
    struct MarkerGeometry {
        /// @brief 该 Timing 标记是否拥有可直接修饰的几何体。
        bool hasMarkerGeometry{ false };
        /// @brief 该 Timing 标记在快照顶点数组中的起点。
        uint32_t markerVertexOffset{ 0 };
        /// @brief 该 Timing 标记占用的顶点数量。
        uint32_t markerVertexCount{ 0 };
        /// @brief 该 Timing 标记在快照索引数组中的起点。
        uint32_t markerIndexOffset{ 0 };
        /// @brief 该 Timing 标记占用的索引数量。
        uint32_t markerIndexCount{ 0 };
    };

    /// @brief Timing 元素在谱面时间轴上的时间。
    double time;
    /// @brief 标记在当前画布快照中的纵坐标。
    float y;
    /// @brief 当前时间点实际包含的 Timing 效果位掩码。
    uint32_t effects;
    /// @brief BPM 效果对应实体；不存在时为 entt::null。
    entt::entity bpmEntity{ entt::null };
    /// @brief Scroll 效果对应实体；不存在时为 entt::null。
    entt::entity scrollEntity{ entt::null };
    entt::entity jumpEntity{ entt::null };  /// @brief Jump 效果实体
    entt::entity hsEntity{ entt::null };    /// @brief HS 效果实体
    /// @brief 当前暂停事件实体。
    entt::entity stopEntity{ entt::null };
    /// @brief BPM 效果的原始数值。
    double bpmValue{ 0.0 };
    /// @brief Scroll 效果的原始数值。
    double scrollValue{ 0.0 };
    double jumpValue{ 0.0 };  /// @brief Jump 原始参数，单位毫秒
    double hsValue{ 1.0 };    /// @brief HS 原始参数
    /// @brief STOP 持续秒数，不参与滚动速度积分。
    double stopValue{ 0.0 };
    /// @brief BPM 标记的快照几何范围。
    MarkerGeometry bpmMarker;
    /// @brief Scroll 标记的快照几何范围。
    MarkerGeometry scrollMarker;
    /// @brief Jump 标记的快照几何范围。
    MarkerGeometry jumpMarker;
    /// @brief HS 标记的快照几何范围。
    MarkerGeometry hsMarker;
    /// @brief 暂停标记对应的交互几何。
    MarkerGeometry stopMarker;
    /// @brief 该 Timing 标记是否拥有可直接修饰的几何体。
    bool hasMarkerGeometry{ false };
    /// @brief 该 Timing 标记在快照顶点数组中的起点。
    uint32_t markerVertexOffset{ 0 };
    /// @brief 该 Timing 标记占用的顶点数量。
    uint32_t markerVertexCount{ 0 };
    /// @brief 该 Timing 标记在快照索引数组中的起点。
    uint32_t markerIndexOffset{ 0 };
    /// @brief 该 Timing 标记占用的索引数量。
    uint32_t markerIndexCount{ 0 };
};

}  // namespace MMM::Common::Render
