#pragma once

#include "config/visual/TrackLayoutConfig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace MMM::Canvas
{

/// @brief 轨道布局单边允许保留的最小归一化跨度。
inline constexpr float TRACK_LAYOUT_MIN_SPAN = 0.01f;

/// @brief 轨道布局编辑器当前命中的拖拽部位。
enum class TrackLayoutDragHandle {
    None,          ///< 未命中任何可拖拽部位。
    Left,          ///< 左边界。
    Top,           ///< 上边界。
    Right,         ///< 右边界。
    Bottom,        ///< 下边界。
    JudgmentLine,  ///< 判定线位置把手。
    Move           ///< 整体移动把手。
};

/// @brief 将判定线位置规整到画布归一化范围。
/// @param position 待规整的判定线位置。
/// @return 位于 `[0,1]` 的判定线位置；非有限值恢复为默认位置。
/// @warning UI 热路径纯计算：布局工具每帧调用；不得引入分配或阻塞操作。
[[nodiscard]] inline float sanitizeJudgmentLinePosition(float position)
{
    // 非有限值无法参与像素投影，回到编辑器约定的默认判定线高度。
    if ( !std::isfinite(position) ) {
        return 0.85f;
    }
    // 有限越界值吸附到画布上下边缘，确保后续命中区域仍可访问。
    return std::clamp(position, 0.0f, 1.0f);
}

/// @brief 将轨道布局规整到可安全编辑的合法范围。
/// @param layout 待规整布局。
/// @return 满足四边位于 `[0,1]` 且宽高不小于最小跨度的布局。
/// @warning UI 热路径纯计算：布局工具每帧调用；不得引入分配或阻塞操作。
///
/// 横纵轴分别处理，保留仍有效的字段，并以当前起始边约束对应终止边。
/// 该函数也是所有移动、缩放和命中入口的共同防御边界。
[[nodiscard]] inline Config::TrackLayout sanitizeTrackLayout(
    Config::TrackLayout layout)
{
    // 每个非有限字段单独回退，保留同一布局中其余仍有效的用户配置。
    const Config::TrackLayout fallback;
    layout.left  = std::isfinite(layout.left) ? layout.left : fallback.left;
    layout.top   = std::isfinite(layout.top) ? layout.top : fallback.top;
    layout.right = std::isfinite(layout.right) ? layout.right : fallback.right;
    layout.bottom =
        std::isfinite(layout.bottom) ? layout.bottom : fallback.bottom;

    // 先约束起始边，再以上一边和最小跨度作为终止边下限。
    layout.left = std::clamp(layout.left, 0.0f, 1.0f - TRACK_LAYOUT_MIN_SPAN);
    layout.right =
        std::clamp(layout.right, layout.left + TRACK_LAYOUT_MIN_SPAN, 1.0f);
    layout.top = std::clamp(layout.top, 0.0f, 1.0f - TRACK_LAYOUT_MIN_SPAN);
    // 横纵轴分别规范化，不改变另一轴的尺寸或位置。
    layout.bottom =
        std::clamp(layout.bottom, layout.top + TRACK_LAYOUT_MIN_SPAN, 1.0f);
    return layout;
}

/// @brief 按指针位置拖动轨道布局的一条边界。
/// @param layout 拖动开始时的布局。
/// @param handle 当前拖动边界。
/// @param normalizedPointer 指针在对应轴上的归一化坐标。
/// @return 应用边界约束后的布局。
/// @warning UI 热路径纯计算：边界拖动期间每帧调用；不得引入分配或阻塞操作。
[[nodiscard]] inline Config::TrackLayout resizeTrackLayout(
    Config::TrackLayout layout, TrackLayoutDragHandle handle,
    float normalizedPointer)
{
    // 拖动前先修复持久化旧值，后续每个句柄只改变对应一条边。
    layout = sanitizeTrackLayout(layout);
    if ( !std::isfinite(normalizedPointer) ) {
        return layout;
    }

    switch ( handle ) {
    case TrackLayoutDragHandle::Left:
        // 左边不能越过右边减去最小跨度。
        layout.left = std::clamp(
            normalizedPointer, 0.0f, layout.right - TRACK_LAYOUT_MIN_SPAN);
        break;
    case TrackLayoutDragHandle::Top:
        // 顶边与左边采用相同的起始边约束。
        layout.top = std::clamp(
            normalizedPointer, 0.0f, layout.bottom - TRACK_LAYOUT_MIN_SPAN);
        break;
    case TrackLayoutDragHandle::Right:
        // 右边不能早于左边加最小跨度，也不能超过画布。
        layout.right = std::clamp(
            normalizedPointer, layout.left + TRACK_LAYOUT_MIN_SPAN, 1.0f);
        break;
    case TrackLayoutDragHandle::Bottom:
        // 底边仅在当前顶边与画布下边界之间变化。
        layout.bottom = std::clamp(
            normalizedPointer, layout.top + TRACK_LAYOUT_MIN_SPAN, 1.0f);
        break;
    case TrackLayoutDragHandle::JudgmentLine:
    case TrackLayoutDragHandle::Move:
    case TrackLayoutDragHandle::None:
        break;
        // 非边界句柄由各自专用函数处理，本函数保持布局不变。
    }
    return layout;
}

/// @brief 平移整个轨道布局并保持其宽高不变。
/// @param layout 拖动开始时的布局。
/// @param deltaX 指针相对起点的归一化横向位移。
/// @param deltaY 指针相对起点的归一化纵向位移。
/// @return 保持完整矩形位于 `[0,1]` 内的布局。
/// @warning UI 热路径纯计算：整体拖动期间每帧调用；不得引入分配或阻塞操作。
[[nodiscard]] inline Config::TrackLayout moveTrackLayout(
    Config::TrackLayout layout, float deltaX, float deltaY)
{
    layout = sanitizeTrackLayout(layout);
    if ( !std::isfinite(deltaX) || !std::isfinite(deltaY) ) {
        return layout;
    }

    const float width  = layout.right - layout.left;
    const float height = layout.bottom - layout.top;
    // 先限制新左上角，再由原始宽高重建右下角，避免边缘裁剪改变尺寸。
    const float left = std::clamp(layout.left + deltaX, 0.0f, 1.0f - width);
    const float top  = std::clamp(layout.top + deltaY, 0.0f, 1.0f - height);
    layout.left      = left;
    layout.right     = left + width;
    layout.top       = top;
    layout.bottom    = top + height;
    return layout;
}

/// @brief 将整个轨道布局中心移动到指定画布像素坐标。
/// @param layout 当前轨道布局。
/// @param centerX 目标中心相对画布左侧的像素坐标。
/// @param centerY 目标中心相对画布顶部的像素坐标。
/// @param viewportWidth 画布宽度。
/// @param viewportHeight 画布高度。
/// @return 保持宽高且完整位于画布内的布局。
/// @warning UI 热路径纯计算：整体轨道吸附期间每帧调用；只允许常量级数值计算。
[[nodiscard]] inline Config::TrackLayout moveTrackLayoutToPixelCenter(
    Config::TrackLayout layout, float centerX, float centerY,
    float viewportWidth, float viewportHeight)
{
    layout = sanitizeTrackLayout(layout);
    if ( !std::isfinite(centerX) || !std::isfinite(centerY) ||
         !std::isfinite(viewportWidth) || !std::isfinite(viewportHeight) ||
         viewportWidth <= 0.0f || viewportHeight <= 0.0f ) {
        return layout;
    }

    const float currentCenterX = (layout.left + layout.right) * 0.5f;
    const float currentCenterY = (layout.top + layout.bottom) * 0.5f;
    // 像素目标先除以视口尺寸转为归一化中心，再复用整体移动边界规则。
    return moveTrackLayout(layout,
                           centerX / viewportWidth - currentCenterX,
                           centerY / viewportHeight - currentCenterY);
}

/// @brief 在画布像素坐标中命中轨道边界、判定线或整体移动把手。
/// @param layout 当前轨道布局。
/// @param judgmentLinePosition 当前判定线归一化位置。
/// @param pointerX 指针相对画布左侧的像素坐标。
/// @param pointerY 指针相对画布顶部的像素坐标。
/// @param viewportWidth 画布宽度。
/// @param viewportHeight 画布高度。
/// @param edgeHitRadius 边界两侧的命中半径。
/// @param moveHandleRadius 中心整体移动把手的命中半径。
/// @return 命中的拖拽部位；未命中时返回 None。
/// @warning UI 热路径纯计算：布局工具每帧调用；只允许常量级数值比较。
[[nodiscard]] inline TrackLayoutDragHandle hitTestTrackLayout(
    Config::TrackLayout layout, float judgmentLinePosition, float pointerX,
    float pointerY, float viewportWidth, float viewportHeight,
    float edgeHitRadius, float moveHandleRadius)
{
    // 无效尺寸或半径会破坏距离比较，统一视为未命中。
    if ( !std::isfinite(pointerX) || !std::isfinite(pointerY) ||
         !std::isfinite(viewportWidth) || !std::isfinite(viewportHeight) ||
         !std::isfinite(edgeHitRadius) || !std::isfinite(moveHandleRadius) ||
         viewportWidth <= 0.0f || viewportHeight <= 0.0f ||
         edgeHitRadius < 0.0f || moveHandleRadius < 0.0f ) {
        return TrackLayoutDragHandle::None;
    }

    layout = sanitizeTrackLayout(layout);
    // 命中测试统一在像素空间进行，使触控半径不随归一化布局尺寸变化。
    const float left    = layout.left * viewportWidth;
    const float right   = layout.right * viewportWidth;
    const float top     = layout.top * viewportHeight;
    const float bottom  = layout.bottom * viewportHeight;
    const float centerX = (left + right) * 0.5f;
    const float centerY = (top + bottom) * 0.5f;
    const float judgmentLineY =
        sanitizeJudgmentLinePosition(judgmentLinePosition) * viewportHeight;

    if ( std::abs(pointerX - centerX) <= moveHandleRadius &&
         std::abs(pointerY - centerY) <= moveHandleRadius ) {
        // 中心移动把手优先，防止极窄布局同时命中边缘时无法整体移动。
        return TrackLayoutDragHandle::Move;
    }
    if ( std::abs(pointerX - right) <= moveHandleRadius &&
         std::abs(pointerY - judgmentLineY) <= edgeHitRadius ) {
        // 判定线把手固定在右边界处，与普通 Right 句柄通过纵坐标区分。
        return TrackLayoutDragHandle::JudgmentLine;
    }

    TrackLayoutDragHandle closest = TrackLayoutDragHandle::None;
    float closestDistance         = std::numeric_limits<float>::infinity();
    auto  consider =
        [&](TrackLayoutDragHandle handle, float distance, bool withinSpan) {
            // 只接受在线段延长命中区内且比当前候选更近的边。
            if ( withinSpan && distance <= edgeHitRadius &&
                 distance < closestDistance ) {
                closest         = handle;
                closestDistance = distance;
            }
        };

    consider(
        TrackLayoutDragHandle::Left,
        std::abs(pointerX - left),
        pointerY >= top - edgeHitRadius && pointerY <= bottom + edgeHitRadius);
    consider(
        TrackLayoutDragHandle::Right,
        std::abs(pointerX - right),
        pointerY >= top - edgeHitRadius && pointerY <= bottom + edgeHitRadius);
    consider(
        TrackLayoutDragHandle::Top,
        std::abs(pointerY - top),
        pointerX >= left - edgeHitRadius && pointerX <= right + edgeHitRadius);
    consider(
        TrackLayoutDragHandle::Bottom,
        std::abs(pointerY - bottom),
        pointerX >= left - edgeHitRadius && pointerX <= right + edgeHitRadius);
    // 等距时保留检查顺序，给角点提供稳定句柄优先级。
    return closest;
}

/// @brief 可独立调整的主画布辅助区域。
enum class AuxiliaryLayoutRegion : std::uint8_t {
    None = 0,    ///< 未选中辅助区域。
    Draft,       ///< 草稿轨道区。
    Annotation,  ///< 批注区。
    Bgm,         ///< BGM 轨道区。
};

/// @brief 辅助区域横向拖动句柄。
enum class HorizontalRegionDragHandle : std::uint8_t {
    None = 0,  ///< 未命中辅助区域。
    Left,      ///< 调整左边界与宽度。
    Right,     ///< 调整右边界与宽度。
    Move,      ///< 仅移动 X 位置。
};

/// @brief 已解析的辅助区域归一化横向边界。
struct HorizontalRegionBounds {
    /// @brief 左边界比例。
    float left{ 0.0F };
    /// @brief 区域总宽度比例。
    float width{ 0.1F };
    /// @brief 返回右边界比例。
    [[nodiscard]] float right() const { return left + width; }
};

/// @brief 宽度缩放边缘的一维像素吸附结果。
struct HorizontalResizeSnapResult {
    /// @brief 应用于缩放句柄的像素横坐标。
    float position{ 0.0F };
    /// @brief 是否命中有效目标边缘。
    bool snapped{ false };
    /// @brief 命中的目标边缘像素横坐标。
    float target{ 0.0F };
};

/// @brief 将宽度缩放句柄吸附到距离最近的其他组件边缘。
/// @param position 尚未吸附的句柄像素横坐标。
/// @param targets 手势开始时冻结的目标边缘像素坐标。
/// @param threshold 最大吸附距离，单位逻辑像素。
/// @return 最近目标位于阈值内时返回其坐标，否则保持原坐标。
/// @warning UI 布局热路径：宽度缩放期间每帧扫描冻结目标；禁止分配或阻塞。
[[nodiscard]] inline HorizontalResizeSnapResult snapHorizontalResizeEdge(
    float position, std::span<const float> targets, float threshold)
{
    HorizontalResizeSnapResult result{ .position = position };
    // 非有限指针位置无法比较，但仍以原输入构造未吸附结果。
    if ( !std::isfinite(position) ) return result;
    // 非有限或负阈值按零处理，精确重合仍可以稳定吸附。
    threshold = std::isfinite(threshold) ? std::max(0.0F, threshold) : 0.0F;
    float bestDistance = std::numeric_limits<float>::infinity();
    for ( const float target : targets ) {
        if ( !std::isfinite(target) ) continue;
        const float distance = std::abs(position - target);
        // 严格小于 bestDistance 使等距目标沿冻结数组顺序稳定选择。
        if ( distance <= threshold && distance < bestDistance ) {
            // 严格采用更近目标，等距时保持目标缓存中的稳定优先级。
            result.position = target;
            result.snapped  = true;
            result.target   = target;
            bestDistance    = distance;
        }
    }
    return result;
}

/// @brief 规整辅助区域横向边界。
/// @param bounds 待规整边界。
/// @return 保持正宽度且数值有限的边界。
[[nodiscard]] inline HorizontalRegionBounds sanitizeHorizontalRegionBounds(
    HorizontalRegionBounds bounds)
{
    // 区域可以位于当前视口之外，以便相机横向移动后继续访问。
    constexpr float minPosition = -4.0F;
    constexpr float maxPosition = 4.0F;
    constexpr float minWidth    = 0.005F;
    constexpr float maxWidth    = 4.0F;
    if ( !std::isfinite(bounds.left) ) bounds.left = 0.0F;
    if ( !std::isfinite(bounds.width) ) bounds.width = 0.1F;
    // 位置允许跨出当前视口，宽度仍保持正值和可编辑上限。
    bounds.left  = std::clamp(bounds.left, minPosition, maxPosition);
    bounds.width = std::clamp(bounds.width, minWidth, maxWidth);
    return bounds;
}

/// @brief 横向缩放辅助区域，不修改任何纵向配置。
/// @param start 拖动开始时的边界。
/// @param handle 左侧或右侧句柄。
/// @param pointerX 当前归一化横坐标。
/// @return 缩放后的有效边界。
[[nodiscard]] inline HorizontalRegionBounds resizeHorizontalRegion(
    HorizontalRegionBounds start, HorizontalRegionDragHandle handle,
    float pointerX)
{
    start = sanitizeHorizontalRegionBounds(start);
    // 无效指针不改变已经规范化的起始布局。
    if ( !std::isfinite(pointerX) ) return start;
    constexpr float minWidth = 0.005F;
    if ( handle == HorizontalRegionDragHandle::Left ) {
        // 右边界固定，左边界不会越过最小宽度。
        const float right = start.right();
        start.left        = std::min(pointerX, right - minWidth);
        start.width       = right - start.left;
    } else if ( handle == HorizontalRegionDragHandle::Right ) {
        // 左边界固定，只更新区域总宽度。
        start.width = std::max(minWidth, pointerX - start.left);
    }
    // 二次规范化处理指针远离视口时可能产生的超大位置或宽度。
    return sanitizeHorizontalRegionBounds(start);
}

/// @brief 横向移动辅助区域并保持宽度。
/// @param start 拖动开始时的边界。
/// @param deltaX 归一化横向位移。
/// @return 仅左边界变化的有效边界。
[[nodiscard]] inline HorizontalRegionBounds moveHorizontalRegion(
    HorizontalRegionBounds start, float deltaX)
{
    start = sanitizeHorizontalRegionBounds(start);
    if ( !std::isfinite(deltaX) ) return start;
    start.left += deltaX;
    // 只更新 left，原宽度经规范化后保持不变。
    return sanitizeHorizontalRegionBounds(start);
}

/// @brief 辅助区整体移动的边缘吸附结果。
struct HorizontalMoveSnapResult {
    HorizontalRegionBounds bounds;            ///< 保持宽度的最终归一化边界。
    bool                   snapped{ false };  ///< 最终边界是否确实到达参考线。
    float                  target{ 0.0F };    ///< 命中的世界像素横坐标。
};

/// @brief 整体移动时让辅助区左边或右边吸附到最近目标。
/// @param bounds 尚未吸附的归一化区域。
/// @param viewportWidth 逻辑像素画布宽度。
/// @param targets 手势开始时冻结的世界像素目标。
/// @param threshold 最大逻辑像素吸附距离。
/// @warning UI 热路径：只扫描冻结的一维目标，不分配或查询其他线程。
[[nodiscard]] inline HorizontalMoveSnapResult snapHorizontalRegionMove(
    HorizontalRegionBounds bounds, float viewportWidth,
    std::span<const float> targets, float threshold)
{
    HorizontalMoveSnapResult result{ sanitizeHorizontalRegionBounds(bounds) };
    if ( !std::isfinite(viewportWidth) || viewportWidth <= 0.0F ) return result;
    const float left      = result.bounds.left * viewportWidth;
    const float right     = result.bounds.right() * viewportWidth;
    const auto  leftSnap  = snapHorizontalResizeEdge(left, targets, threshold);
    const auto  rightSnap = snapHorizontalResizeEdge(right, targets, threshold);
    // 两侧同时命中时选择所需平移最小的一侧，等距时稳定优先左侧。
    const bool useLeft =
        leftSnap.snapped &&
        (!rightSnap.snapped || std::abs(leftSnap.position - left) <=
                                   std::abs(rightSnap.position - right));
    const auto& snap = useLeft ? leftSnap : rightSnap;
    if ( !snap.snapped ) return result;
    const float delta = snap.position - (useLeft ? left : right);
    result.bounds = moveHorizontalRegion(result.bounds, delta / viewportWidth);
    // 位置钳制后必须复核，不能显示并未真正贴合的参考线。
    const float edge =
        (useLeft ? result.bounds.left : result.bounds.right()) * viewportWidth;
    result.snapped = std::abs(edge - snap.target) <= 0.25F;
    result.target  = snap.target;
    return result;
}

/// @brief 主轨道、草稿、批注和 BGM 的冻结横向边界，顺序与辅助区枚举对应。
using HorizontalLayoutBounds = std::array<HorizontalRegionBounds, 4>;

/// @brief 缩放交界处时平移原本贴合该侧的相邻区域及连续相邻链。
/// @param start 按下时冻结的四个区域边界。
/// @param active 被调整区域的数组下标，零代表主轨道。
/// @param candidate 当前区域按自身约束缩放后的候选边界。
/// @param leftEdge 是否拖动左边界。
/// @param contactTolerance 认定原本相接的归一化误差，仅用于冻结起点。
/// @return 整组最终边界；相邻区域宽度不变，未贴合区域保持起点。
/// @pre start 来自同一次手势开始时的合法投影。
/// @pre candidate 已经过当前区域自身的最小宽度约束。
/// @note 位置受限时整组共同缩小位移，不会单独拉开任一个接缝。
/// @warning UI 热路径：固定四区、固定轮数，无堆分配、锁或持久化操作。
[[nodiscard]] inline HorizontalLayoutBounds resizeJoinedHorizontalRegions(
    const HorizontalLayoutBounds& start, std::size_t active,
    HorizontalRegionBounds candidate, bool leftEdge, float contactTolerance)
{
    // 结果首先完整复制起点，未连接成员不受候选位置或浮点累积影响。
    // 输入来自已规整投影；不在联动计算中重新推导可选配置。
    auto result = start;
    if ( active >= start.size() ) return result;
    std::array<bool, 4> joined{};
    // 活动区只提供扩展起点，其自身在后续位移循环中始终单独处理。
    joined[active] = true;
    // 仅沿被拖边一侧追踪首尾相接关系，不把同边对齐或重叠区域当邻居。
    // 重复固定轮数覆盖任意数组顺序，已加入标记阻止闭环重复处理。
    for ( std::size_t pass = 0; pass < start.size(); ++pass ) {
        for ( std::size_t i = 0; i < start.size(); ++i ) {
            if ( joined[i] ) continue;
            for ( std::size_t j = 0; j < start.size(); ++j ) {
                if ( !joined[j] ) continue;
                // 只匹配相反两条边，中心线或同侧边重合不构成连接。
                const float distance = leftEdge
                                           ? start[i].right() - start[j].left
                                           : start[i].left - start[j].right();
                if ( std::abs(distance) <= contactTolerance ) joined[i] = true;
            }
        }
    }
    // 保持活动区另一侧边不动，统一位移只描述交接边缘的变化。
    const float oldEdge = leftEdge ? start[active].left : start[active].right();
    float delta = (leftEdge ? candidate.left : candidate.right()) - oldEdge;
    if ( !std::isfinite(delta) ) return result;
    // 统一约束组位移，避免单独钳制邻居后把交界重新拉开。
    // 主轨道必须留在画布内；辅助区继续支持相机视口外的配置空间。
    for ( std::size_t i = 0; i < start.size(); ++i ) {
        if ( i == active || !joined[i] ) continue;
        // 起点保证零位移合法，所以依次求交不会制造反向跳动。
        const float minLeft = i == 0 ? 0.0F : -4.0F;
        const float maxLeft = i == 0 ? 1.0F - start[i].width : 4.0F;
        delta =
            std::clamp(delta, minLeft - start[i].left, maxLeft - start[i].left);
    }
    // 受邻居边界限制后重新构造活动宽度，不继续使用未经组约束的候选。
    // 从起点重建也允许鼠标改变方向，联动效果立即可逆。
    result[active] = start[active];
    if ( leftEdge ) {
        result[active].left += delta;
        result[active].width -= delta;
    } else {
        result[active].width += delta;
    }
    // 所有跟随区域采用同一位移，保留各自宽度及相互接缝。
    for ( std::size_t i = 0; i < start.size(); ++i )
        if ( i != active && joined[i] ) result[i].left += delta;
    return result;
}

/// @brief 将完整横向区域边界写回配置，统一处理单轨宽度和锚点方向。
/// @param layout 本次手势的配置候选。
/// @param index 主轨道零，草稿一，批注二，BGM 三。
/// @param bounds 整个区域的归一化边界。
/// @param draftCount 草稿组轨道数。
/// @param bgmCount BGM 组轨道数。
/// @warning UI 热路径：仅写内存值，保存配置由鼠标释放路径统一执行。
inline void storeHorizontalLayoutBounds(Config::TrackLayout&   layout,
                                        std::size_t            index,
                                        HorizontalRegionBounds bounds,
                                        std::uint32_t          draftCount,
                                        std::uint32_t          bgmCount)
{
    if ( index == 0 ) {
        layout.left  = bounds.left;
        layout.right = bounds.right();
        return;
    }
    // 拒绝无效区域编号，不能把未知枚举默认为 BGM 覆盖配置。
    if ( index > 3 ) return;
    auto& region = index == 1   ? layout.draftLanes
                   : index == 2 ? layout.annotation
                                : layout.bgmLanes;
    // 草稿向左增长，继续锁定右锚；其他区域使用左锚，清除竞争的旧右锚。
    if ( index == 1 ) {
        region.left.reset();
        region.right = bounds.right();
    } else {
        region.left = bounds.left;
        region.right.reset();
    }
    // 数量变化前后仍按单轨语义存储，零轨道使用一作为防御性分母。
    // 轨道组的总宽度只用于本轮屏幕交互，不作为新的持久化字段。
    const auto count = index == 1   ? std::max(draftCount, 1U)
                       : index == 3 ? std::max(bgmCount, 1U)
                                    : 1U;
    region.width     = bounds.width / static_cast<float>(count);
}

/// @brief 比较布局候选是否需要发布，包含辅助区域可选锚点的存在状态。
/// @warning UI 热路径：固定字段比较，不序列化配置或执行文件操作。
[[nodiscard]] inline bool horizontalLayoutsEqual(const Config::TrackLayout& a,
                                                 const Config::TrackLayout& b)
{
    // 相同指针位置从同一冻结起点计算，精确比较即可消除静止帧重复发布。
    const auto sameRegion = [](const auto& x, const auto& y) {
        return x.left == y.left && x.right == y.right && x.width == y.width;
    };
    return a.left == b.left && a.right == b.right && a.top == b.top &&
           a.bottom == b.bottom && sameRegion(a.draftLanes, b.draftLanes) &&
           sameRegion(a.annotation, b.annotation) &&
           sameRegion(a.bgmLanes, b.bgmLanes);
}

/// @brief 在像素空间命中辅助区域横向句柄。
/// @param bounds 归一化横向边界。
/// @param top 顶部像素边界，来自主轨道区。
/// @param bottom 底部像素边界，来自主轨道区。
/// @param pointerX 指针像素横坐标。
/// @param pointerY 指针像素纵坐标。
/// @param viewportWidth 逻辑视口宽度。
/// @param edgeHitRadius 边缘命中半径。
/// @param moveHandleRadius 中心移动句柄半径。
/// @return 最近的横向句柄；纵向边界只参与命中，不可编辑。
/// @warning UI 热路径纯计算：辅助区布局编辑期间每帧调用，不执行分配。
[[nodiscard]] inline HorizontalRegionDragHandle hitTestHorizontalRegion(
    HorizontalRegionBounds bounds, float top, float bottom, float pointerX,
    float pointerY, float viewportWidth, float edgeHitRadius,
    float moveHandleRadius)
{
    bounds = sanitizeHorizontalRegionBounds(bounds);
    // 纵向只决定是否允许命中，辅助区不拥有独立的纵向编辑状态。
    if ( viewportWidth <= 0.0F || pointerY < top - edgeHitRadius ||
         pointerY > bottom + edgeHitRadius ) {
        return HorizontalRegionDragHandle::None;
    }
    const float left   = bounds.left * viewportWidth;
    const float right  = bounds.right() * viewportWidth;
    const float center = (left + right) * 0.5F;
    // 边缘优先于中心，窄区域仍可稳定缩放。
    if ( std::abs(pointerX - left) <= edgeHitRadius ) {
        return HorizontalRegionDragHandle::Left;
    }
    if ( std::abs(pointerX - right) <= edgeHitRadius ) {
        return HorizontalRegionDragHandle::Right;
    }
    const float centerY = (top + bottom) * 0.5F;
    // 移动把手要求同时靠近横向中心和区域纵向中心。
    if ( std::abs(pointerX - center) <= moveHandleRadius &&
         std::abs(pointerY - centerY) <= moveHandleRadius ) {
        return HorizontalRegionDragHandle::Move;
    }
    return HorizontalRegionDragHandle::None;
}

}  // namespace MMM::Canvas
