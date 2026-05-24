#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"

#include <algorithm>
#include <cmath>

namespace MMM::Logic::System
{

/**
 * @brief 根据纹理比例计算绘制尺寸
 */
static glm::vec2 getDrawSize(RenderSnapshot* snapshot, TextureID id,
                             float baseW, float baseH)
{
    auto itBase = snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Note));
    if ( itBase == snapshot->uvMap.end() ) return { baseW, baseH };

    auto it = snapshot->uvMap.find(static_cast<uint32_t>(id));
    if ( it == snapshot->uvMap.end() ) return { baseW, baseH };

    float wRatio = it->second.z / itBase->second.z;
    float hRatio = it->second.w / itBase->second.w;

    return { baseW * wRatio, baseH * hRatio };
}

/**
 * @brief 获取纹理宽高比
 */
static float getTexAspect(RenderSnapshot* snapshot, TextureID id)
{
    auto it = snapshot->uvMap.find(static_cast<uint32_t>(id));
    if ( it == snapshot->uvMap.end() ) return 1.0f;
    return it->second.z / it->second.w;
}

void NoteRenderSystem::renderPolyline(
    const ScrollCache* cache, Batcher& batcher, const NoteComponent& note,
    const Config::EditorConfig& config, RenderSnapshot* snapshot,
    double currentAbsY, double currentTime, float judgmentLineY, float leftX,
    float rightX, float topY, float bottomY, float singleTrackW,
    float renderScaleY, glm::vec4 colorHold, glm::vec4 colorNode,
    glm::vec4 colorArrow, entt::entity entity, bool generateHitboxes,
    HoverPart glowPart, int glowSubIndex)
{
    if ( !cache ) return;

    float noteW = singleTrackW * config.visual.noteScaleX;
    float noteH = (singleTrackW / getTexAspect(snapshot, TextureID::Note)) *
                  config.visual.noteScaleY;

    // 1. 绘制主体连接段
    drawPolylineBody(batcher,
                     note,
                     cache,
                     snapshot,
                     judgmentLineY,
                     leftX,
                     singleTrackW,
                     renderScaleY,
                     currentAbsY,
                     currentTime,
                     topY,
                     bottomY,
                     noteW,
                     noteH,
                     colorHold,
                     entity,
                     generateHitboxes,
                     glowPart,
                     glowSubIndex);

    // 2. 绘制中间节点
    drawPolylineNodes(batcher,
                      note,
                      cache,
                      snapshot,
                      judgmentLineY,
                      leftX,
                      singleTrackW,
                      renderScaleY,
                      currentAbsY,
                      currentTime,
                      topY,
                      bottomY,
                      noteW,
                      noteH,
                      colorNode,
                      config,
                      entity,
                      generateHitboxes,
                      glowPart,
                      glowSubIndex);

    // 3. 绘制起始磁头
    drawPolylineHead(batcher,
                     note,
                     cache,
                     snapshot,
                     judgmentLineY,
                     leftX,
                     singleTrackW,
                     renderScaleY,
                     currentAbsY,
                     currentTime,
                     topY,
                     bottomY,
                     noteW,
                     noteH,
                     colorHold,
                     config,
                     entity,
                     generateHitboxes,
                     glowPart,
                     glowSubIndex);

    // 4. 绘制尾部装饰 (Flick箭头或Hold结束线)
    drawPolylineDecoration(batcher,
                           note,
                           cache,
                           snapshot,
                           judgmentLineY,
                           leftX,
                           singleTrackW,
                           renderScaleY,
                           currentAbsY,
                           currentTime,
                           topY,
                           bottomY,
                           noteW,
                           noteH,
                           colorHold,
                           colorArrow,
                           config,
                           entity,
                           generateHitboxes,
                           glowPart,
                           glowSubIndex);
}

void NoteRenderSystem::drawPolylineBody(
    Batcher& batcher, const NoteComponent& note, const ScrollCache* cache,
    RenderSnapshot* snapshot, float judgmentLineY, float leftX,
    float singleTrackW, float renderScaleY, double currentAbsY,
    double currentTime, float topY, float bottomY, float noteW, float noteH,
    glm::vec4 colorHold, entt::entity entity, bool generateHitboxes,
    HoverPart glowPart, int glowSubIndex)
{
    if ( note.m_subNotes.empty() ) return;
    if ( glowPart != HoverPart::None && glowPart != HoverPart::HoldBody )
        return;

    for ( size_t i = 0; i < note.m_subNotes.size(); ++i ) {
        // 如果是 Glow Pass 且 subIndex 不匹配，则跳过
        if ( glowPart != HoverPart::None && glowSubIndex != -1 &&
             glowSubIndex != static_cast<int>(i) ) {
            continue;
        }

        const auto& sub = note.m_subNotes[i];

        double displayDeltaStart =
            cache->getDisplayDelta(sub.timestamp, currentAbsY, sub.timestamp);
        double displayDeltaEnd =
            cache->getDisplayDelta(sub.timestamp + sub.duration,
                                   currentAbsY,
                                   sub.timestamp + sub.duration);

        double maxDelta =
            (judgmentLineY - topY) / static_cast<double>(renderScaleY);
        double minDelta =
            (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
        double padDelta = noteH / static_cast<double>(renderScaleY);

        if ( !NoteRenderSystem::isCarrierVisible(sub.timestamp,
                                                 sub.timestamp + sub.duration,
                                                 currentTime,
                                                 displayDeltaStart,
                                                 displayDeltaEnd,
                                                 maxDelta + padDelta,
                                                 minDelta - padDelta) ) {
            continue;
        }

        float subStartY = judgmentLineY -
                          static_cast<float>(displayDeltaStart) * renderScaleY;

        float subEndTrack = (float)sub.trackIndex;
        float subEndY     = subStartY;

        // 子物件自身 Body (水平 Flick 或 垂直 Hold)
        if ( sub.type == ::MMM::NoteType::FLICK && sub.dtrack != 0 ) {
            subEndTrack  = (float)sub.trackIndex + sub.dtrack;
            auto itBodyH = snapshot->uvMap.find(
                static_cast<uint32_t>(TextureID::HoldBodyHorizontal));
            if ( itBodyH != snapshot->uvMap.end() ) {
                float drawH =
                    noteH * (itBodyH->second.w /
                             snapshot->uvMap.at(uint32_t(TextureID::Note)).w);
                float drawW      = std::abs(sub.dtrack) * singleTrackW;
                float startTrack = std::min((float)sub.trackIndex, subEndTrack);
                float bodyX =
                    leftX + startTrack * singleTrackW + singleTrackW * 0.5f;

                glm::vec4 finalBodyColor = colorHold;
                if ( snapshot->erasingEntities.count(entity) &&
                     (snapshot->erasingSubIndex == static_cast<int>(i) ||
                      snapshot->erasingSubIndex == 0 ||
                      snapshot->erasingSubIndex == -1) ) {
                    finalBodyColor = { 1.0f, 0.2f, 0.2f, colorHold.a * 0.5f };
                }

                batcher.setTexture(TextureID::HoldBodyHorizontal);
                batcher.pushQuad(bodyX,
                                 subStartY + drawH * 0.5f,
                                 drawW,
                                 drawH,
                                 finalBodyColor);

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
        } else if ( sub.type == ::MMM::NoteType::HOLD && sub.duration > 0 ) {
            subEndY =
                judgmentLineY -
                static_cast<float>(cache->getDisplayDelta(
                    sub.timestamp + sub.duration, currentAbsY, sub.timestamp)) *
                    renderScaleY;
            glm::vec2 bodySize = getDrawSize(
                snapshot, TextureID::HoldBodyVertical, noteW, noteH);
            float bodyX = leftX + sub.trackIndex * singleTrackW +
                          (singleTrackW - bodySize.x) * 0.5f;

            glm::vec4 finalBodyColor = colorHold;
            if ( snapshot->erasingEntities.count(entity) &&
                 (snapshot->erasingSubIndex == static_cast<int>(i) ||
                  snapshot->erasingSubIndex == 0 ||
                  snapshot->erasingSubIndex == -1) ) {
                finalBodyColor = { 1.0f, 0.2f, 0.2f, colorHold.a * 0.5f };
            }

            batcher.setTexture(TextureID::HoldBodyVertical);
            float sy = judgmentLineY -
                       static_cast<float>(cache->getDisplayDelta(
                           sub.timestamp, currentAbsY, sub.timestamp)) *
                           renderScaleY;
            float ey =
                judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                                    sub.timestamp + sub.duration,
                                    currentAbsY,
                                    sub.timestamp + sub.duration)) *
                                    renderScaleY;
            batcher.pushFreeQuad({ bodyX, sy },
                                 { bodyX + bodySize.x, sy },
                                 { bodyX + bodySize.x, ey },
                                 { bodyX, ey },
                                 finalBodyColor);

            if ( generateHitboxes && entity != entt::null ) {
                float hitY = std::min(subStartY, subEndY);
                float hitH = std::abs(subStartY - subEndY);
                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::HoldBody,
                                               static_cast<int>(i),
                                               bodyX,
                                               hitY,
                                               bodySize.x,
                                               hitH });
            }
        }

        // 过渡 Body (连接当前子物件末尾到下一个子物件开头)
        if ( i + 1 < note.m_subNotes.size() ) {
            const auto& next = note.m_subNotes[i + 1];
            float       nextStartY =
                judgmentLineY -
                static_cast<float>(cache->getDisplayDelta(
                    next.timestamp, currentAbsY, next.timestamp)) *
                    renderScaleY;
            glm::vec2 bodySize = getDrawSize(
                snapshot, TextureID::HoldBodyVertical, noteW, noteH);
            float curBodyX  = leftX + subEndTrack * singleTrackW +
                              (singleTrackW - bodySize.x) * 0.5f;
            float nextBodyX = leftX + next.trackIndex * singleTrackW +
                              (singleTrackW - bodySize.x) * 0.5f;

            glm::vec4 finalTransColor = colorHold;
            if ( snapshot->erasingEntities.count(entity) &&
                 (snapshot->erasingSubIndex == static_cast<int>(i + 1) ||
                  snapshot->erasingSubIndex == 0 ||
                  snapshot->erasingSubIndex == -1) ) {
                finalTransColor = { 1.0f, 0.2f, 0.2f, colorHold.a * 0.5f };
            }

            batcher.setTexture(TextureID::HoldBodyVertical);

            double tStart = sub.timestamp + sub.duration;
            double tEnd   = next.timestamp;

            float sy =
                judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                                    tStart, currentAbsY, tStart)) *
                                    renderScaleY;
            float ey =
                judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                                    tEnd, currentAbsY, tEnd)) *
                                    renderScaleY;

            float x1 = curBodyX;
            float x2 = nextBodyX;

            batcher.pushFreeQuad({ x1, sy },
                                 { x1 + bodySize.x, sy },
                                 { x2 + bodySize.x, ey },
                                 { x2, ey },
                                 finalTransColor);

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

void NoteRenderSystem::drawPolylineNodes(
    Batcher& batcher, const NoteComponent& note, const ScrollCache* cache,
    RenderSnapshot* snapshot, float judgmentLineY, float leftX,
    float singleTrackW, float renderScaleY, double currentAbsY,
    double currentTime, float topY, float bottomY, float noteW, float noteH,
    glm::vec4 colorNode, const Config::EditorConfig& config,
    entt::entity entity, bool generateHitboxes, HoverPart glowPart,
    int glowSubIndex)
{
    if ( note.m_subNotes.empty() ) return;
    if ( glowPart != HoverPart::None && glowPart != HoverPart::PolylineNode )
        return;

    // 从第1个子物件开始(第0个是Head)
    for ( size_t i = 1; i < note.m_subNotes.size(); ++i ) {
        if ( glowPart != HoverPart::None && glowSubIndex != -1 &&
             glowSubIndex != static_cast<int>(i) ) {
            continue;
        }

        const auto& sub = note.m_subNotes[i];
        double      displayDeltaStart =
            cache->getDisplayDelta(sub.timestamp, currentAbsY, sub.timestamp);
        double displayDeltaEnd = displayDeltaStart;

        double maxDelta =
            (judgmentLineY - topY) / static_cast<double>(renderScaleY);
        double minDelta =
            (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
        double padDelta = noteH / static_cast<double>(renderScaleY);

        if ( !NoteRenderSystem::isCarrierVisible(sub.timestamp,
                                                 sub.timestamp,
                                                 currentTime,
                                                 displayDeltaStart,
                                                 displayDeltaEnd,
                                                 maxDelta + padDelta,
                                                 minDelta - padDelta) ) {
            continue;
        }

        float subStartY = judgmentLineY -
                          static_cast<float>(displayDeltaStart) * renderScaleY;
        glm::vec2 nodeSize =
            getDrawSize(snapshot, TextureID::Node, noteW, noteH);
        float nodeX = leftX + sub.trackIndex * singleTrackW +
                      (singleTrackW - nodeSize.x) * 0.5f;

        glm::vec4 finalNodeColor = colorNode;
        if ( snapshot->erasingEntities.count(entity) &&
             (snapshot->erasingSubIndex == static_cast<int>(i) ||
              snapshot->erasingSubIndex == 0 ||
              snapshot->erasingSubIndex == -1) ) {
            finalNodeColor = { 1.0f, 0.2f, 0.2f, colorNode.a * 0.5f };
        }

        batcher.setTexture(TextureID::Node);
        batcher.pushFilledQuad(
            nodeX,
            subStartY + nodeSize.y * 0.5f,
            nodeSize.x,
            nodeSize.y,
            { getTexAspect(snapshot, TextureID::Node), 1.0f },
            config.visual.noteFillMode,
            finalNodeColor);

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

void NoteRenderSystem::drawPolylineHead(
    Batcher& batcher, const NoteComponent& note, const ScrollCache* cache,
    RenderSnapshot* snapshot, float judgmentLineY, float leftX,
    float singleTrackW, float renderScaleY, double currentAbsY,
    double currentTime, float topY, float bottomY, float noteW, float noteH,
    glm::vec4 colorHold, const Config::EditorConfig& config,
    entt::entity entity, bool generateHitboxes, HoverPart glowPart,
    int glowSubIndex)
{
    if ( glowPart != HoverPart::None &&
         !(glowPart == HoverPart::PolylineNode && glowSubIndex == 0) )
        return;

    if ( note.m_subNotes.empty() ) return;

    const auto& firstSub = note.m_subNotes.front();

    double displayDeltaStart = cache->getDisplayDelta(
        firstSub.timestamp, currentAbsY, firstSub.timestamp);
    double displayDeltaEnd = displayDeltaStart;

    double maxDelta =
        (judgmentLineY - topY) / static_cast<double>(renderScaleY);
    double minDelta =
        (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
    double padDelta = noteH / static_cast<double>(renderScaleY);

    if ( !NoteRenderSystem::isCarrierVisible(firstSub.timestamp,
                                             firstSub.timestamp,
                                             currentTime,
                                             displayDeltaStart,
                                             displayDeltaEnd,
                                             maxDelta + padDelta,
                                             minDelta - padDelta) ) {
        return;
    }

    float headY =
        judgmentLineY - static_cast<float>(displayDeltaStart) * renderScaleY;
    glm::vec2 headSize = getDrawSize(snapshot, TextureID::Note, noteW, noteH);
    float     headX    = leftX + firstSub.trackIndex * singleTrackW +
                         (singleTrackW - headSize.x) * 0.5f;

    glm::vec4 finalHeadColor = colorHold;
    if ( snapshot->erasingEntities.count(entity) &&
         (snapshot->erasingSubIndex == 0 || snapshot->erasingSubIndex == -1) ) {
        finalHeadColor = { 1.0f, 0.2f, 0.2f, colorHold.a * 0.5f };
    }

    batcher.setTexture(TextureID::Note);
    batcher.pushFilledQuad(headX,
                           headY + headSize.y * 0.5f,
                           headSize.x,
                           headSize.y,
                           { getTexAspect(snapshot, TextureID::Note), 1.0f },
                           config.visual.noteFillMode,
                           finalHeadColor);

    if ( generateHitboxes && entity != entt::null ) {
        snapshot->hitboxes.push_back({ entity,
                                       HoverPart::PolylineNode,
                                       0,
                                       headX,
                                       headY - headSize.y * 0.5f,
                                       headSize.x,
                                       headSize.y });
    }
}

void NoteRenderSystem::drawPolylineDecoration(
    Batcher& batcher, const NoteComponent& note, const ScrollCache* cache,
    RenderSnapshot* snapshot, float judgmentLineY, float leftX,
    float singleTrackW, float renderScaleY, double currentAbsY,
    double currentTime, float topY, float bottomY, float noteW, float noteH,
    glm::vec4 colorHold, glm::vec4 colorArrow,
    const Config::EditorConfig& config, entt::entity entity,
    bool generateHitboxes, HoverPart glowPart, int glowSubIndex)
{
    if ( note.m_subNotes.empty() ) return;

    int         lastIdx = static_cast<int>(note.m_subNotes.size() - 1);
    const auto& last    = note.m_subNotes.back();
    bool        isLastGlow =
        (glowPart == HoverPart::None) ||
        (glowPart == HoverPart::FlickArrow && glowSubIndex == lastIdx) ||
        (glowPart == HoverPart::HoldEnd && glowSubIndex == lastIdx);

    if ( !isLastGlow ) return;

    double targetTime = last.timestamp;
    if ( last.type == ::MMM::NoteType::HOLD ) {
        targetTime = last.timestamp + last.duration;
    }
    double displayDelta =
        cache->getDisplayDelta(targetTime, currentAbsY, targetTime);
    double maxDelta =
        (judgmentLineY - topY) / static_cast<double>(renderScaleY);
    double minDelta =
        (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
    double padDelta = noteH / static_cast<double>(renderScaleY);

    if ( !NoteRenderSystem::isCarrierVisible(targetTime,
                                             targetTime,
                                             currentTime,
                                             displayDelta,
                                             displayDelta,
                                             maxDelta + padDelta,
                                             minDelta - padDelta) ) {
        return;
    }

    float lStartY =
        judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                            last.timestamp, currentAbsY, last.timestamp)) *
                            renderScaleY;

    if ( last.type == ::MMM::NoteType::FLICK ) {
        if ( glowPart == HoverPart::None ||
             glowPart == HoverPart::FlickArrow ) {
            TextureID arrowId = (last.dtrack < 0) ? TextureID::FlickArrowLeft
                                                  : TextureID::FlickArrowRight;
            glm::vec2 arrowSize = getDrawSize(snapshot, arrowId, noteW, noteH);
            float     lEndTrack = (float)last.trackIndex + last.dtrack;
            float     arrowX    = leftX + lEndTrack * singleTrackW +
                                  (singleTrackW - arrowSize.x) * 0.5f;

            glm::vec4 finalArrowColor = colorArrow;
            if ( snapshot->erasingEntities.count(entity) &&
                 (snapshot->erasingSubIndex == lastIdx ||
                  snapshot->erasingSubIndex == 0 ||
                  snapshot->erasingSubIndex == -1) ) {
                finalArrowColor = { 1.0f, 0.2f, 0.2f, colorArrow.a * 0.5f };
            }

            batcher.setTexture(arrowId);
            batcher.pushFilledQuad(arrowX,
                                   lStartY + arrowSize.y * 0.5f,
                                   arrowSize.x,
                                   arrowSize.y,
                                   { getTexAspect(snapshot, arrowId), 1.0f },
                                   config.visual.noteFillMode,
                                   finalArrowColor);

            if ( generateHitboxes && entity != entt::null ) {
                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::FlickArrow,
                                               lastIdx,
                                               arrowX,
                                               lStartY - arrowSize.y * 0.5f,
                                               arrowSize.x,
                                               arrowSize.y });
            }
        }
    } else if ( last.type == ::MMM::NoteType::HOLD ) {
        if ( glowPart == HoverPart::None || glowPart == HoverPart::HoldEnd ) {
            float subEndY =
                judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                                    last.timestamp + last.duration,
                                    currentAbsY,
                                    last.timestamp)) *
                                    renderScaleY;
            glm::vec2 endSize =
                getDrawSize(snapshot, TextureID::HoldEnd, noteW, noteH);
            float endX = leftX + last.trackIndex * singleTrackW +
                         (singleTrackW - endSize.x) * 0.5f;

            glm::vec4 finalEndColor = colorHold;
            if ( snapshot->erasingEntities.count(entity) &&
                 (snapshot->erasingSubIndex == lastIdx ||
                  snapshot->erasingSubIndex == 0 ||
                  snapshot->erasingSubIndex == -1) ) {
                finalEndColor = { 1.0f, 0.2f, 0.2f, colorHold.a * 0.5f };
            }

            batcher.setTexture(TextureID::HoldEnd);
            batcher.pushFilledQuad(
                endX,
                subEndY + endSize.y * 0.5f,
                endSize.x,
                endSize.y,
                { getTexAspect(snapshot, TextureID::HoldEnd), 1.0f },
                config.visual.noteFillMode,
                finalEndColor);

            if ( generateHitboxes && entity != entt::null ) {
                snapshot->hitboxes.push_back({ entity,
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

}  // namespace MMM::Logic::System
