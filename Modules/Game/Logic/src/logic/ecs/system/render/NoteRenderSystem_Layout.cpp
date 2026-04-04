#include "Batcher.h"
#include "logic/ecs/system/NoteRenderSystem.h"

namespace MMM::Logic::System
{

void NoteRenderSystem::renderTrackLayout(
    Batcher& batcher, float viewportWidth, float viewportHeight,
    float judgmentLineY, int32_t trackCount, const EditorConfig& config,
    float& leftX, float& rightX, float& topY, float& bottomY, float& trackAreaW,
    float& singleTrackW)
{
    leftX            = viewportWidth * config.trackLayout.left;
    rightX           = viewportWidth * config.trackLayout.right;
    topY             = viewportHeight * config.trackLayout.top;
    bottomY          = viewportHeight * config.trackLayout.bottom;
    trackAreaW       = rightX - leftX;
    float trackAreaH = bottomY - topY;
    singleTrackW     = trackAreaW / static_cast<float>(trackCount);

    // 绘制轨道布局包围框
    batcher.setTexture(TextureID::None);
    batcher.pushStrokeRect(leftX,
                           topY,
                           rightX,
                           bottomY,
                           config.trackBoxLineWidth,
                           { 0.5f, 0.5f, 0.5f, 1.0f });

    // 绘制判定线 (横向贯穿轨道区域)
    batcher.pushQuad(leftX,
                     judgmentLineY + config.judgelineWidth * 0.5f,
                     trackAreaW,
                     config.judgelineWidth,
                     { 1.0f, 1.0f, 1.0f, 1.0f });
}

}  // namespace MMM::Logic::System
