#pragma once

#include "audio/BackgroundSpectrum.h"
#include "config/visual/BackgroundConfig.h"
#include "config/visual/CanvasComponentConfig.h"

namespace MMM::Logic::System
{

struct Batcher;

/// @brief 在背景资源上方绘制从画布中心向两侧展开的立体声柱状频谱。
class BackgroundSpectrumRenderSystem
{
public:
    /// @brief 绘制一帧背景频谱。
    /// @param batcher 当前渲染快照的批处理器。
    /// @param viewportWidth 画布视口宽度。
    /// @param viewportHeight 画布视口高度。
    /// @param config 背景频谱视觉配置。
    /// @param placement 背景频谱在自定义布局中的位置和整体缩放。
    /// @param levels 当前左右声道归一化频段。
    /// @warning 主画布快照生成热路径：启用频谱时每次 update 执行，只允许
    /// 固定频段遍历和几何写入，禁止加入资源访问、动态分配或阻塞操作。
    static void render(Batcher& batcher, float viewportWidth,
                       float                                   viewportHeight,
                       const Config::BackgroundSpectrumConfig& config,
                       const Config::CanvasComponentPlacement& placement,
                       const Audio::BackgroundSpectrumLevels&  levels);
};

}  // namespace MMM::Logic::System
