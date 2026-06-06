#pragma once

#include "config/EditorConfig.h"
#include <cstddef>
#include <cstdint>
#include <entt/entt.hpp>
#include <utility>
#include <vector>

namespace MMM
{
class BeatMap;
}

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
    double       jumpValue{ 0.0 };  /// @brief Jump 原始参数，单位毫秒。
    double       hsValue{ 1.0 };    /// @brief HS 原始参数。
    double       hs{ 1.0 };         /// @brief 当前区间生效的 HS 倍率。
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
    /// @param timelineRegistry 时间线注册表。
    /// @param config 当前编辑器配置。
    /// @param beatmap 当前 Session 绑定的谱面；为空时使用保守默认值。
    /// @warning 逻辑热路径低频分支：会完整遍历/排序时间线，只能在 isDirty
    /// 时执行，禁止每 update 无条件调用。
    void rebuild(const entt::registry&       timelineRegistry,
                 const Config::EditorConfig& config, MMM::BeatMap* beatmap);

    /// @brief 获取给定时间戳对应的绝对 Y 坐标。
    double getAbsY(double t) const;

    /// @brief 获取播放渲染锚点使用的绝对 Y 坐标。
    /// @param t 当前播放视觉时间。
    /// @return 渲染锚点 AbsY。
    /// @warning 热路径：每次渲染快照生成时调用；只允许做二分查找。
    double getVisualAnchorAbsY(double t) const;

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

    /// @brief 获取从 startTime 到 endTime 被 ScrollSegment
    /// 边界切割后的所有时间子区间
    std::vector<std::pair<double, double>> getTimeSlices(double startTime,
                                                         double endTime) const;

    /// @brief 获取所有分段信息 (只读)
    const std::vector<ScrollSegment>& getSegments() const { return m_segments; }

    /// @brief 脏标记，用于触发延迟重建
    bool isDirty{ true };

private:
    std::vector<ScrollSegment> m_segments;

    double m_lastZoom{ 1.0 };  // 记录最后一次 rebuild 使用的缩放

    /// @brief 亚帧正负高速 SV 抵消窗口。
    struct MicroImpulseWindow {
        /// @brief 窗口起始时间。
        double startTime{ 0.0 };
        /// @brief 窗口结束时间。
        double endTime{ 0.0 };
        /// @brief 窗口起点原始 AbsY。
        double startAbsY{ 0.0 };
        /// @brief 窗口终点原始 AbsY。
        double endAbsY{ 0.0 };
    };

    /// @brief 按起始时间排序的亚帧抵消脉冲窗口。
    std::vector<MicroImpulseWindow> m_microImpulseWindows;

    /// @brief 当前缓存是否包含 Jump 断层。
    bool m_hasJumpEffects{ false };

    struct TimingEntry {
        entt::entity             entity;
        const TimelineComponent* component;
    };
    std::vector<TimingEntry> m_rebuildScratch;

    /// @brief 单个滚动段覆盖的 AbsY 区间索引项。
    struct AbsYRangeEntry {
        /// @brief 区间下界。
        double minAbsY{ 0.0 };
        /// @brief 区间上界。
        double maxAbsY{ 0.0 };
        /// @brief 对应的 m_segments 下标。
        std::size_t segmentIndex{ 0 };
    };

    /// @brief 按 AbsY 下界排序的滚动段区间索引。
    std::vector<AbsYRangeEntry> m_absYRangeIndex;

    /// @brief 重建 AbsY 区间索引。
    void rebuildAbsYRangeIndex();

    /// @brief 重建亚帧抵消脉冲窗口。
    /// @warning 逻辑低频路径：仅在 ScrollCache rebuild 时完整扫描分段。
    void rebuildMicroImpulseWindows();

    /// @brief 获取原始分段积分 AbsY，不应用亚帧脉冲窗口修正。
    /// @param t 查询时间。
    /// @return 原始 AbsY。
    /// @warning 热路径：每次坐标查询可能调用；只能做二分查找。
    double getRawAbsY(double t) const;

    /// @brief 将原始 AbsY 投影到渲染用亚帧脉冲窗口。
    /// @param t 查询时间。
    /// @param rawAbsY 原始分段积分 AbsY。
    /// @return 渲染用 AbsY。
    /// @warning 热路径：每次坐标查询可能调用；只能做二分查找。
    double applyMicroImpulseWindow(double t, double rawAbsY) const;

    /// @brief 获取指定滚动段覆盖的 AbsY 区间。
    std::pair<double, double> getSegmentAbsYRange(std::size_t index) const;
};

}  // namespace MMM::Logic::System
