#pragma once

#include "common/render/PlanarObservationCamera.h"

/// @brief 兼容画布原有入口，逻辑筛选与最终录制共用底层摄像机数学。
/// @note 本头只保留名称映射，不能在此另写投影或裁剪公式。
namespace MMM::Canvas
{
/// @brief 旧配置的角度与距离约束保持兼容。
using Common::Render::planarObservationAngle;
using Common::Render::planarObservationYaw;
using Common::Render::planarObservationDistance;
using Common::Render::planarObservationEnabled;
/// @brief 连续手势与最终渲染共用同一组约束。
using Common::Render::planarObservationDrag;
using Common::Render::planarObservationZoom;
/// @brief 主场景透视与背景屏幕投影仍为两个独立入口。
using Common::Render::planarObservationProjection;
using Common::Render::canvasBackgroundProjection;
/// @brief 裁剪矩形在最终投影后才转换为屏幕范围。
using Common::Render::planarObservationScissor;
}  // namespace MMM::Canvas
