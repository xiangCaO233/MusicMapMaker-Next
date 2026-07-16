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
    constexpr PreviewDensityColor low{ 0.31f, 0.76f, 0.38f };
    constexpr PreviewDensityColor medium{ 0.95f, 0.61f, 0.16f };
    constexpr PreviewDensityColor high{ 0.91f, 0.24f, 0.24f };

    const float density             = std::clamp(normalizedDensity, 0.0f, 1.0f);
    const PreviewDensityColor start = density <= 0.5f ? low : medium;
    const PreviewDensityColor end   = density <= 0.5f ? medium : high;
    const float t = density <= 0.5f ? density * 2.0f : (density - 0.5f) * 2.0f;
    return {
        start.r + (end.r - start.r) * t,
        start.g + (end.g - start.g) * t,
        start.b + (end.b - start.b) * t,
    };
}

}  // namespace MMM::Canvas
