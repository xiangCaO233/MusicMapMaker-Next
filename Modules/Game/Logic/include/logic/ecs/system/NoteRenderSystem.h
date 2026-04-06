#pragma once

#include "config/EditorConfig.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/NoteComponent.h"
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
     * @param cameraId 视口 ID
     * @param currentTime 当前播放时间
     * @param viewportWidth 视口总宽度
     * @param viewportHeight 视口总高度
     * @param judgmentLineY 判定线位置 (视口空间)
     * @param trackCount 轨道数量
     * @param config 编辑器配置
     * @param mainViewportHeight 主画布视口高度 (用于预览区缩放对齐)
     */
    static void generateSnapshot(
        entt::registry& registry, const entt::registry& timelineRegistry,
        RenderSnapshot* snapshot, const std::string& cameraId,
        double currentTime, float viewportWidth, float viewportHeight,
        float judgmentLineY, int32_t trackCount,
        const Config::EditorConfig& config, float mainViewportHeight = 1000.0f);

private:
    static void renderTrackLayout(Batcher& batcher, float viewportWidth,
                                  float viewportHeight, float judgmentLineY,
                                  int32_t                     trackCount,
                                  const Config::EditorConfig& config,
                                  float& leftX, float& rightX, float& topY,
                                  float& bottomY, float& trackAreaW,
                                  float& singleTrackW);

    static void renderNotes(entt::registry& registry, RenderSnapshot* snapshot,
                            const std::string& cameraId, double currentTime,
                            float judgmentLineY, int32_t trackCount,
                            const Config::EditorConfig& config,
                            Batcher& batcher, float leftX, float rightX,
                            float topY, float bottomY, float singleTrackW,
                            float renderScaleY);

    static void renderTap(Batcher& batcher, const NoteComponent& note,
                          const Config::EditorConfig& config, float x, float y,
                          float w, float h, float aspect, glm::vec4 color);

    static void renderHold(Batcher& batcher, const NoteComponent& note,
                           const Config::EditorConfig& config,
                           RenderSnapshot* snapshot, float x, float y, float w,
                           float h, float visualH, float singleTrackW,
                           glm::vec4 color, glm::vec4 hoverTint);

    static void renderFlick(Batcher& batcher, const NoteComponent& note,
                            const Config::EditorConfig& config,
                            RenderSnapshot* snapshot, float x, float y, float w,
                            float h, float singleTrackW, glm::vec4 color,
                            glm::vec4 arrowColor, glm::vec4 hoverTint);

    static void renderPolyline(entt::registry& registry, Batcher& batcher,
                               const NoteComponent&        note,
                               const Config::EditorConfig& config,
                               RenderSnapshot* snapshot, double currentTime,
                               float judgmentLineY, float leftX, float rightX,
                               float topY, float bottomY, float singleTrackW,
                               float renderScaleY, glm::vec4 colorHold,
                               glm::vec4 colorNode, glm::vec4 colorArrow,
                               glm::vec4 hoverTint);
};

}  // namespace MMM::Logic::System
