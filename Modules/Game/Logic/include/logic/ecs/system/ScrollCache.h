#pragma once

#include <cstdint>
#include <entt/entt.hpp>
#include <vector>

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
    double       bpmValue{ 0.0 };
    double       scrollValue{ 0.0 };
};

constexpr uint32_t SCROLL_EFFECT_BPM    = 1 << 0;
constexpr uint32_t SCROLL_EFFECT_SCROLL = 1 << 1;

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
    void rebuild(const entt::registry& timelineRegistry);

    /// @brief 获取给定时间戳对应的绝对 Y 坐标 (对数时间复杂度)
    double getAbsY(double t) const;

    /// @brief 获取给定绝对 Y 坐标对应的时间戳 (反向映射)
    double getTime(double absY) const;

    /// @brief 获取给定时间戳对应的流速倍率
    double getSpeedAt(double t) const;

    /// @brief 获取所有分段信息 (只读)
    const std::vector<ScrollSegment>& getSegments() const { return m_segments; }

    /// @brief 脏标记，用于触发延迟重建
    bool isDirty{ true };

private:
    std::vector<ScrollSegment> m_segments;
};

}  // namespace MMM::Logic::System
