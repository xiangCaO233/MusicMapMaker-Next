#include "canvas/PreviewDensityInteraction.h"
#include "canvas/PreviewDensityColor.h"

#include <cmath>
#include <limits>
#include <optional>

namespace
{

/// @brief 使用小容差比较密度栏时间映射结果。
/// @param value 待检查可选时间。
/// @param expected 期望时间。
/// @return 时间存在且足够接近期望值时返回 true。
bool near(const std::optional<double>& value, double expected)
{
    return value && std::abs(*value - expected) < 1e-9;
}

/// @brief 验证全谱时间到密度栏坐标的正向投影。
/// @return 谱面开头、中央和末尾均落在正确位置时返回 true。
bool testTimeProjection()
{
    return near(MMM::Canvas::previewDensityYAtTime(0.0, 10.0, 110.0, 200.0),
                110.0) &&
           near(MMM::Canvas::previewDensityYAtTime(100.0, 10.0, 110.0, 200.0),
                60.0) &&
           near(MMM::Canvas::previewDensityYAtTime(200.0, 10.0, 110.0, 200.0),
                10.0);
}

/// @brief 验证协作者时间超出谱面范围时标记仍限制在密度栏端点。
/// @return 上下越界输入均正确限制时返回 true。
bool testTimeProjectionClamp()
{
    return near(MMM::Canvas::previewDensityYAtTime(-20.0, 10.0, 110.0, 200.0),
                110.0) &&
           near(MMM::Canvas::previewDensityYAtTime(240.0, 10.0, 110.0, 200.0),
                10.0);
}

/// @brief 使用小容差比较密度颜色分量。
/// @param value 待检查颜色分量。
/// @param expected 期望颜色分量。
/// @return 两个分量足够接近时返回 true。
bool nearColor(float value, float expected)
{
    return std::abs(value - expected) < 1e-6f;
}

/// @brief 验证密度颜色的绿色、橙色、红色锚点和输入限制。
/// @return 所有颜色锚点符合预期时返回 true。
bool testDensityColorGradient()
{
    const auto low     = MMM::Canvas::previewDensityColorAt(0.0f);
    const auto medium  = MMM::Canvas::previewDensityColorAt(0.5f);
    const auto high    = MMM::Canvas::previewDensityColorAt(1.0f);
    const auto clamped = MMM::Canvas::previewDensityColorAt(2.0f);
    return low.g > low.r && low.g > low.b && medium.r > medium.g &&
           medium.g > medium.b && high.r > high.g && high.r > high.b &&
           nearColor(high.r, clamped.r) && nearColor(high.g, clamped.g) &&
           nearColor(high.b, clamped.b);
}

/// @brief 验证密度栏顶部、中央和底部的反向时间轴映射。
/// @return 行为符合预期时返回 true。
bool testVerticalAxisMapping()
{
    return near(MMM::Canvas::previewDensityTimeAtY(10.0, 10.0, 110.0, 200.0),
                200.0) &&
           near(MMM::Canvas::previewDensityTimeAtY(60.0, 10.0, 110.0, 200.0),
                100.0) &&
           near(MMM::Canvas::previewDensityTimeAtY(110.0, 10.0, 110.0, 200.0),
                0.0);
}

/// @brief 验证拖出密度栏后目标时间仍会限制在全谱范围内。
/// @return 行为符合预期时返回 true。
bool testOutOfBoundsClamp()
{
    return near(MMM::Canvas::previewDensityTimeAtY(-20.0, 10.0, 110.0, 200.0),
                200.0) &&
           near(MMM::Canvas::previewDensityTimeAtY(140.0, 10.0, 110.0, 200.0),
                0.0);
}

/// @brief 验证无效坐标、范围和时长不会生成 Seek 目标。
/// @return 行为符合预期时返回 true。
bool testInvalidInputs()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    return !MMM::Canvas::previewDensityYAtTime(nan, 10.0, 110.0, 200.0) &&
           !MMM::Canvas::previewDensityYAtTime(60.0, nan, 110.0, 200.0) &&
           !MMM::Canvas::previewDensityYAtTime(60.0, 110.0, 10.0, 200.0) &&
           !MMM::Canvas::previewDensityYAtTime(60.0, 10.0, 110.0, 0.0) &&
           !MMM::Canvas::previewDensityYAtTime(60.0, 10.0, 110.0, nan) &&
           !MMM::Canvas::previewDensityTimeAtY(nan, 10.0, 110.0, 200.0) &&
           !MMM::Canvas::previewDensityTimeAtY(60.0, nan, 110.0, 200.0) &&
           !MMM::Canvas::previewDensityTimeAtY(60.0, 110.0, 10.0, 200.0) &&
           !MMM::Canvas::previewDensityTimeAtY(60.0, 10.0, 110.0, 0.0) &&
           !MMM::Canvas::previewDensityTimeAtY(60.0, 10.0, 110.0, -1.0) &&
           !MMM::Canvas::previewDensityTimeAtY(60.0, 10.0, 110.0, nan);
}

/// @brief 验证密度栏仅在拖动变化时预览，并在松手时固定提交一次。
/// @return 按下、静止、移动和松手帧均得到正确发布类型时返回 true。
bool testSeekDispatchLifecycle()
{
    using MMM::Canvas::PreviewDensitySeekDispatch;
    using MMM::Canvas::resolvePreviewDensitySeekDispatch;
    return resolvePreviewDensitySeekDispatch(true, false, false, false) ==
               PreviewDensitySeekDispatch::Preview &&
           resolvePreviewDensitySeekDispatch(true, false, true, false) ==
               PreviewDensitySeekDispatch::None &&
           resolvePreviewDensitySeekDispatch(true, false, true, true) ==
               PreviewDensitySeekDispatch::Preview &&
           resolvePreviewDensitySeekDispatch(false, true, true, false) ==
               PreviewDensitySeekDispatch::Commit &&
           resolvePreviewDensitySeekDispatch(false, true, false, true) ==
               PreviewDensitySeekDispatch::None;
}

}  // namespace

/// @brief 覆盖预览密度栏拖动 Seek 的纵向时间映射。
/// @return 所有检查通过时返回 0。
int main()
{
    return testTimeProjection() && testTimeProjectionClamp() &&
                   testVerticalAxisMapping() && testOutOfBoundsClamp() &&
                   testInvalidInputs() && testDensityColorGradient() &&
                   testSeekDispatchLifecycle()
               ? 0
               : 1;
}
