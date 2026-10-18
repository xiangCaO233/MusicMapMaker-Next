#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

namespace MMM::Canvas
{

/// @brief 视野四边形经过相机裁剪后的固定容量凸多边形。
struct CollaborationViewportPolygon {
    /// @brief 画布局部屏幕坐标，按顺时针排列以供抗锯齿填充。
    std::array<glm::vec2, 16> points{};
    /// @brief 有效顶点数量；小于三表示范围完全不可见。
    std::size_t count{ 0 };
};

/// @brief 用实际场景矩阵投影并裁剪远端视野，不在原二维画布边界预裁剪。
/// @param matrix 当前谱面场景到 Vulkan 裁剪空间的矩阵。
/// @param minimum 源平面矩形左上角，包含渲染使用的绝对 Y 偏移。
/// @param maximum 源平面矩形右下角。
/// @param size 画布逻辑像素尺寸。
/// @return 视锥内的凸多边形；退化或无效输入返回空多边形。
/// @details 对四条矩形边进行六平面 Sutherland-Hodgman 裁剪。
/// 原矩形可以越出正视画布，也可以跨越近平面，不能先用二维 AABB 判空。
/// 返回顶点只供本帧使用，不包含桌面坐标；调用方需叠加画布屏幕原点。
/// 输出保留可见裁剪边，因此跟随姓名可锚定在实际可见范围上。
/// 不把矩阵存入网络消息，各客户端仍用自己的相机观察同一个时间范围。
/// @warning 每帧每位参与者执行固定规模裁剪，不分配内存或访问会话锁。
inline CollaborationViewportPolygon projectCollaborationViewportPolygon(
    const glm::mat4& matrix, glm::vec2 minimum, glm::vec2 maximum,
    glm::vec2 size)
{
    CollaborationViewportPolygon result;
    if ( !std::isfinite(size.x) || !std::isfinite(size.y) || size.x <= 0 ||
         size.y <= 0 || maximum.x <= minimum.x || maximum.y <= minimum.y )
        return result;
    // 在齐次空间裁剪后才做透视除法；跨近平面的框不能因一个角在身后而整框消失。
    // 两份固定缓冲轮换输入输出，交点插入不会使正在遍历的边失效。
    // 六轮后保留同一源面的凸性，调用方无需逐帧构造新的几何对象。
    std::array<glm::dvec4, 16> polygon{}, output{};
    const glm::dmat4           transform(matrix);
    polygon[0] = transform * glm::dvec4(minimum.x, minimum.y, 0, 1);
    polygon[1] = transform * glm::dvec4(maximum.x, minimum.y, 0, 1);
    polygon[2] = transform * glm::dvec4(maximum.x, maximum.y, 0, 1);
    polygon[3] = transform * glm::dvec4(minimum.x, maximum.y, 0, 1);
    // 转到 double 后执行插值，减少远距离源坐标与接近近平面时的误差。
    // 任一矩阵点积非有限都会污染后续交点，必须在裁剪前整体拒绝。
    std::size_t count = 4;
    for ( std::size_t i = 0; i < count; ++i )
        for ( int axis = 0; axis < 4; ++axis )
            if ( !std::isfinite(polygon[i][axis]) ) return result;
    /// @brief 裁剪半空间的有符号距离，非负侧保留。
    /// @note Vulkan 深度为零到 w，不能套用 OpenGL 的负 w 近平面。
    const auto distance = [](const glm::dvec4& point, int plane) {
        switch ( plane ) {
        case 0: return point.w + point.x;
        case 1: return point.w - point.x;
        case 2: return point.w + point.y;
        case 3: return point.w - point.y;
        case 4: return point.z;
        default: return point.w - point.z;
        }
    };
    // 凸四边形依次裁剪六个半空间最多产生十个角，固定容量保留边界余量。
    for ( int plane = 0; plane < 6 && count >= 3; ++plane ) {
        std::size_t written          = 0;
        auto        previous         = polygon[count - 1];
        double      previousDistance = distance(previous, plane);
        for ( std::size_t i = 0; i < count; ++i ) {
            const auto   current         = polygon[i];
            const double currentDistance = distance(current, plane);
            if ( (previousDistance >= 0) != (currentDistance >= 0) ) {
                // 只有跨半空间时才求交，分母不为零且比例位于零到一之间。
                if ( written >= output.size() ) return result;
                const double ratio =
                    previousDistance / (previousDistance - currentDistance);
                output[written++] = previous + ratio * (current - previous);
            }
            if ( currentDistance >= 0 ) {
                if ( written >= output.size() ) return result;
                output[written++] = current;
            }
            previous         = current;
            previousDistance = currentDistance;
        }
        count   = written;
        polygon = output;
    }
    // 只有点或线的交集不构成可填充范围，避免退化多边形进入 ImGui。
    if ( count < 3 ) return result;
    // 透视除法只消费已裁剪点，输出不会因相机旋转越过画布剪裁边界。
    for ( std::size_t i = 0; i < count; ++i ) {
        if ( polygon[i].w <= 1e-12 ) return {};
        result.points[i] =
            glm::vec2((polygon[i].x / polygon[i].w + 1) * size.x * 0.5,
                      (polygon[i].y / polygon[i].w + 1) * size.y * 0.5);
    }
    // 翻转相机可能反转绕序；填充仍必须保持屏幕顺时针，避免内侧抗锯齿毛边。
    // 面积判定还排除角点重复造成的退化，不以任意相机角度作为显隐开关。
    double area = 0;
    for ( std::size_t i = 0; i < count; ++i ) {
        const auto& a = result.points[i];
        const auto& b = result.points[(i + 1) % count];
        area += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
    }
    if ( std::abs(area) <= 1e-8 ) return {};
    if ( area < 0 )
        std::reverse(result.points.begin(), result.points.begin() + count);
    result.count = count;
    return result;
}

/// @brief 远端视口在本地画布中的横向可见范围。
struct CollaborationViewportHorizontalRange {
    /// @brief 裁剪后的左边界。
    float leftX{ 0.0F };
    /// @brief 裁剪后的右边界。
    float rightX{ 0.0F };
};

/// @brief 计算同侧多人离屏视口箭头的横向槽位。
/// @param contentLeft 本地轨道区左边界。
/// @param contentRight 本地轨道区右边界。
/// @param canvasWidth 本地画布宽度。
/// @param slotIndex 当前箭头按临时 PeerId 排列后的槽位索引。
/// @param slotCount 当前画布同一侧的箭头总数。
/// @return 输入有效时返回不会越过画布安全边距的箭头中心 X 坐标。
///
/// 多人箭头优先在轨道区内均匀展开；轨道区不足以保持最小间距时，退回
/// 整幅画布的安全范围。slotIndex 的稳定排序由调用方负责。
/// @warning UI 热路径：每个离屏参与者调用一次；只执行常量数值计算，禁止加入
/// 分配或阻塞操作。
inline std::optional<float> layoutCollaborationViewportIndicatorX(
    float contentLeft, float contentRight, float canvasWidth,
    std::size_t slotIndex, std::size_t slotCount)
{
    // 槽位需要完整的有限几何与有效索引；无效时不绘制离屏箭头。
    if ( !std::isfinite(contentLeft) || !std::isfinite(contentRight) ||
         !std::isfinite(canvasWidth) || canvasWidth <= 20.0F ||
         contentRight <= contentLeft || slotCount == 0 ||
         slotIndex >= slotCount ) {
        return std::nullopt;
    }

    // 画布安全边距保证箭头中心不会贴住或越过裁剪边缘。
    constexpr float CANVAS_EDGE_PADDING = 10.0F;
    // 间距只用于判断轨道区域是否足够容纳多人，不强制最终像素间隔。
    constexpr float INDICATOR_SPACING = 24.0F;
    const float     canvasLeft        = CANVAS_EDGE_PADDING;
    const float     canvasRight       = canvasWidth - CANVAS_EDGE_PADDING;
    const float     contentCenter     = (contentLeft + contentRight) * 0.5F;
    if ( slotCount == 1 ) {
        // 单个参与者优先对齐轨道区域中心，轨道离屏时再夹到画布边缘。
        return std::clamp(contentCenter, canvasLeft, canvasRight);
    }

    // 多人槽位默认使用轨道区内缩后的范围，避免箭头覆盖轨道外装饰。
    float slotLeft =
        std::clamp(contentLeft + CANVAS_EDGE_PADDING, canvasLeft, canvasRight);
    float slotRight =
        std::clamp(contentRight - CANVAS_EDGE_PADDING, canvasLeft, canvasRight);
    const float requiredSpan =
        INDICATOR_SPACING * static_cast<float>(slotCount - 1);
    if ( slotRight <= slotLeft || slotRight - slotLeft < requiredSpan ) {
        // 轨道区过窄时退回整幅画布，否则多个箭头会堆叠到同一位置。
        slotLeft  = canvasLeft;
        slotRight = canvasRight;
    }

    // 首尾槽固定在可用区两端，中间槽按稳定索引均匀插值。
    const float progress =
        static_cast<float>(slotIndex) / static_cast<float>(slotCount - 1);
    return slotLeft + (slotRight - slotLeft) * progress;
}

/// @brief 将协作视野框锚定到本地轨道区并裁剪到画布内。
/// @param localContentLeft 本地相机下协作轨道区左边界。
/// @param localContentRight 本地相机下协作轨道区右边界。
/// @param canvasWidth 本地画布宽度。
/// @return 输入有效时返回经过画布边缘裁剪的范围。
///
/// 完全离屏的范围不会消失，而会收敛为对应画布边缘的最小宽度提示条。
/// @warning UI 热路径：每个远端参与者调用一次；只执行常量数值计算，禁止加入
/// 分配或阻塞操作。
inline std::optional<CollaborationViewportHorizontalRange>
projectCollaborationViewportHorizontalRange(float localContentLeft,
                                            float localContentRight,
                                            float canvasWidth)
{
    // 无效或反向轨道范围没有可解释的协作视野框。
    if ( !std::isfinite(localContentLeft) ||
         !std::isfinite(localContentRight) || !std::isfinite(canvasWidth) ||
         canvasWidth <= 4.0F || localContentRight <= localContentLeft ) {
        return std::nullopt;
    }

    // 提升到 double 后再次检查，避免极端 float 输入在后续运算中溢出。
    const double rawLeft  = static_cast<double>(localContentLeft);
    const double rawRight = static_cast<double>(localContentRight);
    if ( !std::isfinite(rawLeft) || !std::isfinite(rawRight) ) {
        return std::nullopt;
    }

    // 完全离屏的视野保留三像素边缘条，向用户提示远端所在方向。
    constexpr float EDGE_PADDING          = 2.0F;
    constexpr float MINIMUM_VISIBLE_WIDTH = 3.0F;
    const float     maximumX              = canvasWidth - EDGE_PADDING;
    if ( rawRight <= static_cast<double>(EDGE_PADDING) ) {
        // 整个范围位于左侧时固定在左安全边缘。
        return CollaborationViewportHorizontalRange{
            EDGE_PADDING, EDGE_PADDING + MINIMUM_VISIBLE_WIDTH
        };
    }
    if ( rawLeft >= static_cast<double>(maximumX) ) {
        // 整个范围位于右侧时固定在右安全边缘。
        return CollaborationViewportHorizontalRange{
            maximumX - MINIMUM_VISIBLE_WIDTH, maximumX
        };
    }

    // 部分可见范围先逐边裁剪，保留仍位于画布内的真实宽度。
    float leftX =
        std::clamp(static_cast<float>(rawLeft), EDGE_PADDING, maximumX);
    float rightX =
        std::clamp(static_cast<float>(rawRight), EDGE_PADDING, maximumX);
    if ( rightX - leftX < MINIMUM_VISIBLE_WIDTH ) {
        // 贴左边时向右补足最小宽度，其余情况从右边界向左扩张。
        if ( leftX <= EDGE_PADDING ) {
            rightX = std::min(maximumX, leftX + MINIMUM_VISIBLE_WIDTH);
        } else {
            leftX = std::max(EDGE_PADDING, rightX - MINIMUM_VISIBLE_WIDTH);
        }
    }
    // 返回值始终位于安全边距内且 leftX 不大于 rightX。
    return CollaborationViewportHorizontalRange{ leftX, rightX };
}

/// @brief 使用本地可见边界和判定线锚点投影协作视野时间。
/// @param time 待投影的远端视觉时间。
/// @param visualTime 本地判定线对应的视觉时间。
/// @param visibleTimeStart 本地画布底边对应的时间。
/// @param visibleTimeEnd 本地画布顶边对应的时间。
/// @param judgmentLineY 本地判定线 Y 坐标。
/// @param canvasHeight 本地画布高度。
/// @return 输入有效且边界可映射时返回画布 Y 坐标。
///
/// 判定线把时间轴分为两个独立线性区间，以支持上下区域滚动尺度不同的
/// 快照；范围外时间沿最近区间外推，由绘制层决定是否显示离屏提示。
/// @warning UI 热路径：缺少 ScrollSegment 时每个远端边界调用一次；只执行常量
/// 数值计算，禁止加入分配或阻塞操作。
inline std::optional<float> projectCollaborationViewportTime(
    double time, double visualTime, double visibleTimeStart,
    double visibleTimeEnd, float judgmentLineY, float canvasHeight)
{
    // 所有锚点必须有限；零高画布无法建立时间到像素的比例。
    if ( !std::isfinite(time) || !std::isfinite(visualTime) ||
         !std::isfinite(visibleTimeStart) || !std::isfinite(visibleTimeEnd) ||
         !std::isfinite(judgmentLineY) || !std::isfinite(canvasHeight) ||
         canvasHeight <= 0.0F ) {
        return std::nullopt;
    }

    // epsilon 同时处理快照计算中的浮点噪声与近乎重合的时间锚点。
    constexpr double TIME_EPSILON = 1e-9;
    // between 同时支持正常与反向时间范围，不预设谱面滚动方向。
    const auto between = [](double value, double first, double second) {
        return value >= std::min(first, second) - TIME_EPSILON &&
               value <= std::max(first, second) + TIME_EPSILON;
    };
    const auto interpolate = [](double value,
                                double firstTime,
                                double secondTime,
                                float  firstY,
                                float  secondY) -> std::optional<float> {
        // 两个时间锚点重合时没有可定义的局部比例。
        const double denominator = secondTime - firstTime;
        if ( std::abs(denominator) <= TIME_EPSILON ) return std::nullopt;
        const double ratio     = (value - firstTime) / denominator;
        const double projected = static_cast<double>(firstY) +
                                 ratio * static_cast<double>(secondY - firstY);
        // 极端外推可能溢出，不能把无穷坐标交给 ImGui。
        if ( !std::isfinite(projected) ) return std::nullopt;
        return static_cast<float>(projected);
    };

    if ( std::abs(time - visualTime) <= TIME_EPSILON ) {
        // 判定线时间直接返回精确锚点，避免插值误差造成框线抖动。
        return judgmentLineY;
    }
    if ( between(time, visibleTimeStart, visualTime) ) {
        // 下半区在画布底边与判定线之间独立插值。
        return interpolate(
            time, visibleTimeStart, visualTime, canvasHeight, judgmentLineY);
    }
    if ( between(time, visualTime, visibleTimeEnd) ) {
        // 上半区在判定线与画布顶边之间独立插值。
        return interpolate(
            time, visualTime, visibleTimeEnd, judgmentLineY, 0.0F);
    }

    // 超出可见范围时仍沿对应半区线性外推，供调用方判断离屏方向。
    const bool startSide = (visualTime >= visibleTimeStart)
                               ? time < visibleTimeStart
                               : time > visibleTimeStart;
    return startSide
               ? interpolate(time,
                             visibleTimeStart,
                             visualTime,
                             canvasHeight,
                             judgmentLineY)
               : interpolate(
                     time, visualTime, visibleTimeEnd, judgmentLineY, 0.0F);
}

/// @brief 将本地画布 Y 坐标反投影为协作视野边界时间。
/// @param canvasY 待反投影的本地画布 Y 坐标。
/// @param visualTime 本地判定线对应的视觉时间。
/// @param visibleTimeStart 本地整幅画布底边对应的时间。
/// @param visibleTimeEnd 本地整幅画布顶边对应的时间。
/// @param judgmentLineY 本地判定线 Y 坐标。
/// @param canvasHeight 本地画布高度。
/// @return 输入有效且锚点可映射时返回视觉时间。
///
/// 反投影与正向投影共享底边、判定线和顶边三个锚点，供发布端从实际轨道
/// 可见矩形计算远端协作视野，而不是错误使用整幅画布边缘。
/// @warning UI 热路径：发布本地协作视野时调用两次；只执行常量数值计算，
/// 禁止加入分配或阻塞操作。
inline std::optional<double> unprojectCollaborationViewportTime(
    float canvasY, double visualTime, double visibleTimeStart,
    double visibleTimeEnd, float judgmentLineY, float canvasHeight)
{
    // 判定线还必须位于画布高度范围内，才能把画布拆成上下两个区间。
    if ( !std::isfinite(canvasY) || !std::isfinite(visualTime) ||
         !std::isfinite(visibleTimeStart) || !std::isfinite(visibleTimeEnd) ||
         !std::isfinite(judgmentLineY) || !std::isfinite(canvasHeight) ||
         canvasHeight <= 0.0F || judgmentLineY < 0.0F ||
         judgmentLineY > canvasHeight ) {
        return std::nullopt;
    }

    // 像素 epsilon 用于识别判定线锚点和退化的局部 Y 区间。
    constexpr double POSITION_EPSILON = 1e-6;
    const auto interpolate = [](float  value,
                                float  firstY,
                                float  secondY,
                                double firstTime,
                                double secondTime) -> std::optional<double> {
        // Y 锚点重合意味着该半区没有高度，无法反推出时间比例。
        const double denominator =
            static_cast<double>(secondY) - static_cast<double>(firstY);
        if ( std::abs(denominator) <= POSITION_EPSILON ) {
            return std::nullopt;
        }
        const double ratio =
            (static_cast<double>(value) - static_cast<double>(firstY)) /
            denominator;
        const double time = firstTime + ratio * (secondTime - firstTime);
        // 非有限时间不能发布到协作状态或后续网络序列化。
        if ( !std::isfinite(time) ) return std::nullopt;
        return time;
    };

    if ( std::abs(static_cast<double>(canvasY - judgmentLineY)) <=
         POSITION_EPSILON ) {
        // 判定线位置直接映射到本地视觉时间，与正向投影保持互逆锚点。
        return visualTime;
    }
    if ( canvasY > judgmentLineY ) {
        // 判定线下方使用底边可见时间，允许 canvasY 在画布外继续外推。
        return interpolate(
            canvasY, canvasHeight, judgmentLineY, visibleTimeStart, visualTime);
    }
    // 判定线上方使用顶边可见时间，保持非等速上下区间的独立比例。
    return interpolate(
        canvasY, judgmentLineY, 0.0F, visualTime, visibleTimeEnd);
}

}  // namespace MMM::Canvas
