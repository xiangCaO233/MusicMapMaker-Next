#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"

#include <algorithm>
#include <cmath>

namespace MMM::Logic::System
{

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

static float getTexAspect(RenderSnapshot* snapshot, TextureID id)
{
    auto it = snapshot->uvMap.find(static_cast<uint32_t>(id));
    if ( it == snapshot->uvMap.end() ) return 1.0f;
    return it->second.z / it->second.w;
}

void NoteRenderSystem::renderTap(Batcher&                           batcher,
                                 const ::MMM::Logic::NoteComponent& note,
                                 const Config::EditorConfig& config, float x,
                                 float y, float w, float h, float aspect,
                                 glm::vec4 color)
{
    batcher.setTexture(TextureID::Note);
    batcher.pushFilledQuad(x,
                           y + h * 0.5f,
                           w,
                           h,
                           { aspect, 1.0f },
                           config.visual.noteFillMode,
                           color);
}

void NoteRenderSystem::renderHold(Batcher&                           batcher,
                                  const ::MMM::Logic::NoteComponent& note,
                                  const Config::EditorConfig&        config,
                                  RenderSnapshot* snapshot, float x, float w,
                                  float h, float singleTrackW,
                                  glm::vec4 headColor, glm::vec4 bodyColor,
                                  glm::vec4 endColor, const ScrollCache* cache,
                                  double currentAbsY, float judgmentLineY,
                                  float renderScaleY, HoverPart glowPart)
{
    glm::vec2 headSize = getDrawSize(snapshot, TextureID::Note, w, h);
    glm::vec2 endSize  = getDrawSize(snapshot, TextureID::HoldEnd, w, h);
    glm::vec2 bodySize =
        getDrawSize(snapshot, TextureID::HoldBodyVertical, w, h);

    float  headX       = x;
    float  endX        = x + (w - endSize.x) * 0.5f;
    float  bodyX       = x + (w - bodySize.x) * 0.5f;
    double holdEndTime = note.m_timestamp + note.m_duration;

    float headY =
        judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                            note.m_timestamp, currentAbsY, note.m_timestamp)) *
                            renderScaleY;
    float endY =
        judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                            holdEndTime, currentAbsY, note.m_timestamp)) *
                            renderScaleY;

    // 1. 连接体。
    if ( glowPart == HoverPart::None || glowPart == HoverPart::HoldBody ) {
        batcher.setTexture(TextureID::HoldBodyVertical);
        float sy = judgmentLineY -
                   static_cast<float>(cache->getDisplayDelta(
                       note.m_timestamp, currentAbsY, note.m_timestamp)) *
                       renderScaleY;
        float ey =
            judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                                holdEndTime, currentAbsY, note.m_timestamp)) *
                                renderScaleY;
        batcher.pushFreeQuad({ bodyX, sy },
                             { bodyX + bodySize.x, sy },
                             { bodyX + bodySize.x, ey },
                             { bodyX, ey },
                             bodyColor);
    }

    // 2. 头部。
    if ( glowPart == HoverPart::None || glowPart == HoverPart::Head ) {
        batcher.setTexture(TextureID::Note);
        batcher.pushFilledQuad(
            headX,
            headY + headSize.y * 0.5f,
            headSize.x,
            headSize.y,
            { getTexAspect(snapshot, TextureID::Note), 1.0f },
            config.visual.noteFillMode,
            headColor);
    }

    // 3. 尾部。
    if ( glowPart == HoverPart::None || glowPart == HoverPart::HoldEnd ) {
        batcher.setTexture(TextureID::HoldEnd);
        batcher.pushFilledQuad(
            endX,
            endY + endSize.y * 0.5f,
            endSize.x,
            endSize.y,
            { getTexAspect(snapshot, TextureID::HoldEnd), 1.0f },
            config.visual.noteFillMode,
            endColor);
    }
}

void NoteRenderSystem::renderFlick(Batcher&                           batcher,
                                   const ::MMM::Logic::NoteComponent& note,
                                   const Config::EditorConfig&        config,
                                   RenderSnapshot* snapshot, float x, float y,
                                   float w, float h, float singleTrackW,
                                   glm::vec4 headColor, glm::vec4 bodyColor,
                                   glm::vec4 arrowColor, HoverPart glowPart)
{
    glm::vec2 headSize = getDrawSize(snapshot, TextureID::Note, w, h);
    float     headX    = x;

    // 1. 横向连接体。
    if ( note.m_dtrack != 0 &&
         (glowPart == HoverPart::None || glowPart == HoverPart::HoldBody) ) {
        auto itBodyH = snapshot->uvMap.find(
            static_cast<uint32_t>(TextureID::HoldBodyHorizontal));
        if ( itBodyH != snapshot->uvMap.end() ) {
            float drawH = h * (itBodyH->second.w /
                               snapshot->uvMap.at(uint32_t(TextureID::Note)).w);
            float drawW = std::abs(note.m_dtrack) * singleTrackW;
            float startTrack = std::min(0.0f, (float)note.m_dtrack);
            float bodyX      = x + (w - singleTrackW) * 0.5f +
                               startTrack * singleTrackW + singleTrackW * 0.5f;

            batcher.setTexture(TextureID::HoldBodyHorizontal);
            batcher.pushQuad(bodyX, y + drawH * 0.5f, drawW, drawH, bodyColor);
        }
    }

    // 2. 头部。
    if ( glowPart == HoverPart::None || glowPart == HoverPart::Head ) {
        batcher.setTexture(TextureID::Note);
        batcher.pushFilledQuad(
            headX,
            y + headSize.y * 0.5f,
            headSize.x,
            headSize.y,
            { getTexAspect(snapshot, TextureID::Note), 1.0f },
            config.visual.noteFillMode,
            headColor);
    }

    // 3. 箭头。
    if ( note.m_dtrack != 0 &&
         (glowPart == HoverPart::None || glowPart == HoverPart::FlickArrow) ) {
        TextureID arrowId   = (note.m_dtrack < 0) ? TextureID::FlickArrowLeft
                                                  : TextureID::FlickArrowRight;
        glm::vec2 arrowSize = getDrawSize(snapshot, arrowId, w, h);
        float     arrowX    = x + (w - singleTrackW) * 0.5f +
                              note.m_dtrack * singleTrackW +
                              (singleTrackW - arrowSize.x) * 0.5f;
        batcher.setTexture(arrowId);
        batcher.pushFilledQuad(arrowX,
                               y + arrowSize.y * 0.5f,
                               arrowSize.x,
                               arrowSize.y,
                               { getTexAspect(snapshot, arrowId), 1.0f },
                               config.visual.noteFillMode,
                               arrowColor);
    }
}

}  // namespace MMM::Logic::System
