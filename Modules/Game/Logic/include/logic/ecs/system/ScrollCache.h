#pragma once

#include <entt/entt.hpp>
#include <vector>

namespace MMM::Logic::System
{

/**
 * @brief 预计算的积分段，用于通过二分查找实现 O(log T) 的极速时间坐标映射
 */
struct ScrollSegment {
    double time;
    double absY;
    double speed;
};

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

    /// @brief 脏标记，用于触发延迟重建
    bool isDirty{ true };

private:
    std::vector<ScrollSegment> m_segments;
};

}  // namespace MMM::Logic::System
