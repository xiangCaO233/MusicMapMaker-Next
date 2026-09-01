#pragma once

#include "logic/session/CanvasCamera.h"

#include <cmath>
#include <cstdint>

namespace MMM::Logic::System
{

/// @brief 单个音符端点在统一画布轨道中的实际渲染几何。
struct NoteLaneGeometry {
    /// @brief 端点所属轨道的左边界。
    float leftX{ 0.0F };

    /// @brief 端点所属区域的单轨宽度。
    float width{ 0.0F };

    /// @brief 按端点轨宽缩放后的基础音符宽度。
    float noteW{ 0.0F };

    /// @brief 按端点轨宽缩放后的基础音符高度。
    float noteH{ 0.0F };

    /// @brief 返回端点所属轨道的横向中心。
    /// @return 轨道中心逻辑坐标。
    [[nodiscard]] float centerX() const { return leftX + width * 0.5F; }
};

/// @brief 按绝对轨道解析端点的真实横向几何与音符尺寸。
/// @param absoluteTrack 草稿、玩家与 BGM 共用的绝对轨道索引。
/// @param laneProjection 主画布统一轨道投影；兼容视口可为空。
/// @param fallbackLeftX 兼容连续轨道区左边界。
/// @param fallbackWidth 兼容连续轨道的单轨宽度。
/// @param fallbackNoteW 兼容轨宽下的基础音符宽度。
/// @param fallbackNoteH 兼容轨宽下的基础音符高度。
/// @return 端点所在实际轨道的边界、宽度及等比例音符尺寸。
/// @warning 渲染热路径会为每个 Flick/Polyline 端点调用；仅允许常量计算。
[[nodiscard]] inline NoteLaneGeometry resolveNoteLaneGeometry(
    std::int32_t absoluteTrack, const CanvasLaneProjection* laneProjection,
    float fallbackLeftX, float fallbackWidth, float fallbackNoteW = 0.0F,
    float fallbackNoteH = 0.0F)
{
    NoteLaneGeometry result{
        .leftX =
            fallbackLeftX + static_cast<float>(absoluteTrack) * fallbackWidth,
        .width = fallbackWidth,
        .noteW = fallbackNoteW,
        .noteH = fallbackNoteH,
    };

    // 主画布端点必须逐轨解析，不能按根节点所在区域的宽度连续外推。
    if ( laneProjection ) {
        const auto address = CanvasLaneAddress::fromAbsoluteTrack(
            absoluteTrack,
            laneProjection->playerLaneCount,
            laneProjection->draftLaneCount);
        if ( const auto bounds = laneProjection->bounds(address) ) {
            result.leftX = bounds->leftX;
            result.width = bounds->rightX - bounds->leftX;
        }
    }

    // 音符尺寸随端点自身轨宽缩放；未请求尺寸时保持零值。
    if ( std::isfinite(fallbackWidth) && fallbackWidth > 0.0F &&
         std::isfinite(result.width) && result.width > 0.0F ) {
        const float scale = result.width / fallbackWidth;
        result.noteW      = fallbackNoteW * scale;
        result.noteH      = fallbackNoteH * scale;
    }
    return result;
}

}  // namespace MMM::Logic::System
