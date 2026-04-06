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
    static void render(Batcher& batcher, float viewportWidth,
                       float viewportHeight, const Config::EditorConfig& config,
                       const RenderSnapshot* snapshot);
};

}  // namespace MMM::Logic::System
