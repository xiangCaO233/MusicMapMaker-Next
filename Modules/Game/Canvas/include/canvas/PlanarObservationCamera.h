#pragma once

#include "common/render/CanvasRenderTypes.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace MMM::Canvas
{
/// @brief 将实验观察角限制到可安全观察平面的范围。
/// @param degrees 离开正视方向的角度，单位度。
/// @return 非有限输入回退零，其余限制到零至四十五度。
inline float planarObservationAngle(float degrees)
{
    // 零角同时是关闭实验模式的哨兵，损坏配置不能意外启用观察模式。
    return std::isfinite(degrees) ? std::clamp(degrees, 0.0F, 45.0F) : 0.0F;
}

/// @brief 为二维像素画布构造高度固定为一的平面观察摄像机。
/// @param width 逻辑画布宽度，不能使用物理 framebuffer 宽度。
/// @param height 逻辑画布高度，同时定义一个世界单位的长度。
/// @param yOffset 旧二维投影的播放补偿，单位逻辑像素。
/// @param degrees 离开俯视方向的倾角；零角保留原二维矩阵。
/// @return 像素坐标直接进入现有 Shader 的复合变换矩阵。
/// @warning 每帧录制调用一次：仅作固定规模矩阵运算，禁止分配与锁等待。
/// @note 平面仍是二维谱面，摄像机不向物件写入 Z 坐标。
/// @note 倾角绕横轴变化，摄像机高度固定，水平退距随倾角变化。
/// @note 输入是逻辑像素，与窗口 DPI 和发光降采样无关。
/// @note 各几何层必须共用本帧返回值，不能分别读取可变设置。
/// @note 两种模式的深度矩阵不同，但零角附近 XY 尺寸连续。
/// @note 近平面和远平面仅覆盖单位平面实验，不作为通用场景参数。
/// @note 逻辑宽高来自离屏视口，最小尺寸保护用于避免除零。
inline glm::mat4 planarObservationProjection(float width, float height,
                                             float yOffset, float degrees)
{
    const float safeWidth  = std::max(1.0F, width);
    const float safeHeight = std::max(1.0F, height);
    const float angle      = planarObservationAngle(degrees);
    // 零角精确保留旧正交路径及其 Z 约定，不改变既有二维皮肤效果。
    if ( angle == 0.0F ) {
        return glm::ortho(
            0.0F, safeWidth, -yOffset, safeHeight - yOffset, -1.0F, 1.0F);
    }
    // 场景平面以画布中心为原点，高度一单位；世界 Y 向上，像素 Y 向下。
    // 播放补偿必须在进入场景前应用，不能在透视后再平移屏幕像素。
    glm::mat4 model(1.0F);
    model[0][0] = 1.0F / safeHeight;
    model[1][1] = -1.0F / safeHeight;
    model[3][0] = -safeWidth * 0.5F / safeHeight;
    model[3][1] = 0.5F - yOffset / safeHeight;
    // 倾斜时始终看向平面中心；沿地面后退而不是降低摄像机的 Z 高度。
    const float     radians = angle * 0.017453292519943295F;
    const glm::vec3 eye(0.0F, -std::tan(radians), 1.0F);
    const auto      view =
        glm::lookAtRH(eye, glm::vec3(0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    // 半高为 0.5、正视距离为一，视角据此固定，零角附近与二维尺寸连续。
    // 显式使用 Vulkan 的零至一深度范围，不依赖全局 GLM 编译宏。
    auto projection = glm::perspectiveRH_ZO(
        2.0F * std::atan(0.5F), safeWidth / safeHeight, 0.01F, 4.0F);
    // Vulkan 正高度 viewport 的 Y 方向与传统透视相反，仅在投影处翻转。
    projection[1][1] *= -1.0F;
    return projection * view * model;
}

/// @brief 将原二维裁剪区投影为保守屏幕包围矩形。
/// @param scissor 原画布逻辑像素裁剪区域。
/// @param matrix 本次录制使用的同一代观察矩阵。
/// @param width 逻辑视口宽度。
/// @param height 逻辑视口高度。
/// @return 限制在视口内的逻辑像素包围框，随后仍需转换物理像素。
/// @note 实验版只采用保守矩形，不宣称实现了透视梯形的精确模板裁剪。
/// @note 二维模式直接沿用整数裁剪，不经过本函数。
/// @warning 每批次固定投影四角，禁止遍历场景、重新构造矩阵或动态分配。
inline Common::Render::CanvasScissor planarObservationScissor(
    Common::Render::CanvasScissor scissor, const glm::mat4& matrix, float width,
    float height)
{
    // 空裁剪区不能因透视向外取整变成可见的单像素区域。
    if ( scissor.width == 0 || scissor.height == 0 ) return { 0, 0, 0, 0 };
    const float left   = static_cast<float>(scissor.x);
    const float top    = static_cast<float>(scissor.y);
    const float right  = left + static_cast<float>(scissor.width);
    const float bottom = top + static_cast<float>(scissor.height);
    float       minX = width, minY = height, maxX = 0.0F, maxY = 0.0F;
    // 原矩形的四角在平面上；透视后矩形变成梯形，外包框不会截掉其几何。
    for ( const auto& point :
          std::array<glm::vec4, 4>{ glm::vec4(left, top, 0.0F, 1.0F),
                                    glm::vec4(right, top, 0.0F, 1.0F),
                                    glm::vec4(left, bottom, 0.0F, 1.0F),
                                    glm::vec4(right, bottom, 0.0F, 1.0F) } ) {
        const auto clip = matrix * point;
        // 极端外部裁剪区穿越近裁剪面时退回完整视口，优先避免错误剔除。
        if ( !std::isfinite(clip.w) || clip.w <= 0.0001F ) {
            return { 0,
                     0,
                     static_cast<std::uint32_t>(width),
                     static_cast<std::uint32_t>(height) };
        }
        const float x = (clip.x / clip.w + 1.0F) * width * 0.5F;
        const float y = (clip.y / clip.w + 1.0F) * height * 0.5F;
        minX          = std::min(minX, x);
        minY          = std::min(minY, y);
        maxX          = std::max(maxX, x);
        maxY          = std::max(maxY, y);
    }
    // 向外取整保留边缘像素；逻辑裁剪与 DPI 缩放仍是两个独立步骤。
    const auto x0 =
        static_cast<std::int32_t>(std::clamp(std::floor(minX), 0.0F, width));
    const auto y0 =
        static_cast<std::int32_t>(std::clamp(std::floor(minY), 0.0F, height));
    const auto x1 =
        static_cast<std::int32_t>(std::clamp(std::ceil(maxX), 0.0F, width));
    const auto y1 =
        static_cast<std::int32_t>(std::clamp(std::ceil(maxY), 0.0F, height));
    return { x0,
             y0,
             static_cast<std::uint32_t>(std::max(0, x1 - x0)),
             static_cast<std::uint32_t>(std::max(0, y1 - y0)) };
}
}  // namespace MMM::Canvas
