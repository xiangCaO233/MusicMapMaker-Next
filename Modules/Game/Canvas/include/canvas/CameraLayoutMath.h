#pragma once

#include "canvas/PlanarObservationCamera.h"
#include "config/VisualConfig.h"
#include <algorithm>
#include <cmath>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace MMM::Canvas
{
/// @brief 按世界 Z、Y、X 顺序构造相机旋转，参数为弧度。
inline glm::mat4 cameraEulerRotation(float z, float y, float x)
{
    // GLM 四元数的欧拉构造使用同一次序，避免依赖实验扩展头。
    return glm::mat4_cast(glm::quat(glm::vec3(x, y, z)));
}
/// @brief 从正交旋转矩阵恢复 XYZ 欧拉角，输出弧度。
inline void cameraEulerAngles(const glm::mat4& matrix, float& z, float& y,
                              float& x)
{
    // 提取仅接收正交旋转矩阵，不把缩放或透视矩阵当作姿态。
    // 四元数归属 GLM 的稳定扩展接口，避免实验宏污染调用方。
    // 欧拉表示可以变化，但重建后的方向必须保持一致。
    const auto angles = glm::eulerAngles(glm::quat_cast(glm::mat3(matrix)));
    x                 = angles.x;
    y                 = angles.y;
    z                 = angles.z;
}
/// @brief 相机世界变换；列向量依次为右、上、后方和位置。
/// @warning 每帧固定矩阵计算，无分配、锁或场景遍历。
inline glm::mat4 canvasCameraTransform(const Config::VisualConfig& visual)
{
    // 自由姿态使用 Rz Ry Rx 次序，数值输入与旋转环共用世界轴约定。
    if ( visual.canvasCameraPoseEnabled ) {
        // 配置载入和控件输入负责有限值及范围检查。
        // 矩阵构造不再次访问全局配置，也不持有跨线程状态。
        // 调用方在同一 UI 帧内保证这组姿态不被并发写入。
        const auto& r      = visual.canvasCameraRotation;
        auto        result = cameraEulerRotation(
            glm::radians(r[2]), glm::radians(r[1]), glm::radians(r[0]));
        // 位移放在最后一列，表示相机在世界中的位置。
        // 局部轴仅描述方向，不能乘入画布像素尺度。
        // 最终视图矩阵由此刚体变换的逆矩阵得到。
        const auto& p = visual.canvasCameraPosition;
        result[3]     = glm::vec4(p[0], p[1], p[2], 1.0F);
        return result;
    }
    // 旧摄像机旋转的是平面；反向变换相机可保持切换前后的画面一致。
    const auto orbit = glm::rotate(
        glm::mat4(1),
        glm::radians(planarObservationYaw(visual.debugCanvasCameraYawDegrees)),
        glm::vec3(0, 0, 1));
    // 旧俯仰使用退距正切，迁移必须保留这种距离定义。
    // 只把旧角度当作自由欧拉角会改变画面中心和距离。
    // 因此先恢复旧视图，再求逆得到实际位置与方向。
    const glm::vec3 eye =
        glm::vec3(0,
                  -std::tan(glm::radians(planarObservationAngle(
                      visual.debugCanvasCameraAngleDegrees))),
                  1) *
        planarObservationDistance(visual.debugCanvasCameraDistance);
    return glm::inverse(glm::lookAtRH(eye, glm::vec3(0), glm::vec3(0, 1, 0)) *
                        orbit);
}

/// @brief 首次编辑时将旧观察姿态转换为自由位置和旋转，避免相机跳动。
inline void enableCanvasCameraPose(Config::VisualConfig& visual)
{
    // 只在旧格式第一次进入自由编辑时转换。
    // 后续切换模式不得覆盖已经保存的精确数值。
    // 转换结束的启用标志与姿态一起持久化。
    if ( visual.canvasCameraPoseEnabled ) return;
    const auto transform = canvasCameraTransform(visual);
    float      z{}, y{}, x{};
    // 与构造时的乘法次序对称；旧俯仰限制避免转换点出现万向节锁。
    cameraEulerAngles(transform, z, y, x);
    visual.canvasCameraPosition    = { transform[3].x,
                                       transform[3].y,
                                       transform[3].z };
    visual.canvasCameraRotation    = { glm::degrees(x),
                                       glm::degrees(y),
                                       glm::degrees(z) };
    visual.canvasCameraPoseEnabled = true;
}

/// @brief 自由相机投影，画布像素仍位于固定 XY 平面，不改谱面几何。
/// @warning 每帧仅执行固定矩阵运算，与拾取共用结果。
inline glm::mat4 canvasCameraPoseProjection(float width, float height,
                                            float                       offset,
                                            const Config::VisualConfig& visual)
{
    const float w = std::max(width, 1.0F), h = std::max(height, 1.0F);
    glm::mat4   model(1);
    // 画布高对应一个世界单位；屏幕向下的 Y 转换为世界向上的 Y。
    model[0][0] = 1 / h;
    model[1][1] = -1 / h;
    model[3][0] = -w * 0.5F / h;
    model[3][1] = 0.5F - offset / h;
    // 固定视角与旧平面观察原型一致，减少迁移时的视觉变化。
    // 自由位置允许离开旧距离限制，远裁剪面覆盖配置的有效空间。
    // 近裁剪面保持为正，避免相机位于平面上时除零。
    auto projection =
        glm::perspectiveRH_ZO(2 * std::atan(0.5F), w / h, 0.01F, 512.0F);
    projection[1][1] *= -1;
    return projection * glm::inverse(canvasCameraTransform(visual)) * model;
}

/// @brief 世界点映射到操作视图屏幕坐标；近平面后方不产生可点击句柄。
/// @warning 每个固定辅助线端点调用，不查询真实物件或图形资源。
inline bool projectCameraHandle(const glm::mat4& matrix, glm::vec3 point,
                                glm::vec2 origin, glm::vec2 size,
                                glm::vec2& screen)
{
    // 点必须处于相机前方，不能仅检查 XY 是否落在窗口。
    // 输出坐标只在成功时有效。
    // 调用方必须先检查返回值再建立命中区域。
    const auto clip = matrix * glm::vec4(point, 1);
    if ( !std::isfinite(clip.w) || clip.w <= 0.001F ) return false;
    const auto ndc = glm::vec3(clip) / clip.w;
    if ( !std::isfinite(ndc.x) || !std::isfinite(ndc.y) || ndc.z < 0 ||
         ndc.z > 1 )
        return false;
    screen = origin + (glm::vec2(ndc) + 1.0F) * size * 0.5F;
    return true;
}

/// @brief 鼠标到屏幕线段距离，供平移轴和旋转环统一拾取。
inline float cameraHandleDistance(glm::vec2 mouse, glm::vec2 a, glm::vec2 b)
{
    const auto  delta   = b - a;
    const float length2 = glm::dot(delta, delta);
    // 视线方向轴退化成点时也不允许除零。
    // 投影比例限制在线段两端，延长线不参与轴拾取。
    // 退化线段按端点距离处理。
    // 此距离采用逻辑像素，适合固定宽度的鼠标容差。
    const float t =
        length2 > 0.0001F
            ? std::clamp(glm::dot(mouse - a, delta) / length2, 0.0F, 1.0F)
            : 0;
    return glm::length(mouse - (a + delta * t));
}
/// @brief 鼠标绕屏幕中心的有符号转角，供旋转环逐帧累计。
/// @param previous 前一帧相对旋转中心的屏幕向量。
/// @param current 当前帧相对旋转中心的屏幕向量。
/// @return 弧度；穿过中心时返回零以避免角度跳跃。
/// @warning 连续旋转热路径，仅计算固定数量点积和叉积。
inline float cameraRingDelta(glm::vec2 previous, glm::vec2 current)
{
    // 中心附近方向不稳定，不把微小鼠标抖动放大成半周旋转。
    if ( glm::dot(previous, previous) < 16 || glm::dot(current, current) < 16 )
        return 0;
    // atan2 保留正负号，跨越正负半周时仍累计最短的连续角度。
    return std::atan2(previous.x * current.y - previous.y * current.x,
                      glm::dot(previous, current));
}
}  // namespace MMM::Canvas
