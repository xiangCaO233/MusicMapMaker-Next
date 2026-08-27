#pragma once

#include <cstdint>
#include <entt/entity/entity.hpp>
#include <vector>

namespace MMM::Common::Render
{

/// @brief UI 消费的滚动时间分段快照。
struct ScrollSegment {
    /// @brief 分段起始时间，单位秒。
    double time{ 0.0 };
    /// @brief 分段起点积分坐标。
    double absY{ 0.0 };
    /// @brief 分段内积分速度。
    double speed{ 0.0 };
    /// @brief 当前时间点包含的 Timing 效果位。
    std::uint32_t effects{ 0 };
    /// @brief 当前时间点的 BPM 实体。
    entt::entity bpmEntity{ entt::null };
    /// @brief 当前时间点的 SV 实体。
    entt::entity scrollEntity{ entt::null };
    /// @brief 当前时间点的 Jump 实体。
    entt::entity jumpEntity{ entt::null };
    /// @brief 当前时间点的 HS 实体。
    entt::entity hsEntity{ entt::null };
    /// @brief BPM 事件原始值。
    double bpmValue{ 0.0 };
    /// @brief SV 事件原始值。
    double scrollValue{ 0.0 };
    /// @brief Jump 事件原始值，单位毫秒。
    double jumpValue{ 0.0 };
    /// @brief HS 事件原始值。
    double hsValue{ 1.0 };
    /// @brief 当前区间生效的 HS 倍率。
    double hs{ 1.0 };
    /// @brief 当前区间生效的 BPM。
    double activeBpmValue{ 0.0 };
    /// @brief 当前区间生效的 SV。
    double activeScrollValue{ 1.0 };
};

/// @brief 滚动分段中的 BPM 事件位。
inline constexpr std::uint32_t SCROLL_EFFECT_BPM = 1U << 0U;
/// @brief 滚动分段中的 SV 事件位。
inline constexpr std::uint32_t SCROLL_EFFECT_SCROLL = 1U << 1U;
/// @brief 滚动分段中的 Jump 事件位。
inline constexpr std::uint32_t SCROLL_EFFECT_JUMP = 1U << 2U;
/// @brief 滚动分段中的 HS 事件位。
inline constexpr std::uint32_t SCROLL_EFFECT_HS = 1U << 3U;

/// @brief 预览窗口全谱物件密度的固定时长滑动窗口快照。
struct PreviewDensitySnapshot {
    /// @brief 各采样时间窗口内的物件数量。
    std::vector<std::uint32_t> counts;
    /// @brief 密度时间轴覆盖的谱面总时长。
    double duration{ 0.0 };
    /// @brief 相邻密度采样中心的时间间隔。
    double sampleInterval{ 0.0 };
    /// @brief 每个密度样本使用的滑动窗口时长。
    double windowDuration{ 2.0 };
    /// @brief 当前快照中的最大窗口物件数。
    std::uint32_t maxCount{ 0 };

    /// @brief 清空密度样本并保留向量容量。
    /// @warning 快照热路径：不释放已有容量。
    void clear()
    {
        counts.clear();
        duration       = 0.0;
        sampleInterval = 0.0;
        windowDuration = 2.0;
        maxCount       = 0;
    }
};

}  // namespace MMM::Common::Render
