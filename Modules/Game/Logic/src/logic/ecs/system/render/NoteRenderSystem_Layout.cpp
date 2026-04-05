#include "Batcher.h"
#include "logic/ecs/system/NoteRenderSystem.h"

namespace MMM::Logic::System
{

void NoteRenderSystem::renderTrackLayout(
    Batcher& batcher, float viewportWidth, float viewportHeight,
    float judgmentLineY, int32_t trackCount, const Common::EditorConfig& config,
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

    // 绘制轨道底板
    batcher.setTexture(TextureID::Track);
    auto uvIt =
        batcher.snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Track));
    if ( uvIt != batcher.snapshot->uvMap.end() ) {
        // uvMap.z, w 是归一化的宽高，基于 2048 尺寸
        float texW_px = uvIt->second.z * 2048.0f;
        float texH_px = uvIt->second.w * 2048.0f;

        if ( texW_px > 0 && texH_px > 0 ) {
            float texAspect = texW_px / texH_px;
            float drawH =
                singleTrackW /
                texAspect;  // 每个轨道按照宽度等比缩放计算出的单块高度

            for ( int i = 0; i < trackCount; ++i ) {
                float trackX   = leftX + i * singleTrackW;
                float currentY = bottomY;  // 从底向上绘制

                while ( currentY > topY ) {
                    float remainH     = currentY - topY;
                    float actualDrawH = std::min(drawH, remainH);

                    float vMax = 1.0f;
                    float vMin = 1.0f - (actualDrawH / drawH);

                    batcher.pushUVQuad(
                        trackX,
                        currentY,
                        singleTrackW,
                        actualDrawH,
                        glm::vec2(uvIt->second.x,
                                  uvIt->second.y + vMin * uvIt->second.w),
                        glm::vec2(uvIt->second.x + uvIt->second.z,
                                  uvIt->second.y + vMax * uvIt->second.w),
                        { 1.0f, 1.0f, 1.0f, 1.0f });

                    currentY -= drawH;
                }
            }
        }
    }

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
