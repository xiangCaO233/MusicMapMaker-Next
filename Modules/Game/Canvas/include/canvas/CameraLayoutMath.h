#pragma once

#include "canvas/PlanarObservationCamera.h"
#include "common/render/CanvasCameraMath.h"

/// @brief 兼容画布原有入口，逻辑筛选与最终录制共用底层摄像机数学。
/// @note 本头只保留名称映射，不能在此另写投影或裁剪公式。
namespace MMM::Canvas
{
/// @brief 欧拉旋转构造与反解保持同一次序。
using Common::Render::cameraEulerRotation;
using Common::Render::cameraEulerAngles;
/// @brief 配置到视图矩阵只保留一份实现，供渲染与逻辑共同使用。
using Common::Render::canvasCameraTransform;
using Common::Render::enableCanvasCameraPose;
using Common::Render::canvasCameraPoseProjection;
/// @brief 操作句柄仍使用既有画布命名空间，避免上层接口迁移。
using Common::Render::projectCameraHandle;
using Common::Render::cameraHandleDistance;
/// @brief 连续旋转手势沿用公共角度约定。
using Common::Render::cameraRingDelta;
}  // namespace MMM::Canvas
