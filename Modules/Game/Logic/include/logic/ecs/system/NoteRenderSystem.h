#pragma once

#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorConfig.h"
#include <entt/entt.hpp>

namespace MMM::Logic::System
{

struct Batcher;

/**
 * @brief 音符渲染快照生成系统
 *
 * 将 ECS 逻辑坐标转换并剔除到视口快照 (RenderSnapshot) 中供 UI 线程渲染。
 */
class NoteRenderSystem
{
public:
    /**
     * @brief 生成快照
     *
     * @param registry 音符注册表
     * @param timelineRegistry 时间线注册表 (用于坐标积分映射)
     * @param snapshot 目标渲染快照缓冲区
     * @param currentTime 当前播放时间
     * @param viewportWidth 视口总宽度
     * @param viewportHeight 视口总高度
     * @param judgmentLineY 判定线位置 (视口空间)
     * @param trackCount 轨道数量
     * @param config 编辑器配置
     */
    static void generateSnapshot(entt::registry&       registry,
                                 const entt::registry& timelineRegistry,
                                 RenderSnapshot* snapshot, double currentTime,
                                 float viewportWidth, float viewportHeight,
                                 float judgmentLineY, int32_t trackCount,
                                 const EditorConfig& config);

private:
    static void renderTrackLayout(Batcher& batcher, float viewportWidth,
                                  float viewportHeight, float judgmentLineY,
                                  int32_t             trackCount,
                                  const EditorConfig& config, float& leftX,
                                  float& rightX, float& topY, float& bottomY,
                                  float& trackAreaW, float& singleTrackW);

    static void renderNotes(entt::registry& registry, RenderSnapshot* snapshot,
                            double currentTime, float judgmentLineY,
                            int32_t trackCount, const EditorConfig& config,
                            Batcher& batcher, float leftX, float rightX,
                            float topY, float bottomY, float singleTrackW);
};

}  // namespace MMM::Logic::System
