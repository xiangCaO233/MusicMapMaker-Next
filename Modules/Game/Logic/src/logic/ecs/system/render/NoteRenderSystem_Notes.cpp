#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace MMM::Logic::System
{

static std::vector<entt::entity> getNotesInRange(
    entt::registry& registry, const ScrollCache* cache, double currentAbsY,
    float judgmentLineY, float topY, float bottomY, float renderScaleY);

void NoteRenderSystem::renderNotes(
    entt::registry& registry, RenderSnapshot* snapshot,
    const std::string& cameraId, double currentTime, float judgmentLineY,
    int32_t trackCount, const Config::EditorConfig& config, Batcher& batcher,
    float leftX, float rightX, float topY, float bottomY, float singleTrackW,
    float renderScaleY)
{
    // 1. 准备上下文与颜色
    NoteRenderSystem::NoteRenderContext ctx =
        NoteRenderSystem::prepareNoteRenderContext(
            registry, snapshot, currentTime, singleTrackW, config);
    if ( !ctx.cache ) return;

    auto noteEntities = getNotesInRange(registry,
                                        ctx.cache,
                                        ctx.currentAbsY,
                                        judgmentLineY,
                                        topY,
                                        bottomY,
                                        renderScaleY);

    bool shouldGenerateHitboxes = cameraId == "Basic2DCanvas";

    // 2. 生成碰撞盒并获取可见实体
    if ( shouldGenerateHitboxes ) {
        NoteRenderSystem::generateNoteHitboxes(registry,
                                               snapshot,
                                               ctx,
                                               noteEntities,
                                               judgmentLineY,
                                               leftX,
                                               topY,
                                               bottomY,
                                               singleTrackW,
                                               renderScaleY,
                                               config);
    }

    // 3. 基础层渲染
    NoteRenderSystem::renderNoteBaseLayer(registry,
                                          snapshot,
                                          ctx,
                                          config,
                                          noteEntities,
                                          batcher,
                                          (float)currentTime,
                                          judgmentLineY,
                                          leftX,
                                          rightX,
                                          topY,
                                          bottomY,
                                          singleTrackW,
                                          renderScaleY,
                                          shouldGenerateHitboxes);

    // 4. 发光层渲染
    NoteRenderSystem::renderNoteGlowLayer(registry,
                                          snapshot,
                                          ctx,
                                          config,
                                          (float)currentTime,
                                          judgmentLineY,
                                          leftX,
                                          rightX,
                                          topY,
                                          bottomY,
                                          singleTrackW,
                                          renderScaleY);

    // 5. 笔刷预览渲染
    if ( snapshot->brush.isActive ) {
        NoteRenderSystem::renderBrushPreview(snapshot,
                                             ctx,
                                             config,
                                             batcher,
                                             judgmentLineY,
                                             leftX,
                                             singleTrackW,
                                             renderScaleY);
    }
}

NoteRenderSystem::NoteRenderContext NoteRenderSystem::prepareNoteRenderContext(
    entt::registry& registry, RenderSnapshot* snapshot, double currentTime,
    float singleTrackW, const Config::EditorConfig& config)
{
    NoteRenderSystem::NoteRenderContext ctx{};

    const auto** cachePtr = registry.ctx().find<const ScrollCache*>();
    if ( !cachePtr || !(*cachePtr) ) return ctx;
    ctx.cache       = *cachePtr;
    ctx.currentAbsY = ctx.cache->getAbsY(currentTime);
    ctx.currentTime = currentTime;

    auto itBase = snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Note));
    if ( itBase == snapshot->uvMap.end() ) return ctx;
    ctx.baseAspect = itBase->second.z / itBase->second.w;

    ctx.noteW = singleTrackW * config.visual.noteScaleX;
    ctx.noteH = (singleTrackW / ctx.baseAspect) * config.visual.noteScaleY;

    auto& skin      = Config::SkinManager::instance();
    auto  color_tap = skin.getColor("note_tap");
    ctx.colorTap    = { color_tap.r, color_tap.g, color_tap.b, color_tap.a };

    auto color_hold = skin.getColor("note_hold");
    ctx.colorHold = { color_hold.r, color_hold.g, color_hold.b, color_hold.a };

    auto color_node = skin.getColor("note_node");
    ctx.colorNode = { color_node.r, color_node.g, color_node.b, color_node.a };

    auto color_arrow = skin.getColor("note_flick_arrow");
    ctx.colorArrow   = {
        color_arrow.r, color_arrow.g, color_arrow.b, color_arrow.a
    };

    return ctx;
}

static std::vector<entt::entity> getNotesInRange(
    entt::registry& registry, const ScrollCache* cache, double currentAbsY,
    float judgmentLineY, float topY, float bottomY, float renderScaleY)
{
    std::vector<entt::entity> result;
    const auto**              sortedEntitiesPtr =
        registry.ctx().find<const std::vector<entt::entity>*>();
    if ( !sortedEntitiesPtr || !(*sortedEntitiesPtr) ) return result;
    const auto** maxEndPrefixPtr =
        registry.ctx().find<const std::vector<double>*>();

    const auto& entities = **sortedEntitiesPtr;
    size_t      count    = entities.size();
    if ( count == 0 || !cache || std::abs(renderScaleY) < 1e-6f ) {
        return result;
    }

    auto getCarrierEnd = [&](const NoteComponent& note) {
        double carrierEnd = note.m_timestamp + std::max(0.0, note.m_duration);
        if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
            for ( const auto& sub : note.m_subNotes ) {
                carrierEnd = std::max(
                    carrierEnd, sub.timestamp + std::max(0.0, sub.duration));
            }
        }
        return carrierEnd;
    };

    if ( cache->getSegments().size() > 2048 &&
         cache->getSegments().size() > count ) {
        result.reserve(count);
        double maxDelta =
            (judgmentLineY - topY) / static_cast<double>(renderScaleY);
        double minDelta =
            (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
        double padDelta = 128.0 / static_cast<double>(std::abs(renderScaleY));

        for ( auto entity : entities ) {
            const auto& note = registry.get<const NoteComponent>(entity);
            if ( note.m_isSubNote ) continue;

            double minDisplayDelta = cache->getDisplayDelta(
                note.m_timestamp, currentAbsY, note.m_timestamp);
            double maxDisplayDelta = minDisplayDelta;

            auto includeTime = [&](double time) {
                double displayDelta =
                    cache->getDisplayDelta(time, currentAbsY, time);
                minDisplayDelta = std::min(minDisplayDelta, displayDelta);
                maxDisplayDelta = std::max(maxDisplayDelta, displayDelta);
            };

            includeTime(note.m_timestamp + std::max(0.0, note.m_duration));
            if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
                for ( const auto& sub : note.m_subNotes ) {
                    includeTime(sub.timestamp);
                    includeTime(sub.timestamp + std::max(0.0, sub.duration));
                }
            }

            if ( maxDisplayDelta >= minDelta - padDelta &&
                 minDisplayDelta <= maxDelta + padDelta ) {
                result.push_back(entity);
            }
        }
        return result;
    }

    double topAbsY = currentAbsY +
                     (judgmentLineY - topY) / static_cast<double>(renderScaleY);
    double bottomAbsY = currentAbsY + (judgmentLineY - bottomY) /
                                          static_cast<double>(renderScaleY);
    auto   timeRanges = cache->getTimeRangesForAbsYWindow(
        std::min(topAbsY, bottomAbsY), std::max(topAbsY, bottomAbsY));

    std::vector<std::pair<double, double>> scanRanges;
    scanRanges.reserve(timeRanges.size());
    for ( const auto& [rangeStart, rangeEnd] : timeRanges ) {
        double scanStart = std::max(0.0, rangeStart - 10.0);
        if ( rangeEnd < scanStart ) continue;

        if ( !scanRanges.empty() &&
             scanStart <= scanRanges.back().second + 1e-6 ) {
            scanRanges.back().second =
                std::max(scanRanges.back().second, rangeEnd);
            continue;
        }
        scanRanges.emplace_back(scanStart, rangeEnd);
    }

    const std::vector<double>* maxEndPrefix =
        (maxEndPrefixPtr && *maxEndPrefixPtr &&
         (*maxEndPrefixPtr)->size() == entities.size())
            ? *maxEndPrefixPtr
            : nullptr;

    std::unordered_set<entt::entity> seen;
    auto                             addEntity = [&](entt::entity entity) {
        if ( seen.insert(entity).second ) {
            result.push_back(entity);
        }
    };

    for ( const auto& [rangeStart, rangeEnd] : scanRanges ) {
        auto startIt = std::lower_bound(
            entities.begin(),
            entities.end(),
            rangeStart,
            [&](entt::entity entity, double val) {
                return registry.get<const NoteComponent>(entity).m_timestamp <
                       val;
            });

        auto historyBegin = entities.begin();
        if ( maxEndPrefix && startIt != entities.begin() ) {
            auto prefixSearchEnd =
                maxEndPrefix->begin() + (startIt - entities.begin());
            auto prefixIt = std::lower_bound(
                maxEndPrefix->begin(), prefixSearchEnd, rangeStart);
            historyBegin =
                entities.begin() + (prefixIt - maxEndPrefix->begin());
        }

        for ( auto cur = historyBegin; cur != startIt; ++cur ) {
            entt::entity entity = *cur;
            const auto&  note   = registry.get<const NoteComponent>(entity);
            if ( note.m_isSubNote ) continue;
            if ( getCarrierEnd(note) >= rangeStart ) {
                addEntity(entity);
            }
        }

        auto it = std::lower_bound(
            entities.begin(),
            entities.end(),
            rangeStart,
            [&](entt::entity entity, double val) {
                return registry.get<const NoteComponent>(entity).m_timestamp <
                       val;
            });

        for ( auto cur = it; cur != entities.end(); ++cur ) {
            entt::entity entity = *cur;
            const auto&  note   = registry.get<const NoteComponent>(entity);
            if ( note.m_timestamp > rangeEnd ) {
                break;
            }

            if ( note.m_isSubNote ) continue;
            addEntity(entity);
        }
    }

    return result;
}

void NoteRenderSystem::generateNoteHitboxes(
    entt::registry& registry, RenderSnapshot* snapshot,
    const NoteRenderSystem::NoteRenderContext& ctx,
    const std::vector<entt::entity>& noteEntities, float judgmentLineY,
    float leftX, float topY, float bottomY, float singleTrackW,
    float renderScaleY, const Config::EditorConfig& config)
{
    // Pass 1: Body (Lower Priority)
    for ( auto entity : noteEntities ) {
        const auto& transform = registry.get<const TransformComponent>(entity);
        const auto& note      = registry.get<const NoteComponent>(entity);

        double displayDeltaStart = ctx.cache->getDisplayDelta(
            note.m_timestamp, ctx.currentAbsY, note.m_timestamp);
        double displayDeltaEnd =
            ctx.cache->getDisplayDelta(note.m_timestamp + note.m_duration,
                                       ctx.currentAbsY,
                                       note.m_timestamp + note.m_duration);

        double maxDelta =
            (judgmentLineY - topY) / static_cast<double>(renderScaleY);
        double minDelta =
            (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
        double padDelta = ctx.noteH / static_cast<double>(renderScaleY);

        if ( !NoteRenderSystem::isCarrierVisible(
                 note.m_timestamp,
                 note.m_timestamp + note.m_duration,
                 ctx.currentTime,
                 displayDeltaStart,
                 displayDeltaEnd,
                 maxDelta + padDelta,
                 minDelta - padDelta) ) {
            continue;
        }

        float screenY = judgmentLineY -
                        static_cast<float>(displayDeltaStart) * renderScaleY;

        float visualH =
            static_cast<float>(displayDeltaStart - displayDeltaEnd) *
            renderScaleY;

        if ( !note.m_isSubNote ) {
            if ( note.m_type == ::MMM::NoteType::FLICK && note.m_dtrack != 0 ) {
                float startTrack =
                    std::min((float)note.m_trackIndex,
                             (float)note.m_trackIndex + note.m_dtrack);
                float drawW = std::abs(note.m_dtrack) * singleTrackW;
                float bodyX =
                    leftX + startTrack * singleTrackW + singleTrackW * 0.5f;

                float drawH   = ctx.noteH;
                auto  itBodyH = snapshot->uvMap.find(
                    static_cast<uint32_t>(TextureID::HoldBodyHorizontal));
                if ( itBodyH != snapshot->uvMap.end() ) {
                    drawH = ctx.noteH *
                            (itBodyH->second.w /
                             snapshot->uvMap.at(uint32_t(TextureID::Note)).w);
                }

                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::HoldBody,
                                               -1,
                                               bodyX,
                                               screenY - drawH * 0.5f,
                                               drawW,
                                               drawH });
            } else if ( note.m_type == ::MMM::NoteType::HOLD &&
                        std::abs(visualH) > ctx.noteH * 0.1f ) {
                float bodyW  = ctx.noteW;
                auto  itBody = snapshot->uvMap.find(
                    static_cast<uint32_t>(TextureID::HoldBodyVertical));
                if ( itBody != snapshot->uvMap.end() ) {
                    float baseWRatio =
                        snapshot->uvMap.at(uint32_t(TextureID::Note)).z;
                    bodyW = ctx.noteW * (itBody->second.z / baseWRatio);
                }

                float bodyX = leftX + note.m_trackIndex * singleTrackW +
                              (singleTrackW - bodyW) * 0.5f;
                float bodyY = std::min(screenY, screenY + visualH);
                float bodyH = std::abs(visualH);

                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::HoldBody,
                                               -1,
                                               bodyX,
                                               bodyY,
                                               bodyW,
                                               bodyH });
            }
        }
    }

    // Pass 2: Head/End/Arrow (Higher Priority)
    for ( auto entity : noteEntities ) {
        const auto& transform = registry.get<const TransformComponent>(entity);
        const auto& note      = registry.get<const NoteComponent>(entity);
        float       screenY =
            judgmentLineY -
            static_cast<float>(ctx.cache->getDisplayDelta(
                note.m_timestamp, ctx.currentAbsY, note.m_timestamp)) *
                renderScaleY;
        float endY =
            judgmentLineY - static_cast<float>(ctx.cache->getDisplayDelta(
                                note.m_timestamp + note.m_duration,
                                ctx.currentAbsY,
                                note.m_timestamp)) *
                                renderScaleY;

        float minY = std::min(screenY, endY) - ctx.noteH;
        float maxY = std::max(screenY, endY) + ctx.noteH;
        if ( minY > bottomY || maxY < topY ) continue;

        float headX = leftX + note.m_trackIndex * singleTrackW +
                      (singleTrackW - ctx.noteW) * 0.5f;

        if ( !note.m_isSubNote && note.m_type != ::MMM::NoteType::POLYLINE ) {
            // 所有非 Polyline 音符的 Head
            snapshot->hitboxes.push_back({ entity,
                                           HoverPart::Head,
                                           -1,
                                           headX,
                                           screenY - ctx.noteH * 0.5f,
                                           ctx.noteW,
                                           ctx.noteH });

            if ( note.m_type == ::MMM::NoteType::FLICK && note.m_dtrack != 0 ) {
                TextureID arrowId = (note.m_dtrack < 0)
                                        ? TextureID::FlickArrowLeft
                                        : TextureID::FlickArrowRight;
                auto  it = snapshot->uvMap.find(static_cast<uint32_t>(arrowId));
                float arrowW = ctx.noteW;
                float arrowH = ctx.noteH;
                if ( it != snapshot->uvMap.end() ) {
                    float baseWRatio =
                        snapshot->uvMap.at(uint32_t(TextureID::Note)).z;
                    float baseHRatio =
                        snapshot->uvMap.at(uint32_t(TextureID::Note)).w;
                    float wRatio = it->second.z / baseWRatio;
                    float hRatio = it->second.w / baseHRatio;
                    arrowW       = ctx.noteW * wRatio;
                    arrowH       = ctx.noteH * hRatio;
                }

                float arrowX =
                    leftX + (note.m_trackIndex + note.m_dtrack) * singleTrackW +
                    (singleTrackW - arrowW) * 0.5f;

                snapshot->hitboxes.push_back({ entity,
                                               HoverPart::FlickArrow,
                                               -1,
                                               arrowX,
                                               screenY - arrowH * 0.5f,
                                               arrowW,
                                               arrowH });
            } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
                auto it = snapshot->uvMap.find(
                    static_cast<uint32_t>(TextureID::HoldEnd));
                float endW = ctx.noteW;
                float endH = ctx.noteH;
                if ( it != snapshot->uvMap.end() ) {
                    float baseWRatio =
                        snapshot->uvMap.at(uint32_t(TextureID::Note)).z;
                    float baseHRatio =
                        snapshot->uvMap.at(uint32_t(TextureID::Note)).w;
                    endW = ctx.noteW * (it->second.z / baseWRatio);
                    endH = ctx.noteH * (it->second.w / baseHRatio);
                }

                snapshot->hitboxes.push_back(
                    { entity,
                      HoverPart::HoldEnd,
                      -1,
                      leftX + note.m_trackIndex * singleTrackW +
                          (singleTrackW - endW) * 0.5f,
                      endY - endH * 0.5f,
                      endW,
                      endH });
            }
        }
    }
}

void NoteRenderSystem::renderNoteBaseLayer(
    entt::registry& registry, RenderSnapshot* snapshot,
    const NoteRenderSystem::NoteRenderContext& ctx,
    const Config::EditorConfig&                config,
    const std::vector<entt::entity>& noteEntities, Batcher& batcher,
    float currentTime, float judgmentLineY, float leftX, float rightX,
    float topY, float bottomY, float singleTrackW, float renderScaleY,
    bool generateHitboxes)
{
    std::vector<entt::entity> visibleEntities;
    for ( auto entity : noteEntities ) {
        const auto& note = registry.get<const NoteComponent>(entity);
        if ( note.m_isSubNote ) continue;

        double displayDeltaStart = ctx.cache->getDisplayDelta(
            note.m_timestamp, ctx.currentAbsY, note.m_timestamp);
        double displayDeltaEnd =
            ctx.cache->getDisplayDelta(note.m_timestamp + note.m_duration,
                                       ctx.currentAbsY,
                                       note.m_timestamp + note.m_duration);

        double maxDelta =
            (judgmentLineY - topY) / static_cast<double>(renderScaleY);
        double minDelta =
            (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
        double padDelta = ctx.noteH / static_cast<double>(renderScaleY);

        if ( note.m_type != ::MMM::NoteType::POLYLINE ) {
            if ( !NoteRenderSystem::isCarrierVisible(
                     note.m_timestamp,
                     note.m_timestamp + note.m_duration,
                     ctx.currentTime,
                     displayDeltaStart,
                     displayDeltaEnd,
                     maxDelta + padDelta,
                     minDelta - padDelta) ) {
                continue;
            }
        }

        visibleEntities.push_back(entity);
    }

    std::sort(visibleEntities.begin(),
              visibleEntities.end(),
              [&registry, &ctx](entt::entity a, entt::entity b) {
                  const auto &nA = registry.get<const NoteComponent>(a),
                             &nB = registry.get<const NoteComponent>(b);
                  return (std::abs(nA.m_timestamp - nB.m_timestamp) > 1e-6)
                             ? (nA.m_timestamp > nB.m_timestamp)
                             : (a > b);
              });

    for ( auto entity : visibleEntities ) {
        const auto& transform = registry.get<const TransformComponent>(entity);
        const auto& note      = registry.get<const NoteComponent>(entity);

        // 处理拖拽/选中/剪切时的视觉反馈
        float alphaMul   = 1.0f;
        bool  isSelected = false;
        if ( auto* ic = registry.try_get<InteractionComponent>(entity) ) {
            if ( ic->isDragging || ic->isCut ) {
                alphaMul = 0.5f;
            }
            isSelected = ic->isSelected;
        }

        float screenY =
            judgmentLineY -
            static_cast<float>(ctx.cache->getDisplayDelta(
                note.m_timestamp, ctx.currentAbsY, note.m_timestamp)) *
                renderScaleY;
        float visualH = static_cast<float>(ctx.cache->getDisplayDelta(
                            note.m_timestamp + note.m_duration,
                            ctx.cache->getAbsY(note.m_timestamp),
                            note.m_timestamp)) *
                        renderScaleY;
        float trackX  = leftX + note.m_trackIndex * singleTrackW;

        // 应用 Alpha 与 选中高亮
        glm::vec4 curColorTap   = ctx.colorTap;
        glm::vec4 curColorHold  = ctx.colorHold;
        glm::vec4 curColorNode  = ctx.colorNode;
        glm::vec4 curColorArrow = ctx.colorArrow;

        if ( isSelected ) {
            curColorTap  = { 1.0f, 1.0f, 0.4f, 1.0f };
            curColorHold = { 1.0f, 1.0f, 0.4f, 1.0f };
        }

        bool isFullErasing = snapshot->erasingEntities.count(entity) &&
                             (snapshot->erasingSubIndex == -1);
        if ( isFullErasing ) {
            curColorTap   = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorHold  = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorNode  = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorArrow = { 1.0f, 0.2f, 0.2f, 1.0f };
            alphaMul *= 0.5f;
        }

        curColorTap.a *= alphaMul;
        curColorHold.a *= alphaMul;
        curColorNode.a *= alphaMul;
        curColorArrow.a *= alphaMul;

        if ( note.m_type == ::MMM::NoteType::NOTE )
            NoteRenderSystem::renderTap(
                batcher,
                note,
                config,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                screenY,
                ctx.noteW,
                ctx.noteH,
                ctx.baseAspect,
                curColorTap);
        else if ( note.m_type == ::MMM::NoteType::HOLD )
            NoteRenderSystem::renderHold(
                batcher,
                note,
                config,
                snapshot,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                ctx.noteW,
                ctx.noteH,
                singleTrackW,
                curColorHold,
                ctx.cache,
                ctx.currentAbsY,
                judgmentLineY,
                renderScaleY);
        else if ( note.m_type == ::MMM::NoteType::FLICK )
            NoteRenderSystem::renderFlick(
                batcher,
                note,
                config,
                snapshot,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                screenY,
                ctx.noteW,
                ctx.noteH,
                singleTrackW,
                curColorHold,
                curColorArrow);
        else if ( note.m_type == ::MMM::NoteType::POLYLINE )
            NoteRenderSystem::renderPolyline(ctx.cache,
                                             batcher,
                                             note,
                                             config,
                                             snapshot,
                                             ctx.currentAbsY,
                                             ctx.currentTime,
                                             judgmentLineY,
                                             leftX,
                                             rightX,
                                             topY,
                                             bottomY,
                                             singleTrackW,
                                             renderScaleY,
                                             curColorHold,
                                             curColorNode,
                                             curColorArrow,
                                             entity,
                                             generateHitboxes);
    }
    batcher.flush();
}

void NoteRenderSystem::renderNoteGlowLayer(
    entt::registry& registry, RenderSnapshot* snapshot,
    const NoteRenderSystem::NoteRenderContext& ctx,
    const Config::EditorConfig& config, float currentTime, float judgmentLineY,
    float leftX, float rightX, float topY, float bottomY, float singleTrackW,
    float renderScaleY)
{
    auto interactionView = registry.view<const InteractionComponent>();
    std::vector<entt::entity> hoveredEntities;
    for ( auto entity : interactionView ) {
        const auto& ic =
            interactionView.get<const InteractionComponent>(entity);
        if ( ic.isHovered ) hoveredEntities.push_back(entity);
    }

    if ( hoveredEntities.empty() ) return;

    std::sort(hoveredEntities.begin(),
              hoveredEntities.end(),
              [&registry, &ctx](entt::entity a, entt::entity b) {
                  const auto &nA = registry.get<const NoteComponent>(a),
                             &nB = registry.get<const NoteComponent>(b);
                  return (std::abs(nA.m_timestamp - nB.m_timestamp) > 1e-6)
                             ? (nA.m_timestamp > nB.m_timestamp)
                             : (a > b);
              });

    Batcher glowBatcher(snapshot, &snapshot->glowCmds);
    for ( auto entity : hoveredEntities ) {
        const auto& transform = registry.get<const TransformComponent>(entity);
        const auto& note      = registry.get<const NoteComponent>(entity);
        const auto& ic = registry.get<const InteractionComponent>(entity);
        float       screenY =
            judgmentLineY -
            static_cast<float>(ctx.cache->getDisplayDelta(
                note.m_timestamp, ctx.currentAbsY, note.m_timestamp)) *
                renderScaleY;
        float     visualH  = static_cast<float>(ctx.cache->getDisplayDelta(
                                 note.m_timestamp + note.m_duration,
                                 ctx.cache->getAbsY(note.m_timestamp),
                                 note.m_timestamp)) *
                             renderScaleY;
        float     trackX   = leftX + note.m_trackIndex * singleTrackW;
        HoverPart glowPart = static_cast<HoverPart>(ic.hoveredPart);
        int       glowIdx  = ic.hoveredSubIndex;

        if ( note.m_type == ::MMM::NoteType::NOTE )
            NoteRenderSystem::renderTap(
                glowBatcher,
                note,
                config,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                screenY,
                ctx.noteW,
                ctx.noteH,
                ctx.baseAspect,
                ctx.colorTap);
        else if ( note.m_type == ::MMM::NoteType::HOLD )
            NoteRenderSystem::renderHold(
                glowBatcher,
                note,
                config,
                snapshot,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                ctx.noteW,
                ctx.noteH,
                singleTrackW,
                ctx.colorHold,
                ctx.cache,
                ctx.currentAbsY,
                judgmentLineY,
                renderScaleY,
                glowPart);
        else if ( note.m_type == ::MMM::NoteType::FLICK )
            NoteRenderSystem::renderFlick(
                glowBatcher,
                note,
                config,
                snapshot,
                trackX + (singleTrackW - ctx.noteW) * 0.5f,
                screenY,
                ctx.noteW,
                ctx.noteH,
                singleTrackW,
                ctx.colorHold,
                ctx.colorArrow,
                glowPart);
        else if ( note.m_type == ::MMM::NoteType::POLYLINE )
            NoteRenderSystem::renderPolyline(ctx.cache,
                                             glowBatcher,
                                             note,
                                             config,
                                             snapshot,
                                             ctx.currentAbsY,
                                             ctx.currentTime,
                                             judgmentLineY,
                                             leftX,
                                             rightX,
                                             topY,
                                             bottomY,
                                             singleTrackW,
                                             renderScaleY,
                                             ctx.colorHold,
                                             ctx.colorNode,
                                             ctx.colorArrow,
                                             entity,
                                             false,
                                             glowPart,
                                             glowIdx);
    }
    glowBatcher.flush();
}

void NoteRenderSystem::renderBrushPreview(
    RenderSnapshot* snapshot, const NoteRenderSystem::NoteRenderContext& ctx,
    const Config::EditorConfig& config, Batcher& batcher, float judgmentLineY,
    float leftX, float singleTrackW, float renderScaleY)
{
    const auto& brush = snapshot->brush;
    if ( !brush.isActive ) return;

    double noteAbsY = ctx.cache->getAbsY(brush.time);
    float  screenY =
        judgmentLineY - static_cast<float>(ctx.cache->getDisplayDelta(
                            brush.time, ctx.currentAbsY, brush.time)) *
                            renderScaleY;

    float trackX = leftX + brush.track * singleTrackW;

    glm::vec4 color = ctx.colorTap;
    if ( brush.type == ::MMM::NoteType::HOLD ||
         brush.type == ::MMM::NoteType::POLYLINE ) {
        color = ctx.colorHold;
    }
    color.a *= 0.5f;  // 半透明效果

    NoteComponent tempNote;
    tempNote.m_type       = brush.type;
    tempNote.m_timestamp  = brush.time;
    tempNote.m_duration   = brush.duration;
    tempNote.m_trackIndex = brush.track;
    tempNote.m_dtrack     = brush.dtrack;

    if ( brush.type == ::MMM::NoteType::NOTE ) {
        NoteRenderSystem::renderTap(batcher,
                                    tempNote,
                                    config,
                                    trackX + (singleTrackW - ctx.noteW) * 0.5f,
                                    screenY,
                                    ctx.noteW,
                                    ctx.noteH,
                                    ctx.baseAspect,
                                    color);
    } else if ( brush.type == ::MMM::NoteType::HOLD ) {
        NoteRenderSystem::renderHold(batcher,
                                     tempNote,
                                     config,
                                     snapshot,
                                     trackX + (singleTrackW - ctx.noteW) * 0.5f,
                                     ctx.noteW,
                                     ctx.noteH,
                                     singleTrackW,
                                     color,
                                     ctx.cache,
                                     ctx.currentAbsY,
                                     judgmentLineY,
                                     renderScaleY);
    } else if ( brush.type == ::MMM::NoteType::FLICK ) {
        NoteRenderSystem::renderFlick(
            batcher,
            tempNote,
            config,
            snapshot,
            trackX + (singleTrackW - ctx.noteW) * 0.5f,
            screenY,
            ctx.noteW,
            ctx.noteH,
            singleTrackW,
            color,
            ctx.colorArrow * glm::vec4(1, 1, 1, 0.5f));
    } else if ( brush.type == ::MMM::NoteType::POLYLINE ) {
        tempNote.m_subNotes = brush.polylineSegments;
        if ( !tempNote.m_subNotes.empty() ) {
            tempNote.m_timestamp  = tempNote.m_subNotes.front().timestamp;
            tempNote.m_trackIndex = tempNote.m_subNotes.front().trackIndex;
        }

        float rightX  = leftX + snapshot->trackCount * singleTrackW;
        float topY    = 0.0f;
        float bottomY = judgmentLineY * 2.0f;  // 简单包围盒

        NoteRenderSystem::renderPolyline(
            ctx.cache,
            batcher,
            tempNote,
            config,
            snapshot,
            ctx.currentAbsY,
            ctx.currentTime,
            judgmentLineY,
            leftX,
            rightX,
            topY,
            bottomY,
            singleTrackW,
            renderScaleY,
            ctx.colorHold * glm::vec4(1, 1, 1, 0.5f),
            ctx.colorNode * glm::vec4(1, 1, 1, 0.5f),
            ctx.colorArrow * glm::vec4(1, 1, 1, 0.5f));
    } else {
        // Fallback debug drawing
        float x = trackX + (singleTrackW - ctx.noteW) * 0.5f;
        batcher.setTexture(TextureID::None);
        batcher.pushQuad(x,
                         screenY + ctx.noteH * 0.5f,
                         ctx.noteW,
                         ctx.noteH,
                         { 1.0f, 0.0f, 0.0f, 0.5f });
    }
    batcher.flush();
}

}  // namespace MMM::Logic::System
