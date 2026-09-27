#pragma once

#include <algorithm>

namespace MMM::Canvas
{

/// @brief 预览密度颜色的 RGB 分量。
struct PreviewDensityColor {
    /// @brief 红色分量。
    float r{ 0.0f };

    /// @brief 绿色分量。
    float g{ 0.0f };

    /// @brief 蓝色分量。
    float b{ 0.0f };
};

/// @brief 根据归一化密度计算绿色、橙色到红色的分段渐变。
/// @param normalizedDensity 归一化密度，函数内部限制到 0 到 1。
/// @return 可直接用于 UI 绘制的 RGB 颜色。
/// @warning UI 热路径纯计算：密度栏每个可见行调用；只允许常量级算术。
constexpr PreviewDensityColor previewDensityColorAt(float normalizedDensity)
{
    // 三个锚点分别表达低、中、高密度，保持亮度足以覆盖在深色预览背景上。
    constexpr PreviewDensityColor low{ 0.31f, 0.76f, 0.38f };
    constexpr PreviewDensityColor medium{ 0.95f, 0.61f, 0.16f };
    constexpr PreviewDensityColor high{ 0.91f, 0.24f, 0.24f };

    // 外部密度可能因窗口截取或峰值变化短暂越界，先限制到有效色带范围。
    const float density = std::clamp(normalizedDensity, 0.0f, 1.0f);
    // 前半段从绿色过渡到橙色，后半段再从橙色过渡到红色。
    const PreviewDensityColor start = density <= 0.5f ? low : medium;
    const PreviewDensityColor end   = density <= 0.5f ? medium : high;
    const float t = density <= 0.5f ? density * 2.0f : (density - 0.5f) * 2.0f;
    // 三个通道共用局部分段参数，确保每个锚点处颜色连续且无跳变。
    return {
        start.r + (end.r - start.r) * t,
        start.g + (end.g - start.g) * t,
        start.b + (end.b - start.b) * t,
    };
}

}  // namespace MMM::Canvas
