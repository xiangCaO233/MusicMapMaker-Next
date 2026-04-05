#include "logic/ecs/system/NoteRenderSystem.h"
#include "Batcher.h"
#include "logic/ecs/system/BackgroundRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"

namespace MMM::Logic::System
{

void NoteRenderSystem::generateSnapshot(entt::registry&       registry,
                                        const entt::registry& timelineRegistry,
                                        RenderSnapshot*       snapshot,
                                        double currentTime, float viewportWidth,
                                        float viewportHeight,
                                        float judgmentLineY, int32_t trackCount,
                                        const EditorConfig& config)
{
    const auto* cache = timelineRegistry.ctx().find<ScrollCache>();
    if ( !cache ) return;

    // 将 ScrollCache 临时注入 noteRegistry 以便在 renderNotes 中使用
    registry.ctx().insert_or_assign(cache);

    Batcher batcher(snapshot);

    // 绘制背景
    BackgroundRenderSystem::render(
        batcher, viewportWidth, viewportHeight, config, snapshot);

    float leftX, rightX, topY, bottomY, trackAreaW, singleTrackW;
    renderTrackLayout(batcher,
                      viewportWidth,
                      viewportHeight,
                      judgmentLineY,
                      trackCount,
                      config,
                      leftX,
                      rightX,
                      topY,
                      bottomY,
                      trackAreaW,
                      singleTrackW);

    renderNotes(registry,
                snapshot,
                currentTime,
                judgmentLineY,
                trackCount,
                config,
                batcher,
                leftX,
                rightX,
                topY,
                bottomY,
                singleTrackW);

    batcher.flush();
}

}  // namespace MMM::Logic::System
