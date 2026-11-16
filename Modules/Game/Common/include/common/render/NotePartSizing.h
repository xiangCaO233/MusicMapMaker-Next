#pragma once

#include "common/render/TextureID.h"
#include "config/VisualConfig.h"
#include <cmath>
#include <glm/vec2.hpp>

namespace MMM::Common::Render
{
/// @brief 从实际绘制纹理及折线语境解析尺寸配置身份。
/// @note 折线首节点使用 Head；只有 Node 纹理使用中间节点配置。
/// @warning 几何与拾取热路径：固定分支，不访问 ECS 或资源表。
[[nodiscard]] inline Config::NoteSizePart noteSizePart(TextureID texture,
                                                       bool polyline = false)
{
    using P = Config::NoteSizePart;
    switch ( texture ) {
    case TextureID::HoldBodyVertical:
        return polyline ? P::PolylineBody : P::HoldBody;
    case TextureID::HoldBodyHorizontal:
        return polyline ? P::PolylineBody : P::FlickBody;
    case TextureID::HoldEnd: return P::HoldTail;
    case TextureID::FlickArrowLeft:
    case TextureID::FlickArrowRight: return P::FlickTail;
    case TextureID::Node: return P::PolylineNode;
    default: return P::Head;
    }
}

/// @brief 从旧头部基准几何换算为独立部件基准，消除头部尺寸的传播。
/// @param visual 当前同一帧的视觉配置。
/// @param part 已解析的部件身份，不接受 Count。
/// @return 分别作用于宽高的比例；合法旧配置迁移后精确返回一。
/// @note 只转换布局基准，皮肤倍率、填充和三维补偿仍各自在原阶段处理。
/// @warning 逐部件热路径：固定数组读取及算术，不分配或访问全局单例。
[[nodiscard]] inline glm::vec2 notePartSizeRatio(
    const Config::VisualConfig& visual, Config::NoteSizePart part)
{
    if ( part == Config::NoteSizePart::Head ) return { 1, 1 };
    // 这里的比值只转换旧几何基准，不能作为新的配置值保存。
    // 部件自身存储绝对尺寸，因此 Head 之后如何变化都不会传播。
    const auto scale = visual.notePartScale(part);
    // 非法头部不能作为除数；配置读取和 UI 会规整正常输入。
    // 这里仍保护直接构造配置的工具和测试，防止向顶点传播非有限数。
    const auto ratio = [](float value, float head) {
        // 某一轴损坏时只恢复该轴，不丢弃另一轴的合法尺寸。
        const float safeHead = std::isfinite(head) && head > 0 ? head : .95F;
        return (std::isfinite(value) && value > 0 ? value : .95F) / safeHead;
    };
    // 轨宽与皮肤比例留给调用方，渲染、拾取和框选共用此转换。
    // 按值返回两个数，不借用配置成员地址或延长其生命周期。
    return { ratio(scale.x, visual.noteScaleX),
             ratio(scale.y, visual.noteScaleY) };
}
}  // namespace MMM::Common::Render
