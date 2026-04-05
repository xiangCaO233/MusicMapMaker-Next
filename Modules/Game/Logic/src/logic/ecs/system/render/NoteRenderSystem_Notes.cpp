#include "Batcher.h"
#include "config/skin/SkinConfig.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include <unordered_set>

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

    // 1. 获取基准纹理 (Note) 的比例与信息
    auto itBase = snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Note));
    if ( itBase == snapshot->uvMap.end() ) return;
    float baseWRatio = itBase->second.z;
    float baseHRatio = itBase->second.w;
    float baseAspect = baseWRatio / baseHRatio;

    // 2. 计算基准尺寸 (画布空间)
    float noteW = singleTrackW * config.noteScaleX;
    float noteH = noteW / baseAspect;

    // 3. 定义辅助 Lambda 计算任意纹理在画布上的比例尺寸
    auto getDrawSize = [&](TextureID id) -> glm::vec2 {
        auto it = snapshot->uvMap.find(static_cast<uint32_t>(id));
        if ( it == snapshot->uvMap.end() ) return { noteW, noteH };
        float wRatio = it->second.z / baseWRatio;
        float drawW  = noteW * wRatio;
        float drawH  = drawW * (it->second.w / it->second.z);
        return { drawW, drawH };
    };

    auto getTexAspect = [&](TextureID id) -> float {
        auto it = snapshot->uvMap.find(static_cast<uint32_t>(id));
        if ( it == snapshot->uvMap.end() ) return 1.0f;
        return it->second.z / it->second.w;
    };

    // 4. 获取皮肤配色方案
    auto& skin   = Config::SkinManager::instance();
    auto  toVec4 = [](const Config::Color& c) {
        return glm::vec4(c.r, c.g, c.b, c.a);
    };

    glm::vec4 colorTap   = toVec4(skin.getColor("note_tap"));
    glm::vec4 colorHold  = toVec4(skin.getColor("note_hold"));
    glm::vec4 colorNode  = toVec4(skin.getColor("note_node"));
    glm::vec4 colorArrow = toVec4(skin.getColor("note_flick_arrow"));

    // 5. 搜集所有可见物件（包括子物件溯源父物件）
    std::unordered_set<entt::entity> visibleEntities;
    for ( auto entity : noteView ) {
        const auto& transform = noteView.get<const TransformComponent>(entity);
        const auto& note      = noteView.get<const NoteComponent>(entity);

        float minY    = transform.m_pos.y;
        float visualH = transform.m_size.y;
        float screenY = judgmentLineY - minY;

        // 视口剔除 (Y 轴剔除)
        if ( screenY - visualH > bottomY || screenY < topY ) continue;

        // 同步拾取包围盒
        snapshot->hitboxes.push_back({ entity,
                                       leftX +
                                           note.m_trackIndex * singleTrackW +
                                           (singleTrackW - noteW) * 0.5f,
                                       screenY - visualH,
                                       noteW,
                                       visualH });

        // 如果是子物件，溯源父物件；否则作为独立物件渲染
        if ( note.m_isSubNote ) {
            if ( note.m_parentPolyline != entt::null ) {
                visibleEntities.insert(note.m_parentPolyline);
            }
        } else {
            visibleEntities.insert(entity);
        }
    }

    // 6. 统一渲染去重后的物件列表
    for ( auto entity : visibleEntities ) {
        if ( !registry.all_of<TransformComponent, NoteComponent>(entity) )
            continue;
        const auto& transform = registry.get<TransformComponent>(entity);
        const auto& note      = registry.get<NoteComponent>(entity);

        const InteractionComponent* interaction =
            registry.try_get<InteractionComponent>(entity);
        bool isHovered = interaction ? interaction->isHovered : false;

        float minY    = transform.m_pos.y;
        float visualH = transform.m_size.y;
        float screenY = judgmentLineY - minY;

        // 设置颜色高亮叠加
        glm::vec4 hoverTint = { 1.0f, 1.0f, 1.0f, 1.0f };
        if ( isHovered ) hoverTint = { 1.0f, 0.5f, 0.0f, 1.0f };

        if ( note.m_type == ::MMM::NoteType::NOTE ) {
            batcher.setTexture(TextureID::Note);
            batcher.pushFilledQuad(leftX + note.m_trackIndex * singleTrackW +
                                       (singleTrackW - noteW) * 0.5f,
                                   screenY + noteH * 0.5f,
                                   noteW,
                                   noteH,
                                   { getTexAspect(TextureID::Note), 1.0f },
                                   config.noteFillMode,
                                   colorTap * hoverTint);
        } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
            // 图层顺序: Body (最下) -> Head -> End (最上)
            glm::vec2 headSize = getDrawSize(TextureID::Note);
            glm::vec2 endSize  = getDrawSize(TextureID::HoldEnd);
            glm::vec2 bodySize = getDrawSize(TextureID::HoldBodyVertical);

            float headX = leftX + note.m_trackIndex * singleTrackW +
                          (singleTrackW - headSize.x) * 0.5f;
            float endX  = leftX + note.m_trackIndex * singleTrackW +
                          (singleTrackW - endSize.x) * 0.5f;
            float bodyX = leftX + note.m_trackIndex * singleTrackW +
                          (singleTrackW - bodySize.x) * 0.5f;

            // 1. Body
            batcher.setTexture(TextureID::HoldBodyVertical);
            batcher.pushQuad(
                bodyX, screenY, bodySize.x, visualH, colorHold * hoverTint);

            // 2. Head (复用 Note 纹理)
            batcher.setTexture(TextureID::Note);
            batcher.pushFilledQuad(headX,
                                   screenY + headSize.y * 0.5f,
                                   headSize.x,
                                   headSize.y,
                                   { getTexAspect(TextureID::Note), 1.0f },
                                   config.noteFillMode,
                                   colorHold * hoverTint);

            // 3. End
            batcher.setTexture(TextureID::HoldEnd);
            batcher.pushFilledQuad(endX,
                                   screenY - visualH + endSize.y * 0.5f,
                                   endSize.x,
                                   endSize.y,
                                   { getTexAspect(TextureID::HoldEnd), 1.0f },
                                   config.noteFillMode,
                                   colorHold * hoverTint);

        } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
            // 图层顺序: BodyH (最下) -> Head -> Arrow (最上)
            glm::vec2 headSize = getDrawSize(TextureID::Note);
            float     headX    = leftX + note.m_trackIndex * singleTrackW +
                                 (singleTrackW - headSize.x) * 0.5f;

            // 1. BodyH
            if ( note.m_dtrack != 0 ) {
                auto itBodyH = snapshot->uvMap.find(
                    static_cast<uint32_t>(TextureID::HoldBodyHorizontal));
                if ( itBodyH != snapshot->uvMap.end() ) {
                    float drawH = noteH * (itBodyH->second.w / baseHRatio);
                    float drawW = std::abs(note.m_dtrack) * singleTrackW;
                    float startTrack =
                        std::min((float)note.m_trackIndex,
                                 (float)note.m_trackIndex + note.m_dtrack);
                    float bodyX =
                        leftX + startTrack * singleTrackW + singleTrackW * 0.5f;

                    batcher.setTexture(TextureID::HoldBodyHorizontal);
                    batcher.pushQuad(bodyX,
                                     screenY + drawH * 0.5f,
                                     drawW,
                                     drawH,
                                     colorHold * hoverTint);
                }
            }

            // 2. Head (复用 Note 纹理)
            batcher.setTexture(TextureID::Note);
            batcher.pushQuad(headX,
                             screenY + headSize.y * 0.5f,
                             headSize.x,
                             headSize.y,
                             colorHold * hoverTint);

            // 3. Arrow
            if ( note.m_dtrack != 0 ) {
                TextureID arrowId   = (note.m_dtrack < 0)
                                          ? TextureID::FlickArrowLeft
                                          : TextureID::FlickArrowRight;
                glm::vec2 arrowSize = getDrawSize(arrowId);
                float     arrowX =
                    leftX + (note.m_trackIndex + note.m_dtrack) * singleTrackW +
                    (singleTrackW - arrowSize.x) * 0.5f;
                batcher.setTexture(arrowId);
                batcher.pushFilledQuad(arrowX,
                                       screenY + arrowSize.y * 0.5f,
                                       arrowSize.x,
                                       arrowSize.y,
                                       { getTexAspect(arrowId), 1.0f },
                                       config.noteFillMode,
                                       colorArrow * hoverTint);
            }
        } else if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
            // Polyline 图层排序逻辑 (分轮绘制)

            // 第一轮: 绘制所有 Body (BodyH, BodyV, Transition)
            for ( size_t i = 0; i < note.m_subNotes.size(); ++i ) {
                const auto& sub          = note.m_subNotes[i];
                double      subStartAbsY = cache->getAbsY(sub.timestamp);
                float       subStartY =
                    judgmentLineY -
                    static_cast<float>(subStartAbsY - currentAbsY);

                float subEndTrack = (float)sub.trackIndex;
                float subEndY     = subStartY;

                // 子物件自身 Body
                if ( sub.type == ::MMM::NoteType::FLICK && sub.dtrack != 0 ) {
                    subEndTrack  = (float)sub.trackIndex + sub.dtrack;
                    auto itBodyH = snapshot->uvMap.find(
                        static_cast<uint32_t>(TextureID::HoldBodyHorizontal));
                    if ( itBodyH != snapshot->uvMap.end() ) {
                        float drawH = noteH * (itBodyH->second.w / baseHRatio);
                        float drawW = std::abs(sub.dtrack) * singleTrackW;
                        float startTrack =
                            std::min((float)sub.trackIndex, subEndTrack);
                        float bodyX = leftX + startTrack * singleTrackW +
                                      singleTrackW * 0.5f;
                        batcher.setTexture(TextureID::HoldBodyHorizontal);
                        batcher.pushQuad(bodyX,
                                         subStartY + drawH * 0.5f,
                                         drawW,
                                         drawH,
                                         colorHold * hoverTint);
                    }
                } else if ( sub.type == ::MMM::NoteType::HOLD &&
                            sub.duration > 0 ) {
                    double subEndAbsY =
                        cache->getAbsY(sub.timestamp + sub.duration);
                    subEndY = judgmentLineY -
                              static_cast<float>(subEndAbsY - currentAbsY);
                    glm::vec2 bodySize =
                        getDrawSize(TextureID::HoldBodyVertical);
                    float bodyX = leftX + sub.trackIndex * singleTrackW +
                                  (singleTrackW - bodySize.x) * 0.5f;
                    batcher.setTexture(TextureID::HoldBodyVertical);
                    batcher.pushQuad(bodyX,
                                     subStartY,
                                     bodySize.x,
                                     subStartY - subEndY,
                                     colorHold * hoverTint);
                }

                // 过渡 Body
                if ( i + 1 < note.m_subNotes.size() ) {
                    const auto& next          = note.m_subNotes[i + 1];
                    double      nextStartAbsY = cache->getAbsY(next.timestamp);
                    float       nextStartY =
                        judgmentLineY -
                        static_cast<float>(nextStartAbsY - currentAbsY);
                    glm::vec2 bodySize =
                        getDrawSize(TextureID::HoldBodyVertical);
                    float curBodyX  = leftX + subEndTrack * singleTrackW +
                                      (singleTrackW - bodySize.x) * 0.5f;
                    float nextBodyX = leftX + next.trackIndex * singleTrackW +
                                      (singleTrackW - bodySize.x) * 0.5f;
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
                    static_cast<float>(subStartAbsY - currentAbsY);
                glm::vec2 nodeSize = getDrawSize(TextureID::Node);
                float     nodeX    = leftX + sub.trackIndex * singleTrackW +
                                     (singleTrackW - nodeSize.x) * 0.5f;
                batcher.pushFilledQuad(nodeX,
                                       subStartY + nodeSize.y * 0.5f,
                                       nodeSize.x,
                                       nodeSize.y,
                                       { getTexAspect(TextureID::Node), 1.0f },
                                       config.noteFillMode,
                                       colorNode * hoverTint);
            }

            // 第三轮: 绘制起始 HoldHead (复用 Note 纹理)
            const auto& first      = note.m_subNotes[0];
            double      fStartAbsY = cache->getAbsY(first.timestamp);
            float       fStartY =
                judgmentLineY - static_cast<float>(fStartAbsY - currentAbsY);
            glm::vec2 headSize = getDrawSize(TextureID::Note);
            float     headX    = leftX + first.trackIndex * singleTrackW +
                                 (singleTrackW - headSize.x) * 0.5f;
            batcher.setTexture(TextureID::Note);
            batcher.pushFilledQuad(headX,
                                   fStartY + headSize.y * 0.5f,
                                   headSize.x,
                                   headSize.y,
                                   { getTexAspect(TextureID::Note), 1.0f },
                                   config.noteFillMode,
                                   colorHold * hoverTint);

            // 第四轮: 绘制结尾特殊样式 (End 或 Arrow)
            const auto& last       = note.m_subNotes.back();
            double      lStartAbsY = cache->getAbsY(last.timestamp);
            float       lStartY =
                judgmentLineY - static_cast<float>(lStartAbsY - currentAbsY);
            float lEndTrack =
                (float)last.trackIndex +
                (last.type == ::MMM::NoteType::FLICK ? last.dtrack : 0);
            float lEndY =
                lStartY -
                (last.type == ::MMM::NoteType::HOLD
                     ? static_cast<float>(
                           cache->getAbsY(last.timestamp + last.duration) -
                           lStartAbsY)
                     : 0);

            if ( last.type == ::MMM::NoteType::FLICK ) {
                TextureID arrowId   = (last.dtrack < 0)
                                          ? TextureID::FlickArrowLeft
                                          : TextureID::FlickArrowRight;
                glm::vec2 arrowSize = getDrawSize(arrowId);
                float     arrowX    = leftX + lEndTrack * singleTrackW +
                                      (singleTrackW - arrowSize.x) * 0.5f;
                batcher.setTexture(arrowId);
                batcher.pushFilledQuad(arrowX,
                                       lStartY + arrowSize.y * 0.5f,
                                       arrowSize.x,
                                       arrowSize.y,
                                       { getTexAspect(arrowId), 1.0f },
                                       config.noteFillMode,
                                       colorArrow * hoverTint);
            } else if ( last.type == ::MMM::NoteType::HOLD ) {
                glm::vec2 endSize = getDrawSize(TextureID::HoldEnd);
                float     endX    = leftX + last.trackIndex * singleTrackW +
                                    (singleTrackW - endSize.x) * 0.5f;
                batcher.setTexture(TextureID::HoldEnd);
                batcher.pushFilledQuad(
                    endX,
                    lEndY + endSize.y * 0.5f,
                    endSize.x,
                    endSize.y,
                    { getTexAspect(TextureID::HoldEnd), 1.0f },
                    config.noteFillMode,
                    colorHold * hoverTint);
            }
        }
    }
}

}  // namespace MMM::Logic::System
