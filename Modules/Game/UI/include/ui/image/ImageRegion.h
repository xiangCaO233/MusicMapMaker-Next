#pragma once

#include <vector>

namespace MMM::UI
{
/// @brief 图片原始坐标中的半开矩形，原点在左上角。
/// @details 坐标与查看器显示比例、窗口 DPI 和纹理像素数独立。
struct ImageRegion {
    /// @brief 左上边界。
    int m_x{}, m_y{};
    /// @brief 矩形覆盖的原始像素数。
    int m_width{}, m_height{};
};

/// @brief 可交给纹理上传阶段的连续 RGBA8 像素。
/// @details 像素可能是整图缩略图或放大的可见区，不承载原图中的偏移。
struct ImagePixels {
    /// @brief 实际纹理宽高。
    int m_width{}, m_height{};
    /// @brief 连续排列的像素，行间无填充。
    std::vector<unsigned char> m_pixels;
};
}  // namespace MMM::UI
