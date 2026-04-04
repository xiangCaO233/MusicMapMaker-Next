#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"

namespace MMM::Logic::System
{

void NoteRenderSystem::generateSnapshot(entt::registry&       registry,
                                        const entt::registry& timelineRegistry,
                                        RenderSnapshot*       snapshot,
                                        double                currentTime,
                                        float                 viewportHeight,
                                        float                 judgmentLineY)
{
    auto noteView =
        registry.view<const TransformComponent, const NoteComponent>();

    auto* cache = timelineRegistry.ctx().find<ScrollCache>();
    if ( !cache ) return;
    double currentAbsY = cache->getAbsY(currentTime);

    // 内部批处理器，负责根据 TextureID 自动切分 DrawCall
    struct Batcher {
        RenderSnapshot*  snapshot;
        TextureID        currentTex = TextureID::None;
        UI::BrushDrawCmd currentCmd;

        Batcher(RenderSnapshot* s) : snapshot(s)
        {
            currentCmd.indexOffset     = 0;
            currentCmd.vertexOffset    = 0;
            currentCmd.indexCount      = 0;
            currentCmd.texture         = VK_NULL_HANDLE;
            currentCmd.customTextureId = 0;
        }

        void setTexture(TextureID tex)
        {
            if ( currentCmd.indexCount > 0 && currentTex != tex ) {
                snapshot->cmds.push_back(currentCmd);
                currentCmd.indexCount   = 0;
                currentCmd.indexOffset  = snapshot->indices.size();
                currentCmd.vertexOffset = 0;
            }
            if ( currentCmd.indexCount == 0 ) {
                currentCmd.customTextureId = static_cast<uint32_t>(tex);
            }
            currentTex = tex;
        }

        /// @brief 推送一个矩形，如果是带纹理的，颜色固定为白色
        void pushQuad(float x, float y, float w, float h, glm::vec4 color)
        {
            pushFreeQuad(
                { x, y }, { x + w, y }, { x + w, y - h }, { x, y - h }, color);
        }

        void pushFreeQuad(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3,
                          glm::vec2 p4, glm::vec4 color)
        {
            uint32_t baseIndex = snapshot->vertices.size();

            Graphic::Vertex::VKBasicVertex v1, v2, v3, v4;
            v1.pos = { p1.x, p1.y, 0.0f };
            v2.pos = { p2.x, p2.y, 0.0f };
            v3.pos = { p3.x, p3.y, 0.0f };
            v4.pos = { p4.x, p4.y, 0.0f };

            // 标准 UV
            v1.uv = { 0.0f, 1.0f };
            v2.uv = { 1.0f, 1.0f };
            v3.uv = { 1.0f, 0.0f };
            v4.uv = { 0.0f, 0.0f };

            v1.color = v2.color = v3.color =
                v4.color        = { color.r, color.g, color.b, color.a };

            snapshot->vertices.push_back(v1);
            snapshot->vertices.push_back(v2);
            snapshot->vertices.push_back(v3);
            snapshot->vertices.push_back(v4);

            snapshot->indices.push_back(baseIndex + 0);
            snapshot->indices.push_back(baseIndex + 1);
            snapshot->indices.push_back(baseIndex + 2);
            snapshot->indices.push_back(baseIndex + 2);
            snapshot->indices.push_back(baseIndex + 3);
            snapshot->indices.push_back(baseIndex + 0);

            currentCmd.indexCount += 6;
        }

        void flush()
        {
            if ( currentCmd.indexCount > 0 ) {
                snapshot->cmds.push_back(currentCmd);
                currentCmd.indexCount = 0;
            }
        }
    } batcher(snapshot);

    for ( auto entity : noteView ) {
        const auto& transform = noteView.get<const TransformComponent>(entity);
        const auto& note      = noteView.get<const NoteComponent>(entity);
        const InteractionComponent* interaction =
            registry.try_get<InteractionComponent>(entity);
        bool isHovered = interaction ? interaction->isHovered : false;

        float minY = transform.m_pos.y;
        float h    = transform.m_size.y;
        float minX = transform.m_pos.x;
        float w    = transform.m_size.x;

        // 计算判定线上的屏幕 Y (主音符起始位置)
        // 注意：NoteTransformSystem 将 minY 设置为起始时间的 relative Y
        float screenY = judgmentLineY - minY;

        // 简易视口剔除
        if ( screenY - h > viewportHeight ) continue;
        if ( screenY < 0.0f ) continue;

        // 同步拾取包围盒 (使用 Transform 中的 Bounding Box)
        snapshot->hitboxes.push_back({ entity, minX, screenY - h, w, h });

        // 默认绘制宽度 (50) 和起始 X
        float startX = note.m_trackIndex * 60.0f + 20.0f;
        float noteW  = 50.0f;

        // 设置颜色：如果是带贴图的，使用白色以保持贴图原色
        // 如果被悬停，则叠加一层橙色高亮 (0.5 混合)
        glm::vec4 texColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        if ( isHovered ) texColor = { 1.0f, 0.5f, 0.0f, 1.0f };

        if ( note.m_type == ::MMM::NoteType::NOTE ) {
            batcher.setTexture(TextureID::Note);
            batcher.pushQuad(startX, screenY, noteW, 20.0f, texColor);
        } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
            // Body
            batcher.setTexture(TextureID::HoldBodyVertical);
            batcher.pushQuad(startX,
                             screenY - 10.0f,
                             noteW,
                             std::max(0.0f, h - 20.0f),
                             texColor);
            // End
            batcher.setTexture(TextureID::HoldEnd);
            batcher.pushQuad(
                startX, screenY - h + 20.0f, noteW, 20.0f, texColor);
            // Head
            batcher.setTexture(TextureID::HoldHead);
            batcher.pushQuad(startX, screenY, noteW, 20.0f, texColor);
        } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
            batcher.setTexture(TextureID::HoldHead);
            batcher.pushQuad(startX, screenY, noteW, 20.0f, texColor);

            if ( note.m_dtrack < 0 ) {
                batcher.setTexture(TextureID::FlickArrowLeft);
                batcher.pushQuad(startX, screenY, noteW, 20.0f, texColor);
            } else if ( note.m_dtrack > 0 ) {
                batcher.setTexture(TextureID::FlickArrowRight);
                batcher.pushQuad(startX, screenY, noteW, 20.0f, texColor);
            }
        } else if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
            // 针对 Polyline 的多段绘制逻辑 (连接相邻子物件形成连续轨迹)
            for ( size_t i = 0; i < note.m_subNotes.size(); ++i ) {
                const auto& sub     = note.m_subNotes[i];
                float       subX    = sub.trackIndex * 60.0f + 20.0f;
                double      subAbsY = cache->getAbsY(sub.timestamp);
                float       subScreenY =
                    judgmentLineY - static_cast<float>(subAbsY - currentAbsY);

                // 如果有下一个点，绘制连接段
                if ( i + 1 < note.m_subNotes.size() ) {
                    const auto& next     = note.m_subNotes[i + 1];
                    float       nextX    = next.trackIndex * 60.0f + 20.0f;
                    double      nextAbsY = cache->getAbsY(next.timestamp);
                    float       nextScreenY =
                        judgmentLineY -
                        static_cast<float>(nextAbsY - currentAbsY);

                    // 如果是普通长键或者折线连接，绘制平滑过渡段
                    batcher.setTexture(TextureID::HoldBodyVertical);
                    batcher.pushFreeQuad({ subX, subScreenY - 10.0f },
                                         { subX + noteW, subScreenY - 10.0f },
                                         { nextX + noteW, nextScreenY + 10.0f },
                                         { nextX, nextScreenY + 10.0f },
                                         texColor);
                }

                // 绘制节点装饰 (Head/End/Flick)
                if ( i == 0 ) {
                    batcher.setTexture(TextureID::HoldHead);
                    batcher.pushQuad(subX, subScreenY, noteW, 20.0f, texColor);
                } else if ( i == note.m_subNotes.size() - 1 ) {
                    batcher.setTexture(TextureID::HoldEnd);
                    batcher.pushQuad(subX, subScreenY, noteW, 20.0f, texColor);
                }

                if ( sub.type == ::MMM::NoteType::FLICK ) {
                    if ( sub.dtrack < 0 ) {
                        batcher.setTexture(TextureID::FlickArrowLeft);
                        batcher.pushQuad(
                            subX, subScreenY, noteW, 20.0f, texColor);
                    } else if ( sub.dtrack > 0 ) {
                        batcher.setTexture(TextureID::FlickArrowRight);
                        batcher.pushQuad(
                            subX, subScreenY, noteW, 20.0f, texColor);
                    }
                }
            }
        }
    }

    batcher.flush();
}

}  // namespace MMM::Logic::System
