#pragma once

#include "config/EditorConfig.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/NoteComponent.h"
#include <entt/entt.hpp>

namespace MMM::Logic::System
{

struct Batcher;
struct ScrollCache;

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
     * @param hitFXSystem 打击特效系统 (可选)
     */
    static void generateSnapshot(
        entt::registry& registry, const entt::registry& timelineRegistry,
        RenderSnapshot* snapshot, const std::string& cameraId,
        double currentTime, float viewportWidth, float viewportHeight,
        float judgmentLineY, int32_t trackCount,
        const Config::EditorConfig& config, float mainViewportHeight = 1000.0f,
        class HitFXSystem* hitFXSystem = nullptr);

private:
    // --- 内部逻辑拆分方法 ---

    static void generateTimelineSnapshot(
        RenderSnapshot* snapshot, Batcher& batcher, double currentTime,
        float viewportWidth, float viewportHeight, float judgmentLineY,
        const Config::EditorConfig& config, const ScrollCache* cache);

    static void generatePreviewSnapshot(
        RenderSnapshot* snapshot, Batcher& batcher, double currentTime,
        float viewportWidth, float viewportHeight, float judgmentLineY,
        int32_t trackCount, const Config::EditorConfig& config,
        float mainViewportHeight, float& leftX, float& rightX, float& topY,
        float& bottomY, float& trackAreaW, float& singleTrackW,
        float& renderScaleY);

    static void generateMainCanvasSnapshot(
        entt::registry& registry, const entt::registry& timelineRegistry,
        RenderSnapshot* snapshot, Batcher& batcher, double currentTime,
        float viewportWidth, float viewportHeight, float judgmentLineY,
        int32_t trackCount, const Config::EditorConfig& config,
        const ScrollCache* cache, float& leftX, float& rightX, float& topY,
        float& bottomY, float& trackAreaW, float& singleTrackW,
        float renderScaleY);

    static void renderTrackLayout(Batcher& batcher, float viewportWidth,
                                  float viewportHeight, float judgmentLineY,
                                  int32_t                     trackCount,
                                  const Config::EditorConfig& config,
                                  const entt::registry&       timelineRegistry,
                                  double currentTime, const ScrollCache* cache,
                                  float& leftX, float& rightX, float& topY,
                                  float& bottomY, float& trackAreaW,
                                  float& singleTrackW, float renderScaleY);

    static void drawTrackBackground(Batcher& batcher, int32_t trackCount,
                                    float leftX, float topY, float bottomY,
                                    float singleTrackW);

    static void drawJudgmentArea(Batcher& batcher, int32_t trackCount,
                                 float leftX, float judgmentLineY,
                                 float singleTrackW, float trackAreaW,
                                 const Config::EditorConfig& config);

    static void drawBeatLines(Batcher& batcher, float viewportHeight,
                              float                       judgmentLineY,
                              const Config::EditorConfig& config,
                              const entt::registry&       timelineRegistry,
                              double currentTime, const ScrollCache* cache,
                              float leftX, float topY, float bottomY,
                              float trackAreaW, float renderScaleY);

    static void renderNotes(entt::registry& registry, RenderSnapshot* snapshot,
                            const std::string& cameraId, double currentTime,
                            float judgmentLineY, int32_t trackCount,
                            const Config::EditorConfig& config,
                            Batcher& batcher, float leftX, float rightX,
                            float topY, float bottomY, float singleTrackW,
                            float renderScaleY);

    struct NoteRenderContext {
        float              noteW;
        float              noteH;
        float              baseAspect;
        glm::vec4          colorTap;
        glm::vec4          colorHold;
        glm::vec4          colorNode;
        glm::vec4          colorArrow;
        const ScrollCache* cache;
        double             currentAbsY;
    };

    static NoteRenderContext prepareNoteRenderContext(
        entt::registry& registry, RenderSnapshot* snapshot, double currentTime,
        float singleTrackW, const Config::EditorConfig& config);

    static void generateNoteHitboxes(entt::registry&          registry,
                                     RenderSnapshot*          snapshot,
                                     const NoteRenderContext& ctx,
                                     float judgmentLineY, float leftX,
                                     float topY, float bottomY,
                                     float singleTrackW, float renderScaleY,
                                     const Config::EditorConfig& config);

    static void renderNoteBaseLayer(entt::registry&             registry,
                                    RenderSnapshot*             snapshot,
                                    const NoteRenderContext&    ctx,
                                    const Config::EditorConfig& config,
                                    Batcher& batcher, float currentTime,
                                    float judgmentLineY, float leftX,
                                    float rightX, float topY, float bottomY,
                                    float singleTrackW, float renderScaleY);

    static void renderNoteGlowLayer(
        entt::registry& registry, RenderSnapshot* snapshot,
        const NoteRenderContext& ctx, const Config::EditorConfig& config,
        float currentTime, float judgmentLineY, float leftX, float rightX,
        float topY, float bottomY, float singleTrackW, float renderScaleY);

    static void renderTap(Batcher& batcher, const NoteComponent& note,
                          const Config::EditorConfig& config, float x, float y,
                          float w, float h, float aspect, glm::vec4 color);

    static void renderHold(Batcher& batcher, const NoteComponent& note,
                           const Config::EditorConfig& config,
                           RenderSnapshot* snapshot, float x, float y, float w,
                           float h, float visualH, float singleTrackW,
                           glm::vec4 color,
                           HoverPart glowPart = HoverPart::None);

    static void renderFlick(Batcher& batcher, const NoteComponent& note,
                            const Config::EditorConfig& config,
                            RenderSnapshot* snapshot, float x, float y, float w,
                            float h, float singleTrackW, glm::vec4 color,
                            glm::vec4 arrowColor,
                            HoverPart glowPart = HoverPart::None);

    static void renderPolyline(
        const ScrollCache* cache, Batcher& batcher, const NoteComponent& note,
        const Config::EditorConfig& config, RenderSnapshot* snapshot,
        double currentAbsY, float judgmentLineY, float leftX, float rightX,
        float topY, float bottomY, float singleTrackW, float renderScaleY,
        glm::vec4 colorHold, glm::vec4 colorNode, glm::vec4 colorArrow,
        entt::entity entity = entt::null, bool generateHitboxes = false,
        HoverPart glowPart = HoverPart::None, int glowSubIndex = -1);

    static void debugRenderHitboxes(Batcher& batcher, RenderSnapshot* snapshot);

    static void drawPolylineBody(Batcher& batcher, const NoteComponent& note,
                                 const ScrollCache* cache,
                                 RenderSnapshot* snapshot, float judgmentLineY,
                                 float leftX, float singleTrackW,
                                 float renderScaleY, double currentAbsY,
                                 float noteW, float noteH, glm::vec4 colorHold,
                                 entt::entity entity, bool generateHitboxes,
                                 HoverPart glowPart, int glowSubIndex);

    static void drawPolylineNodes(Batcher& batcher, const NoteComponent& note,
                                  const ScrollCache* cache,
                                  RenderSnapshot* snapshot, float judgmentLineY,
                                  float leftX, float singleTrackW,
                                  float renderScaleY, double currentAbsY,
                                  float noteW, float noteH, glm::vec4 colorNode,
                                  const Config::EditorConfig& config,
                                  entt::entity entity, bool generateHitboxes,
                                  HoverPart glowPart, int glowSubIndex);

    static void drawPolylineHead(Batcher& batcher, const NoteComponent& note,
                                 const ScrollCache* cache,
                                 RenderSnapshot* snapshot, float judgmentLineY,
                                 float leftX, float singleTrackW,
                                 float renderScaleY, double currentAbsY,
                                 float noteW, float noteH, glm::vec4 colorHold,
                                 const Config::EditorConfig& config,
                                 entt::entity entity, bool generateHitboxes,
                                 HoverPart glowPart, int glowSubIndex);

    static void drawPolylineDecoration(
        Batcher& batcher, const NoteComponent& note, const ScrollCache* cache,
        RenderSnapshot* snapshot, float judgmentLineY, float leftX,
        float singleTrackW, float renderScaleY, double currentAbsY, float noteW,
        float noteH, glm::vec4 colorHold, glm::vec4 colorArrow,
        const Config::EditorConfig& config, entt::entity entity,
        bool generateHitboxes, HoverPart glowPart, int glowSubIndex);

    static void renderMarqueeBox(Batcher& batcher,
                                 const RenderSnapshot::MarqueeBoxSnapshot& box,
                                 float judgmentLineY, float leftX,
                                 float singleTrackW, float renderScaleY,
                                 const ScrollCache* cache, double renderTime,
                                 float viewportWidth, float viewportHeight);

    static void renderBrushPreview(RenderSnapshot*             snapshot,
                                   const NoteRenderContext&    ctx,
                                   const Config::EditorConfig& config,
                                   Batcher& batcher, float judgmentLineY,
                                   float leftX, float singleTrackW,
                                   float renderScaleY);
};

}  // namespace MMM::Logic::System
