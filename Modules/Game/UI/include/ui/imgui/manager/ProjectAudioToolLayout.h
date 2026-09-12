#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace MMM::UI::ProjectAudioToolLayout
{

/// @brief 项目音频工具画布中的逻辑矩形。
///
/// 坐标与尺寸使用未乘 DPI 和相机倍率的画布单位。所有几何 helper
/// 都假定宽高非负， 但面积计算会防御性钳制异常负值。
struct Rect {
    /// @brief 左边界逻辑坐标。
    float x{ 0.0F };

    /// @brief 上边界逻辑坐标。
    float y{ 0.0F };

    /// @brief 逻辑宽度。
    float width{ 0.0F };

    /// @brief 逻辑高度。
    float height{ 0.0F };

    /// @brief 获取矩形右边界。
    /// @return `x + width`，与左边界使用同一逻辑坐标系。
    [[nodiscard]] float right() const { return x + width; }

    /// @brief 获取矩形下边界。
    /// @return `y + height`，与上边界使用同一逻辑坐标系。
    [[nodiscard]] float bottom() const { return y + height; }
};

/// @brief 项目音频工具画布允许的最小相机倍率。
///
/// 下限保证缩小时控制方块仍具有可交互尺寸。
inline constexpr float MINIMUM_CAMERA_ZOOM = 0.5F;

/// @brief 项目音频工具画布允许的最大相机倍率。
///
/// 上限避免滚动范围和像素尺寸在高 DPI 下过度膨胀。
inline constexpr float MAXIMUM_CAMERA_ZOOM = 4.0F;

/// @brief 单格滚轮对应的相机倍率。
///
/// 使用乘法步进使放大和缩小互为倒数，并保持连续滚轮增量可用。
inline constexpr float CAMERA_ZOOM_STEP = 1.2F;

/// @brief 围绕鼠标缩放项目音频画布后的相机状态。
///
/// 返回值已完成有限值检查和范围限制，可直接写回视图相机成员。
struct CameraZoomResult {
    /// @brief 应用范围限制后的画布倍率。
    float zoom{ 1.0F };

    /// @brief 保持鼠标锚点所需的水平滚动像素。
    float scrollX{ 0.0F };

    /// @brief 保持鼠标锚点所需的垂直滚动像素。
    float scrollY{ 0.0F };
};

/// @brief 将画布相机缩放到指定倍率并保持鼠标锚点。
/// @param currentZoom 当前画布倍率。
/// @param targetZoom 目标画布倍率。
/// @param dpiScale 当前窗口内容缩放。
/// @param scrollX 当前水平滚动像素。
/// @param scrollY 当前垂直滚动像素。
/// @param pointerX 鼠标相对可见画布左边界的像素坐标。
/// @param pointerY 鼠标相对可见画布上边界的像素坐标。
/// @return 新倍率与保持鼠标下逻辑坐标所需的滚动位置。
///
/// 算法先用旧缩放把指针所在内容像素还原为逻辑锚点，再用新缩放投影回内容空间，
/// 最后反推出滚动位置。DPI 同时出现在旧新比例中，但必须保留以匹配画布像素换算。
/// @warning UI 热路径：仅在 Ctrl+滚轮或缩放滑条变化时执行常量数学运算，
/// 不得引入分配。
[[nodiscard]] inline CameraZoomResult zoomCameraToPointer(
    float currentZoom, float targetZoom, float dpiScale, float scrollX,
    float scrollY, float pointerX, float pointerY)
{
    // 非有限倍率回退 1 倍，有限值则钳制到公开相机范围。
    const float safeZoom =
        std::isfinite(currentZoom)
            ? std::clamp(currentZoom, MINIMUM_CAMERA_ZOOM, MAXIMUM_CAMERA_ZOOM)
            : 1.0F;
    // DPI 至少保留一个正数，防止逻辑锚点换算除零。
    const float safeDpiScale =
        std::isfinite(dpiScale) ? std::max(0.01F, dpiScale) : 1.0F;
    // 滚动值不允许为负，NaN 与无穷回退可见起点。
    const float safeScrollX =
        std::isfinite(scrollX) ? std::max(0.0F, scrollX) : 0.0F;
    const float safeScrollY =
        std::isfinite(scrollY) ? std::max(0.0F, scrollY) : 0.0F;
    // 无效目标倍率保持当前安全倍率，不制造相机跳变。
    const float nextZoom =
        std::isfinite(targetZoom)
            ? std::clamp(targetZoom, MINIMUM_CAMERA_ZOOM, MAXIMUM_CAMERA_ZOOM)
            : safeZoom;
    if ( std::abs(nextZoom - safeZoom) <= 1e-6F ) {
        // 实际倍率未变时直接返回清洗后的相机状态。
        return { safeZoom, safeScrollX, safeScrollY };
    }

    // 指针异常时以可见区左上角作为缩放锚点。
    const float safePointerX = std::isfinite(pointerX) ? pointerX : 0.0F;
    const float safePointerY = std::isfinite(pointerY) ? pointerY : 0.0F;
    const float oldScale     = safeDpiScale * safeZoom;
    const float nextScale    = safeDpiScale * nextZoom;
    // 旧内容像素除以旧比例得到指针下的逻辑坐标。
    const float anchorX = (safeScrollX + safePointerX) / oldScale;
    const float anchorY = (safeScrollY + safePointerY) / oldScale;
    // 新滚动值使相同逻辑锚点重新落到原指针像素，并钳制在零以上。
    return {
        nextZoom,
        std::max(0.0F, anchorX * nextScale - safePointerX),
        std::max(0.0F, anchorY * nextScale - safePointerY),
    };
}

/// @brief 按滚轮增量围绕鼠标位置缩放画布相机。
/// @param currentZoom 当前画布倍率。
/// @param wheelDelta 本帧垂直滚轮增量，正值放大、负值缩小。
/// @param dpiScale 当前窗口内容缩放。
/// @param scrollX 当前水平滚动像素。
/// @param scrollY 当前垂直滚动像素。
/// @param pointerX 鼠标相对可见画布左边界的像素坐标。
/// @param pointerY 鼠标相对可见画布上边界的像素坐标。
/// @return 新倍率与保持鼠标下逻辑坐标所需的滚动位置。
///
/// 滚轮增量作为 `CAMERA_ZOOM_STEP` 的指数，因此触控板小数增量也能平滑缩放。
/// 最终倍率与锚点修正统一委托给 `zoomCameraToPointer`。
/// @warning UI 热路径：仅在 Ctrl+滚轮触发时执行常量数学运算，不得引入分配。
[[nodiscard]] inline CameraZoomResult zoomCameraAtPointer(
    float currentZoom, float wheelDelta, float dpiScale, float scrollX,
    float scrollY, float pointerX, float pointerY)
{
    // 先清洗当前倍率，确保指数结果以合法基值计算。
    const float safeZoom =
        std::isfinite(currentZoom)
            ? std::clamp(currentZoom, MINIMUM_CAMERA_ZOOM, MAXIMUM_CAMERA_ZOOM)
            : 1.0F;
    if ( !std::isfinite(wheelDelta) || std::abs(wheelDelta) <= 1e-6F ) {
        // 无有效滚轮变化时仍返回规范化后的滚动与倍率。
        return zoomCameraToPointer(
            safeZoom, safeZoom, dpiScale, scrollX, scrollY, pointerX, pointerY);
    }
    // 正增量乘大倍率，负增量通过负指数按相同比例缩小。
    return zoomCameraToPointer(
        safeZoom,
        safeZoom * std::pow(CAMERA_ZOOM_STEP, wheelDelta),
        dpiScale,
        scrollX,
        scrollY,
        pointerX,
        pointerY);
}

/// @brief 一个下层方块及其已有上层遮挡，用于校验新增方块后的可见面积。
///
/// 该结构在拖动开始时预计算，拖动热路径只读取面积和互不重叠的可见单元。
struct VisibilityConstraint {
    /// @brief 必须保留可见面积的下层方块。
    Rect base;

    /// @brief 已经位于基础方块上方且与其相交的不可移动遮挡。
    std::vector<Rect> fixedOccluders;

    /// @brief 扣除固定遮挡后互不重叠的可见矩形。
    ///
    /// 单元之间无重叠，使候选遮挡面积可以直接求和而不重复计算。
    std::vector<Rect> fixedVisibleCells;

    /// @brief 固定遮挡在基础方块内的并集面积。
    float fixedCoveredArea{ 0.0F };

    /// @brief 扣除固定遮挡后剩余的可见面积。
    float fixedVisibleArea{ 0.0F };
};

/// @brief 单轴吸附锁；拖过释放阈值前保持落在同一目标位置。
///
/// `position` 是被吸附对象的最终轴坐标，`targetLine`
/// 是用于绘制辅助线的外部锚点。
struct AxisSnapLock {
    /// @brief 当前锁定的方块左坐标或上坐标。
    std::optional<float> position;

    /// @brief 当前命中的目标参考线逻辑坐标。
    std::optional<float> targetLine;
};

/// @brief 二维吸附锁。
///
/// 两轴独立释放，允许对象在保持一个轴对齐时沿另一个轴继续移动。
struct SnapLocks {
    /// @brief 水平吸附锁。
    AxisSnapLock x;

    /// @brief 垂直吸附锁。
    AxisSnapLock y;
};

/// @brief 单轴缩放方向。
enum class ResizeEdge {
    /// @brief 当前轴不参与缩放。
    None,
    /// @brief 拖动左边或上边，最大边保持不变。
    Minimum,
    /// @brief 拖动右边或下边，最小边保持不变。
    Maximum,
};

/// @brief 根据文字宽度、按钮内边距和类型下限计算默认方块宽度。
/// @param textWidth 当前字体测得的标签宽度。
/// @param horizontalPadding 方块单侧水平内边距。
/// @param minimumWidth 该类型允许的最小宽度。
/// @return 向上取整后的安全逻辑宽度。
///
/// 额外两个像素吸收字体测量和裁剪边界误差，避免刚好相等时省略号闪动。
[[nodiscard]] inline float calculateDefaultWidth(float textWidth,
                                                 float horizontalPadding,
                                                 float minimumWidth)
{
    // 类型下限和内容需求取较大者，再对齐到完整逻辑像素。
    return std::ceil(
        std::max(minimumWidth, textWidth + horizontalPadding * 2.0F + 2.0F));
}

/// @brief 根据固定按钮行计算不会压缩控件的最小方块宽度。
/// @param buttonSize 单个方形按钮边长。
/// @param buttonSpacing 相邻按钮的水平间距。
/// @param horizontalPadding 方块单侧内边距。
/// @param buttonCount 按钮数量。
/// @return 完整容纳按钮行的逻辑宽度。
[[nodiscard]] inline float calculateControlMinimumWidth(float buttonSize,
                                                        float buttonSpacing,
                                                        float horizontalPadding,
                                                        std::size_t buttonCount)
{
    // 无按钮时只有左右内边距，不执行 `buttonCount - 1` 的无符号下溢。
    if ( buttonCount == 0 ) return horizontalPadding * 2.0F;
    // 总宽度由两侧 padding、全部按钮和按钮间隙组成。
    return horizontalPadding * 2.0F +
           buttonSize * static_cast<float>(buttonCount) +
           buttonSpacing * static_cast<float>(buttonCount - 1U);
}

/// @brief 根据类型、文件名、进度条和按钮行计算最小方块高度。
/// @param textLineHeight 单行标签高度。
/// @param progressHeight 进度条高度。
/// @param progressSpacing 进度条与相邻内容间距。
/// @param buttonSize 底部按钮行高度。
/// @param verticalPadding 方块单侧垂直内边距。
/// @param itemSpacing 文本及控件组间距。
/// @return 不裁剪所有固定内容所需的逻辑高度。
[[nodiscard]] inline float calculateControlMinimumHeight(
    float textLineHeight, float progressHeight, float progressSpacing,
    float buttonSize, float verticalPadding, float itemSpacing)
{
    // 两行文本、一个进度条和按钮行按固定视觉顺序累加。
    return verticalPadding * 2.0F + textLineHeight * 2.0F + itemSpacing * 2.0F +
           progressHeight + progressSpacing + buttonSize;
}

/// @brief 计算矩形面积。
/// @param rect 输入逻辑矩形。
/// @return 宽高负值按零处理后的非负面积。
[[nodiscard]] inline float area(const Rect& rect)
{
    // 分轴钳制让反向拖动产生的临时负尺寸不会形成正面积。
    return std::max(0.0F, rect.width) * std::max(0.0F, rect.height);
}

/// @brief 计算两个矩形的交集。
/// @param lhs 第一个逻辑矩形。
/// @param rhs 第二个逻辑矩形。
/// @return 正面积交集；仅接触边或不相交时返回空。
[[nodiscard]] inline std::optional<Rect> intersection(const Rect& lhs,
                                                      const Rect& rhs)
{
    // 交集边界分别取两矩形内侧极值。
    const float left   = std::max(lhs.x, rhs.x);
    const float top    = std::max(lhs.y, rhs.y);
    const float right  = std::min(lhs.right(), rhs.right());
    const float bottom = std::min(lhs.bottom(), rhs.bottom());
    // 零宽或零高不构成遮挡面积。
    if ( right <= left || bottom <= top ) return std::nullopt;
    // 返回矩形重新表示为左上角和尺寸。
    return Rect{ left, top, right - left, bottom - top };
}

/// @brief 计算多个遮挡矩形在基础矩形内的并集面积。
/// @param base 被遮挡的基础矩形。
/// @param occluders 可能相互重叠的上层矩形。
/// @return 遮挡并集在 base 内的面积，范围为零到 base 面积。
///
/// 算法把所有交集左右边界作为 X 切片，在每个切片中合并 Y 区间后累加面积。这样
/// 同一区域被多个方块覆盖时只计算一次，不依赖像素栅格精度。
/// @warning 低频布局路径：仅在拖动或叠层变化时调用；会排序局部交点，
/// 禁止在未发生布局变化的普通绘制帧中调用。
[[nodiscard]] inline float coveredArea(const Rect&           base,
                                       std::span<const Rect> occluders)
{
    // 无面积基础矩形或空遮挡集合直接得到零。
    if ( area(base) <= 0.0F || occluders.empty() ) return 0.0F;

    // clipped 保存有效交集，X 坐标包含 base 两端以形成完整切片。
    std::vector<Rect>  clipped;
    std::vector<float> xCoordinates{ base.x, base.right() };
    clipped.reserve(occluders.size());
    xCoordinates.reserve(occluders.size() * 2 + 2);
    for ( const auto& occluder : occluders ) {
        // 完全位于 base 外的矩形不参与后续排序。
        const auto clippedRect = intersection(base, occluder);
        if ( !clippedRect ) continue;
        clipped.push_back(*clippedRect);
        xCoordinates.push_back(clippedRect->x);
        xCoordinates.push_back(clippedRect->right());
    }
    // 没有任何相交遮挡时无需建立切片。
    if ( clipped.empty() ) return 0.0F;

    // 去重后的相邻 X 坐标定义互不重叠的竖向条带。
    std::ranges::sort(xCoordinates);
    xCoordinates.erase(std::unique(xCoordinates.begin(), xCoordinates.end()),
                       xCoordinates.end());

    float covered = 0.0F;
    // 每个条带复用 Y 区间容器，避免循环内重复分配容量。
    std::vector<std::pair<float, float>> yIntervals;
    yIntervals.reserve(clipped.size());
    for ( std::size_t xIndex = 1; xIndex < xCoordinates.size(); ++xIndex ) {
        const float left  = xCoordinates[xIndex - 1];
        const float right = xCoordinates[xIndex];
        // 重复或反向边界没有条带面积。
        if ( right <= left ) continue;
        // 中点测试在一个无额外 X 边界的条带内代表整个条带覆盖关系。
        const float midpoint = (left + right) * 0.5F;

        yIntervals.clear();
        for ( const auto& clippedRect : clipped ) {
            // 严格内部测试排除仅接触条带边缘的矩形。
            if ( midpoint > clippedRect.x && midpoint < clippedRect.right() ) {
                yIntervals.emplace_back(clippedRect.y, clippedRect.bottom());
            }
        }
        if ( yIntervals.empty() ) continue;

        // 按起点排序后线性合并重叠或接触的 Y 区间。
        std::ranges::sort(yIntervals);
        float intervalStart = yIntervals.front().first;
        float intervalEnd   = yIntervals.front().second;
        float coveredY      = 0.0F;
        for ( std::size_t i = 1; i < yIntervals.size(); ++i ) {
            if ( yIntervals[i].first <= intervalEnd ) {
                // 重叠区间扩展当前并集末端，不重复累计。
                intervalEnd = std::max(intervalEnd, yIntervals[i].second);
            } else {
                // 出现间隙时提交上一段并开始新段。
                coveredY += intervalEnd - intervalStart;
                intervalStart = yIntervals[i].first;
                intervalEnd   = yIntervals[i].second;
            }
        }
        // 提交最后一个 Y 并集段并乘以条带宽度。
        coveredY += intervalEnd - intervalStart;
        covered += (right - left) * coveredY;
    }
    // 浮点累计误差可能略超出基础面积，最终约束到合法范围。
    return std::clamp(covered, 0.0F, area(base));
}

/// @brief 计算基础矩形扣除遮挡并集后的可见比例。
/// @param base 需要保留可见区域的基础矩形。
/// @param occluders 上层遮挡集合。
/// @return 零到一之间的可见面积比例。
[[nodiscard]] inline float visibleRatio(const Rect&           base,
                                        std::span<const Rect> occluders)
{
    // 无正面积矩形没有可定义的可见比例，按零处理。
    const float baseArea = area(base);
    if ( baseArea <= 0.0F ) return 0.0F;
    // 遮挡使用并集面积，避免重叠遮挡重复扣减。
    return std::clamp(
        1.0F - coveredArea(base, occluders) / baseArea, 0.0F, 1.0F);
}

/// @brief 将一个矩形扣除遮挡交集并追加为最多四个互不重叠矩形。
/// @param source 需要裁切的矩形。
/// @param occluder 遮挡矩形。
/// @param output 接收剩余可见矩形。
///
/// 输出按上、下、左、右四个不重叠区域拆分，其中左右区域只覆盖交集的垂直跨度，
/// 从而不会与上下区域重叠。调用方负责清空或保留 output 的既有内容。
inline void appendSubtractedRect(const Rect& source, const Rect& occluder,
                                 std::vector<Rect>& output)
{
    // 无交集时 source 整体仍然可见。
    const auto clipped = intersection(source, occluder);
    if ( !clipped ) {
        output.push_back(source);
        return;
    }

    // 缓存 source 最大边，避免每个候选重复计算。
    const float sourceRight  = source.right();
    const float sourceBottom = source.bottom();
    if ( clipped->y > source.y ) {
        // 上方条带横跨 source 全宽。
        output.push_back(
            Rect{ source.x, source.y, source.width, clipped->y - source.y });
    }
    if ( clipped->bottom() < sourceBottom ) {
        // 下方条带横跨 source 全宽。
        output.push_back(Rect{ source.x,
                               clipped->bottom(),
                               source.width,
                               sourceBottom - clipped->bottom() });
    }
    if ( clipped->x > source.x ) {
        // 左方条带只占交集高度，避免覆盖已输出的上下条带。
        output.push_back(Rect{
            source.x, clipped->y, clipped->x - source.x, clipped->height });
    }
    if ( clipped->right() < sourceRight ) {
        // 右方条带采用相同的交集高度限制。
        output.push_back(Rect{ clipped->right(),
                               clipped->y,
                               sourceRight - clipped->right(),
                               clipped->height });
    }
}

/// @brief 将可能重叠的矩形预处理成互不重叠的并集单元。
/// @param rects 输入矩形集合。
/// @return 表示相同覆盖并集且彼此不重叠的矩形单元。
///
/// 每个新矩形依次扣除已接受单元，只把剩余部分追加到并集，因此输出可在拖动热路径
/// 中直接求交集面积，无需再次去重。
/// @warning 批量拖动开始时的低频路径：允许分配和矩形裁切，禁止每帧调用。
[[nodiscard]] inline std::vector<Rect> buildUnionCells(
    std::span<const Rect> rects)
{
    // 三个容器分别保存最终并集、本轮剩余块和下一次裁切结果。
    std::vector<Rect> unionCells;
    std::vector<Rect> remainingCells;
    std::vector<Rect> nextRemainingCells;
    for ( const auto& rect : rects ) {
        // 零面积输入不会改变覆盖并集。
        if ( area(rect) <= 0.0F ) continue;
        remainingCells.clear();
        remainingCells.push_back(rect);
        for ( const auto& existing : unionCells ) {
            // 当前剩余块逐一扣除一个已接受单元。
            nextRemainingCells.clear();
            nextRemainingCells.reserve(remainingCells.size() * 2 + 2);
            for ( const auto& remaining : remainingCells ) {
                appendSubtractedRect(remaining, existing, nextRemainingCells);
            }
            remainingCells.swap(nextRemainingCells);
            // 新矩形已被既有并集完全覆盖时可提前结束。
            if ( remainingCells.empty() ) break;
        }
        // 剩余块与既有单元均不重叠，可以安全追加。
        unionCells.insert(
            unionCells.end(), remainingCells.begin(), remainingCells.end());
    }
    return unionCells;
}

/// @brief 预处理一个下层方块的固定遮挡，过滤不相交方块并缓存并集面积。
/// @param base 需要维持可见比例的下层方块。
/// @param fixedOccluders 当前已经位于其上方的不可移动方块。
/// @return 可供拖动热路径重复查询的可见性约束缓存。
/// @warning 拖动开始时的低频路径：允许分配和并集计算，禁止每帧重建。
[[nodiscard]] inline VisibilityConstraint prepareVisibilityConstraint(
    const Rect& base, std::span<const Rect> fixedOccluders)
{
    // 先保存基础矩形，并按输入上限预留相交遮挡容量。
    VisibilityConstraint constraint;
    constraint.base = base;
    constraint.fixedOccluders.reserve(fixedOccluders.size());
    for ( const auto& occluder : fixedOccluders ) {
        // 完全不相交的方块不会影响该约束，提前过滤。
        if ( intersection(base, occluder) ) {
            constraint.fixedOccluders.push_back(occluder);
        }
    }

    if ( area(base) > 0.0F ) {
        // 裁切从完整基础矩形开始。
        constraint.fixedVisibleCells.push_back(base);
    }
    std::vector<Rect> remainingCells;
    for ( const auto& occluder : constraint.fixedOccluders ) {
        // 每轮从当前互不重叠可见单元中扣除一个固定遮挡。
        remainingCells.clear();
        remainingCells.reserve(constraint.fixedVisibleCells.size() * 2 + 2);
        for ( const auto& visibleCell : constraint.fixedVisibleCells ) {
            appendSubtractedRect(visibleCell, occluder, remainingCells);
        }
        constraint.fixedVisibleCells.swap(remainingCells);
        // 已完全遮挡后不再需要处理其余遮挡。
        if ( constraint.fixedVisibleCells.empty() ) break;
    }
    // 单元互不重叠，因此可直接求和得到固定可见面积。
    for ( const auto& visibleCell : constraint.fixedVisibleCells ) {
        constraint.fixedVisibleArea += area(visibleCell);
    }
    // 覆盖面积从基础面积减可见面积得到，并防御浮点负误差。
    constraint.fixedCoveredArea =
        std::max(0.0F, area(base) - constraint.fixedVisibleArea);
    return constraint;
}

/// @brief 计算固定遮挡上再加入一个候选前景方块后的可见比例。
/// @param constraint 已预处理的基础方块与固定遮挡。
/// @param candidate 本帧待评估的单个前景方块。
/// @return 加入候选后基础方块剩余可见面积比例。
///
/// 候选只与固定可见单元求交，因此不会重复扣除已经被固定遮挡覆盖的面积。
/// @warning 拖动热路径：只读取预切分的可见矩形，禁止分配、排序或复制遮挡。
[[nodiscard]] inline float visibleRatioWithCandidate(
    const VisibilityConstraint& constraint, const Rect& candidate)
{
    // 无正面积基础方块按完全不可见处理。
    const float baseArea = area(constraint.base);
    if ( baseArea <= 0.0F ) return 0.0F;

    // 候选不触碰基础方块时直接返回固定遮挡后的比例。
    const auto candidateIntersection = intersection(constraint.base, candidate);
    if ( !candidateIntersection ) {
        return std::clamp(
            1.0F - constraint.fixedCoveredArea / baseArea, 0.0F, 1.0F);
    }
    if ( constraint.fixedVisibleCells.empty() ) {
        // 存在固定遮挡且无剩余单元表示基础方块已经完全不可见。
        if ( !constraint.fixedOccluders.empty() ) return 0.0F;
        // 无固定遮挡缓存时直接从完整基础面积扣除候选交集。
        return std::clamp(
            1.0F - area(*candidateIntersection) / baseArea, 0.0F, 1.0F);
    }

    // 可见单元互不重叠，候选新增覆盖面积可以线性累加。
    float newlyCoveredArea = 0.0F;
    for ( const auto& visibleCell : constraint.fixedVisibleCells ) {
        const auto newlyCovered =
            intersection(visibleCell, *candidateIntersection);
        if ( newlyCovered ) {
            newlyCoveredArea += area(*newlyCovered);
        }
    }
    // 从固定可见面积扣除新增覆盖，再相对原始基础面积归一化。
    return std::clamp(
        (constraint.fixedVisibleArea - newlyCoveredArea) / baseArea,
        0.0F,
        1.0F);
}

/// @brief 获取固定遮挡本身留下的可见比例。
/// @param constraint 已预处理的可见性约束。
/// @return 不加入移动候选时的基础方块可见比例。
/// @warning 拖动热路径：只读取预计算面积，禁止引入几何重建。
[[nodiscard]] inline float fixedVisibleRatio(
    const VisibilityConstraint& constraint)
{
    // 无正面积基础方块没有可见内容。
    const float baseArea = area(constraint.base);
    if ( baseArea <= 0.0F ) return 0.0F;
    if ( constraint.fixedVisibleCells.empty() &&
         constraint.fixedOccluders.empty() ) {
        // 空缓存既可能来自未裁切，也可能来自默认构造；无遮挡时视为完全可见。
        return 1.0F;
    }
    // 使用预计算覆盖面积完成常量时间查询。
    return std::clamp(
        1.0F - constraint.fixedCoveredArea / baseArea, 0.0F, 1.0F);
}

/// @brief 计算候选方块相对既有布局新增的可见比例缺口。
/// @param constraint 一个下层方块的缓存约束。
/// @param candidate 待放置的前景方块。
/// @param minimumVisibleRatio 期望保留的最小可见比例。
/// @return 当前比例低于可达要求的正差值，否则为零。
///
/// 若固定遮挡本身已低于目标，要求会降到固定基线，候选只需不进一步恶化布局。
/// @warning 拖动热路径：只允许调用无分配的缓存查询。
[[nodiscard]] inline float visibilityDeficit(
    const VisibilityConstraint& constraint, const Rect& candidate,
    float minimumVisibleRatio)
{
    // 不能要求移动候选修复不可移动遮挡已经造成的缺口。
    const float requiredRatio =
        std::min(minimumVisibleRatio, fixedVisibleRatio(constraint));
    // max 把满足约束的负差裁成零，便于多个约束累加。
    return std::max(
        0.0F, requiredRatio - visibleRatioWithCandidate(constraint, candidate));
}

/// @brief 计算一组已去重矩形平移后对下层方块造成的可见比例。
/// @param constraint 一个下层方块的缓存约束。
/// @param candidateUnionCells 批量选中方块的无重叠并集单元。
/// @param deltaX 并集相对初始位置的水平位移。
/// @param deltaY 并集相对初始位置的垂直位移。
/// @return 固定遮挡与平移候选共同作用后的可见比例。
/// @warning 批量拖动热路径：候选单元与固定可见单元均已缓存，禁止分配。
[[nodiscard]] inline float visibleRatioWithTranslatedCandidates(
    const VisibilityConstraint& constraint,
    std::span<const Rect> candidateUnionCells, float deltaX, float deltaY)
{
    // 基础矩形无面积时无需遍历候选单元。
    const float baseArea = area(constraint.base);
    if ( baseArea <= 0.0F ) return 0.0F;

    // lambda 对一个可见单元累计所有互不重叠候选的新增覆盖。
    float      newlyCoveredArea      = 0.0F;
    const auto accumulateCoveredArea = [&](const Rect& visibleCell) {
        for ( const auto& candidateCell : candidateUnionCells ) {
            // 平移只改变并集单元原点，不改变其尺寸和互不重叠性质。
            const Rect translated{
                candidateCell.x + deltaX,
                candidateCell.y + deltaY,
                candidateCell.width,
                candidateCell.height,
            };
            const auto newlyCovered = intersection(visibleCell, translated);
            if ( newlyCovered ) {
                // visibleCell 与候选单元均互不重叠，交集面积不会重复累计。
                newlyCoveredArea += area(*newlyCovered);
            }
        }
    };

    // 默认基线使用固定遮挡预计算出的可见面积。
    float baselineVisibleArea = constraint.fixedVisibleArea;
    if ( constraint.fixedVisibleCells.empty() &&
         constraint.fixedOccluders.empty() ) {
        baselineVisibleArea = baseArea;
        // 无缓存遮挡时以完整基础矩形作为唯一可见单元。
        accumulateCoveredArea(constraint.base);
    } else {
        // 只在固定遮挡留下的区域中计算候选新增覆盖。
        for ( const auto& visibleCell : constraint.fixedVisibleCells ) {
            accumulateCoveredArea(visibleCell);
        }
    }
    // 结果始终相对基础方块原面积归一化。
    return std::clamp(
        (baselineVisibleArea - newlyCoveredArea) / baseArea, 0.0F, 1.0F);
}

/// @brief 计算批量候选相对既有布局新增的可见比例缺口。
/// @param constraint 一个下层方块的缓存约束。
/// @param candidateUnionCells 候选方块的无重叠并集单元。
/// @param deltaX 候选并集水平位移。
/// @param deltaY 候选并集垂直位移。
/// @param minimumVisibleRatio 期望的最小可见比例。
/// @return 相对可达要求的非负缺口。
/// @warning 批量拖动热路径：只允许调用无分配的缓存查询。
[[nodiscard]] inline float translatedVisibilityDeficit(
    const VisibilityConstraint& constraint,
    std::span<const Rect> candidateUnionCells, float deltaX, float deltaY,
    float minimumVisibleRatio)
{
    // 固定遮挡低于配置目标时，以固定基线作为本次拖动上限。
    const float requiredRatio =
        std::min(minimumVisibleRatio, fixedVisibleRatio(constraint));
    return std::max(
        0.0F,
        requiredRatio - visibleRatioWithTranslatedCandidates(
                            constraint, candidateUnionCells, deltaX, deltaY));
}

/// @brief 计算批量候选对全部下层方块造成的总可见比例缺口。
/// @param constraints 所有受影响下层方块的预计算约束。
/// @param candidateUnionCells 候选方块的无重叠并集单元。
/// @param deltaX 候选并集水平位移。
/// @param deltaY 候选并集垂直位移。
/// @param minimumVisibleRatio 期望的最小可见比例。
/// @return 每个约束缺口之和，用于比较候选位置优劣。
/// @warning 批量拖动热路径：每帧遍历缓存，不得复制或重建候选矩形。
[[nodiscard]] inline float translatedVisibilityDeficit(
    std::span<const VisibilityConstraint> constraints,
    std::span<const Rect> candidateUnionCells, float deltaX, float deltaY,
    float minimumVisibleRatio)
{
    // 总缺口为零表示全部下层方块均满足各自可达要求。
    float deficit = 0.0F;
    for ( const auto& constraint : constraints ) {
        deficit += translatedVisibilityDeficit(constraint,
                                               candidateUnionCells,
                                               deltaX,
                                               deltaY,
                                               minimumVisibleRatio);
    }
    return deficit;
}

/// @brief 将矩形限制在工具画布边界内。
/// @param rect 待限制的矩形。
/// @param bounds 允许放置的画布边界。
/// @return 尺寸不变且原点钳制后的矩形。
///
/// 当方块尺寸大于边界时，上限回退到边界起点，使其至少从左上角开始而不产生反向
/// clamp 区间；尺寸裁剪由调用方的最小最大尺寸逻辑负责。
[[nodiscard]] inline Rect clampToBounds(Rect rect, const Rect& bounds)
{
    // 每个轴独立钳制，拖动不会改变宽高。
    rect.x = std::clamp(
        rect.x, bounds.x, std::max(bounds.x, bounds.right() - rect.width));
    rect.y = std::clamp(
        rect.y, bounds.y, std::max(bounds.y, bounds.bottom() - rect.height));
    return rect;
}

/// @brief 同尺寸方块堆叠后为下层方块保留的最小可见比例。
///
/// 该值同时作为最紧密堆叠位置和一般可见性约束默认门槛。
inline constexpr float STACK_MINIMUM_VISIBLE_RATIO = 0.35F;

/// @brief 同尺寸方块可自由组合的精确堆叠可见比例，对应覆盖 65%、50%、25%。
///
/// 数组按从紧到松排列；距离相同时后遍历候选可覆盖前者，保持既有交互选择规则。
inline constexpr std::array<float, 3> STACK_VISIBLE_RATIOS{
    STACK_MINIMUM_VISIBLE_RATIO,
    0.50F,
    0.75F,
};

/// @brief 判断两个方块是否可视为同尺寸方块。
/// @param lhs 第一个方块。
/// @param rhs 第二个方块。
/// @return 宽高差均在半个逻辑像素内时返回 true。
[[nodiscard]] inline bool hasMatchingSize(const Rect& lhs, const Rect& rhs)
{
    // 容差吸收 DPI 反算和拖动缩放产生的浮点舍入。
    constexpr float SIZE_TOLERANCE = 0.5F;
    return std::abs(lhs.width - rhs.width) <= SIZE_TOLERANCE &&
           std::abs(lhs.height - rhs.height) <= SIZE_TOLERANCE;
}

/// @brief 收集矩形水平方向的边缘与中心锚点。
/// @param rect 输入矩形。
/// @return 左边、水平中心和右边坐标。
[[nodiscard]] inline std::array<float, 3> horizontalTargets(const Rect& rect)
{
    return {
        rect.x,
        rect.x + rect.width * 0.5F,
        rect.right(),
    };
}

/// @brief 收集矩形垂直方向的边缘与中心锚点。
/// @param rect 输入矩形。
/// @return 上边、垂直中心和下边坐标。
[[nodiscard]] inline std::array<float, 3> verticalTargets(const Rect& rect)
{
    return {
        rect.y,
        rect.y + rect.height * 0.5F,
        rect.bottom(),
    };
}

/// @brief 对一个轴执行自身左中右或上中下到目标锚点的吸附。
/// @param rawPosition 方块在该轴上的未吸附最小坐标。
/// @param size 方块在该轴上的尺寸。
/// @param targetAnchors 画布和其他方块提供的候选参考线。
/// @param snapThreshold 首次进入吸附的最大距离。
/// @param releaseThreshold 已锁定后允许偏离的释放距离。
/// @param lock 跨帧维护的该轴吸附锁。
/// @return 应用锁定或最近候选后的方块最小坐标。
///
/// 方块自身的最小边、中心和最大边分别尝试对齐每条目标线。独立释放阈值形成滞后，
/// 防止指针在吸附阈值附近移动时位置反复跳动。
/// @warning 拖动热路径：遍历已缓存锚点，不得在函数内查询布局或项目状态。
[[nodiscard]] inline float snapAxis(float rawPosition, float size,
                                    std::span<const float> targetAnchors,
                                    float snapThreshold, float releaseThreshold,
                                    AxisSnapLock& lock)
{
    if ( lock.position ) {
        // 未超过释放阈值时保持精确锁定位置，不重新竞争其他锚点。
        if ( std::abs(rawPosition - *lock.position) <= releaseThreshold ) {
            return *lock.position;
        }
        // 超过释放阈值后同时清除位置和辅助线目标。
        lock.position.reset();
        lock.targetLine.reset();
    }

    // 比例把对象最小边、中心和最大边转换为同一个最小坐标候选。
    constexpr std::array<float, 3> OWN_ANCHOR_RATIOS{ 0.0F, 0.5F, 1.0F };
    float                          bestPosition = rawPosition;
    float                          bestDistance = snapThreshold;
    std::optional<float>           bestTarget;
    for ( const float target : targetAnchors ) {
        for ( const float ratio : OWN_ANCHOR_RATIOS ) {
            // 要让自身 ratio 锚点落在 target，最小坐标应减去 size * ratio。
            const float candidate = target - size * ratio;
            const float distance  = std::abs(rawPosition - candidate);
            if ( distance <= bestDistance ) {
                // 相同距离允许后遇到的目标覆盖，保持确定的遍历顺序语义。
                bestDistance = distance;
                bestPosition = candidate;
                bestTarget   = target;
            }
        }
    }
    if ( bestPosition != rawPosition ) {
        // 锁同时保存最终对象位置和外部参考线，供下一帧与辅助线绘制使用。
        lock.position   = bestPosition;
        lock.targetLine = bestTarget;
    }
    return bestPosition;
}

/// @brief 将拖动方块吸附到同尺寸堆叠位置、其它方块和可见画布的锚点。
/// @param rawRect 当前未吸附候选矩形。
/// @param visibleCanvas 当前可见画布矩形。
/// @param otherRects 按从下到上顺序缓存的其他方块。
/// @param snapThreshold 首次吸附阈值。
/// @param releaseThreshold 锁定释放阈值。
/// @param locks 横纵轴跨帧吸附状态。
/// @return 优先堆叠吸附，其次普通锚点吸附后的矩形。
///
/// 同尺寸堆叠需要两轴同时满足特定可见比例，优先级高于一般边缘或中心吸附。倒序
/// 遍历使视觉上更靠前的方块优先成为堆叠目标。
/// @warning 拖动热路径：按缓存的图层顺序检查方块，禁止查询项目资源或重建布局。
[[nodiscard]] inline Rect snapRect(Rect rawRect, const Rect& visibleCanvas,
                                   std::span<const Rect> otherRects,
                                   float snapThreshold, float releaseThreshold,
                                   SnapLocks& locks)
{
    // 每轴先根据原始指针位置判断已有锁是否超过释放阈值。
    const auto retainLock = [releaseThreshold](float         rawPosition,
                                               AxisSnapLock& lock) {
        if ( !lock.position ||
             std::abs(rawPosition - *lock.position) <= releaseThreshold ) {
            // 无锁或仍在滞后范围内时保持当前状态。
            return;
        }
        lock.position.reset();
        lock.targetLine.reset();
    };
    retainLock(rawRect.x, locks.x);
    retainLock(rawRect.y, locks.y);

    // 堆叠候选在已有锁时必须与锁定位置完全一致，否则使用首次吸附阈值。
    const auto axisCanAttach = [snapThreshold](float rawPosition,
                                               float stackedPosition,
                                               const AxisSnapLock& lock) {
        if ( lock.position ) {
            return std::abs(*lock.position - stackedPosition) <= 1e-4F;
        }
        return std::abs(rawPosition - stackedPosition) <= snapThreshold;
    };
    for ( auto target = otherRects.rbegin(); target != otherRects.rend();
          ++target ) {
        // 只有近似同尺寸方块支持预定义覆盖比例堆叠。
        if ( !hasMatchingSize(rawRect, *target) ) continue;

        const float stackedX = target->x;
        if ( !axisCanAttach(rawRect.x, stackedX, locks.x) ) {
            continue;
        }

        std::optional<float> stackedY;
        // 在允许阈值内选择最接近原始纵坐标的堆叠比例。
        float bestVerticalDistance = snapThreshold;
        for ( const float visibleRatio : STACK_VISIBLE_RATIOS ) {
            const float candidateY = target->y + target->height * visibleRatio;
            if ( !axisCanAttach(rawRect.y, candidateY, locks.y) ) continue;

            const float distance =
                locks.y.position ? 0.0F : std::abs(rawRect.y - candidateY);
            if ( distance > bestVerticalDistance ) continue;
            bestVerticalDistance = distance;
            stackedY             = candidateY;
        }
        if ( !stackedY ) continue;

        // 两轴一起锁定到堆叠位置，避免下一帧退化为普通单轴吸附。
        rawRect.x          = stackedX;
        rawRect.y          = *stackedY;
        locks.x.position   = stackedX;
        locks.x.targetLine = target->x;
        locks.y.position   = *stackedY;
        locks.y.targetLine = *stackedY;
        return rawRect;
    }

    if ( locks.x.position && locks.y.position ) {
        // 已锁定的完整二维位置优先保留，不重新构建普通锚点列表。
        rawRect.x = *locks.x.position;
        rawRect.y = *locks.y.position;
        return rawRect;
    }

    // 普通吸附收集可见画布与每个其他方块的边缘、中心参考线。
    std::vector<float> xTargets;
    std::vector<float> yTargets;
    xTargets.reserve(otherRects.size() * 3 + 3);
    yTargets.reserve(otherRects.size() * 3 + 3);
    // 画布自身三条锚点始终排在其他方块之前。
    xTargets.push_back(visibleCanvas.x);
    xTargets.push_back(visibleCanvas.x + visibleCanvas.width * 0.5F);
    xTargets.push_back(visibleCanvas.right());
    yTargets.push_back(visibleCanvas.y);
    yTargets.push_back(visibleCanvas.y + visibleCanvas.height * 0.5F);
    yTargets.push_back(visibleCanvas.bottom());
    for ( const auto& rect : otherRects ) {
        // 每个方块每轴贡献最小边、中心和最大边。
        const auto horizontal = horizontalTargets(rect);
        const auto vertical   = verticalTargets(rect);
        xTargets.insert(xTargets.end(), horizontal.begin(), horizontal.end());
        yTargets.insert(yTargets.end(), vertical.begin(), vertical.end());
    }

    // 两轴独立调用 snapAxis，允许只对齐水平或垂直参考线。
    rawRect.x = snapAxis(rawRect.x,
                         rawRect.width,
                         xTargets,
                         snapThreshold,
                         releaseThreshold,
                         locks.x);
    rawRect.y = snapAxis(rawRect.y,
                         rawRect.height,
                         yTargets,
                         snapThreshold,
                         releaseThreshold,
                         locks.y);
    return rawRect;
}

/// @brief 收集当前画布和其它方块在一个轴上的全部吸附锚点。
/// @param horizontal true 收集 X 轴锚点，false 收集 Y 轴锚点。
/// @param visibleCanvas 当前可见画布矩形。
/// @param otherRects 参与吸附的其他方块。
/// @return 画布锚点在前、方块锚点随后且保留输入顺序的数组。
/// @warning 缩放交互的低频准备路径：会分配返回容器，静止帧不得调用。
[[nodiscard]] inline std::vector<float> collectAxisTargets(
    bool horizontal, const Rect& visibleCanvas,
    std::span<const Rect> otherRects)
{
    // 每个矩形贡献三个锚点，提前保留精确上界。
    std::vector<float> targets;
    targets.reserve(otherRects.size() * 3 + 3);
    if ( horizontal ) {
        // 水平模式收集左、中心、右坐标。
        targets.push_back(visibleCanvas.x);
        targets.push_back(visibleCanvas.x + visibleCanvas.width * 0.5F);
        targets.push_back(visibleCanvas.right());
        for ( const auto& rect : otherRects ) {
            const auto anchors = horizontalTargets(rect);
            targets.insert(targets.end(), anchors.begin(), anchors.end());
        }
    } else {
        // 垂直模式收集上、中心、下坐标。
        targets.push_back(visibleCanvas.y);
        targets.push_back(visibleCanvas.y + visibleCanvas.height * 0.5F);
        targets.push_back(visibleCanvas.bottom());
        for ( const auto& rect : otherRects ) {
            const auto anchors = verticalTargets(rect);
            targets.insert(targets.end(), anchors.begin(), anchors.end());
        }
    }
    return targets;
}

/// @brief 对正在缩放的单轴执行自身边缘或中心到目标锚点的吸附。
/// @param rawMinimum 当前候选最小边坐标。
/// @param rawMaximum 当前候选最大边坐标。
/// @param edge 本轴正在拖动的边。
/// @param targetAnchors 外部边缘和中心参考线。
/// @param minimumSize 本轴允许的最小尺寸。
/// @param snapThreshold 首次吸附阈值。
/// @param releaseThreshold 锁定释放阈值。
/// @param lock 本轴跨帧吸附状态。
/// @return 吸附后的活动边坐标。
///
/// 非活动边保持固定，通过反解活动边位置使缩放后矩形自身边缘或中心对齐目标；不满足
/// 最小尺寸的候选会被跳过。
[[nodiscard]] inline float snapResizeAxis(
    float rawMinimum, float rawMaximum, ResizeEdge edge,
    std::span<const float> targetAnchors, float minimumSize,
    float snapThreshold, float releaseThreshold, AxisSnapLock& lock)
{
    if ( edge == ResizeEdge::None ) {
        // 未参与缩放的轴不能保留上一轮吸附锁。
        lock.position.reset();
        lock.targetLine.reset();
        return edge == ResizeEdge::Minimum ? rawMinimum : rawMaximum;
    }

    // rawEdge 统一表示当前被指针直接控制的边。
    const float rawEdge = edge == ResizeEdge::Minimum ? rawMinimum : rawMaximum;
    if ( lock.position ) {
        // 活动边仍在释放阈值内时直接保持锁定。
        if ( std::abs(rawEdge - *lock.position) <= releaseThreshold ) {
            return *lock.position;
        }
        // 指针离开滞后范围后允许重新寻找目标。
        lock.position.reset();
        lock.targetLine.reset();
    }

    // 三个比例代表缩放后矩形自身的最小边、中心和最大边。
    constexpr std::array<float, 3> OWN_ANCHOR_RATIOS{ 0.0F, 0.5F, 1.0F };
    float                          bestEdge     = rawEdge;
    float                          bestDistance = snapThreshold;
    std::optional<float>           bestTarget;
    for ( const float target : targetAnchors ) {
        for ( const float ratio : OWN_ANCHOR_RATIOS ) {
            float candidate = rawEdge;
            if ( edge == ResizeEdge::Minimum ) {
                // 最大边固定时，自身最大边无法通过移动最小边对齐任意目标。
                if ( ratio >= 1.0F ) continue;
                candidate = (target - ratio * rawMaximum) / (1.0F - ratio);
                if ( rawMaximum - candidate < minimumSize ) continue;
            } else {
                // 最小边固定时，自身最小边不随活动最大边移动。
                if ( ratio <= 0.0F ) continue;
                candidate = (target - (1.0F - ratio) * rawMinimum) / ratio;
                if ( candidate - rawMinimum < minimumSize ) continue;
            }
            const float distance = std::abs(rawEdge - candidate);
            if ( distance <= bestDistance ) {
                // 在阈值内保留离原始活动边最近的合法候选。
                bestDistance = distance;
                bestEdge     = candidate;
                bestTarget   = target;
            }
        }
    }
    if ( bestEdge != rawEdge ) {
        // 锁定活动边位置，并保留外部目标线供 UI 绘制。
        lock.position   = bestEdge;
        lock.targetLine = bestTarget;
    }
    return bestEdge;
}

/// @brief 将缩放中的活动边吸附到方块及可见画布的边缘和中心锚点。
/// @param rawRect 当前未吸附的缩放矩形。
/// @param horizontalEdge 水平轴活动边。
/// @param verticalEdge 垂直轴活动边。
/// @param visibleCanvas 可见画布边界。
/// @param otherRects 参与吸附的其他方块。
/// @param minimumWidth 最小允许宽度。
/// @param minimumHeight 最小允许高度。
/// @param snapThreshold 首次吸附阈值。
/// @param releaseThreshold 锁定释放阈值。
/// @param locks 两轴跨帧吸附锁。
/// @return 调整原点及尺寸后的吸附矩形。
[[nodiscard]] inline Rect snapResizeRect(
    Rect rawRect, ResizeEdge horizontalEdge, ResizeEdge verticalEdge,
    const Rect& visibleCanvas, std::span<const Rect> otherRects,
    float minimumWidth, float minimumHeight, float snapThreshold,
    float releaseThreshold, SnapLocks& locks)
{
    // 每轴目标列表在本次尺寸变化时构建一次。
    const auto xTargets = collectAxisTargets(true, visibleCanvas, otherRects);
    const auto yTargets = collectAxisTargets(false, visibleCanvas, otherRects);

    // 在改写矩形前保存非活动最大边，供最小边缩放反算尺寸。
    const float rawRight          = rawRect.right();
    const float rawBottom         = rawRect.bottom();
    const float snappedHorizontal = snapResizeAxis(rawRect.x,
                                                   rawRight,
                                                   horizontalEdge,
                                                   xTargets,
                                                   minimumWidth,
                                                   snapThreshold,
                                                   releaseThreshold,
                                                   locks.x);
    const float snappedVertical   = snapResizeAxis(rawRect.y,
                                                   rawBottom,
                                                   verticalEdge,
                                                   yTargets,
                                                   minimumHeight,
                                                   snapThreshold,
                                                   releaseThreshold,
                                                   locks.y);
    if ( horizontalEdge == ResizeEdge::Minimum ) {
        // 左边活动时右边固定，原点和宽度同时变化。
        rawRect.x     = snappedHorizontal;
        rawRect.width = rawRight - snappedHorizontal;
    } else if ( horizontalEdge == ResizeEdge::Maximum ) {
        // 右边活动时原点固定，只更新宽度。
        rawRect.width = snappedHorizontal - rawRect.x;
    }
    if ( verticalEdge == ResizeEdge::Minimum ) {
        // 上边活动时下边固定，原点和高度同时变化。
        rawRect.y      = snappedVertical;
        rawRect.height = rawBottom - snappedVertical;
    } else if ( verticalEdge == ResizeEdge::Maximum ) {
        // 下边活动时原点固定，只更新高度。
        rawRect.height = snappedVertical - rawRect.y;
    }
    return rawRect;
}

/// @brief 计算全部可见比例约束的总缺口。
/// @warning 拖动热路径：每帧遍历约束缓存，禁止复制、分配或重建固定遮挡。
[[nodiscard]] inline float visibilityDeficit(
    const Rect& candidate, std::span<const VisibilityConstraint> constraints,
    float minimumVisibleRatio)
{
    float deficit = 0.0F;
    for ( const auto& constraint : constraints ) {
        deficit +=
            visibilityDeficit(constraint, candidate, minimumVisibleRatio);
    }
    return deficit;
}

/// @brief 修正前景方块位置，使所有下层方块至少保留指定可见比例。
/// @warning 拖动热路径的低频几何分支：仅在方块位置变化时调用，最多执行固定
/// 轮数的局部约束检查，禁止在静止帧重复执行。
[[nodiscard]] inline Rect constrainVisibility(
    Rect candidate, const Rect& canvasBounds,
    std::span<const VisibilityConstraint> constraints,
    float                                 minimumVisibleRatio)
{
    candidate                = clampToBounds(candidate, canvasBounds);
    constexpr int MAX_PASSES = 8;
    for ( int pass = 0; pass < MAX_PASSES; ++pass ) {
        if ( visibilityDeficit(candidate, constraints, minimumVisibleRatio) <=
             1e-5F ) {
            break;
        }

        Rect  best = candidate;
        float bestDeficit =
            visibilityDeficit(candidate, constraints, minimumVisibleRatio);
        float bestDistance = std::numeric_limits<float>::max();
        for ( const auto& constraint : constraints ) {
            if ( visibilityDeficit(
                     constraint, candidate, minimumVisibleRatio) <= 1e-5F ) {
                continue;
            }

            const Rect&               base = constraint.base;
            const std::array<Rect, 4> alternatives{
                Rect{ base.x + base.width * minimumVisibleRatio,
                      candidate.y,
                      candidate.width,
                      candidate.height },
                Rect{ base.right() - base.width * minimumVisibleRatio -
                          candidate.width,
                      candidate.y,
                      candidate.width,
                      candidate.height },
                Rect{ candidate.x,
                      base.y + base.height * minimumVisibleRatio,
                      candidate.width,
                      candidate.height },
                Rect{ candidate.x,
                      base.bottom() - base.height * minimumVisibleRatio -
                          candidate.height,
                      candidate.width,
                      candidate.height },
            };
            for ( auto alternative : alternatives ) {
                alternative         = clampToBounds(alternative, canvasBounds);
                const float deficit = visibilityDeficit(
                    alternative, constraints, minimumVisibleRatio);
                const float deltaX   = alternative.x - candidate.x;
                const float deltaY   = alternative.y - candidate.y;
                const float distance = deltaX * deltaX + deltaY * deltaY;
                if ( deficit < bestDeficit - 1e-5F ||
                     (std::abs(deficit - bestDeficit) <= 1e-5F &&
                      distance < bestDistance) ) {
                    best         = alternative;
                    bestDeficit  = deficit;
                    bestDistance = distance;
                }
            }
        }
        if ( best.x == candidate.x && best.y == candidate.y ) break;
        candidate = best;
    }
    return candidate;
}

/// @brief 沿当前缩放轨迹限制候选矩形，使下层方块始终保留最小可见比例。
/// @warning 缩放交互路径：只在尺寸变化时执行固定轮数二分，禁止在静止帧调用。
[[nodiscard]] inline Rect constrainResizeVisibility(
    const Rect& previous, const Rect& candidate,
    std::span<const VisibilityConstraint> constraints,
    float                                 minimumVisibleRatio)
{
    const float previousDeficit =
        visibilityDeficit(previous, constraints, minimumVisibleRatio);
    const float candidateDeficit =
        visibilityDeficit(candidate, constraints, minimumVisibleRatio);
    if ( candidateDeficit <= 1e-5F ||
         (previousDeficit > 1e-5F &&
          candidateDeficit < previousDeficit - 1e-5F) ) {
        return candidate;
    }

    float         validAmount          = 0.0F;
    float         invalidAmount        = 1.0F;
    constexpr int BINARY_SEARCH_PASSES = 12;
    for ( int pass = 0; pass < BINARY_SEARCH_PASSES; ++pass ) {
        const float amount = (validAmount + invalidAmount) * 0.5F;
        const Rect  trial{
            previous.x + (candidate.x - previous.x) * amount,
            previous.y + (candidate.y - previous.y) * amount,
            previous.width + (candidate.width - previous.width) * amount,
            previous.height + (candidate.height - previous.height) * amount,
        };
        if ( visibilityDeficit(trial, constraints, minimumVisibleRatio) <=
             1e-5F ) {
            validAmount = amount;
        } else {
            invalidAmount = amount;
        }
    }
    return {
        previous.x + (candidate.x - previous.x) * validAmount,
        previous.y + (candidate.y - previous.y) * validAmount,
        previous.width + (candidate.width - previous.width) * validAmount,
        previous.height + (candidate.height - previous.height) * validAmount,
    };
}

/// @brief 沿批量移动轨迹限制组合外框，避免选中方块遮住固定下层方块。
/// @warning 批量拖动热路径：仅候选位置无效时执行固定轮数二分，不得分配。
[[nodiscard]] inline Rect constrainTranslatedVisibility(
    const Rect& previousBounds, Rect candidateBounds, const Rect& initialBounds,
    std::span<const Rect> candidateUnionCells, const Rect& canvasBounds,
    std::span<const VisibilityConstraint> constraints,
    float                                 minimumVisibleRatio)
{
    candidateBounds      = clampToBounds(candidateBounds, canvasBounds);
    const auto deficitAt = [&](const Rect& bounds) {
        return translatedVisibilityDeficit(constraints,
                                           candidateUnionCells,
                                           bounds.x - initialBounds.x,
                                           bounds.y - initialBounds.y,
                                           minimumVisibleRatio);
    };

    const float previousDeficit  = deficitAt(previousBounds);
    const float candidateDeficit = deficitAt(candidateBounds);
    if ( candidateDeficit <= 1e-5F ||
         (previousDeficit > 1e-5F &&
          candidateDeficit < previousDeficit - 1e-5F) ) {
        return candidateBounds;
    }

    float         validAmount          = 0.0F;
    float         invalidAmount        = 1.0F;
    constexpr int BINARY_SEARCH_PASSES = 12;
    for ( int pass = 0; pass < BINARY_SEARCH_PASSES; ++pass ) {
        const float amount = (validAmount + invalidAmount) * 0.5F;
        const Rect  trial{
            previousBounds.x + (candidateBounds.x - previousBounds.x) * amount,
            previousBounds.y + (candidateBounds.y - previousBounds.y) * amount,
            previousBounds.width,
            previousBounds.height,
        };
        if ( deficitAt(trial) <= 1e-5F ) {
            validAmount = amount;
        } else {
            invalidAmount = amount;
        }
    }
    return {
        previousBounds.x + (candidateBounds.x - previousBounds.x) * validAmount,
        previousBounds.y + (candidateBounds.y - previousBounds.y) * validAmount,
        previousBounds.width,
        previousBounds.height,
    };
}

/// @brief 查找基础方块未被上层方块遮挡的最大网格单元，用于放置文本标签。
/// @warning 低频布局路径：只在布局或叠层变化时重建标签裁切缓存。
[[nodiscard]] inline Rect largestVisibleCell(const Rect&           base,
                                             std::span<const Rect> occluders)
{
    std::vector<float> xCoordinates{ base.x, base.right() };
    std::vector<float> yCoordinates{ base.y, base.bottom() };
    for ( const auto& occluder : occluders ) {
        const auto clipped = intersection(base, occluder);
        if ( !clipped ) continue;
        xCoordinates.push_back(clipped->x);
        xCoordinates.push_back(clipped->right());
        yCoordinates.push_back(clipped->y);
        yCoordinates.push_back(clipped->bottom());
    }
    std::ranges::sort(xCoordinates);
    std::ranges::sort(yCoordinates);
    xCoordinates.erase(std::unique(xCoordinates.begin(), xCoordinates.end()),
                       xCoordinates.end());
    yCoordinates.erase(std::unique(yCoordinates.begin(), yCoordinates.end()),
                       yCoordinates.end());

    Rect best{};
    for ( std::size_t xIndex = 1; xIndex < xCoordinates.size(); ++xIndex ) {
        for ( std::size_t yIndex = 1; yIndex < yCoordinates.size(); ++yIndex ) {
            Rect cell{
                xCoordinates[xIndex - 1],
                yCoordinates[yIndex - 1],
                xCoordinates[xIndex] - xCoordinates[xIndex - 1],
                yCoordinates[yIndex] - yCoordinates[yIndex - 1],
            };
            const float midpointX = cell.x + cell.width * 0.5F;
            const float midpointY = cell.y + cell.height * 0.5F;
            const bool  covered =
                std::ranges::any_of(occluders, [&](const Rect& occluder) {
                    return midpointX > occluder.x &&
                           midpointX < occluder.right() &&
                           midpointY > occluder.y &&
                           midpointY < occluder.bottom();
                });
            if ( !covered && area(cell) > area(best) ) {
                best = cell;
            }
        }
    }
    return area(best) > 0.0F ? best : base;
}

/// @brief 从基础可见区域中扣除一个移动遮挡并选取最大标签矩形。
/// @warning 拖动热路径：固定执行至多四次面积比较，不分配、不排序。
[[nodiscard]] inline Rect largestVisibleCellWithOneOccluder(
    const Rect& base, const Rect& occluder)
{
    const auto clipped = intersection(base, occluder);
    if ( !clipped ) return base;

    const std::array candidates{
        Rect{ base.x, base.y, base.width, clipped->y - base.y },
        Rect{ base.x,
              clipped->bottom(),
              base.width,
              base.bottom() - clipped->bottom() },
        Rect{ base.x, clipped->y, clipped->x - base.x, clipped->height },
        Rect{ clipped->right(),
              clipped->y,
              base.right() - clipped->right(),
              clipped->height },
    };
    Rect best{};
    for ( const auto& candidate : candidates ) {
        if ( area(candidate) > area(best) ) {
            best = candidate;
        }
    }
    return area(best) > 0.0F ? best : base;
}

}  // namespace MMM::UI::ProjectAudioToolLayout
