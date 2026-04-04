#include "Batcher.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"

namespace MMM::Logic::System
{

void NoteRenderSystem::renderNotes(entt::registry& registry,
                                   RenderSnapshot* snapshot, double currentTime,
                                   float judgmentLineY, int32_t trackCount,
                                   const EditorConfig& config, Batcher& batcher,
                                   float leftX, float rightX, float topY,
                                   float bottomY, float singleTrackW)
{
    auto noteView =
        registry.view<const TransformComponent, const NoteComponent>();

    const auto** cachePtr = registry.ctx().find<const ScrollCache*>();
    if ( !cachePtr || !(*cachePtr) ) return;
    const ScrollCache* cache       = *cachePtr;
    double             currentAbsY = cache->getAbsY(currentTime);


    // 物件实际渲染宽度和高度 (由缩放控制)
    float noteW       = singleTrackW * config.noteScaleX;
    float noteH       = 20.0f * config.noteScaleY;
    float noteXOffset = (singleTrackW - noteW) * 0.5f;

    for ( auto entity : noteView ) {
        const auto& transform = noteView.get<const TransformComponent>(entity);
        const auto& note      = noteView.get<const NoteComponent>(entity);
        const InteractionComponent* interaction =
            registry.try_get<InteractionComponent>(entity);
        bool isHovered = interaction ? interaction->isHovered : false;

        float minY    = transform.m_pos.y;
        float visualH = transform.m_size.y;

        // 计算判定线上的屏幕 Y (主音符起始位置)
        float screenY = judgmentLineY - minY;

        // 简易视口剔除 (Y 轴剔除)
        if ( screenY - visualH > bottomY ) continue;
        if ( screenY < topY ) continue;

        // 当前音符所在轨道 X (居中绘制)
        float startX = leftX + note.m_trackIndex * singleTrackW + noteXOffset;

        // 设置颜色：如果是被悬停，则叠加一层橙色高亮
        glm::vec4 texColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        if ( isHovered ) texColor = { 1.0f, 0.5f, 0.0f, 1.0f };

        // 同步拾取包围盒
        snapshot->hitboxes.push_back(
            { entity, startX, screenY - visualH, noteW, visualH });

        if ( note.m_type == ::MMM::NoteType::NOTE ) {
            batcher.setTexture(TextureID::Note);
            batcher.pushQuad(startX, screenY, noteW, noteH, texColor);
        } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
            // Body
            batcher.setTexture(TextureID::HoldBodyVertical);
            batcher.pushQuad(startX,
                             screenY - noteH * 0.5f,
                             noteW,
                             std::max(0.0f, visualH - noteH),
                             texColor);
            // End
            batcher.setTexture(TextureID::HoldEnd);
            batcher.pushQuad(
                startX, screenY - visualH + noteH, noteW, noteH, texColor);
            // Head
            batcher.setTexture(TextureID::HoldHead);
            batcher.pushQuad(startX, screenY, noteW, noteH, texColor);
        } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
            batcher.setTexture(TextureID::HoldHead);
            batcher.pushQuad(startX, screenY, noteW, noteH, texColor);

            if ( note.m_dtrack < 0 ) {
                batcher.setTexture(TextureID::FlickArrowLeft);
                batcher.pushQuad(startX, screenY, noteW, noteH, texColor);
            } else if ( note.m_dtrack > 0 ) {
                batcher.setTexture(TextureID::FlickArrowRight);
                batcher.pushQuad(startX, screenY, noteW, noteH, texColor);
            }
        } else if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
            for ( size_t i = 0; i < note.m_subNotes.size(); ++i ) {
                const auto& sub = note.m_subNotes[i];
                float       subX =
                    leftX + sub.trackIndex * singleTrackW + noteXOffset;
                double subAbsY = cache->getAbsY(sub.timestamp);
                float  subScreenY =
                    judgmentLineY - static_cast<float>(subAbsY - currentAbsY);

                if ( i + 1 < note.m_subNotes.size() ) {
                    const auto& next = note.m_subNotes[i + 1];
                    float       nextX =
                        leftX + next.trackIndex * singleTrackW + noteXOffset;
                    double nextAbsY = cache->getAbsY(next.timestamp);
                    float  nextScreenY =
                        judgmentLineY -
                        static_cast<float>(nextAbsY - currentAbsY);

                    batcher.setTexture(TextureID::HoldBodyVertical);
                    batcher.pushFreeQuad(
                        { subX, subScreenY - noteH * 0.5f },
                        { subX + noteW, subScreenY - noteH * 0.5f },
                        { nextX + noteW, nextScreenY + noteH * 0.5f },
                        { nextX, nextScreenY + noteH * 0.5f },
                        texColor);
                }

                if ( i == 0 ) {
                    batcher.setTexture(TextureID::HoldHead);
                    batcher.pushQuad(subX, subScreenY, noteW, noteH, texColor);
                } else if ( i == note.m_subNotes.size() - 1 ) {
                    batcher.setTexture(TextureID::HoldEnd);
                    batcher.pushQuad(subX, subScreenY, noteW, noteH, texColor);
                }

                if ( sub.type == ::MMM::NoteType::FLICK ) {
                    if ( sub.dtrack < 0 ) {
                        batcher.setTexture(TextureID::FlickArrowLeft);
                        batcher.pushQuad(
                            subX, subScreenY, noteW, noteH, texColor);
                    } else if ( sub.dtrack > 0 ) {
                        batcher.setTexture(TextureID::FlickArrowRight);
                        batcher.pushQuad(
                            subX, subScreenY, noteW, noteH, texColor);
                    }
                }
            }
        }
    }
}

}  // namespace MMM::Logic::System
