#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
#include "logic/ecs/system/render/NoteLaneGeometry.h"

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

/// @brief 获取 Polyline 子物件主体末端时间。
/// @warning 热路径：Polyline 几何生成时按子物件调用；保持纯计算，不得分配。
static double getSubCarrierEndTime(const NoteComponent::SubNote& sub)
{
    if ( sub.type == ::MMM::NoteType::HOLD ) {
        return sub.timestamp + sub.duration;
    }
    return sub.timestamp;
}

/// @brief 获取 Polyline 子物件主体末端 HS 锚点时间。
/// @warning 热路径：Polyline 几何生成时按子物件调用；保持纯计算，不得访问缓存。
static double getSubCarrierEndAnchorTime(const NoteComponent::SubNote& sub)
{
    if ( sub.type == ::MMM::NoteType::HOLD ) {
        return sub.timestamp;
    }
    return getSubCarrierEndTime(sub);
}

void NoteRenderSystem::renderPolyline(
    const ScrollCache* cache, Batcher& batcher, const NoteComponent& note,
    const Config::EditorConfig& config, RenderSnapshot* snapshot,
    double currentAbsY, double currentTime, float judgmentLineY, float leftX,
    float topY, float bottomY, float singleTrackW, float renderScaleY,
    glm::vec4 colorHead, glm::vec4 colorHoldBody, glm::vec4 colorHoldEnd,
    glm::vec4 colorNode, glm::vec4 colorArrow, entt::entity entity,
    bool generateHitboxes, HoverPart glowPart, int glowSubIndex,
    const CanvasLaneProjection* laneProjection)
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
                     colorHoldBody,
                     entity,
                     generateHitboxes,
                     glowPart,
                     glowSubIndex,
                     laneProjection);

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
                      glowSubIndex,
                      laneProjection);

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
                     colorHead,
                     config,
                     entity,
                     generateHitboxes,
                     glowPart,
                     glowSubIndex,
                     laneProjection);

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
                           colorHoldEnd,
                           colorArrow,
                           config,
                           entity,
                           generateHitboxes,
                           glowPart,
                           glowSubIndex,
                           laneProjection);
}

void NoteRenderSystem::drawPolylineBody(
    Batcher& batcher, const NoteComponent& note, const ScrollCache* cache,
    RenderSnapshot* snapshot, float judgmentLineY, float leftX,
    float singleTrackW, float renderScaleY, double currentAbsY,
    double currentTime, float topY, float bottomY, float noteW, float noteH,
    glm::vec4 colorHold, entt::entity entity, bool generateHitboxes,
    HoverPart glowPart, int glowSubIndex,
    const CanvasLaneProjection* laneProjection)
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
        const double subEndTime       = getSubCarrierEndTime(sub);
        const double subEndAnchorTime = getSubCarrierEndAnchorTime(sub);
        double       displayDeltaEnd =
            cache->getDisplayDelta(subEndTime, currentAbsY, subEndAnchorTime);

        double maxDelta =
            (judgmentLineY - topY) / static_cast<double>(renderScaleY);
        double minDelta =
            (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
        const auto startLane = resolveNoteLaneGeometry(
            sub.trackIndex, laneProjection, leftX, singleTrackW, noteW, noteH);
        const std::int32_t subEndTrack = sub.type == ::MMM::NoteType::FLICK
                                             ? sub.trackIndex + sub.dtrack
                                             : sub.trackIndex;
        const auto         endLane     = resolveNoteLaneGeometry(
            subEndTrack, laneProjection, leftX, singleTrackW, noteW, noteH);
        const double padDelta = std::max(startLane.noteH, endLane.noteH) /
                                static_cast<double>(renderScaleY);

        if ( !NoteRenderSystem::isCarrierVisible(sub.timestamp,
                                                 subEndTime,
                                                 currentTime,
                                                 displayDeltaStart,
                                                 displayDeltaEnd,
                                                 maxDelta + padDelta,
                                                 minDelta - padDelta) ) {
            continue;
        }

        float subStartY = judgmentLineY -
                          static_cast<float>(displayDeltaStart) * renderScaleY;

        float subEndY = subStartY;

        // 子物件自身 Body (水平 Flick 或 垂直 Hold)
        if ( sub.type == ::MMM::NoteType::FLICK && sub.dtrack != 0 ) {
            auto itBodyH = snapshot->uvMap.find(
                static_cast<uint32_t>(TextureID::HoldBodyHorizontal));
            if ( itBodyH != snapshot->uvMap.end() ) {
                const float drawH =
                    startLane.noteH *
                    (itBodyH->second.w /
                     snapshot->uvMap.at(uint32_t(TextureID::Note)).w);
                // Flick
                // 连接体跨越两个真实轨道中心，独立区域间隙也属于连接范围。
                const float drawW =
                    std::abs(endLane.centerX() - startLane.centerX());
                const float bodyX =
                    std::min(startLane.centerX(), endLane.centerX());

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
            subEndY = judgmentLineY -
                      static_cast<float>(cache->getDisplayDelta(
                          subEndTime, currentAbsY, subEndAnchorTime)) *
                          renderScaleY;
            const glm::vec2 bodySize = getDrawSize(snapshot,
                                                   TextureID::HoldBodyVertical,
                                                   startLane.noteW,
                                                   startLane.noteH);
            const float     bodyX =
                startLane.leftX + (startLane.width - bodySize.x) * 0.5F;

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
            float ey = judgmentLineY -
                       static_cast<float>(cache->getDisplayDelta(
                           subEndTime, currentAbsY, subEndAnchorTime)) *
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
            const auto      nextLane = resolveNoteLaneGeometry(next.trackIndex,
                                                          laneProjection,
                                                          leftX,
                                                          singleTrackW,
                                                          noteW,
                                                          noteH);
            const glm::vec2 curBodySize =
                getDrawSize(snapshot,
                            TextureID::HoldBodyVertical,
                            endLane.noteW,
                            endLane.noteH);
            const glm::vec2 nextBodySize =
                getDrawSize(snapshot,
                            TextureID::HoldBodyVertical,
                            nextLane.noteW,
                            nextLane.noteH);
            const float curBodyX =
                endLane.leftX + (endLane.width - curBodySize.x) * 0.5F;
            const float nextBodyX =
                nextLane.leftX + (nextLane.width - nextBodySize.x) * 0.5F;

            glm::vec4 finalTransColor = colorHold;
            if ( snapshot->erasingEntities.count(entity) &&
                 (snapshot->erasingSubIndex == static_cast<int>(i + 1) ||
                  snapshot->erasingSubIndex == 0 ||
                  snapshot->erasingSubIndex == -1) ) {
                finalTransColor = { 1.0f, 0.2f, 0.2f, colorHold.a * 0.5f };
            }

            batcher.setTexture(TextureID::HoldBodyVertical);

            double tStart       = getSubCarrierEndTime(sub);
            double tStartAnchor = getSubCarrierEndAnchorTime(sub);
            double tEnd         = next.timestamp;

            float sy =
                judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                                    tStart, currentAbsY, tStartAnchor)) *
                                    renderScaleY;
            float ey =
                judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                                    tEnd, currentAbsY, tEnd)) *
                                    renderScaleY;

            float x1 = curBodyX;
            float x2 = nextBodyX;

            batcher.pushFreeQuad({ x1, sy },
                                 { x1 + curBodySize.x, sy },
                                 { x2 + nextBodySize.x, ey },
                                 { x2, ey },
                                 finalTransColor);

            if ( generateHitboxes && entity != entt::null ) {
                const float xmin = std::min(curBodyX, nextBodyX);
                const float xmax = std::max(curBodyX + curBodySize.x,
                                            nextBodyX + nextBodySize.x);
                float       ymin = std::min(subEndY, nextStartY);
                float       ymax = std::max(subEndY, nextStartY);
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
    int glowSubIndex, const CanvasLaneProjection* laneProjection)
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
        const auto lane = resolveNoteLaneGeometry(
            sub.trackIndex, laneProjection, leftX, singleTrackW, noteW, noteH);
        const double padDelta = lane.noteH / static_cast<double>(renderScaleY);

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
        const glm::vec2 nodeSize =
            getDrawSize(snapshot, TextureID::Node, lane.noteW, lane.noteH);
        const float nodeX = lane.leftX + (lane.width - nodeSize.x) * 0.5F;

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
    glm::vec4 colorHead, const Config::EditorConfig& config,
    entt::entity entity, bool generateHitboxes, HoverPart glowPart,
    int glowSubIndex, const CanvasLaneProjection* laneProjection)
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
    const auto lane = resolveNoteLaneGeometry(
        firstSub.trackIndex, laneProjection, leftX, singleTrackW, noteW, noteH);
    const double padDelta = lane.noteH / static_cast<double>(renderScaleY);

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
    const glm::vec2 headSize =
        getDrawSize(snapshot, TextureID::Note, lane.noteW, lane.noteH);
    const float headX = lane.leftX + (lane.width - headSize.x) * 0.5F;

    glm::vec4 finalHeadColor = colorHead;
    if ( snapshot->erasingEntities.count(entity) &&
         (snapshot->erasingSubIndex == 0 || snapshot->erasingSubIndex == -1) ) {
        finalHeadColor = { 1.0f, 0.2f, 0.2f, colorHead.a * 0.5f };
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
    glm::vec4 colorHoldEnd, glm::vec4 colorArrow,
    const Config::EditorConfig& config, entt::entity entity,
    bool generateHitboxes, HoverPart glowPart, int glowSubIndex,
    const CanvasLaneProjection* laneProjection)
{
    if ( note.m_subNotes.empty() ) return;

    int         lastIdx = static_cast<int>(note.m_subNotes.size() - 1);
    const auto& last    = note.m_subNotes.back();
    bool        isLastGlow =
        (glowPart == HoverPart::None) ||
        (glowPart == HoverPart::FlickArrow && glowSubIndex == lastIdx) ||
        (glowPart == HoverPart::HoldEnd && glowSubIndex == lastIdx);

    if ( !isLastGlow ) return;

    double targetTime       = getSubCarrierEndTime(last);
    double targetAnchorTime = getSubCarrierEndAnchorTime(last);
    double displayDelta =
        cache->getDisplayDelta(targetTime, currentAbsY, targetAnchorTime);
    double maxDelta =
        (judgmentLineY - topY) / static_cast<double>(renderScaleY);
    double minDelta =
        (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
    const std::int32_t decorationTrack = last.type == ::MMM::NoteType::FLICK
                                             ? last.trackIndex + last.dtrack
                                             : last.trackIndex;
    const auto         lane            = resolveNoteLaneGeometry(
        decorationTrack, laneProjection, leftX, singleTrackW, noteW, noteH);
    const double padDelta = lane.noteH / static_cast<double>(renderScaleY);

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
            const glm::vec2 arrowSize =
                getDrawSize(snapshot, arrowId, lane.noteW, lane.noteH);
            const float arrowX = lane.leftX + (lane.width - arrowSize.x) * 0.5F;

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
            float subEndY = judgmentLineY -
                            static_cast<float>(cache->getDisplayDelta(
                                targetTime, currentAbsY, targetAnchorTime)) *
                                renderScaleY;
            const glm::vec2 endSize = getDrawSize(
                snapshot, TextureID::HoldEnd, lane.noteW, lane.noteH);
            const float endX = lane.leftX + (lane.width - endSize.x) * 0.5F;

            glm::vec4 finalEndColor = colorHoldEnd;
            if ( snapshot->erasingEntities.count(entity) &&
                 (snapshot->erasingSubIndex == lastIdx ||
                  snapshot->erasingSubIndex == 0 ||
                  snapshot->erasingSubIndex == -1) ) {
                finalEndColor = { 1.0f, 0.2f, 0.2f, colorHoldEnd.a * 0.5f };
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
