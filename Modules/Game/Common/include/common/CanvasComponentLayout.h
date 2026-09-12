#pragma once

#include "config/visual/CanvasComponentConfig.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

namespace MMM::Logic
{

/// @brief 可选画布组件的拖动部位。
/// @note 四角值用于缩放，Move 用于平移，None 表示未命中可交互区域。
enum class CanvasComponentDragHandle {
    None,
    Move,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

/// @brief 允许保存的最小字号高度比例。
/// @note 下限防止组件缩小到无法继续命中的尺寸。
inline constexpr float CANVAS_COMPONENT_MIN_FONT_SIZE_RATIO = 0.0125f;
/// @brief 允许保存的最大字号高度比例。
/// @note 上限防止单个组件覆盖整个画布交互区域。
inline constexpr float CANVAS_COMPONENT_MAX_FONT_SIZE_RATIO = 0.25f;
/// @brief 允许通过布局包围框设置的最小物件横纵缩放。
/// @note 横纵轴分别钳制，允许保持非等比缩放结果。
inline constexpr float NOTE_RENDER_MIN_SCALE = 0.5f;
/// @brief 允许通过布局包围框设置的最大物件横纵缩放。
/// @note 与最小值共同构成可持久化的视觉配置范围。
inline constexpr float NOTE_RENDER_MAX_SCALE = 3.0f;

/// @brief 画布组件在当前视口中的像素边界。
struct CanvasComponentBounds {
    /// @brief 左边界的画布局部横坐标。
    float left{ 0.0f };
    /// @brief 上边界的画布局部纵坐标。
    float top{ 0.0f };
    /// @brief 右边界的画布局部横坐标。
    float right{ 0.0f };
    /// @brief 下边界的画布局部纵坐标。
    float bottom{ 0.0f };

    /// @brief 取得边界宽度。
    /// @return 非负像素宽度。
    /// @warning 布局热路径：仅执行常量级减法和钳制。
    [[nodiscard]] float width() const { return std::max(0.0f, right - left); }

    /// @brief 取得边界高度。
    /// @return 非负像素高度。
    /// @warning 布局热路径：仅执行常量级减法和钳制。
    [[nodiscard]] float height() const { return std::max(0.0f, bottom - top); }

    /// @brief 判断像素点是否位于边界内。
    /// @param x 画布局部横坐标。
    /// @param y 画布局部纵坐标。
    /// @return 点位于边界内时返回 true。
    /// @warning 指针命中热路径：不得加入额外容器查询或状态读取。
    [[nodiscard]] bool contains(float x, float y) const
    {
        return x >= left && x <= right && y >= top && y <= bottom;
    }
};

/// @brief 画布组件布局计算使用的二维像素点。
struct CanvasComponentPoint {
    /// @brief 横坐标。
    float x{ 0.0f };
    /// @brief 纵坐标。
    float y{ 0.0f };
};

/// @brief 物件渲染的独立横纵缩放。
struct NoteRenderScale {
    /// @brief 横向缩放。
    float x{ 1.0f };
    /// @brief 纵向缩放。
    float y{ 1.0f };
};

/// @brief 依据以物件中心为固定点的四角拖动计算独立横纵缩放。
/// @param startScale 拖动开始时的横纵缩放。
/// @param handle 当前拖动的包围框角点。
/// @param startBounds 拖动开始时的物件像素边界。
/// @param pointerX 当前指针横坐标。
/// @param pointerY 当前指针纵坐标。
/// @return 限制在视觉配置合法范围内的横纵缩放。
/// @warning UI 布局热路径：物件缩放期间每帧调用；只允许常量级数值计算。
[[nodiscard]] inline NoteRenderScale resizeNoteRenderScale(
    NoteRenderScale startScale, CanvasComponentDragHandle handle,
    const CanvasComponentBounds& startBounds, float pointerX, float pointerY)
{
    // 输入缩放来自持久化配置，先恢复非有限值并限制到允许范围。
    const auto sanitizeScale = [](float value) {
        if ( !std::isfinite(value) ) return 1.0f;
        return std::clamp(value, NOTE_RENDER_MIN_SCALE, NOTE_RENDER_MAX_SCALE);
    };
    startScale.x = sanitizeScale(startScale.x);
    startScale.y = sanitizeScale(startScale.y);
    // 非缩放把手、退化边界或无效指针不能形成可靠比例，保持起始值。
    if ( handle == CanvasComponentDragHandle::None ||
         handle == CanvasComponentDragHandle::Move ||
         startBounds.width() <= 0.0f || startBounds.height() <= 0.0f ||
         !std::isfinite(pointerX) || !std::isfinite(pointerY) ) {
        return startScale;
    }

    // 物件中心在本次拖动期间固定，各轴相对中心独立计算目标半尺寸。
    const float centerX   = (startBounds.left + startBounds.right) * 0.5f;
    const float centerY   = (startBounds.top + startBounds.bottom) * 0.5f;
    const bool  dragsLeft = handle == CanvasComponentDragHandle::TopLeft ||
                            handle == CanvasComponentDragHandle::BottomLeft;
    // 上侧把手使用中心到指针的反向距离，下侧把手使用正向距离。
    const bool  dragsTop = handle == CanvasComponentDragHandle::TopLeft ||
                           handle == CanvasComponentDragHandle::TopRight;
    const float targetHalfWidth =
        dragsLeft ? centerX - pointerX : pointerX - centerX;
    const float targetHalfHeight =
        dragsTop ? centerY - pointerY : pointerY - centerY;
    // 目标半尺寸与起始半尺寸的比值叠加到各轴起始缩放。
    // 指针越过中心产生的负比例最终由 sanitizeScale 钳制到合法下限。
    startScale.x = sanitizeScale(startScale.x * targetHalfWidth /
                                 (startBounds.width() * 0.5f));
    startScale.y = sanitizeScale(startScale.y * targetHalfHeight /
                                 (startBounds.height() * 0.5f));
    return startScale;
}

/// @brief 组件移动对齐到目标线后的二维吸附结果。
struct CanvasComponentSnapResult {
    /// @brief 应用于组件的新中心位置。
    CanvasComponentPoint center;
    /// @brief 横向是否已吸附。
    bool snappedX{ false };
    /// @brief 纵向是否已吸附。
    bool snappedY{ false };
    /// @brief 横向吸附目标线的像素坐标。
    float targetX{ 0.0f };
    /// @brief 纵向吸附目标线的像素坐标。
    float targetY{ 0.0f };
};

/// @brief 将组件左、中、右与上、中、下对齐到各轴最近的目标线。
/// @param bounds 尚未吸附的组件像素边界。
/// @param xTargets 可吸附的纵向目标线横坐标。
/// @param yTargets 可吸附的横向目标线纵坐标。
/// @param threshold 最大吸附距离，单位像素。
/// @return 横纵轴独立选择最近目标后的组件中心与目标线。
/// @warning UI 拖动热路径：每帧扫描已缓存的目标坐标，禁止加入文件访问、
/// 阻塞操作或共享所有权复制。
[[nodiscard]] inline CanvasComponentSnapResult snapCanvasComponentBounds(
    const CanvasComponentBounds& bounds, std::span<const float> xTargets,
    std::span<const float> yTargets, float threshold)
{
    CanvasComponentSnapResult result;
    // 未吸附时默认保持当前中心，横纵轴结果互不影响。
    result.center = { (bounds.left + bounds.right) * 0.5f,
                      (bounds.top + bounds.bottom) * 0.5f };
    if ( bounds.width() <= 0.0f || bounds.height() <= 0.0f ) {
        // 退化边界没有可靠的左中右或上中下参考线。
        return result;
    }

    // 负阈值和非有限阈值都退化为仅允许精确重合的零阈值。
    threshold = std::isfinite(threshold) ? std::max(0.0f, threshold) : 0.0f;
    // 单轴帮助器比较组件三条参考线与所有预先缓存的目标线。
    const auto snapAxis = [threshold](const std::array<float, 3>& sourceLines,
                                      std::span<const float>
                                             targets,
                                      float& center,
                                      bool&  snapped,
                                      float& targetLine) {
        // 初始距离设为最大值，确保首个阈值内候选能够成为当前最优项。
        float bestDistance = std::numeric_limits<float>::max();
        float bestOffset   = 0.0f;
        for ( float target : targets ) {
            // 无效目标线不参与距离计算，防止 NaN 污染最优值。
            if ( !std::isfinite(target) ) continue;
            for ( float source : sourceLines ) {
                // 偏移保留方向，绝对值只用于比较吸附距离。
                const float offset   = target - source;
                const float distance = std::abs(offset);
                if ( distance <= threshold && distance < bestDistance ) {
                    // 只接受更近候选；等距时保持目标列表中先出现的线。
                    bestDistance = distance;
                    bestOffset   = offset;
                    targetLine   = target;
                    snapped      = true;
                }
            }
        }
        // 找到候选后一次性移动中心，避免遍历途中累计多条吸附偏移。
        if ( snapped ) center += bestOffset;
    };

    // X 轴比较左、中、右三线，Y 轴比较上、中、下三线。
    const std::array<float, 3> sourceX{ bounds.left,
                                        result.center.x,
                                        bounds.right };
    const std::array<float, 3> sourceY{ bounds.top,
                                        result.center.y,
                                        bounds.bottom };
    // 两次调用分别写入轴向标志和目标线，允许只吸附一个方向。
    snapAxis(
        sourceX, xTargets, result.center.x, result.snappedX, result.targetX);
    snapAxis(
        sourceY, yTargets, result.center.y, result.snappedY, result.targetY);
    return result;
}

/// @brief 规整画布组件的归一化锚点。
/// @param placement 待规整布局。
/// @return 锚点有限且处于画布范围内的布局。
[[nodiscard]] inline Config::CanvasComponentPlacement
sanitizeCanvasComponentPlacement(Config::CanvasComponentPlacement placement)
{
    // 默认布局为每个非法字段提供独立回退值，避免整体丢弃其他有效设置。
    const Config::CanvasComponentPlacement fallback;
    if ( !std::isfinite(placement.anchorX) ) {
        // 锚点非有限时恢复默认横向位置。
        placement.anchorX = fallback.anchorX;
    }
    if ( !std::isfinite(placement.anchorY) ) {
        // 锚点非有限时恢复默认纵向位置。
        placement.anchorY = fallback.anchorY;
    }
    if ( !std::isfinite(placement.fontSizeRatio) ) {
        // 字号比例非有限时恢复配置类型提供的默认大小。
        placement.fontSizeRatio = fallback.fontSizeRatio;
    }
    // 锚点限制在归一化画布区域，字号限制在可交互范围。
    placement.anchorX       = std::clamp(placement.anchorX, 0.0f, 1.0f);
    placement.anchorY       = std::clamp(placement.anchorY, 0.0f, 1.0f);
    placement.fontSizeRatio = std::clamp(placement.fontSizeRatio,
                                         CANVAS_COMPONENT_MIN_FONT_SIZE_RATIO,
                                         CANVAS_COMPONENT_MAX_FONT_SIZE_RATIO);
    for ( std::size_t index = 0U; index < placement.color.size(); ++index ) {
        // 每个颜色分量单独恢复，保留同一颜色中的其他有效分量。
        if ( !std::isfinite(placement.color[index]) ) {
            placement.color[index] = fallback.color[index];
        }
        // 颜色存储契约使用归一化分量，越界输入在保存前钳制。
        placement.color[index] = std::clamp(placement.color[index], 0.0f, 1.0f);
    }
    return placement;
}

/// @brief 计算画布组件在指定布局区域中的像素边界。
/// @param placement 组件布局。
/// @param region 组件允许占用的像素区域。
/// @param contentWidth 组件内容宽度。
/// @param contentHeight 组件内容高度。
/// @return 完整限制在布局区域内的组件边界。
/// @warning 热路径：布局编辑和快照生成时调用；只允许常量级数值计算。
[[nodiscard]] inline CanvasComponentBounds canvasComponentBoundsInRegion(
    const Config::CanvasComponentPlacement& placement,
    const CanvasComponentBounds& region, float contentWidth,
    float contentHeight)
{
    // 区域允许调用方以任意角点顺序传入，先规范化为左上到右下。
    const float regionLeft   = std::min(region.left, region.right);
    const float regionTop    = std::min(region.top, region.bottom);
    const float regionRight  = std::max(region.left, region.right);
    const float regionBottom = std::max(region.top, region.bottom);
    const float regionWidth  = regionRight - regionLeft;
    const float regionHeight = regionBottom - regionTop;
    // 无效或退化区域无法放置内容，返回零边界避免传播 NaN。
    if ( !std::isfinite(regionWidth) || !std::isfinite(regionHeight) ||
         regionWidth <= 0.0f || regionHeight <= 0.0f ) {
        return {};
    }

    // 内容尺寸非有限时视为零，过大时限制为区域完整尺寸。
    const float width = std::clamp(
        std::isfinite(contentWidth) ? contentWidth : 0.0f, 0.0f, regionWidth);
    const float height =
        std::clamp(std::isfinite(contentHeight) ? contentHeight : 0.0f,
                   0.0f,
                   regionHeight);

    // 位置计算始终使用规整后的锚点，防止旧配置中的非法值进入投影。
    const auto  sanitized  = sanitizeCanvasComponentPlacement(placement);
    const float halfWidth  = width * 0.5f;
    const float halfHeight = height * 0.5f;
    // 内容占满区域时强制居中，否则把锚点中心限制到可完整容纳内容的范围。
    const float centerX =
        width >= regionWidth
            ? regionLeft + regionWidth * 0.5f
            : std::clamp(regionLeft + sanitized.anchorX * regionWidth,
                         regionLeft + halfWidth,
                         regionRight - halfWidth);
    const float centerY =
        height >= regionHeight
            ? regionTop + regionHeight * 0.5f
            : std::clamp(regionTop + sanitized.anchorY * regionHeight,
                         regionTop + halfHeight,
                         regionBottom - halfHeight);
    // 由最终中心与半尺寸还原边界，保证结果不超出规范化区域。
    return { centerX - halfWidth,
             centerY - halfHeight,
             centerX + halfWidth,
             centerY + halfHeight };
}

/// @brief 计算画布组件的像素边界。
/// @param placement 组件布局。
/// @param viewportWidth 画布宽度。
/// @param viewportHeight 画布高度。
/// @param contentWidth 组件内容宽度。
/// @param contentHeight 组件内容高度。
/// @return 完整限制在画布内的组件边界。
/// @warning 热路径：布局编辑和快照生成时调用；只允许常量级数值计算。
[[nodiscard]] inline CanvasComponentBounds canvasComponentBounds(
    const Config::CanvasComponentPlacement& placement, float viewportWidth,
    float viewportHeight, float contentWidth, float contentHeight)
{
    // 全画布版本委托给区域实现，保持边界钳制规则只有一份。
    return canvasComponentBoundsInRegion(
        placement,
        { 0.0f, 0.0f, viewportWidth, viewportHeight },
        contentWidth,
        contentHeight);
}

/// @brief 将画布组件移动到指定区域内的像素中心并保持完整可见。
/// @param placement 原布局。
/// @param centerX 目标像素中心横坐标。
/// @param centerY 目标像素中心纵坐标。
/// @param region 组件允许占用的像素区域。
/// @param contentWidth 组件内容宽度。
/// @param contentHeight 组件内容高度。
/// @return 更新后的归一化布局。
/// @warning 热路径：组件拖动期间每帧调用；只允许常量级数值计算。
[[nodiscard]] inline Config::CanvasComponentPlacement
moveCanvasComponentInRegion(Config::CanvasComponentPlacement placement,
                            float centerX, float centerY,
                            const CanvasComponentBounds& region,
                            float contentWidth, float contentHeight)
{
    // 与边界计算相同，先规范化可能反向传入的布局区域。
    const float regionLeft   = std::min(region.left, region.right);
    const float regionTop    = std::min(region.top, region.bottom);
    const float regionRight  = std::max(region.left, region.right);
    const float regionBottom = std::max(region.top, region.bottom);
    const float regionWidth  = regionRight - regionLeft;
    const float regionHeight = regionBottom - regionTop;
    // 退化区域或非法目标中心不能用于归一化，保持并规整原布局。
    if ( regionWidth <= 0.0f || regionHeight <= 0.0f ||
         !std::isfinite(centerX) || !std::isfinite(centerY) ) {
        return sanitizeCanvasComponentPlacement(placement);
    }

    // 把像素中心转换为相对区域的归一化锚点。
    placement.anchorX = (centerX - regionLeft) / regionWidth;
    placement.anchorY = (centerY - regionTop) / regionHeight;
    placement         = sanitizeCanvasComponentPlacement(placement);
    // 重新计算完整边界，让过大的目标位移受到内容半尺寸约束。
    const auto bounds = canvasComponentBoundsInRegion(
        placement, region, contentWidth, contentHeight);
    placement.anchorX =
        ((bounds.left + bounds.right) * 0.5f - regionLeft) / regionWidth;
    placement.anchorY =
        ((bounds.top + bounds.bottom) * 0.5f - regionTop) / regionHeight;
    // 最后再次规整浮点误差，确保返回值可以直接写回配置。
    return sanitizeCanvasComponentPlacement(placement);
}

/// @brief 将画布组件移动到指定像素中心并保持完整可见。
/// @param placement 原布局。
/// @param centerX 目标像素中心横坐标。
/// @param centerY 目标像素中心纵坐标。
/// @param viewportWidth 画布宽度。
/// @param viewportHeight 画布高度。
/// @param contentWidth 组件内容宽度。
/// @param contentHeight 组件内容高度。
/// @return 更新后的归一化布局。
/// @warning 热路径：组件拖动期间每帧调用；只允许常量级数值计算。
[[nodiscard]] inline Config::CanvasComponentPlacement moveCanvasComponent(
    Config::CanvasComponentPlacement placement, float centerX, float centerY,
    float viewportWidth, float viewportHeight, float contentWidth,
    float contentHeight)
{
    // 全画布版本同样委托给区域实现，零点到视口尺寸构成有效区域。
    return moveCanvasComponentInRegion(
        placement,
        centerX,
        centerY,
        { 0.0f, 0.0f, viewportWidth, viewportHeight },
        contentWidth,
        contentHeight);
}

/// @brief 按相同像素位移移动组件并保持其内容尺寸。
/// @param placement 移动开始时的布局。
/// @param startBounds 移动开始时的组件边界。
/// @param offsetX 横向同步位移，单位像素。
/// @param offsetY 纵向同步位移，单位像素。
/// @param region 组件允许占用的像素区域。
/// @return 应用位移并限制在布局区域内的布局。
/// @warning 热路径：同步组件移动期间每帧调用；只允许常量级数值计算。
[[nodiscard]] inline Config::CanvasComponentPlacement
moveCanvasComponentByOffsetInRegion(Config::CanvasComponentPlacement placement,
                                    const CanvasComponentBounds& startBounds,
                                    float offsetX, float offsetY,
                                    const CanvasComponentBounds& region)
{
    // 非有限位移不能形成稳定目标中心，直接保留规整后的起始布局。
    if ( !std::isfinite(offsetX) || !std::isfinite(offsetY) ) {
        return sanitizeCanvasComponentPlacement(placement);
    }
    // 位移基于拖动开始时的中心，避免连续帧把增量重复累计。
    const float startCenterX = (startBounds.left + startBounds.right) * 0.5f;
    const float startCenterY = (startBounds.top + startBounds.bottom) * 0.5f;
    // 内容尺寸沿用起始边界，使移动只改变位置而不改变组件大小。
    return moveCanvasComponentInRegion(placement,
                                       startCenterX + offsetX,
                                       startCenterY + offsetY,
                                       region,
                                       startBounds.width(),
                                       startBounds.height());
}

/// @brief 命中组件移动区域或四角缩放把手。
/// @param bounds 组件像素边界。
/// @param pointerX 指针横坐标。
/// @param pointerY 指针纵坐标。
/// @param cornerRadius 四角命中半径。
/// @return 命中的拖动部位。
/// @warning 指针交互热路径：每帧只执行固定次数的距离与边界比较。
[[nodiscard]] inline CanvasComponentDragHandle hitTestCanvasComponent(
    const CanvasComponentBounds& bounds, float pointerX, float pointerY,
    float cornerRadius)
{
    // 四角使用轴对齐方形命中区，便于触控和高 DPI 下保持稳定手感。
    const auto nearCorner = [&](float x, float y) {
        return std::abs(pointerX - x) <= cornerRadius &&
               std::abs(pointerY - y) <= cornerRadius;
    };
    // 角点优先于内部移动区域，边缘重叠时保证用户仍可缩放。
    if ( nearCorner(bounds.left, bounds.top) ) {
        return CanvasComponentDragHandle::TopLeft;
    }
    if ( nearCorner(bounds.right, bounds.top) ) {
        return CanvasComponentDragHandle::TopRight;
    }
    if ( nearCorner(bounds.left, bounds.bottom) ) {
        return CanvasComponentDragHandle::BottomLeft;
    }
    if ( nearCorner(bounds.right, bounds.bottom) ) {
        return CanvasComponentDragHandle::BottomRight;
    }
    // 未命中角点时，边界内部解释为移动，外部返回 None。
    return bounds.contains(pointerX, pointerY)
               ? CanvasComponentDragHandle::Move
               : CanvasComponentDragHandle::None;
}

/// @brief 从拖动部位取得固定不动的对角点。
/// @param bounds 拖动开始时的组件边界。
/// @param handle 当前拖动部位。
/// @return 缩放时保持固定的对角点。
[[nodiscard]] inline CanvasComponentPoint canvasComponentOppositeCorner(
    const CanvasComponentBounds& bounds, CanvasComponentDragHandle handle)
{
    // 每个缩放把手固定其对角点，使字号和中心可以同时更新。
    switch ( handle ) {
    case CanvasComponentDragHandle::TopLeft:
        return { bounds.right, bounds.bottom };
    case CanvasComponentDragHandle::TopRight:
        return { bounds.left, bounds.bottom };
    case CanvasComponentDragHandle::BottomLeft:
        return { bounds.right, bounds.top };
    case CanvasComponentDragHandle::BottomRight:
        return { bounds.left, bounds.top };
    case CanvasComponentDragHandle::None:
    case CanvasComponentDragHandle::Move: break;
    }
    // 非缩放把手没有对角点语义，回退到组件中心。
    return { (bounds.left + bounds.right) * 0.5f,
             (bounds.top + bounds.bottom) * 0.5f };
}

/// @brief 按四角拖动结果在指定区域内等比调整组件字号和中心位置。
/// @param placement 缩放开始时的布局。
/// @param handle 当前缩放把手。
/// @param startBounds 缩放开始时的组件边界。
/// @param targetFontSizeRatio 目标字号相对画布高度的比例。
/// @param region 组件允许占用的像素区域。
/// @return 以当前把手对角点为固定点更新后的布局。
/// @warning 热路径：同步组件缩放期间每帧调用；只允许常量级数值计算。
[[nodiscard]] inline Config::CanvasComponentPlacement
resizeCanvasComponentToFontSizeInRegion(
    Config::CanvasComponentPlacement placement,
    CanvasComponentDragHandle handle, const CanvasComponentBounds& startBounds,
    float targetFontSizeRatio, const CanvasComponentBounds& region)
{
    // 先规整旧配置，保证字号分母为合法正值且锚点可计算。
    placement                = sanitizeCanvasComponentPlacement(placement);
    const float regionWidth  = region.width();
    const float regionHeight = region.height();
    // 非缩放把手、退化尺寸、无效区域或非法目标字号均保持原布局。
    if ( handle == CanvasComponentDragHandle::None ||
         handle == CanvasComponentDragHandle::Move ||
         startBounds.width() <= 0.0f || startBounds.height() <= 0.0f ||
         regionWidth <= 0.0f || regionHeight <= 0.0f ||
         !std::isfinite(targetFontSizeRatio) ) {
        return placement;
    }

    // 目标字号先限制到持久化范围，再计算实际应用到像素边界的比例。
    const float startRatio   = placement.fontSizeRatio;
    placement.fontSizeRatio  = std::clamp(targetFontSizeRatio,
                                          CANVAS_COMPONENT_MIN_FONT_SIZE_RATIO,
                                          CANVAS_COMPONENT_MAX_FONT_SIZE_RATIO);
    const float appliedScale = placement.fontSizeRatio / startRatio;
    const float width        = startBounds.width() * appliedScale;
    const float height       = startBounds.height() * appliedScale;

    // 固定对角点后，根据把手所在方向决定新中心向左上或右下移动。
    const CanvasComponentPoint opposite =
        canvasComponentOppositeCorner(startBounds, handle);
    const bool  growsLeft = handle == CanvasComponentDragHandle::TopLeft ||
                            handle == CanvasComponentDragHandle::BottomLeft;
    const bool  growsUp   = handle == CanvasComponentDragHandle::TopLeft ||
                            handle == CanvasComponentDragHandle::TopRight;
    const float centerX =
        opposite.x + (growsLeft ? -width * 0.5f : width * 0.5f);
    const float centerY =
        opposite.y + (growsUp ? -height * 0.5f : height * 0.5f);
    // 区域移动帮助器负责最终边界钳制，并把中心还原为归一化锚点。
    return moveCanvasComponentInRegion(
        placement, centerX, centerY, region, width, height);
}

/// @brief 按四角拖动结果在指定区域内等比调整组件字号和中心位置。
/// @param placement 拖动开始时的布局。
/// @param handle 当前缩放把手。
/// @param startBounds 拖动开始时的组件边界。
/// @param pointerX 当前指针横坐标。
/// @param pointerY 当前指针纵坐标。
/// @param region 组件允许占用的像素区域。
/// @return 更新后的布局。
/// @warning 热路径：组件缩放期间每帧调用；只允许常量级数值计算。
[[nodiscard]] inline Config::CanvasComponentPlacement
resizeCanvasComponentInRegion(Config::CanvasComponentPlacement placement,
                              CanvasComponentDragHandle        handle,
                              const CanvasComponentBounds&     startBounds,
                              float pointerX, float pointerY,
                              const CanvasComponentBounds& region)
{
    // 起始配置和区域尺寸在进行向量投影前先规范化。
    placement                = sanitizeCanvasComponentPlacement(placement);
    const float regionWidth  = region.width();
    const float regionHeight = region.height();
    // 无效把手或退化几何没有稳定缩放轴，直接返回规整布局。
    if ( handle == CanvasComponentDragHandle::None ||
         handle == CanvasComponentDragHandle::Move ||
         startBounds.width() <= 0.0f || startBounds.height() <= 0.0f ||
         regionWidth <= 0.0f || regionHeight <= 0.0f ) {
        return placement;
    }

    // 对角点固定不动，拖动角点决定相对起始向量的缩放投影。
    const CanvasComponentPoint opposite =
        canvasComponentOppositeCorner(startBounds, handle);
    CanvasComponentPoint startCorner;
    // 将枚举把手还原为拖动开始时对应的实际角点坐标。
    switch ( handle ) {
    case CanvasComponentDragHandle::TopLeft:
        startCorner = { startBounds.left, startBounds.top };
        break;
    case CanvasComponentDragHandle::TopRight:
        startCorner = { startBounds.right, startBounds.top };
        break;
    case CanvasComponentDragHandle::BottomLeft:
        startCorner = { startBounds.left, startBounds.bottom };
        break;
    case CanvasComponentDragHandle::BottomRight:
        startCorner = { startBounds.right, startBounds.bottom };
        break;
    case CanvasComponentDragHandle::None:
    case CanvasComponentDragHandle::Move: return placement;
    }

    // 起始向量描述原尺寸，指针向量描述当前指针相对固定点的位置。
    const CanvasComponentPoint startVector{ startCorner.x - opposite.x,
                                            startCorner.y - opposite.y };
    const CanvasComponentPoint pointerVector{ pointerX - opposite.x,
                                              pointerY - opposite.y };
    const float                denominator =
        startVector.x * startVector.x + startVector.y * startVector.y;
    // 极小起始向量会放大数值误差，视为不可缩放并保持原布局。
    if ( denominator <= 1e-6f ) return placement;

    // 点积投影得到沿原对角线方向的等比缩放，忽略垂直抖动分量。
    const float scale = std::max(
        0.01f,
        (pointerVector.x * startVector.x + pointerVector.y * startVector.y) /
            denominator);
    // 字号帮助器负责合法范围钳制、固定对角点和区域内重新定位。
    return resizeCanvasComponentToFontSizeInRegion(
        placement,
        handle,
        startBounds,
        placement.fontSizeRatio * scale,
        region);
}

/// @brief 按四角拖动结果等比调整组件字号和中心位置。
/// @param placement 拖动开始时的布局。
/// @param handle 当前缩放把手。
/// @param startBounds 拖动开始时的组件边界。
/// @param pointerX 当前指针横坐标。
/// @param pointerY 当前指针纵坐标。
/// @param viewportWidth 画布宽度。
/// @param viewportHeight 画布高度。
/// @return 更新后的布局。
/// @warning 热路径：组件缩放期间每帧调用；只允许常量级数值计算。
[[nodiscard]] inline Config::CanvasComponentPlacement resizeCanvasComponent(
    Config::CanvasComponentPlacement placement,
    CanvasComponentDragHandle handle, const CanvasComponentBounds& startBounds,
    float pointerX, float pointerY, float viewportWidth, float viewportHeight)
{
    // 全画布版本委托给区域实现，确保缩放算法与子区域布局保持一致。
    return resizeCanvasComponentInRegion(
        placement,
        handle,
        startBounds,
        pointerX,
        pointerY,
        { 0.0f, 0.0f, viewportWidth, viewportHeight });
}

}  // namespace MMM::Logic
