#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"

namespace MMM::Logic::System
{

static glm::vec2 getDrawSize(RenderSnapshot* snapshot, TextureID id,
                             float baseW, float baseH)
{
    auto itBase = snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Note));
    if ( itBase == snapshot->uvMap.end() ) return { baseW, baseH };
    float baseWRatio = itBase->second.z;

    auto it = snapshot->uvMap.find(static_cast<uint32_t>(id));
    if ( it == snapshot->uvMap.end() ) return { baseW, baseH };

    float wRatio = it->second.z / baseWRatio;
    float drawW  = baseW * wRatio;
    float drawH  = drawW * (it->second.w / it->second.z);
    return { drawW, drawH };
}

static float getTexAspect(RenderSnapshot* snapshot, TextureID id)
{
    auto it = snapshot->uvMap.find(static_cast<uint32_t>(id));
    if ( it == snapshot->uvMap.end() ) return 1.0f;
    return it->second.z / it->second.w;
}

void NoteRenderSystem::renderPolyline(
    entt::registry& registry, Batcher& batcher, const NoteComponent& note,
    const Config::EditorConfig& config, RenderSnapshot* snapshot,
    double currentTime, float judgmentLineY, float leftX, float rightX,
    float topY, float bottomY, float singleTrackW, float renderScaleY,
    glm::vec4 colorHold, glm::vec4 colorNode, glm::vec4 colorArrow,
    entt::entity entity, bool generateHitboxes, HoverPart glowPart,
    int glowSubIndex)
{
    const auto** cachePtr = registry.ctx().find<const ScrollCache*>();
    if ( !cachePtr || !(*cachePtr) ) return;
    const ScrollCache* cache       = *cachePtr;
    double             currentAbsY = cache->getAbsY(currentTime);

    float noteW = singleTrackW * config.visual.noteScaleX;
    float noteH = noteW / getTexAspect(snapshot, TextureID::Note);

    int lastIdx = static_cast<int>(note.m_subNotes.size() - 1);

    // 第一轮: 绘制所有 Body (BodyH, BodyV, Transition)
    if ( glowPart == HoverPart::None || glowPart == HoverPart::HoldBody ) {
        for ( size_t i = 0; i < note.m_subNotes.size(); ++i ) {
            if ( glowPart != HoverPart::None && glowSubIndex != -1 &&
                 glowSubIndex != static_cast<int>(i) ) {
                continue;
            }

            const auto& sub          = note.m_subNotes[i];
            double      subStartAbsY = cache->getAbsY(sub.timestamp);
            float       subStartY =
                judgmentLineY -
                static_cast<float>(subStartAbsY - currentAbsY) * renderScaleY;

            float subEndTrack = (float)sub.trackIndex;
            float subEndY     = subStartY;

            // 子物件自身 Body
            if ( sub.type == ::MMM::NoteType::FLICK && sub.dtrack != 0 ) {
                subEndTrack  = (float)sub.trackIndex + sub.dtrack;
                auto itBodyH = snapshot->uvMap.find(
                    static_cast<uint32_t>(TextureID::HoldBodyHorizontal));
                if ( itBodyH != snapshot->uvMap.end() ) {
                    float drawH =
                        noteH *
                        (itBodyH->second.w /
                         snapshot->uvMap.at(uint32_t(TextureID::Note)).w);
                    float drawW = std::abs(sub.dtrack) * singleTrackW;
                    float startTrack =
                        std::min((float)sub.trackIndex, subEndTrack);
                    float bodyX =
                        leftX + startTrack * singleTrackW + singleTrackW * 0.5f;
                    batcher.setTexture(TextureID::HoldBodyHorizontal);
                    batcher.pushQuad(bodyX,
                                     subStartY + drawH * 0.5f,
                                     drawW,
                                     drawH,
                                     colorHold);

                    if ( generateHitboxes && entity != entt::null ) {
                        snapshot->hitboxes.push_back({ entity,
                                                       HoverPart::HoldBody,
                                                       static_cast<int>(i),
                                                       bodyX,
                                                       subStartY - drawH * 0.5f,
                                                       drawW,
                                                       drawH });
                    }
                }
            } else if ( sub.type == ::MMM::NoteType::HOLD &&
                        sub.duration > 0 ) {
                double subEndAbsY =
                    cache->getAbsY(sub.timestamp + sub.duration);
                subEndY =
                    judgmentLineY -
                    static_cast<float>(subEndAbsY - currentAbsY) * renderScaleY;
                glm::vec2 bodySize = getDrawSize(
                    snapshot, TextureID::HoldBodyVertical, noteW, noteH);
                float bodyX = leftX + sub.trackIndex * singleTrackW +
                              (singleTrackW - bodySize.x) * 0.5f;
                batcher.setTexture(TextureID::HoldBodyVertical);
                batcher.pushQuad(bodyX,
                                 subStartY,
                                 bodySize.x,
                                 subStartY - subEndY,
                                 colorHold);

                if ( generateHitboxes && entity != entt::null ) {
                    snapshot->hitboxes.push_back({ entity,
                                                   HoverPart::HoldBody,
                                                   static_cast<int>(i),
                                                   bodyX,
                                                   subEndY,
                                                   bodySize.x,
                                                   subStartY - subEndY });
                }
            }

            // 过渡 Body (连接当前子物件末尾到下一个子物件开头)
            if ( i + 1 < note.m_subNotes.size() ) {
                const auto& next          = note.m_subNotes[i + 1];
                double      nextStartAbsY = cache->getAbsY(next.timestamp);
                float       nextStartY =
                    judgmentLineY -
                    static_cast<float>(nextStartAbsY - currentAbsY) *
                        renderScaleY;
                glm::vec2 bodySize = getDrawSize(
                    snapshot, TextureID::HoldBodyVertical, noteW, noteH);
                float curBodyX  = leftX + subEndTrack * singleTrackW +
                                  (singleTrackW - bodySize.x) * 0.5f;
                float nextBodyX = leftX + next.trackIndex * singleTrackW +
                                  (singleTrackW - bodySize.x) * 0.5f;

                batcher.setTexture(TextureID::HoldBodyVertical);
                batcher.pushFreeQuad({ curBodyX, subEndY },
                                     { curBodyX + bodySize.x, subEndY },
                                     { nextBodyX + bodySize.x, nextStartY },
                                     { nextBodyX, nextStartY },
                                     colorHold);

                if ( generateHitboxes && entity != entt::null ) {
                    float xmin = std::min(curBodyX, nextBodyX);
                    float xmax = std::max(curBodyX, nextBodyX) + bodySize.x;
                    float ymin = std::min(subEndY, nextStartY);
                    float ymax = std::max(subEndY, nextStartY);
                    snapshot->hitboxes.push_back({ entity,
                                                   HoverPart::HoldBody,
                                                   static_cast<int>(i),
                                                   xmin,
                                                   ymin,
                                                   xmax - xmin,
                                                   ymax - ymin });
                }
            }
        }
    }

    // 第二轮: 绘制 Node 装饰 (所有非头部的节点)
    if ( glowPart == HoverPart::None || glowPart == HoverPart::PolylineNode ) {
        for ( size_t i = 1; i < note.m_subNotes.size(); ++i ) {
            if ( glowPart != HoverPart::None && glowSubIndex != -1 &&
                 glowSubIndex != static_cast<int>(i) ) {
                continue;
            }

            const auto& sub          = note.m_subNotes[i];
            double      subStartAbsY = cache->getAbsY(sub.timestamp);
            float       subStartY =
                judgmentLineY -
                static_cast<float>(subStartAbsY - currentAbsY) * renderScaleY;
            glm::vec2 nodeSize =
                getDrawSize(snapshot, TextureID::Node, noteW, noteH);
            float nodeX = leftX + sub.trackIndex * singleTrackW +
                          (singleTrackW - nodeSize.x) * 0.5f;
            batcher.setTexture(TextureID::Node);
            batcher.pushFilledQuad(
                nodeX,
                subStartY + nodeSize.y * 0.5f,
                nodeSize.x,
                nodeSize.y,
                { getTexAspect(snapshot, TextureID::Node), 1.0f },
                config.visual.noteFillMode,
                colorNode);

            if ( generateHitboxes && entity != entt::null ) {
                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::PolylineNode,
                                               static_cast<int>(i),
                                               nodeX,
                                               subStartY - nodeSize.y * 0.5f,
                                               nodeSize.x,
                                               nodeSize.y });
            }
        }
    }

    // 第三轮: 绘制起始 Head
    if ( glowPart == HoverPart::None ||
         (glowPart == HoverPart::PolylineNode && glowSubIndex == 0) ) {
        const auto& first      = note.m_subNotes[0];
        double      fStartAbsY = cache->getAbsY(first.timestamp);
        float       fStartY =
            judgmentLineY -
            static_cast<float>(fStartAbsY - currentAbsY) * renderScaleY;
        glm::vec2 headSize =
            getDrawSize(snapshot, TextureID::Note, noteW, noteH);
        float headX = leftX + first.trackIndex * singleTrackW +
                      (singleTrackW - headSize.x) * 0.5f;
        batcher.setTexture(TextureID::Note);
        batcher.pushFilledQuad(
            headX,
            fStartY + headSize.y * 0.5f,
            headSize.x,
            headSize.y,
            { getTexAspect(snapshot, TextureID::Note), 1.0f },
            config.visual.noteFillMode,
            colorHold);

        if ( generateHitboxes && entity != entt::null ) {
            snapshot->hitboxes.push_back({ entity,
                                           HoverPart::PolylineNode,
                                           0,
                                           headX,
                                           fStartY - headSize.y * 0.5f,
                                           headSize.x,
                                           headSize.y });
        }
    }

    // 第四轮: 绘制结尾特殊样式 (Flick 箭头 或 Hold 结束线)
    if ( !note.m_subNotes.empty() ) {
        const auto& last = note.m_subNotes.back();
        bool        isLastGlow =
            (glowPart == HoverPart::None) ||
            (glowPart == HoverPart::FlickArrow && glowSubIndex == lastIdx) ||
            (glowPart == HoverPart::HoldEnd && glowSubIndex == lastIdx);

        if ( isLastGlow ) {
            double lStartAbsY = cache->getAbsY(last.timestamp);
            float  lStartY =
                judgmentLineY -
                static_cast<float>(lStartAbsY - currentAbsY) * renderScaleY;

            if ( last.type == ::MMM::NoteType::FLICK ) {
                // 仅当 glowPart 是箭头或全部显示时才发光
                if ( glowPart == HoverPart::None ||
                     glowPart == HoverPart::FlickArrow ) {
                    TextureID arrowId = (last.dtrack < 0)
                                            ? TextureID::FlickArrowLeft
                                            : TextureID::FlickArrowRight;
                    glm::vec2 arrowSize =
                        getDrawSize(snapshot, arrowId, noteW, noteH);
                    float lEndTrack = (float)last.trackIndex + last.dtrack;
                    float arrowX    = leftX + lEndTrack * singleTrackW +
                                      (singleTrackW - arrowSize.x) * 0.5f;

                    batcher.setTexture(arrowId);
                    batcher.pushFilledQuad(
                        arrowX,
                        lStartY + arrowSize.y * 0.5f,
                        arrowSize.x,
                        arrowSize.y,
                        { getTexAspect(snapshot, arrowId), 1.0f },
                        config.visual.noteFillMode,
                        colorArrow);

                    if ( generateHitboxes && entity != entt::null ) {
                        snapshot->hitboxes.push_back(
                            { entity,
                              HoverPart::FlickArrow,
                              lastIdx,
                              arrowX,
                              lStartY - arrowSize.y * 0.5f,
                              arrowSize.x,
                              arrowSize.y });
                    }
                }
            } else if ( last.type == ::MMM::NoteType::HOLD ) {
                if ( glowPart == HoverPart::None ||
                     glowPart == HoverPart::HoldEnd ) {
                    double subEndAbsY =
                        cache->getAbsY(last.timestamp + last.duration);
                    float subEndY =
                        judgmentLineY -
                        static_cast<float>(subEndAbsY - currentAbsY) *
                            renderScaleY;
                    glm::vec2 endSize =
                        getDrawSize(snapshot, TextureID::HoldEnd, noteW, noteH);
                    float endX = leftX + last.trackIndex * singleTrackW +
                                 (singleTrackW - endSize.x) * 0.5f;

                    batcher.setTexture(TextureID::HoldEnd);
                    batcher.pushFilledQuad(
                        endX,
                        subEndY + endSize.y * 0.5f,
                        endSize.x,
                        endSize.y,
                        { getTexAspect(snapshot, TextureID::HoldEnd), 1.0f },
                        config.visual.noteFillMode,
                        colorHold);

                    if ( generateHitboxes && entity != entt::null ) {
                        snapshot->hitboxes.push_back(
                            { entity,
                              HoverPart::HoldEnd,
                              lastIdx,
                              endX,
                              subEndY - endSize.y * 0.5f,
                              endSize.x,
                              endSize.y });
                    }
                }
            }
        }
    }
}

}  // namespace MMM::Logic::System
