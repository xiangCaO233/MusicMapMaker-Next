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
///
/// 固定 200 秒时长与 100 像素轴高，使时间比例可直接对应纵向坐标。
bool testTimeProjection()
{
    // 时间轴方向自下而上：零秒在底部，谱面末尾在顶部。
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
    // 负时间和超出 duration 的时间分别夹到两个端点。
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
///
/// 颜色测试关注色相关系与夹取，不把实现限定为某种整数打包格式。
bool testDensityColorGradient()
{
    // 采样两个端点、中点以及一个超过上界的输入。
    const auto low     = MMM::Canvas::previewDensityColorAt(0.0f);
    const auto medium  = MMM::Canvas::previewDensityColorAt(0.5f);
    const auto high    = MMM::Canvas::previewDensityColorAt(1.0f);
    const auto clamped = MMM::Canvas::previewDensityColorAt(2.0f);
    // 锚点以主导通道判断色相，越界颜色应与高密度端点逐通道一致。
    return low.g > low.r && low.g > low.b && medium.r > medium.g &&
           medium.g > medium.b && high.r > high.g && high.r > high.b &&
           nearColor(high.r, clamped.r) && nearColor(high.g, clamped.g) &&
           nearColor(high.b, clamped.b);
}

/// @brief 验证密度栏顶部、中央和底部的反向时间轴映射。
/// @return 行为符合预期时返回 true。
bool testVerticalAxisMapping()
{
    // 与正向投影使用相同几何，检查顶部、中点和底部的逆映射。
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
    // 指针拖出密度栏上方或下方时仍应产生合法的端点时间。
    return near(MMM::Canvas::previewDensityTimeAtY(-20.0, 10.0, 110.0, 200.0),
                200.0) &&
           near(MMM::Canvas::previewDensityTimeAtY(140.0, 10.0, 110.0, 200.0),
                0.0);
}

/// @brief 验证无效坐标、范围和时长不会生成 Seek 目标。
/// @return 行为符合预期时返回 true。
///
/// 正反投影共享输入约束，本用例确保任一无效参数都不会泄漏 NaN。
bool testInvalidInputs()
{
    // NaN 分别注入时间、顶部、时长与指针，覆盖所有可能污染计算的输入。
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // 反向纵向区间和非正时长没有可定义的映射，必须返回空 optional。
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
///
/// 状态组合模拟完整鼠标生命周期，避免依赖实际 ImGui 输入环境。
bool testSeekDispatchLifecycle()
{
    using MMM::Canvas::PreviewDensitySeekDispatch;
    using MMM::Canvas::resolvePreviewDensitySeekDispatch;
    // 首次按下立即预览，持续按住且目标不变时不重复发布。
    return resolvePreviewDensitySeekDispatch(true, false, false, false) ==
               PreviewDensitySeekDispatch::Preview &&
           resolvePreviewDensitySeekDispatch(true, false, true, false) ==
               PreviewDensitySeekDispatch::None &&
           resolvePreviewDensitySeekDispatch(true, false, true, true) ==
               // 拖动目标变化时继续发送本地预览。
               PreviewDensitySeekDispatch::Preview &&
           resolvePreviewDensitySeekDispatch(false, true, true, false) ==
               // 只有此前确实按住的松手帧提交最终联机状态。
               PreviewDensitySeekDispatch::Commit &&
           resolvePreviewDensitySeekDispatch(false, true, false, true) ==
               PreviewDensitySeekDispatch::None;
}

}  // namespace

/// @brief 覆盖预览密度栏拖动 Seek 的纵向时间映射。
/// @return 所有检查通过时返回 0。
int main()
{
    // 正反投影、边界限制、非法输入、颜色和发布生命周期共同构成密度栏契约。
    return testTimeProjection() && testTimeProjectionClamp() &&
                   testVerticalAxisMapping() && testOutOfBoundsClamp() &&
                   testInvalidInputs() && testDensityColorGradient() &&
                   testSeekDispatchLifecycle()
               ? 0
               : 1;
}
