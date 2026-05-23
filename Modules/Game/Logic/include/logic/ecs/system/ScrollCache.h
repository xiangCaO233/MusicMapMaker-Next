#pragma once

#include "config/EditorConfig.h"
#include <cstdint>
#include <entt/entt.hpp>
#include <utility>
#include <vector>

namespace MMM::Logic
{
struct TimelineComponent;
}

namespace MMM::Logic::System
{

/**
 * @brief 预计算的积分段，用于通过二分查找实现 O(log T) 的极速时间坐标映射
 */
struct ScrollSegment {
    double       time;
    double       absY;
    double       speed;
    uint32_t     effects{ 0 };  /// @brief 该时间戳上包含的效果类型 (位掩码)
    entt::entity bpmEntity{ entt::null };
    entt::entity scrollEntity{ entt::null };
    entt::entity jumpEntity{ entt::null };  /// @brief Jump 效果实体
    entt::entity hsEntity{ entt::null };    /// @brief HS 效果实体
    double       bpmValue{ 0.0 };
    double       scrollValue{ 0.0 };
    double       jumpValue{ 0.0 };  /// @brief Jump 原始参数，单位毫秒
    double       hsValue{ 1.0 };    /// @brief HS 原始参数
    double       hs{ 1.0 };         /// @brief 当前区间生效的 HS 倍率
};

constexpr uint32_t SCROLL_EFFECT_BPM    = 1 << 0;
constexpr uint32_t SCROLL_EFFECT_SCROLL = 1 << 1;
constexpr uint32_t SCROLL_EFFECT_JUMP   = 1 << 2;
constexpr uint32_t SCROLL_EFFECT_HS     = 1 << 3;

/**
 * @brief 全局流速映射缓存类
 *
 * 存储在时间线 registry 的 context 中。通过监听时间线组件的增删改事件
 * 将 isDirty 设为 true，从而实现“修改时 O(T) 重建，查询时 O(log T)”的极速性能。
 */
class ScrollCache
{
public:
    ScrollCache() = default;

    /// @brief 根据时间线注册表重建缓存表
    void rebuild(const entt::registry&       timelineRegistry,
                 const Config::EditorConfig& config);

    /// @brief 获取给定时间戳对应的绝对 Y 坐标 (对数时间复杂度)
    double getAbsY(double t) const;

    /// @brief 获取给定绝对 Y 坐标对应的时间戳 (反向映射)
    double getTime(double absY) const;

    /// @brief 获取给定时间戳对应的流速倍率
    double getSpeedAt(double t) const;

    /// @brief 获取给定时间戳对应的 Malody HS 音符速度倍率
    double getHsAt(double t) const;

    /// @brief 获取从当前卷轴位置到目标时间的显示偏移
    double getDisplayDelta(double t, double currentAbsY,
                           double anchorTime) const;

    /// @brief 是否存在 Jump 效果，存在时可见物件集合不再是连续时间区间
    bool hasJumpEffects() const;

    /// @brief 获取指定时间范围附近 Jump 的最大影响秒数
    double getMaxJumpSecondsInRange(double startTime, double endTime,
                                    double padding = 0.0) const;

    /// @brief 获取给定绝对 Y 可视窗口对应的所有时间区间
    std::vector<std::pair<double, double>> getTimeRangesForAbsYWindow(
        double minAbsY, double maxAbsY) const;

    /// @brief 获取所有分段信息 (只读)
    const std::vector<ScrollSegment>& getSegments() const { return m_segments; }

    /// @brief 脏标记，用于触发延迟重建
    bool isDirty{ true };

private:
    std::vector<ScrollSegment> m_segments;

    double m_lastZoom{ 1.0 };  // 记录最后一次 rebuild 使用的缩放

    struct TimingEntry {
        entt::entity             entity;
        const TimelineComponent* component;
    };
    std::vector<TimingEntry> m_rebuildScratch;
};

}  // namespace MMM::Logic::System
