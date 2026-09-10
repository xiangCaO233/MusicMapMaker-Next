#include "canvas/MarqueeAutoScroll.h"
#include "common/render/RenderSnapshotBuffer.h"
#include "config/AppConfig.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>

namespace MMM::Canvas
{
namespace
{
/// @brief 获取没有 ScrollSegment 时使用的兜底绝对 Y 速度。
/// @return 兜底绝对 Y 速度，单位为像素/秒。
///
/// 回退仅用于尚未生成滚动缓存的过渡帧，并与普通时间轴缩放保持同方向。
double defaultSnapshotAbsYSpeed()
{
    // 无分段快照按当前缩放构造稳定线性速度，避免框选完全无法滚动。
    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    return 500.0 * static_cast<double>(std::max(0.01f, visual.timelineZoom));
}

/// @brief 根据渲染快照中的显示时间估算绝对 Y 坐标。
/// @param snapshot 当前 UI 渲染快照。
/// @param time 显示时间，单位为秒。
/// @return 指定显示时间对应的绝对 Y 坐标。
/// @warning UI 热路径：只读取快照滚动分段缓存。
///
/// scrollSegments 必须按时间升序；目标早于首段时沿首段速度反向外推。
double snapshotAbsYAtTime(const Common::Render::RenderSnapshot& snapshot,
                          double                                time)
{
    if ( snapshot.scrollSegments.empty() ) {
        // 空缓存使用统一线性回退，和反向换算保持互逆。
        return time * defaultSnapshotAbsYSpeed();
    }

    // upper_bound 找到首个晚于目标时间的分段，前一项即当前生效分段。
    auto it = std::upper_bound(
        snapshot.scrollSegments.begin(),
        snapshot.scrollSegments.end(),
        time,
        [](double val, const Common::Render::ScrollSegment& segment) {
            return val < segment.time;
        });

    const auto& segment = it == snapshot.scrollSegments.begin()
                              ? snapshot.scrollSegments.front()
                              : *std::prev(it);
    // 分段内 absY 按缓存起点与局部速度线性外推。
    return segment.absY + (time - segment.time) * segment.speed;
}

/// @brief 尝试在单个 ScrollSegment 内反解显示时间。
/// @param snapshot 当前 UI 渲染快照。
/// @param index 目标 ScrollSegment 索引。
/// @param absY 目标绝对 Y 坐标。
/// @param outTime 解析出的显示时间，单位为秒。
/// @return 目标绝对 Y 坐标位于该分段内时返回 true。
/// @warning UI 热路径：只做常量时间的分段数学计算。
///
/// 正负速度统一通过端点最小值与最大值判断，零速段只接受起始绝对坐标。
bool trySnapshotTimeAtSegmentAbsY(
    const Common::Render::RenderSnapshot& snapshot, size_t index, double absY,
    double& outTime)
{
    constexpr double EPSILON  = 1e-6;
    const auto&      segments = snapshot.scrollSegments;
    // 调用方扫描时仍防御索引越界，避免错误快照造成无效引用。
    if ( index >= segments.size() ) {
        return false;
    }

    const auto& segment = segments[index];
    if ( std::abs(segment.speed) <= EPSILON ) {
        // 零速段只在目标 Y 与段起点重合时有唯一可用的起始时间。
        if ( std::abs(absY - segment.absY) <= EPSILON ) {
            outTime = segment.time;
            return true;
        }
        return false;
    }

    // 有下一段时当前段定义到 nextTime；末段沿速度方向无限延伸。
    const bool hasNext = index + 1 < segments.size();
    // nextTime 同时界定当前段的时间上界和有限端点位置。
    const double nextTime = hasNext ? segments[index + 1].time
                                    : std::numeric_limits<double>::infinity();
    const double endAbsY =
        hasNext
            ? segment.absY + (nextTime - segment.time) * segment.speed
            : (segment.speed > 0.0 ? std::numeric_limits<double>::infinity()
                                   : -std::numeric_limits<double>::infinity());
    const double minAbsY = std::min(segment.absY, endAbsY) - EPSILON;
    const double maxAbsY = std::max(segment.absY, endAbsY) + EPSILON;
    // 两端加入小容差，避免相邻分段连接点因舍入同时被判定为不属于任一段。
    if ( absY < minAbsY || absY > maxAbsY ) {
        // 使用 Y 范围而非时间顺序判断，可兼容负速度和反向滚动分段。
        return false;
    }

    // 通过局部直线反解时间，再验证没有越过本分段的时间区间。
    outTime = segment.time + (absY - segment.absY) / segment.speed;
    return outTime >= segment.time - EPSILON && outTime <= nextTime + EPSILON;
}

/// @brief 根据渲染快照中的绝对 Y 坐标估算显示时间。
/// @param snapshot 当前 UI 渲染快照。
/// @param absY 目标绝对 Y 坐标。
/// @return 指定绝对 Y 坐标对应的显示时间，单位为秒。
/// @warning UI 热路径：通常先测试当前分段，只在跨分段时扫描快照分段。
///
/// 多个速度反转分段可能覆盖同一 absY；当前播放分段具有最高优先级，以保持
/// 连续框选滚动不会突然跳到另一段相同坐标。
double snapshotTimeAtAbsY(const Common::Render::RenderSnapshot& snapshot,
                          double                                absY)
{
    if ( snapshot.scrollSegments.empty() ) {
        // 与 snapshotAbsYAtTime 使用同一回退速度，极端零速时返回当前时间。
        const double speed = defaultSnapshotAbsYSpeed();
        return std::abs(speed) > 1e-9 ? absY / speed : snapshot.currentTime;
    }

    // 自动滚动通常仍在当前播放分段，先定位并优先尝试该索引。
    auto currentIt = std::upper_bound(
        snapshot.scrollSegments.begin(),
        snapshot.scrollSegments.end(),
        snapshot.currentTime,
        [](double val, const Common::Render::ScrollSegment& segment) {
            return val < segment.time;
        });
    const size_t currentIndex =
        currentIt == snapshot.scrollSegments.begin()
            ? 0
            : static_cast<size_t>(std::distance(snapshot.scrollSegments.begin(),
                                                std::prev(currentIt)));

    double outTime = snapshot.currentTime;
    if ( trySnapshotTimeAtSegmentAbsY(snapshot, currentIndex, absY, outTime) ) {
        // 常见路径常量时间返回，不扫描完整分段列表。
        return outTime;
    }

    // 跨越速度反转或大步滚动时才扫描其它分段寻找包含目标 Y 的区间。
    for ( size_t i = 0; i < snapshot.scrollSegments.size(); ++i ) {
        if ( i == currentIndex ) {
            continue;
        }
        if ( trySnapshotTimeAtSegmentAbsY(snapshot, i, absY, outTime) ) {
            // 其它分段按缓存时间顺序选择首个匹配，提供确定结果。
            return outTime;
        }
    }

    // 目标不属于任何有限分段时，从绝对 Y 最近的首尾边缘继续外推。
    const auto& first = snapshot.scrollSegments.front();
    const auto& last  = snapshot.scrollSegments.back();
    const auto& edge =
        std::abs(absY - first.absY) < std::abs(absY - last.absY) ? first : last;
    // 只在完全没有分段覆盖时使用最近端点，避免内部速度反转被越过。
    if ( std::abs(edge.speed) <= 1e-9 ) {
        // 边缘零速无法外推，固定在该段起始时间。
        return edge.time;
    }
    return edge.time + (absY - edge.absY) / edge.speed;
}
}  // namespace

/// @brief 计算框选拖出画布边缘后应滚动到的显示时间。
/// @param snapshot 当前不可变渲染快照及其滚动分段。
/// @param viewportHeight 画布视口逻辑高度。
/// @param mouseY 指针相对画布顶部的局部纵坐标。
/// @param deltaTime 当前 UI 帧间隔。
/// @param isAccelerated 是否应用 Shift 三倍加速。
/// @param scrolled 输出是否产生了有限且变化的目标时间。
/// @return 需要滚动时返回新显示时间，否则返回快照当前时间。
/// @warning UI 热路径：仅在活动框选拖动期间调用，不执行阻塞或分配。
double marqueeAutoScrollTargetTime(
    const Common::Render::RenderSnapshot& snapshot, float viewportHeight,
    float mouseY, float deltaTime, bool isAccelerated, bool& scrolled)
{
    // 输出参数每次调用先归零，所有早退路径都明确表示未发生滚动。
    scrolled = false;
    if ( !std::isfinite(mouseY) || !std::isfinite(viewportHeight) ||
         viewportHeight <= 1.0f ||
         (mouseY >= 0.0f && mouseY <= viewportHeight) ) {
        // 指针仍在视口内时保持当前时间，不启动边缘外自动滚动。
        return snapshot.currentTime;
    }

    // 顶部越界向未来方向滚动，底部越界向过去方向滚动。
    const double direction = mouseY < 0.0f ? 1.0 : -1.0;
    // outsidePixels 始终为正距离，滚动符号只由 direction 决定。
    const float outsidePixels =
        mouseY < 0.0f ? -mouseY : mouseY - viewportHeight;
    if ( outsidePixels <= 0.0f ) {
        return snapshot.currentTime;
    }

    // 用户可把灵敏度设为零以完全禁用此交互。
    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    // 灵敏度来自预览边缘滚动配置，使框选与预览交互遵循同一用户偏好。
    const double sensitivity =
        std::max(0.0f, visual.previewConfig.edgeScrollSensitivity);
    if ( sensitivity <= 1e-6 ) {
        return snapshot.currentTime;
    }

    // 帧间隔限制避免卡顿恢复后一次跳过过大时间范围，并为异常值提供回退。
    const double dt = std::clamp(std::isfinite(deltaTime) && deltaTime > 0.0f
                                     ? static_cast<double>(deltaTime)
                                     : 1.0 / 60.0,
                                 1.0 / 240.0,
                                 1.0 / 15.0);
    // 限制只是积分安全边界，不通过等待或固定窗口延迟本地交互。
    const double ramp =
        // 以视口高度的 18% 作为加速尺度，越界越远滚动越快。
        std::max(0.0,
                 static_cast<double>(outsidePixels) /
                     std::max(1.0, static_cast<double>(viewportHeight) * 0.18));
    const double acceleratedRamp = ramp * ramp;
    // 二次曲线在边缘附近平滑，在指针远离视口时快速提升跨段速度。
    constexpr double SHIFT_AUTO_SCROLL_ACCELERATION = 3.0;
    const double     acceleration =
        // Shift 加速只改变速度倍率，不改变方向或时间映射算法。
        isAccelerated ? SHIFT_AUTO_SCROLL_ACCELERATION : 1.0;
    const double pixelsPerSecond =
        // 基础速度保证轻微越界仍响应，二次项提供远距离快速移动。
        (6000.0 + 24000.0 * acceleratedRamp) * sensitivity * acceleration;
    const double scale = std::abs(snapshot.renderScaleY) > 1e-6f
                             ? static_cast<double>(snapshot.renderScaleY)
                             : 1.0;
    // 预览画布的几何已经按 renderScaleY 缩放，反算逻辑绝对位移时需除回。
    // 位移先在绝对 Y 空间积分，再经快照分段反解为目标显示时间。
    const double currentAbsY =
        snapshotAbsYAtTime(snapshot, snapshot.currentTime);
    const double targetAbsY =
        currentAbsY + direction * pixelsPerSecond * dt / scale;
    // 绝对 Y 可以跨越多个正速、负速或零速段，统一交给反解函数选择时间。
    const double targetTime = snapshotTimeAtAbsY(snapshot, targetAbsY);
    // 只有得到有限且确实变化的时间才通知调用方发布 Seek。
    scrolled = std::isfinite(targetTime) &&
               std::abs(targetTime - snapshot.currentTime) > 1e-6;
    // 无效或无变化结果回退到当前时间，调用方不会发布冗余 Seek。
    return scrolled ? targetTime : snapshot.currentTime;
}

}  // namespace MMM::Canvas
