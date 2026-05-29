#pragma once

#include "config/EditorConfig.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/NoteComponent.h"
#include <entt/entt.hpp>
#include <vector>

namespace MMM::Logic
{
struct TimelineComponent;
}

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
     * @warning
     * 热路径：逻辑线程为每个活动视口生成渲染快照时执行；禁止文件系统访问、完整
     * entt 遍历、完整排序、try/catch 和 shared_ptr 所有权复制。
     */
    static void generateSnapshot(
        entt::registry& registry, const entt::registry& timelineRegistry,
        const std::vector<const TimelineComponent*>& bpmEvents,
        RenderSnapshot* snapshot, const std::string& cameraId,
        double currentTime, float viewportWidth, float viewportHeight,
        float judgmentLineY, int32_t trackCount,
        const Config::EditorConfig& config, float mainViewportHeight = 1000.0f,
        class HitFXSystem* hitFXSystem = nullptr);

private:
    // --- 内部逻辑拆分方法 ---

    /// @warning 热路径：Timeline
    /// 视口快照生成时执行；只允许使用已缓存的时间线数据。
    static void generateTimelineSnapshot(
        RenderSnapshot*                              snapshot,
        const std::vector<const TimelineComponent*>& bpmEvents,
        Batcher& batcher, double currentTime, float viewportWidth,
        float viewportHeight, float judgmentLineY,
        const Config::EditorConfig& config, const ScrollCache* cache);

    /// @warning 热路径：Preview
    /// 视口每次快照生成时执行；禁止进行资源加载或阻塞等待。
    static void generatePreviewSnapshot(
        RenderSnapshot* snapshot, Batcher& batcher, double currentTime,
        float viewportWidth, float viewportHeight, float judgmentLineY,
        int32_t trackCount, const Config::EditorConfig& config,
        float mainViewportHeight, float& leftX, float& rightX, float& topY,
        float& bottomY, float& trackAreaW, float& singleTrackW,
        float& renderScaleY);

    /// @warning 热路径：主画布每次快照生成时执行；禁止文件系统访问和完整
    /// registry 排序。
    static void generateMainCanvasSnapshot(
        entt::registry& registry, const entt::registry& timelineRegistry,
        RenderSnapshot* snapshot, Batcher& batcher, double currentTime,
        float viewportWidth, float viewportHeight, float judgmentLineY,
        int32_t trackCount, const Config::EditorConfig& config,
        const ScrollCache* cache, float& leftX, float& rightX, float& topY,
        float& bottomY, float& trackAreaW, float& singleTrackW,
        float renderScaleY);

    /// @warning 热路径：每次画布快照生成基础轨道布局时执行；仅允许
    /// O(trackCount) 绘制。
    static void renderTrackLayout(Batcher& batcher, float viewportWidth,
                                  float viewportHeight, float judgmentLineY,
                                  int32_t                     trackCount,
                                  const Config::EditorConfig& config,
                                  const entt::registry&       timelineRegistry,
                                  double currentTime, const ScrollCache* cache,
                                  float& leftX, float& rightX, float& topY,
                                  float& bottomY, float& trackAreaW,
                                  float& singleTrackW, float renderScaleY);

    /// @warning 热路径：轨道布局生成时执行；循环次数必须受 trackCount
    /// 和可见高度限制。
    static void drawTrackBackground(Batcher& batcher, int32_t trackCount,
                                    float leftX, float topY, float bottomY,
                                    float singleTrackW);

    /// @warning 热路径：轨道布局生成时执行；禁止读取磁盘或访问全量 ECS。
    static void drawJudgmentArea(Batcher& batcher, int32_t trackCount,
                                 float leftX, float judgmentLineY,
                                 float singleTrackW, float trackAreaW,
                                 const Config::EditorConfig& config);

    /// @warning 热路径：可见拍线每次动态快照生成时执行；BPM
    /// 列表必须由调用方提供缓存，禁止此处完整遍历或排序 timeline registry。
    static void drawBeatLines(
        Batcher& batcher, float viewportHeight, float judgmentLineY,
        const Config::EditorConfig&                  config,
        const std::vector<const TimelineComponent*>& bpmEvents,
        double currentTime, const ScrollCache* cache, float leftX, float topY,
        float bottomY, float trackAreaW, float renderScaleY);

    /// @warning 热路径：Preview timing 线每次动态快照生成时执行；只遍历
    /// ScrollCache 已缓存段。
    static void drawTimingLines(Batcher& batcher, float viewportHeight,
                                float                       judgmentLineY,
                                const Config::EditorConfig& config,
                                double currentTime, const ScrollCache* cache,
                                float leftX, float topY, float bottomY,
                                float trackAreaW, float renderScaleY);

    /// @warning 热路径：每次非 Timeline
    /// 快照生成时执行；必须使用已缓存的可见实体范围，禁止完整 entt 遍历。
    static void renderNotes(entt::registry& registry, RenderSnapshot* snapshot,
                            const std::string& cameraId, double currentTime,
                            float judgmentLineY, int32_t trackCount,
                            const Config::EditorConfig& config,
                            Batcher& batcher, float leftX, float rightX,
                            float topY, float bottomY, float singleTrackW,
                            float renderScaleY);

    /// @warning
    /// 热路径：音符渲染前每次执行；只读取快照和缓存，不得触发资源生命周期变更。
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
        double             currentTime;
    };

    /// @warning
    /// 热路径：每次音符层渲染前执行；禁止文件系统访问和共享指针所有权复制。
    static NoteRenderContext prepareNoteRenderContext(
        entt::registry& registry, RenderSnapshot* snapshot, double currentTime,
        float singleTrackW, const Config::EditorConfig& config);

    /// @warning 热路径：主画布拾取盒生成时执行；输入必须是已剔除实体列表。
    static void generateNoteHitboxes(
        entt::registry& registry, RenderSnapshot* snapshot,
        const NoteRenderContext&         ctx,
        const std::vector<entt::entity>& noteEntities, float judgmentLineY,
        float leftX, float topY, float bottomY, float singleTrackW,
        float renderScaleY, const Config::EditorConfig& config);

    /// @warning
    /// 热路径：音符基础层每次快照生成时执行；依赖预排序输入，禁止每帧完整排序。
    static void renderNoteBaseLayer(
        entt::registry& registry, RenderSnapshot* snapshot,
        const NoteRenderContext& ctx, const Config::EditorConfig& config,
        const std::vector<entt::entity>& noteEntities, Batcher& batcher,
        float currentTime, float judgmentLineY, float leftX, float rightX,
        float topY, float bottomY, float singleTrackW, float renderScaleY,
        bool generateHitboxes);

    /// @warning
    /// 热路径：悬浮发光层每次快照生成时执行；只扫描当前可见实体列表，禁止完整
    /// entt view 遍历。
    static void renderNoteGlowLayer(
        entt::registry& registry, RenderSnapshot* snapshot,
        const NoteRenderContext& ctx, const Config::EditorConfig& config,
        const std::vector<entt::entity>& noteEntities, float currentTime,
        float judgmentLineY, float leftX, float rightX, float topY,
        float bottomY, float singleTrackW, float renderScaleY);

    /// @warning 热路径：单个 Tap 几何生成时执行；不得分配 GPU
    /// 资源或访问文件系统。
    static void renderTap(Batcher& batcher, const NoteComponent& note,
                          const Config::EditorConfig& config, float x, float y,
                          float w, float h, float aspect, glm::vec4 color);

    /// @warning 热路径：单个 Hold
    /// 几何生成时执行；循环范围必须由可见时间段限制。
    static void renderHold(Batcher& batcher, const NoteComponent& note,
                           const Config::EditorConfig& config,
                           RenderSnapshot* snapshot, float x, float w, float h,
                           float singleTrackW, glm::vec4 color,
                           const ScrollCache* cache, double currentAbsY,
                           float judgmentLineY, float renderScaleY,
                           HoverPart glowPart = HoverPart::None);

    /// @warning 热路径：单个 Flick 几何生成时执行；不得触发排序或全量 ECS
    /// 查询。
    static void renderFlick(Batcher& batcher, const NoteComponent& note,
                            const Config::EditorConfig& config,
                            RenderSnapshot* snapshot, float x, float y, float w,
                            float h, float singleTrackW, glm::vec4 color,
                            glm::vec4 arrowColor,
                            HoverPart glowPart = HoverPart::None);

    /// @warning 热路径：单个 Polyline
    /// 几何生成时执行；子节点循环必须只处理当前可见载体范围。
    static void renderPolyline(
        const ScrollCache* cache, Batcher& batcher, const NoteComponent& note,
        const Config::EditorConfig& config, RenderSnapshot* snapshot,
        double currentAbsY, double currentTime, float judgmentLineY,
        float leftX, float rightX, float topY, float bottomY,
        float singleTrackW, float renderScaleY, glm::vec4 colorHold,
        glm::vec4 colorNode, glm::vec4 colorArrow,
        entt::entity entity = entt::null, bool generateHitboxes = false,
        HoverPart glowPart = HoverPart::None, int glowSubIndex = -1);

    /// @brief 绘制当前快照中的音符拾取包围盒，辅助排查悬浮命中区域。
    /// @warning 热路径：仅在 debugDrawHitboxes
    /// 开启时执行；默认渲染路径不得调用。
    static void debugRenderHitboxes(Batcher& batcher, RenderSnapshot* snapshot);

    /// @warning 热路径：Polyline body 几何生成时执行；禁止动态资源加载或完整
    /// registry 遍历。
    static void drawPolylineBody(Batcher& batcher, const NoteComponent& note,
                                 const ScrollCache* cache,
                                 RenderSnapshot* snapshot, float judgmentLineY,
                                 float leftX, float singleTrackW,
                                 float renderScaleY, double currentAbsY,
                                 double currentTime, float topY, float bottomY,
                                 float noteW, float noteH, glm::vec4 colorHold,
                                 entt::entity entity, bool generateHitboxes,
                                 HoverPart glowPart, int glowSubIndex);

    /// @warning 热路径：Polyline 可见性判断内联执行；保持纯计算且不可引入分配。
    static bool isCarrierVisible(double startOffset, double endOffset,
                                 double currentTime, double displayDeltaStart,
                                 double displayDeltaEnd, double maxDelta,
                                 double minDelta)
    {
        bool timeInRange = (startOffset <= currentTime + 0.1) &&
                           (endOffset >= currentTime - 0.1);
        bool spatialInRange =
            (displayDeltaStart <= maxDelta) && (displayDeltaEnd >= minDelta);
        return timeInRange || spatialInRange;
    }

    /// @warning 热路径：Polyline 节点几何生成时执行；只处理可见范围内节点。
    static void drawPolylineNodes(Batcher& batcher, const NoteComponent& note,
                                  const ScrollCache* cache,
                                  RenderSnapshot* snapshot, float judgmentLineY,
                                  float leftX, float singleTrackW,
                                  float renderScaleY, double currentAbsY,
                                  double currentTime, float topY, float bottomY,
                                  float noteW, float noteH, glm::vec4 colorNode,
                                  const Config::EditorConfig& config,
                                  entt::entity entity, bool generateHitboxes,
                                  HoverPart glowPart, int glowSubIndex);

    /// @warning 热路径：Polyline 头部几何生成时执行；不得触发 ECS 全量查询。
    static void drawPolylineHead(Batcher& batcher, const NoteComponent& note,
                                 const ScrollCache* cache,
                                 RenderSnapshot* snapshot, float judgmentLineY,
                                 float leftX, float singleTrackW,
                                 float renderScaleY, double currentAbsY,
                                 double currentTime, float topY, float bottomY,
                                 float noteW, float noteH, glm::vec4 colorHold,
                                 const Config::EditorConfig& config,
                                 entt::entity entity, bool generateHitboxes,
                                 HoverPart glowPart, int glowSubIndex);

    /// @warning 热路径：Polyline 装饰几何生成时执行；不得触发排序或磁盘读取。
    static void drawPolylineDecoration(
        Batcher& batcher, const NoteComponent& note, const ScrollCache* cache,
        RenderSnapshot* snapshot, float judgmentLineY, float leftX,
        float singleTrackW, float renderScaleY, double currentAbsY,
        double currentTime, float topY, float bottomY, float noteW, float noteH,
        glm::vec4 colorHold, glm::vec4 colorArrow,
        const Config::EditorConfig& config, entt::entity entity,
        bool generateHitboxes, HoverPart glowPart, int glowSubIndex);

    /// @warning 热路径：框选区域几何生成时执行；只处理当前快照中的框选列表。
    static void renderMarqueeBox(Batcher& batcher,
                                 const RenderSnapshot::MarqueeBoxSnapshot& box,
                                 float judgmentLineY, float leftX,
                                 float singleTrackW, float renderScaleY,
                                 const ScrollCache* cache, double renderTime,
                                 float viewportWidth, float viewportHeight);

    /// @warning 热路径：绘制工具预览时执行；不得提交逻辑命令或访问文件系统。
    static void renderBrushPreview(RenderSnapshot*             snapshot,
                                   const NoteRenderContext&    ctx,
                                   const Config::EditorConfig& config,
                                   Batcher& batcher, float judgmentLineY,
                                   float leftX, float singleTrackW,
                                   float renderScaleY);
};

}  // namespace MMM::Logic::System
