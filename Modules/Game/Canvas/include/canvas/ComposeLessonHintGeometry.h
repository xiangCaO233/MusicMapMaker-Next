#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace MMM::Canvas
{
/// @brief 创作引导轮廓使用的画布局部坐标点。
struct ComposeLessonHintPoint {
    float x{ 0.0F };  ///< 水平像素坐标。
    float y{ 0.0F };  ///< 垂直像素坐标。
};

/// @brief 合并折线路径各段 Note 范围，返回单个顺时针外轮廓。
/// @param points 已按子段顺序投影的节点和 Hold/Flick 端点。
/// @param halfWidth Note 水平半尺寸，包含批注框的留白。
/// @param halfHeight Note 垂直半尺寸，包含批注框的留白。
/// @return 顺时针外轮廓；无有效点时为空。
/// @details 沿段矩形的暴露边走出外轮廓。短段与折返不会使轮廓自交；
/// 斜段以覆盖两个端点的矩形提示，确保参考 Note 不落在框外。
///
/// 轮廓的输入已经处在画布局部像素坐标系中，时间与轨道投影由调用方完成。
/// 每一对相邻节点形成一个扩张矩形，形状是这些矩形的几何并集。
/// 这样相邻 Note 的占地范围能自然连通，短于 Note 本身的转折也不会
/// 像双侧法线偏移那样折回并形成交叉的轮廓边。
///
/// 每条矩形边分别与其它矩形比较，只保留其外侧仍接触空白的区间。
/// 边段继承矩形的顺时针方向，末端与下一边段起点相接，组成闭合环。
/// 如果路径绕圈留下空洞，最终只返回面积最大的外环；教学高亮需要
/// 一个连续且可填充的提示区，内部空洞不会额外提示某个 Note。
///
/// 浮点比较只用于屏幕像素级边界拼接，不能反向用来判断谱面时间或
/// Note 是否完成。提示坐标随每帧相机与窗口位置重新计算，不做持久化。
///
/// 时间轴反向滚动时，投影后的 Y 顺序可能与谱面时间相反，因此构造
/// 矩形时始终分别对两个端点取最小值和最大值，不能假定后一个节点更低。
/// 玩家轨宽度变化只影响本帧的 X 坐标与 Note 宽度，轮廓算法不缓存
/// 上一帧的像素点，也不会把相机状态混入教学参考数据。
///
/// 矩形边按上、右、下、左顺序生成，所以暴露段天然有向。
/// 共线重叠的外边仅保留一份，内部边则整段移除或切成剩余区间。
/// 端点匹配失败的非闭合环视为数值异常，不向填充器传递残缺顶点。
/// 轮廓仅服务视觉提示；Note 完成判定仍由教学反馈中的谱面几何负责。
/// @warning UI 热路径：只处理当前教学折线的小型节点数组，不访问 ECS。
[[nodiscard]] inline std::vector<ComposeLessonHintPoint>
buildComposeLessonHintPolygon(std::span<const ComposeLessonHintPoint> points,
                              float halfWidth, float halfHeight)
{
    // Flick 端点可能与下一个子段起点重合；零长段不能加入边界。
    // 保留原有顺序是必须的：先后子段构成一条路径，不能按屏幕位置排序。
    // 输入点异常时放弃绘制本帧提示，避免 NaN 进入 ImGui 三角剖分。
    std::vector<ComposeLessonHintPoint> path;
    path.reserve(points.size());
    for ( const auto& point : points ) {
        if ( !std::isfinite(point.x) || !std::isfinite(point.y) ) return {};
        if ( path.empty() || std::hypot(point.x - path.back().x,
                                        point.y - path.back().y) > 0.001F )
            path.push_back(point);
    }
    // 扩张尺寸取自主画布渲染快照；资源尚未加载好时不绘制猜测尺寸。
    // 宽高必须严格大于零，否则轮廓可能退化成线，无法填充或定位。
    if ( path.empty() || !std::isfinite(halfWidth) ||
         !std::isfinite(halfHeight) || halfWidth <= 0.0F || halfHeight <= 0.0F )
        return {};

    /// @brief 一段路径的 Note 扩张矩形。
    /// @details 横向 Flick 与纵向 Hold 都由两端点的包围盒扩张得出。
    /// 相邻矩形在共同节点处至少重叠一个 Note 头部尺寸，故并集连通。
    struct Rect {
        float left;
        float top;
        float right;
        float bottom;
    };
    std::vector<Rect> rects;
    rects.reserve(std::max<std::size_t>(1, path.size() - 1));
    // 扩张不是只给路径线加固定线宽：X/Y 分别按 Note 的真实宽高处理。
    // 因此横向连接保持正确的头部高度，竖向连接保持正确的头部宽度。
    for ( std::size_t index = 1; index < path.size(); ++index ) {
        const auto& a = path[index - 1];
        const auto& b = path[index];
        rects.push_back({ std::min(a.x, b.x) - halfWidth,
                          std::min(a.y, b.y) - halfHeight,
                          std::max(a.x, b.x) + halfWidth,
                          std::max(a.y, b.y) + halfHeight });
    }
    if ( rects.empty() ) {
        // 折线数据只有一个有效节点时也显示一个完整的目标 Note 范围。
        // 不把单点抛弃，以便损坏或旧版本参考仍能给出可见提示。
        const auto point = path.front();
        rects.push_back({ point.x - halfWidth,
                          point.y - halfHeight,
                          point.x + halfWidth,
                          point.y + halfHeight });
    }

    /// @brief 顺时针暴露边，相邻边以相同端点连接。
    /// @details 方向从矩形上边向右开始，依次经过右、下、左四边。
    struct Edge {
        ComposeLessonHintPoint start;
        ComposeLessonHintPoint end;
    };
    /// @brief 一条矩形边上仍可见的标量区间。
    /// @details 横边使用 X 区间，竖边使用 Y 区间，端点均为局部像素。
    struct Interval {
        float low;
        float high;
    };
    constexpr float EPSILON = 0.01F;
    // 百分之一像素远小于实际 Note 留白，只用于边段相接与去重。
    // 这里不使用谱面的时间容差，避免视口缩放改变时几何连接突变。
    std::vector<Edge> edges;
    edges.reserve(rects.size() * 4);
    // 两个区间缓存重复服务所有边；不会为每个矩形边反复分配内存。
    // 裁剪后产生多个断开的可见区间时，容量按段数上限预留。
    std::vector<Interval> visible;
    std::vector<Interval> next;
    visible.reserve(rects.size() + 1);
    next.reserve(rects.size() + 1);
    // 不排序参考物件；逐条裁去相交矩形盖住的边段。
    // 单个参考通常只有数个子段，局部比较比建立每帧全局扫描结构简单。
    for ( std::size_t index = 0; index < rects.size(); ++index ) {
        const Rect& rect = rects[index];
        for ( int side = 0; side < 4; ++side ) {
            const bool  horizontal = side == 0 || side == 2;
            const float fixed      = side == 0   ? rect.top
                                     : side == 1 ? rect.right
                                     : side == 2 ? rect.bottom
                                                 : rect.left;
            const float low        = horizontal ? rect.left : rect.top;
            const float high       = horizontal ? rect.right : rect.bottom;
            // 从完整边开始，逐个扣除其它矩形在边外侧覆盖的区间。
            // 被完全覆盖时 visible 为空，后续矩形就不再参与该边的计算。
            visible.clear();
            visible.push_back({ low, high });
            for ( std::size_t otherIndex = 0; otherIndex < rects.size();
                  ++otherIndex ) {
                if ( otherIndex == index || visible.empty() ) continue;
                const Rect& other = rects[otherIndex];
                // 仅当另一矩形占据边的外侧时裁剪；共线重合边由较早段保留。
                // 例如上边的外侧在 y 更小处，仅相交于同一水平边的矩形
                // 不应把这条外边裁掉，否则并集最上沿会出现缺口。
                // 但上下相邻的矩形共享边时，两侧均被填充，共享边需移除。
                const bool covered =
                    side == 0   ? other.top < fixed - EPSILON &&
                                      other.bottom >= fixed - EPSILON
                    : side == 1 ? other.left <= fixed + EPSILON &&
                                      other.right > fixed + EPSILON
                    : side == 2 ? other.top <= fixed + EPSILON &&
                                      other.bottom > fixed + EPSILON
                                : other.left < fixed - EPSILON &&
                                      other.right >= fixed - EPSILON;
                const float otherFixed = side == 0   ? other.top
                                         : side == 1 ? other.right
                                         : side == 2 ? other.bottom
                                                     : other.left;
                const bool  duplicate  = otherIndex < index &&
                                         std::abs(otherFixed - fixed) < EPSILON;
                // 重合外边既不应消失也不应重复描边；较早的段负责保留。
                // 顺序只用于消除重复边，不改变实际谱面节点的先后语义。
                if ( !covered && !duplicate ) continue;
                const float cutLow  = horizontal ? other.left : other.top;
                const float cutHigh = horizontal ? other.right : other.bottom;
                next.clear();
                for ( const auto& interval : visible ) {
                    // 无交集的剩余区间必须原样继承，避免远处的另一段
                    // 意外把当前 Note 边缘清空。
                    if ( cutHigh <= interval.low + EPSILON ||
                         cutLow >= interval.high - EPSILON ) {
                        next.push_back(interval);
                        continue;
                    }
                    // 扣除中间覆盖后至多留下左右两段；两段都按原方向
                    // 继续与后续矩形比较，最后才写入有向边集合。
                    if ( cutLow > interval.low + EPSILON )
                        next.push_back({ interval.low, cutLow });
                    if ( cutHigh < interval.high - EPSILON )
                        next.push_back({ cutHigh, interval.high });
                }
                visible.swap(next);
            }
            // 有向边的端点顺序保证鞋带面积在屏幕 Y 向下时为正。
            // ImGui 的凹多边形填充依赖这一顺时针约定。
            for ( const auto& interval : visible ) {
                if ( interval.high - interval.low <= EPSILON ) continue;
                if ( side == 0 )
                    edges.push_back(
                        { { interval.low, fixed }, { interval.high, fixed } });
                else if ( side == 1 )
                    edges.push_back(
                        { { fixed, interval.low }, { fixed, interval.high } });
                else if ( side == 2 )
                    edges.push_back(
                        { { interval.high, fixed }, { interval.low, fixed } });
                else
                    edges.push_back(
                        { { fixed, interval.high }, { fixed, interval.low } });
            }
        }
    }

    // 暴露边可能形成内洞；教学提示只取面积最大的外环。
    // 同一闭环中边的首尾坐标来自相同矩形边界，不需要网格吸附。
    // 小容差只吸收时间投影后浮点舍入造成的亚像素差异。
    std::vector<bool>                   visited(edges.size(), false);
    std::vector<ComposeLessonHintPoint> largest;
    double                              largestArea = 0.0;
    for ( std::size_t first = 0; first < edges.size(); ++first ) {
        if ( visited[first] ) continue;
        std::vector<ComposeLessonHintPoint> ring;
        std::size_t                         current = first;
        bool                                closed  = false;
        while ( !visited[current] ) {
            // 已消费边不得再次加入轮廓，否则折返点会重复描线。
            // ring 存放每条边的起点，闭合时末端等于第一点，无需重复存储。
            visited[current] = true;
            ring.push_back(edges[current].start);
            const auto end = edges[current].end;
            if ( std::hypot(end.x - edges[first].start.x,
                            end.y - edges[first].start.y) < EPSILON ) {
                closed = true;
                break;
            }
            std::size_t nextIndex = edges.size();
            // 暴露边数量与当前段落长度成正比；按共享端点找下一边。
            // 找不到闭合后继说明数值退化，此环直接忽略而不交给渲染器。
            for ( std::size_t candidate = 0; candidate < edges.size();
                  ++candidate ) {
                if ( visited[candidate] ) continue;
                const auto start = edges[candidate].start;
                if ( std::hypot(start.x - end.x, start.y - end.y) < EPSILON ) {
                    nextIndex = candidate;
                    break;
                }
            }
            if ( nextIndex == edges.size() ) break;
            current = nextIndex;
        }
        if ( !closed || ring.size() < 3 ) continue;
        // 相邻矩形常留下共线分段；合并它们以减少每帧提交的三角形。
        // 删除的点必须在一条直线上，拐角一定保留；否则凹陷会被抹平。
        // 这也避免三角剖分遇到一串零转角顶点时产生退化三角形。
        std::vector<ComposeLessonHintPoint> simplified;
        simplified.reserve(ring.size());
        for ( std::size_t index = 0; index < ring.size(); ++index ) {
            const auto& before = ring[(index + ring.size() - 1) % ring.size()];
            const auto& point  = ring[index];
            const auto& after  = ring[(index + 1) % ring.size()];
            const float cross  = (point.x - before.x) * (after.y - point.y) -
                                 (point.y - before.y) * (after.x - point.x);
            if ( std::abs(cross) > EPSILON ) simplified.push_back(point);
        }
        if ( simplified.size() < 3 ) continue;
        ring = std::move(simplified);
        // 正鞋带面积对应屏幕坐标中的顺时针方向；面积绝对值用来
        // 区分外环与可能存在的内部孔洞，而不是用边数猜测外轮廓。
        double area = 0.0;
        for ( std::size_t index = 0; index < ring.size(); ++index ) {
            const auto& a = ring[index];
            const auto& b = ring[(index + 1) % ring.size()];
            area +=
                static_cast<double>(a.x) * b.y - static_cast<double>(a.y) * b.x;
        }
        if ( std::abs(area) > largestArea ) {
            // 统一提交顺时针顶点，调用方可直接绘制凹多边形。
            // 只返回一条外环，也就只会出现一个整体提示框。
            largestArea = std::abs(area);
            largest     = std::move(ring);
            if ( area < 0.0 ) std::reverse(largest.begin(), largest.end());
        }
    }
    return largest;
}
}  // namespace MMM::Canvas
