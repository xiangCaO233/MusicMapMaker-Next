#pragma once

#include "config/EditorConfig.h"
#include "logic/BeatmapSyncBuffer.h"

namespace MMM::Logic::System
{

struct Batcher;

/**
 * @brief 背景渲染系统
 * 负责渲染谱面背景图片/视频，并处理 AspectRatio 适配。
 */
class BackgroundRenderSystem
{
public:
    /// @brief
    /// 为已加载谱面绘制背景，缺少背景资源时绘制支持暗化与透明混合的固定覆盖层。
    /// @param batcher 当前渲染快照的批处理器。
    /// @param viewportWidth 画布视口宽度。
    /// @param viewportHeight 画布视口高度。
    /// @param config 编辑器视觉配置。
    /// @param snapshot 当前渲染快照。
    /// @warning 主画布快照生成热路径：每次 update
    /// 执行，禁止加入资源加载、文件系统访问或阻塞等待。
    static void render(Batcher& batcher, float viewportWidth,
                       float viewportHeight, const Config::EditorConfig& config,
                       const RenderSnapshot* snapshot);
};

}  // namespace MMM::Logic::System
