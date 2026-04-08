#include "config/skin/SkinConfig.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
#include <unordered_set>

namespace MMM::Logic::System
{

void NoteRenderSystem::renderNotes(
    entt::registry& registry, RenderSnapshot* snapshot,
    const std::string& cameraId, double currentTime, float judgmentLineY,
    int32_t trackCount, const Config::EditorConfig& config, Batcher& batcher,
    float leftX, float rightX, float topY, float bottomY, float singleTrackW,
    float renderScaleY)
{
    auto noteView =
        registry.view<const TransformComponent, const NoteComponent>();

    const auto** cachePtr = registry.ctx().find<const ScrollCache*>();
    if ( !cachePtr || !(*cachePtr) ) return;
    const auto* cache       = *cachePtr;
    double      currentAbsY = cache->getAbsY(currentTime);

    // 1. 获取基准纹理 (Note) 的比例与信息
    auto itBase = snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Note));
    if ( itBase == snapshot->uvMap.end() ) return;
    float baseAspect = itBase->second.z / itBase->second.w;

    // 2. 计算基准尺寸 (画布空间)
    float noteW = singleTrackW * config.visual.noteScaleX;
    float noteH = noteW / baseAspect;

    // 3. 获取皮肤配色方案
    auto& skin   = Config::SkinManager::instance();
    auto  toVec4 = [](const Config::Color& c) {
        return glm::vec4(c.r, c.g, c.b, c.a);
    };

    glm::vec4 colorTap   = toVec4(skin.getColor("note_tap"));
    glm::vec4 colorHold  = toVec4(skin.getColor("note_hold"));
    glm::vec4 colorNode  = toVec4(skin.getColor("note_node"));
    glm::vec4 colorArrow = toVec4(skin.getColor("note_flick_arrow"));

    // 4. 搜集所有可见物件（包括子物件溯源父物件）
    std::unordered_set<entt::entity> visibleEntities;
    for ( auto entity : noteView ) {
        const auto& transform = noteView.get<const TransformComponent>(entity);
        const auto& note      = noteView.get<const NoteComponent>(entity);

        // 重新基于渲染时间 (currentTime) 计算 Y 坐标以应用 visualOffset
        double noteAbsY = cache->getAbsY(note.m_timestamp);
        float  minY     = static_cast<float>(noteAbsY - currentAbsY);
        float  visualH  = transform.m_size.y * renderScaleY;
        float  screenY  = judgmentLineY - (minY * renderScaleY);

        float hitboxY = screenY;
        float hitboxH = visualH;

        if ( note.m_type == ::MMM::NoteType::NOTE ||
             note.m_type == ::MMM::NoteType::FLICK ) {
            hitboxY = screenY - noteH * 0.5f;
            hitboxH = noteH;
        } else if ( note.m_type == ::MMM::NoteType::HOLD ||
                    note.m_type == ::MMM::NoteType::POLYLINE ) {
            hitboxY = screenY - visualH - noteH * 0.5f;
            hitboxH = visualH + noteH;
        }

        // 视口剔除 (Y 轴剔除)
        if ( hitboxY > bottomY || hitboxY + hitboxH < topY ) continue;

        // 同步拾取包围盒
        snapshot->hitboxes.push_back({ entity,
                                       leftX +
                                           note.m_trackIndex * singleTrackW +
                                           (singleTrackW - noteW) * 0.5f,
                                       hitboxY,
                                       noteW,
                                       hitboxH });

        if ( note.m_isSubNote ) {
            if ( note.m_parentPolyline != entt::null ) {
                visibleEntities.insert(note.m_parentPolyline);
            }
        } else {
            visibleEntities.insert(entity);
        }
    }

    // 5. 排序：按时间戳降序排列（时间越早越晚画，从而在最上层）
    std::vector<entt::entity> sortedEntities(visibleEntities.begin(),
                                             visibleEntities.end());
    std::sort(
        sortedEntities.begin(),
        sortedEntities.end(),
        [&registry](entt::entity a, entt::entity b) {
            const auto& noteA = registry.get<NoteComponent>(a);
            const auto& noteB = registry.get<NoteComponent>(b);
            if ( std::abs(noteA.m_timestamp - noteB.m_timestamp) > 1e-6 ) {
                return noteA.m_timestamp > noteB.m_timestamp;
            }
            return a > b;  // 时间相同时按实体 ID 排序，保证稳定性
        });

    // 6. 统一渲染去重后的物件列表
    std::vector<entt::entity> hoveredEntities;
    for ( auto entity : sortedEntities ) {
        if ( !registry.all_of<TransformComponent, NoteComponent>(entity) )
            continue;
        const auto& transform = registry.get<TransformComponent>(entity);
        const auto& note      = registry.get<NoteComponent>(entity);

        const InteractionComponent* interaction =
            registry.try_get<InteractionComponent>(entity);
        bool isHovered = interaction ? interaction->isHovered : false;

        // 重新基于渲染时间 (currentTime) 计算 Y 坐标以应用 visualOffset
        double noteAbsY = cache->getAbsY(note.m_timestamp);
        float  minY     = static_cast<float>(noteAbsY - currentAbsY);
        float  visualH  = transform.m_size.y * renderScaleY;
        float  screenY  = judgmentLineY - (minY * renderScaleY);

        glm::vec4 hoverTint = { 1.0f, 1.0f, 1.0f, 1.0f };
        if ( isHovered ) {
            // 移除临时占位的橙色调，悬停现在完全依靠发光特效表现
            hoveredEntities.push_back(entity);
        }

        float trackX = leftX + note.m_trackIndex * singleTrackW;

        if ( note.m_type == ::MMM::NoteType::NOTE ) {
            NoteRenderSystem::renderTap(batcher,
                                        note,
                                        config,
                                        trackX + (singleTrackW - noteW) * 0.5f,
                                        screenY,
                                        noteW,
                                        noteH,
                                        baseAspect,
                                        colorTap * hoverTint);
        } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
            NoteRenderSystem::renderHold(batcher,
                                         note,
                                         config,
                                         snapshot,
                                         trackX + (singleTrackW - noteW) * 0.5f,
                                         screenY,
                                         noteW,
                                         noteH,
                                         visualH,
                                         singleTrackW,
                                         colorHold,
                                         hoverTint);
        } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
            NoteRenderSystem::renderFlick(
                batcher,
                note,
                config,
                snapshot,
                trackX + (singleTrackW - noteW) * 0.5f,
                screenY,
                noteW,
                noteH,
                singleTrackW,
                colorHold,
                colorArrow,
                hoverTint);
        } else if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
            NoteRenderSystem::renderPolyline(registry,
                                             batcher,
                                             note,
                                             config,
                                             snapshot,
                                             currentTime,
                                             judgmentLineY,
                                             leftX,
                                             rightX,
                                             topY,
                                             bottomY,
                                             singleTrackW,
                                             renderScaleY,
                                             colorHold,
                                             colorNode,
                                             colorArrow,
                                             hoverTint);
        }
    }
    batcher.flush();

    // 7. 渲染发光层
    if ( !hoveredEntities.empty() ) {
        Batcher glowBatcher(snapshot, &snapshot->glowCmds);
        for ( auto entity : hoveredEntities ) {
            const auto& transform = registry.get<TransformComponent>(entity);
            const auto& note      = registry.get<NoteComponent>(entity);

            double noteAbsY = cache->getAbsY(note.m_timestamp);
            float  minY     = static_cast<float>(noteAbsY - currentAbsY);
            float  visualH  = transform.m_size.y * renderScaleY;
            float  screenY  = judgmentLineY - (minY * renderScaleY);
            float  trackX   = leftX + note.m_trackIndex * singleTrackW;

            // 渲染到发光遮罩层：直接使用物件原始颜色，不再应用选中橙色调
            if ( note.m_type == ::MMM::NoteType::NOTE ) {
                NoteRenderSystem::renderTap(
                    glowBatcher,
                    note,
                    config,
                    trackX + (singleTrackW - noteW) * 0.5f,
                    screenY,
                    noteW,
                    noteH,
                    baseAspect,
                    colorTap);
            } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
                NoteRenderSystem::renderHold(
                    glowBatcher,
                    note,
                    config,
                    snapshot,
                    trackX + (singleTrackW - noteW) * 0.5f,
                    screenY,
                    noteW,
                    noteH,
                    visualH,
                    singleTrackW,
                    colorHold,
                    { 1.0f, 1.0f, 1.0f, 1.0f });
            } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
                NoteRenderSystem::renderFlick(
                    glowBatcher,
                    note,
                    config,
                    snapshot,
                    trackX + (singleTrackW - noteW) * 0.5f,
                    screenY,
                    noteW,
                    noteH,
                    singleTrackW,
                    colorHold,
                    colorArrow,
                    { 1.0f, 1.0f, 1.0f, 1.0f });
            } else if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
                NoteRenderSystem::renderPolyline(registry,
                                                 glowBatcher,
                                                 note,
                                                 config,
                                                 snapshot,
                                                 currentTime,
                                                 judgmentLineY,
                                                 leftX,
                                                 rightX,
                                                 topY,
                                                 bottomY,
                                                 singleTrackW,
                                                 renderScaleY,
                                                 colorHold,
                                                 colorNode,
                                                 colorArrow,
                                                 { 1.0f, 1.0f, 1.0f, 1.0f });
            }
        }
        glowBatcher.flush();
    }
}

}  // namespace MMM::Logic::System
