#pragma once

#include "common/render/CanvasRenderTypes.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace MMM::Canvas
{
/// @brief 将实验观察角限制到可安全观察平面的范围。
/// @param degrees 离开正视方向的角度，单位度。
/// @return 非有限输入回退零，其余限制到零至七十五度。
inline float planarObservationAngle(float degrees)
{
    // 零角同时是关闭实验模式的哨兵，损坏配置不能意外启用观察模式。
    return std::isfinite(degrees) ? std::clamp(degrees, 0.0F, 75.0F) : 0.0F;
}

/// @brief 将水平旋转约束到一周内，损坏值恢复默认朝向。
inline float planarObservationYaw(float degrees)
{
    return std::isfinite(degrees) ? std::clamp(degrees, -180.0F, 180.0F) : 0.0F;
}

/// @brief 限制摄像机距离倍率，非有限输入恢复原型距离。
inline float planarObservationDistance(float distance)
{
    return std::isfinite(distance) ? std::clamp(distance, 0.5F, 4.0F) : 1.0F;
}

/// @brief 判断观察参数是否偏离原二维视图，绘制与输入共用此判定。
/// @warning 每帧调用，仅作有界数值检查，不访问配置或会话锁。
inline bool planarObservationEnabled(float degrees, float yaw = 0.0F,
                                     float distance = 1.0F)
{
    return planarObservationAngle(degrees) != 0.0F ||
           planarObservationYaw(yaw) != 0.0F ||
           planarObservationDistance(distance) != 1.0F;
}

/// @brief 将相邻帧右键拖动转换为俯仰和水平旋转，结果依次存于 x/y。
/// @param degrees 当前俯仰角，单位度。
/// @param yaw 当前水平角，单位度。
/// @param dx 本帧横向逻辑像素增量。
/// @param dy 本帧纵向逻辑像素增量。
/// @return 限制后的俯仰角及绕周连续的水平角。
/// @warning 连续手势每帧调用，不分配、不等待，也不累计固定时间窗口。
inline glm::vec2 planarObservationDrag(float degrees, float yaw, float dx,
                                       float dy)
{
    // 拒绝平台失焦时的无效增量；合法的历史角度仍需规范化。
    const float pitch   = planarObservationAngle(degrees);
    const float heading = planarObservationYaw(yaw);
    if ( !std::isfinite(dx) || !std::isfinite(dy) ) return { pitch, heading };
    // 每四个逻辑像素对应一度，跨越正负一百八十度时保持连续朝向。
    return { planarObservationAngle(pitch + dy * 0.25F),
             std::remainder(heading + dx * 0.25F, 360.0F) };
}

/// @brief 以乘法比例改变观察距离，正滚轮表示拉近，负滚轮表示拉远。
/// @warning 每次滚轮输入仅执行固定规模运算，不读取或修改谱面缩放。
inline float planarObservationZoom(float distance, float wheel)
{
    const float current = planarObservationDistance(distance);
    if ( !std::isfinite(wheel) ) return current;
    // 限制单次外部增量，既避免指数溢出，也不延迟连续滚轮的本地反馈。
    return planarObservationDistance(
        current * std::exp2(-std::clamp(wheel, -16.0F, 16.0F) / 8.0F));
}

/// @brief 为二维像素画布构造可旋转、可调整距离的平面观察摄像机。
/// @param width 逻辑画布宽度，不能使用物理 framebuffer 宽度。
/// @param height 逻辑画布高度，同时定义一个世界单位的长度。
/// @param yOffset 旧二维投影的播放补偿，单位逻辑像素。
/// @param degrees 离开俯视方向的倾角。
/// @param yaw 绕画布中心的水平旋转角，单位度。
/// @param distance 沿观察方向的距离倍率，一表示保留原型距离。
/// @return 像素坐标直接进入现有 Shader 的复合变换矩阵。
/// @warning 每帧录制调用一次：仅作固定规模矩阵运算，禁止分配与锁等待。
/// @note 平面仍是二维谱面，摄像机不向物件写入 Z 坐标。
/// @note 先绕平面中心旋转，再按倾角观察，距离改变不会移动观察中心。
/// @note 输入是逻辑像素，与窗口 DPI 和发光降采样无关。
/// @note 各轨道几何层必须共用本帧返回值，固定背景层使用独立屏幕投影。
/// @note 两种模式的深度矩阵不同，但零角附近 XY 尺寸连续。
/// @note 近平面和远平面仅覆盖单位平面实验，不作为通用场景参数。
/// @note 逻辑宽高来自离屏视口，最小尺寸保护用于避免除零。
inline glm::mat4 planarObservationProjection(float width, float height,
                                             float yOffset, float degrees,
                                             float yaw      = 0.0F,
                                             float distance = 1.0F)
{
    const float safeWidth  = std::max(1.0F, width);
    const float safeHeight = std::max(1.0F, height);
    const float angle      = planarObservationAngle(degrees);
    const float heading    = planarObservationYaw(yaw);
    const float range      = planarObservationDistance(distance);
    // 零角精确保留旧正交路径及其 Z 约定，不改变既有二维皮肤效果。
    if ( !planarObservationEnabled(angle, heading, range) ) {
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
    // 水平角绕平面中心旋转，保持旋转前后的时间轴与轨道坐标不变。
    // 摄像机 up 随水平角一起变化，零俯仰时也能旋转而不会退化为奇异视线。
    const auto orbit = glm::rotate(glm::mat4(1.0F),
                                   heading * 0.017453292519943295F,
                                   glm::vec3(0.0F, 0.0F, 1.0F));
    // 倾斜时始终看向平面中心；距离倍率同时作用于高度和水平退距。
    const float     radians = angle * 0.017453292519943295F;
    const glm::vec3 eye     = glm::vec3(0.0F, -std::tan(radians), 1.0F) * range;
    const auto      view =
        glm::lookAtRH(eye, glm::vec3(0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    // 半高为 0.5、正视距离为一，视角据此固定，零角附近与二维尺寸连续。
    // 显式使用 Vulkan 的零至一深度范围，不依赖全局 GLM 编译宏。
    // 远平面覆盖观察距离和视口对角线，拉远后不能把整张轨道裁掉。
    // 下界保持旧版四单位覆盖范围，不对每帧场景做几何扫描。
    const float diagonal = std::hypot(safeWidth / safeHeight, 1.0F);
    const float farPlane = std::max(4.0F, std::hypot(eye.y, eye.z) + diagonal);
    auto        projection = glm::perspectiveRH_ZO(
        2.0F * std::atan(0.5F), safeWidth / safeHeight, 0.01F, farPlane);
    // Vulkan 正高度 viewport 的 Y 方向与传统透视相反，仅在投影处翻转。
    projection[1][1] *= -1.0F;
    return projection * view * orbit * model;
}

/// @brief 为固定背景层生成无播放补偿的屏幕投影。
/// @param width 逻辑视口宽度，独立于背景资源尺寸。
/// @param height 逻辑视口高度，独立于轨道布局范围。
/// @return 将完整背景映射到视口的正交矩阵。
/// @warning 每帧录制调用一次，仅计算固定大小矩阵，不读取摄像机配置。
inline glm::mat4 canvasBackgroundProjection(float width, float height)
{
    // 背景覆盖整个视口；播放亚帧偏移只能移动轨道，不能使背景边缘露底。
    return glm::ortho(
        0.0F, std::max(1.0F, width), 0.0F, std::max(1.0F, height), -1.0F, 1.0F);
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
