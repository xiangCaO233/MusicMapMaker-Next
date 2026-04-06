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
    entt::registry& registry, Batcher& batcher,
    const ::MMM::Logic::NoteComponent& note, const Config::EditorConfig& config,
    RenderSnapshot* snapshot, double currentTime, float judgmentLineY,
    float leftX, float rightX, float topY, float bottomY, float singleTrackW,
    float renderScaleY, glm::vec4 colorHold, glm::vec4 colorNode,
    glm::vec4 colorArrow, glm::vec4 hoverTint)
{
    const auto** cachePtr = registry.ctx().find<const ScrollCache*>();
    if ( !cachePtr || !(*cachePtr) ) return;
    const ScrollCache* cache       = *cachePtr;
    double             currentAbsY = cache->getAbsY(currentTime);

    float noteW = singleTrackW * config.visual.noteScaleX;
    float noteH = noteW / getTexAspect(snapshot, TextureID::Note);

    // 第一轮: 绘制所有 Body (BodyH, BodyV, Transition)
    for ( size_t i = 0; i < note.m_subNotes.size(); ++i ) {
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
                    noteH * (itBodyH->second.w /
                             snapshot->uvMap.at(uint32_t(TextureID::Note)).w);
                float drawW      = std::abs(sub.dtrack) * singleTrackW;
                float startTrack = std::min((float)sub.trackIndex, subEndTrack);
                float bodyX =
                    leftX + startTrack * singleTrackW + singleTrackW * 0.5f;
                batcher.setTexture(TextureID::HoldBodyHorizontal);
                batcher.pushQuad(bodyX,
                                 subStartY + drawH * 0.5f,
                                 drawW,
                                 drawH,
                                 colorHold * hoverTint);
            }
        } else if ( sub.type == ::MMM::NoteType::HOLD && sub.duration > 0 ) {
            double subEndAbsY = cache->getAbsY(sub.timestamp + sub.duration);
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
                             colorHold * hoverTint);
        } else {
            // 如果是 NOTE 类型，也要确保 subEndY 正确 (虽然不画 body)
            subEndY = subStartY;
        }

        // 过渡 Body (连接当前子物件末尾到下一个子物件开头)
        if ( i + 1 < note.m_subNotes.size() ) {
            const auto& next          = note.m_subNotes[i + 1];
            double      nextStartAbsY = cache->getAbsY(next.timestamp);
            float       nextStartY =
                judgmentLineY -
                static_cast<float>(nextStartAbsY - currentAbsY) * renderScaleY;
            glm::vec2 bodySize = getDrawSize(
                snapshot, TextureID::HoldBodyVertical, noteW, noteH);
            float curBodyX  = leftX + subEndTrack * singleTrackW +
                              (singleTrackW - bodySize.x) * 0.5f;
            float nextBodyX = leftX + next.trackIndex * singleTrackW +
                              (singleTrackW - bodySize.x) * 0.5f;

            // 核心修复：强制设置为 HoldBodyVertical 纹理，防止泄露
            batcher.setTexture(TextureID::HoldBodyVertical);
            batcher.pushFreeQuad({ curBodyX, subEndY },
                                 { curBodyX + bodySize.x, subEndY },
                                 { nextBodyX + bodySize.x, nextStartY },
                                 { nextBodyX, nextStartY },
                                 colorHold * hoverTint);
        }
    }

    // 第二轮: 绘制 Node 装饰 (所有折点)
    batcher.setTexture(TextureID::Node);
    for ( size_t i = 1; i < note.m_subNotes.size(); ++i ) {
        const auto& sub          = note.m_subNotes[i];
        double      subStartAbsY = cache->getAbsY(sub.timestamp);
        float       subStartY =
            judgmentLineY -
            static_cast<float>(subStartAbsY - currentAbsY) * renderScaleY;
        glm::vec2 nodeSize =
            getDrawSize(snapshot, TextureID::Node, noteW, noteH);
        float nodeX = leftX + sub.trackIndex * singleTrackW +
                      (singleTrackW - nodeSize.x) * 0.5f;
        batcher.pushFilledQuad(
            nodeX,
            subStartY + nodeSize.y * 0.5f,
            nodeSize.x,
            nodeSize.y,
            { getTexAspect(snapshot, TextureID::Node), 1.0f },
            config.visual.noteFillMode,
            colorNode * hoverTint);
    }

    // 第三轮: 绘制起始 HoldHead
    const auto& first      = note.m_subNotes[0];
    double      fStartAbsY = cache->getAbsY(first.timestamp);
    float fStartY = judgmentLineY -
                    static_cast<float>(fStartAbsY - currentAbsY) * renderScaleY;
    glm::vec2 headSize = getDrawSize(snapshot, TextureID::Note, noteW, noteH);
    float     headX    = leftX + first.trackIndex * singleTrackW +
                         (singleTrackW - headSize.x) * 0.5f;
    batcher.setTexture(TextureID::Note);
    batcher.pushFilledQuad(headX,
                           fStartY + headSize.y * 0.5f,
                           headSize.x,
                           headSize.y,
                           { getTexAspect(snapshot, TextureID::Note), 1.0f },
                           config.visual.noteFillMode,
                           colorHold * hoverTint);

    // 第四轮: 绘制结尾特殊样式
    const auto& last       = note.m_subNotes.back();
    double      lStartAbsY = cache->getAbsY(last.timestamp);
    float lStartY = judgmentLineY -
                    static_cast<float>(lStartAbsY - currentAbsY) * renderScaleY;
    float lEndTrack = (float)last.trackIndex +
                      (last.type == ::MMM::NoteType::FLICK ? last.dtrack : 0);
    float lEndY =
        lStartY - (last.type == ::MMM::NoteType::HOLD
                       ? static_cast<float>(
                             cache->getAbsY(last.timestamp + last.duration) -
                             lStartAbsY) *
                             renderScaleY
                       : 0);

    if ( last.type == ::MMM::NoteType::FLICK ) {
        TextureID arrowId   = (last.dtrack < 0) ? TextureID::FlickArrowLeft
                                                : TextureID::FlickArrowRight;
        glm::vec2 arrowSize = getDrawSize(snapshot, arrowId, noteW, noteH);
        float     arrowX    = leftX + lEndTrack * singleTrackW +
                              (singleTrackW - arrowSize.x) * 0.5f;
        batcher.setTexture(arrowId);
        batcher.pushFilledQuad(arrowX,
                               lStartY + arrowSize.y * 0.5f,
                               arrowSize.x,
                               arrowSize.y,
                               { getTexAspect(snapshot, arrowId), 1.0f },
                               config.visual.noteFillMode,
                               colorArrow * hoverTint);
    } else if ( last.type == ::MMM::NoteType::HOLD ) {
        glm::vec2 endSize =
            getDrawSize(snapshot, TextureID::HoldEnd, noteW, noteH);
        float endX = leftX + last.trackIndex * singleTrackW +
                     (singleTrackW - endSize.x) * 0.5f;
        batcher.setTexture(TextureID::HoldEnd);
        batcher.pushFilledQuad(
            endX,
            lEndY + endSize.y * 0.5f,
            endSize.x,
            endSize.y,
            { getTexAspect(snapshot, TextureID::HoldEnd), 1.0f },
            config.visual.noteFillMode,
            colorHold * hoverTint);
    }
}

}  // namespace MMM::Logic::System
