#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteColorUtils.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
#include "logic/session/SessionUtils.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
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

    bool shouldGenerateHitboxes = snapshot->acceptsInteraction &&
                                  SessionUtils::isMainCanvasCameraId(cameraId);

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
                                          noteEntities,
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

    // 6. 顶层重叠遮罩
    NoteRenderSystem::renderOverlapMasks(registry,
                                         snapshot,
                                         ctx,
                                         config,
                                         noteEntities,
                                         judgmentLineY,
                                         leftX,
                                         rightX,
                                         topY,
                                         bottomY,
                                         singleTrackW,
                                         renderScaleY);
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

    auto color_head = skin.getData().colors.contains("note_head")
                          ? skin.getColor("note_head")
                          : skin.getColor("note_hold");
    ctx.colorHead = { color_head.r, color_head.g, color_head.b, color_head.a };

    auto color_hold = skin.getColor("note_hold");
    ctx.colorHold = { color_hold.r, color_hold.g, color_hold.b, color_hold.a };

    auto color_end = skin.getData().colors.contains("note_end")
                         ? skin.getColor("note_end")
                         : skin.getColor("note_hold");
    ctx.colorEnd   = { color_end.r, color_end.g, color_end.b, color_end.a };

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

    /// @brief visibleEntities 保持自 SessionContext
    /// 预排序缓存继承来的时间升序。
    /// 基础层按反向顺序绘制，避免在热路径内再次完整排序。
    for ( auto it = visibleEntities.rbegin(); it != visibleEntities.rend();
          ++it ) {
        /// @brief 当前反向遍历到的可见音符实体。
        entt::entity entity    = *it;
        const auto&  transform = registry.get<const TransformComponent>(entity);
        const auto&  note      = registry.get<const NoteComponent>(entity);

        // 处理拖拽/剪切时的视觉反馈；选中反馈由发光层按当前颜色绘制。
        float alphaMul = 1.0f;
        if ( auto* ic = registry.try_get<InteractionComponent>(entity) ) {
            if ( ic->isDragging || ic->isCut ) {
                alphaMul = 0.5f;
            }
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

        // 应用自定义颜色与 Alpha。
        glm::vec4 curColorNote =
            resolveNoteColor(note, NoteColorSlot::Tap, ctx.colorTap);
        glm::vec4 curColorHead =
            resolveNoteColor(note, NoteColorSlot::Head, ctx.colorHead);
        glm::vec4 curColorHoldBody =
            resolveNoteColor(note, NoteColorSlot::Hold, ctx.colorHold);
        glm::vec4 curColorHoldEnd =
            resolveNoteColor(note, NoteColorSlot::End, ctx.colorEnd);
        glm::vec4 curColorNode =
            resolveNoteColor(note, NoteColorSlot::Node, ctx.colorNode);
        glm::vec4 curColorArrow =
            resolveNoteColor(note, NoteColorSlot::FlickArrow, ctx.colorArrow);

        bool isFullErasing = snapshot->erasingEntities.count(entity) &&
                             (snapshot->erasingSubIndex == -1);
        if ( isFullErasing ) {
            curColorNote     = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorHead     = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorHoldBody = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorHoldEnd  = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorNode     = { 1.0f, 0.2f, 0.2f, 1.0f };
            curColorArrow    = { 1.0f, 0.2f, 0.2f, 1.0f };
            alphaMul *= 0.5f;
        }

        curColorNote.a *= alphaMul;
        curColorHead.a *= alphaMul;
        curColorHoldBody.a *= alphaMul;
        curColorHoldEnd.a *= alphaMul;
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
                curColorNote);
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
                curColorHead,
                curColorHoldBody,
                curColorHoldEnd,
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
                curColorHead,
                curColorHoldBody,
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
                                             curColorHead,
                                             curColorHoldBody,
                                             curColorHoldEnd,
                                             curColorNode,
                                             curColorArrow,
                                             entity,
                                             generateHitboxes);
    }
    batcher.flush();
}

/// @brief 绘制悬浮/选中音符的发光层，并使用轨道框限制可见区域。
/// @warning
/// 热路径：悬浮/选中音符每帧绘制；只允许使用已缓存的实体列表和纹理信息。
void NoteRenderSystem::renderNoteGlowLayer(
    entt::registry& registry, RenderSnapshot* snapshot,
    const NoteRenderSystem::NoteRenderContext& ctx,
    const Config::EditorConfig&                config,
    const std::vector<entt::entity>& noteEntities, float currentTime,
    float judgmentLineY, float leftX, float rightX, float topY, float bottomY,
    float singleTrackW, float renderScaleY)
{
    std::vector<entt::entity> glowEntities;
    glowEntities.reserve(noteEntities.size());
    for ( auto entity : noteEntities ) {
        /// @brief 当前可见实体的交互状态；不存在时跳过发光层。
        const auto* ic = registry.try_get<const InteractionComponent>(entity);
        if ( !ic ) continue;
        if ( ic->isHovered || ic->isSelected ) glowEntities.push_back(entity);
    }

    if ( glowEntities.empty() ) return;

    Batcher glowBatcher(snapshot, &snapshot->glowCmds);
    glowBatcher.setScissor(leftX, topY, rightX - leftX, bottomY - topY);
    /// @brief glowEntities
    /// 继承可见实体时间升序，反向绘制即可获得原先的后到前覆盖顺序。
    for ( auto it = glowEntities.rbegin(); it != glowEntities.rend(); ++it ) {
        /// @brief 当前反向遍历到的发光音符实体。
        entt::entity entity    = *it;
        const auto&  transform = registry.get<const TransformComponent>(entity);
        const auto&  note      = registry.get<const NoteComponent>(entity);
        const auto&  ic = registry.get<const InteractionComponent>(entity);
        float        screenY =
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
        if ( ic.isSelected ) {
            glowPart = HoverPart::None;
            glowIdx  = -1;
        }
        glm::vec4 glowNote =
            resolveNoteColor(note, NoteColorSlot::Tap, ctx.colorTap);
        glm::vec4 glowHead =
            resolveNoteColor(note, NoteColorSlot::Head, ctx.colorHead);
        glm::vec4 glowBody =
            resolveNoteColor(note, NoteColorSlot::Hold, ctx.colorHold);
        glm::vec4 glowEnd =
            resolveNoteColor(note, NoteColorSlot::End, ctx.colorEnd);
        glm::vec4 glowNode =
            resolveNoteColor(note, NoteColorSlot::Node, ctx.colorNode);
        glm::vec4 glowArrow =
            resolveNoteColor(note, NoteColorSlot::FlickArrow, ctx.colorArrow);

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
                glowNote);
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
                glowHead,
                glowBody,
                glowEnd,
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
                glowHead,
                glowBody,
                glowArrow,
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
                                             glowHead,
                                             glowBody,
                                             glowEnd,
                                             glowNode,
                                             glowArrow,
                                             entity,
                                             false,
                                             glowPart,
                                             glowIdx);
    }
    glowBatcher.flush();
}

void NoteRenderSystem::renderOverlapMasks(
    entt::registry& registry, RenderSnapshot* snapshot,
    const NoteRenderSystem::NoteRenderContext& ctx,
    const Config::EditorConfig&                config,
    const std::vector<entt::entity>& noteEntities, float judgmentLineY,
    float leftX, float rightX, float topY, float bottomY, float singleTrackW,
    float renderScaleY)
{
    if ( !snapshot || !ctx.cache ) return;

    struct OverlapItem {
        ::MMM::NoteType type{ ::MMM::NoteType::NOTE };
        double          startTime{ 0.0 };
        double          endTime{ 0.0 };
        int             track{ 0 };
        int             dtrack{ 0 };
        entt::entity    owner{ entt::null };
        int             subIndex{ -1 };
    };

    struct OverlapPoint {
        double       time{ 0.0 };
        int          track{ 0 };
        entt::entity owner{ entt::null };
        float        scale{ 1.0f };
        bool         testsFlickBody{ false };
    };

    auto makeEndTime =
        [](double startTime, double duration, ::MMM::NoteType type) {
            if ( type != ::MMM::NoteType::HOLD ) return startTime;
            return startTime + std::max(0.0, duration);
        };

    std::vector<OverlapItem> items;
    items.reserve(noteEntities.size());
    for ( auto entity : noteEntities ) {
        const auto& note = registry.get<const NoteComponent>(entity);
        if ( note.m_isSubNote ) continue;

        if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
            for ( size_t i = 0; i < note.m_subNotes.size(); ++i ) {
                const auto& sub = note.m_subNotes[i];
                items.push_back(
                    { sub.type,
                      sub.timestamp,
                      makeEndTime(sub.timestamp, sub.duration, sub.type),
                      sub.trackIndex,
                      sub.dtrack,
                      entity,
                      static_cast<int>(i) });
            }
            continue;
        }

        items.push_back(
            { note.m_type,
              note.m_timestamp,
              makeEndTime(note.m_timestamp, note.m_duration, note.m_type),
              note.m_trackIndex,
              note.m_dtrack,
              entity,
              -1 });
    }

    if ( items.empty() ) return;

    const double windowSeconds =
        static_cast<double>(
            std::max(0.0f, config.settings.overlapTimeWindowMs)) *
        0.001;
    constexpr double timeEpsilon = 1e-7;

    auto textureSize = [snapshot](TextureID id, float baseW, float baseH) {
        auto itBase =
            snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Note));
        if ( itBase == snapshot->uvMap.end() ) return glm::vec2(baseW, baseH);

        auto it = snapshot->uvMap.find(static_cast<uint32_t>(id));
        if ( it == snapshot->uvMap.end() ) return glm::vec2(baseW, baseH);

        return glm::vec2(baseW * (it->second.z / itBase->second.z),
                         baseH * (it->second.w / itBase->second.w));
    };

    const glm::vec2 verticalBodySize =
        textureSize(TextureID::HoldBodyVertical, ctx.noteW, ctx.noteH);
    const glm::vec2 horizontalBodySize =
        textureSize(TextureID::HoldBodyHorizontal, ctx.noteW, ctx.noteH);

    auto timeToY = [&](double time) {
        return judgmentLineY - static_cast<float>(ctx.cache->getDisplayDelta(
                                   time, ctx.currentAbsY, time)) *
                                   renderScaleY;
    };

    auto sameOwner = [](const OverlapItem& a, const OverlapItem& b) {
        return a.owner != entt::null && a.owner == b.owner;
    };

    auto countUniqueOwners = [](const std::vector<const OverlapItem*>& group) {
        std::unordered_set<entt::entity> owners;
        owners.reserve(group.size());
        for ( const auto* item : group ) {
            owners.insert(item->owner);
        }
        return static_cast<int>(owners.size());
    };

    auto appendMask = [&](float x, float y, float w, float h, int count) {
        if ( w <= 0.0f || h <= 0.0f || count < 2 ) return;
        if ( x > rightX || x + w < leftX || y > bottomY || y + h < topY )
            return;

        // 这里不能预先合并成外接矩形，否则相邻遮罩之间的空区域也会被误涂红。
        // 后面的扫描线拆分会负责去掉重复覆盖并保证同一像素最多只画一层。
        snapshot->overlapMasks.push_back({ x, y, w, h, count });
    };

    auto appendPointMask = [&](double time, int track, float scale, int count) {
        float w = ctx.noteW * scale;
        float h = ctx.noteH * scale;
        float x = leftX + static_cast<float>(track) * singleTrackW +
                  (singleTrackW - w) * 0.5f;
        float y = timeToY(time) - h * 0.5f;
        appendMask(x, y, w, h, count);
    };

    std::vector<const OverlapItem*> notes;
    std::vector<const OverlapItem*> holds;
    std::vector<const OverlapItem*> flicks;
    std::vector<OverlapPoint>       pointMarkers;
    notes.reserve(items.size());
    holds.reserve(items.size());
    flicks.reserve(items.size());
    pointMarkers.reserve(items.size() * 2);

    for ( const auto& item : items ) {
        if ( item.type == ::MMM::NoteType::NOTE ) {
            notes.push_back(&item);
            pointMarkers.push_back(
                { item.startTime, item.track, item.owner, 1.0f, true });
        } else if ( item.type == ::MMM::NoteType::HOLD ) {
            if ( item.endTime > item.startTime + timeEpsilon ) {
                holds.push_back(&item);
                pointMarkers.push_back(
                    { item.endTime, item.track, item.owner, 1.0f, true });
            }
            pointMarkers.push_back(
                { item.startTime, item.track, item.owner, 1.0f, true });
        } else if ( item.type == ::MMM::NoteType::FLICK ) {
            flicks.push_back(&item);
            pointMarkers.push_back(
                { item.startTime, item.track, item.owner, 1.0f, false });
            pointMarkers.push_back({ item.startTime,
                                     item.track + item.dtrack,
                                     item.owner,
                                     1.0f,
                                     true });
        }
    }

    std::sort(notes.begin(),
              notes.end(),
              [](const OverlapItem* a, const OverlapItem* b) {
                  if ( a->track != b->track ) return a->track < b->track;
                  return a->startTime < b->startTime;
              });

    for ( size_t i = 0; i < notes.size(); ) {
        size_t j = i + 1;
        while ( j < notes.size() && notes[j]->track == notes[i]->track &&
                notes[j]->startTime - notes[j - 1]->startTime <
                    windowSeconds ) {
            ++j;
        }

        if ( j - i >= 2 ) {
            std::vector<const OverlapItem*> group;
            group.reserve(j - i);
            double minTime = notes[i]->startTime;
            double maxTime = notes[i]->startTime;
            for ( size_t k = i; k < j; ++k ) {
                group.push_back(notes[k]);
                minTime = std::min(minTime, notes[k]->startTime);
                maxTime = std::max(maxTime, notes[k]->startTime);
            }

            int uniqueCount = countUniqueOwners(group);
            if ( uniqueCount >= 2 ) {
                float y0 = timeToY(minTime);
                float y1 = timeToY(maxTime);
                float x  = leftX + notes[i]->track * singleTrackW +
                           (singleTrackW - ctx.noteW) * 0.5f;
                float y  = std::min(y0, y1) - ctx.noteH * 0.5f;
                float h  = std::abs(y0 - y1) + ctx.noteH;
                appendMask(x, y, ctx.noteW, h, uniqueCount);
            }
        }

        i = j;
    }

    std::sort(pointMarkers.begin(),
              pointMarkers.end(),
              [](const OverlapPoint& a, const OverlapPoint& b) {
                  if ( a.track != b.track ) return a.track < b.track;
                  return a.time < b.time;
              });

    for ( size_t i = 0; i < pointMarkers.size(); ) {
        size_t j = i + 1;
        while ( j < pointMarkers.size() &&
                pointMarkers[j].track == pointMarkers[i].track &&
                pointMarkers[j].time - pointMarkers[j - 1].time <=
                    windowSeconds + timeEpsilon ) {
            ++j;
        }

        if ( j - i >= 2 ) {
            std::unordered_set<entt::entity> owners;
            double                           minTime  = pointMarkers[i].time;
            double                           maxTime  = pointMarkers[i].time;
            float                            maxScale = pointMarkers[i].scale;
            for ( size_t k = i; k < j; ++k ) {
                owners.insert(pointMarkers[k].owner);
                minTime  = std::min(minTime, pointMarkers[k].time);
                maxTime  = std::max(maxTime, pointMarkers[k].time);
                maxScale = std::max(maxScale, pointMarkers[k].scale);
            }

            if ( owners.size() >= 2 ) {
                float w  = ctx.noteW * maxScale;
                float h0 = ctx.noteH * maxScale;
                float y0 = timeToY(minTime);
                float y1 = timeToY(maxTime);
                float x  = leftX + pointMarkers[i].track * singleTrackW +
                           (singleTrackW - w) * 0.5f;
                float y  = std::min(y0, y1) - h0 * 0.5f;
                float h  = std::abs(y0 - y1) + h0;
                appendMask(x, y, w, h, static_cast<int>(owners.size()));
            }
        }

        i = j;
    }

    std::sort(holds.begin(),
              holds.end(),
              [](const OverlapItem* a, const OverlapItem* b) {
                  if ( a->track != b->track ) return a->track < b->track;
                  if ( a->startTime != b->startTime )
                      return a->startTime < b->startTime;
                  return a->endTime < b->endTime;
              });

    for ( size_t i = 0; i < holds.size(); ) {
        size_t j = i + 1;
        while ( j < holds.size() && holds[j]->track == holds[i]->track ) ++j;

        std::vector<double> bounds;
        bounds.reserve((j - i) * 2);
        for ( size_t k = i; k < j; ++k ) {
            bounds.push_back(holds[k]->startTime);
            bounds.push_back(holds[k]->endTime);
        }
        std::sort(bounds.begin(), bounds.end());
        bounds.erase(std::unique(bounds.begin(),
                                 bounds.end(),
                                 [](double a, double b) {
                                     return std::abs(a - b) < 1e-7;
                                 }),
                     bounds.end());

        bool   hasOpenMask = false;
        double openStart   = 0.0;
        double openEnd     = 0.0;
        int    openCount   = 0;

        auto flushOpenMask = [&]() {
            if ( !hasOpenMask ) return;
            float y0 = timeToY(openStart);
            float y1 = timeToY(openEnd);
            float x  = leftX + holds[i]->track * singleTrackW +
                       (singleTrackW - verticalBodySize.x) * 0.5f;
            appendMask(x,
                       std::min(y0, y1),
                       verticalBodySize.x,
                       std::abs(y0 - y1),
                       openCount);
            hasOpenMask = false;
            openCount   = 0;
        };

        for ( size_t k = 0; k + 1 < bounds.size(); ++k ) {
            double segStart = bounds[k];
            double segEnd   = bounds[k + 1];
            if ( segEnd <= segStart + timeEpsilon ) continue;

            std::unordered_set<entt::entity> activeOwners;
            for ( size_t h = i; h < j; ++h ) {
                if ( holds[h]->startTime < segEnd - timeEpsilon &&
                     holds[h]->endTime > segStart + timeEpsilon ) {
                    activeOwners.insert(holds[h]->owner);
                }
            }

            int activeCount = static_cast<int>(activeOwners.size());
            if ( activeCount >= 2 ) {
                if ( !hasOpenMask ) {
                    hasOpenMask = true;
                    openStart   = segStart;
                    openEnd     = segEnd;
                    openCount   = activeCount;
                } else {
                    openEnd   = segEnd;
                    openCount = std::max(openCount, activeCount);
                }
            } else {
                flushOpenMask();
            }
        }
        flushOpenMask();

        i = j;
    }

    auto flickBodyMinTrack = [](const OverlapItem& item) {
        return std::min(item.track, item.track + item.dtrack);
    };
    auto flickBodyMaxTrack = [](const OverlapItem& item) {
        return std::max(item.track, item.track + item.dtrack);
    };

    for ( size_t i = 0; i < flicks.size(); ++i ) {
        const auto& a = *flicks[i];
        if ( a.dtrack == 0 ) continue;

        for ( size_t j = i + 1; j < flicks.size(); ++j ) {
            const auto& b = *flicks[j];
            if ( b.dtrack == 0 || sameOwner(a, b) ) continue;
            if ( std::abs(a.startTime - b.startTime) >
                 windowSeconds + timeEpsilon )
                continue;

            int overlapMin =
                std::max(flickBodyMinTrack(a), flickBodyMinTrack(b));
            int overlapMax =
                std::min(flickBodyMaxTrack(a), flickBodyMaxTrack(b));
            if ( overlapMax <= overlapMin ) continue;

            float y0 = timeToY(a.startTime);
            float y1 = timeToY(b.startTime);
            float x  = leftX + static_cast<float>(overlapMin) * singleTrackW +
                       singleTrackW * 0.5f;
            float w =
                static_cast<float>(overlapMax - overlapMin) * singleTrackW;
            float y = std::min(y0, y1) - horizontalBodySize.y * 0.5f;
            float h = std::abs(y0 - y1) + horizontalBodySize.y;
            appendMask(x, y, w, h, 2);
        }
    }

    for ( const auto& point : pointMarkers ) {
        std::unordered_set<entt::entity> owners;
        owners.insert(point.owner);

        for ( const auto* hold : holds ) {
            if ( hold->owner == point.owner || point.track != hold->track )
                continue;
            if ( point.time > hold->startTime + timeEpsilon &&
                 point.time < hold->endTime - timeEpsilon ) {
                owners.insert(hold->owner);
            }
        }

        if ( owners.size() >= 2 ) {
            appendPointMask(point.time,
                            point.track,
                            point.scale,
                            static_cast<int>(owners.size()));
        }
    }

    for ( const auto& point : pointMarkers ) {
        if ( !point.testsFlickBody ) continue;

        std::unordered_set<entt::entity> owners;
        owners.insert(point.owner);

        for ( const auto* flick : flicks ) {
            if ( flick->owner == point.owner || flick->dtrack == 0 ) continue;
            if ( std::abs(point.time - flick->startTime) >
                 windowSeconds + timeEpsilon )
                continue;

            int minTrack = flickBodyMinTrack(*flick);
            int maxTrack = flickBodyMaxTrack(*flick);
            if ( point.track < minTrack || point.track > maxTrack ) continue;

            owners.insert(flick->owner);
        }

        if ( owners.size() >= 2 ) {
            appendPointMask(point.time,
                            point.track,
                            point.scale,
                            static_cast<int>(owners.size()));
        }
    }

    for ( const auto* flick : flicks ) {
        if ( flick->dtrack == 0 ) continue;

        std::unordered_map<int, std::unordered_set<entt::entity>> ownersByTrack;
        int minTrack = flickBodyMinTrack(*flick);
        int maxTrack = flickBodyMaxTrack(*flick);

        for ( const auto* hold : holds ) {
            if ( sameOwner(*flick, *hold) ) continue;
            if ( flick->startTime <= hold->startTime + timeEpsilon ||
                 flick->startTime >= hold->endTime - timeEpsilon )
                continue;
            if ( hold->track < minTrack || hold->track > maxTrack ) continue;

            auto& owners = ownersByTrack[hold->track];
            owners.insert(flick->owner);
            owners.insert(hold->owner);
        }

        for ( const auto& [track, owners] : ownersByTrack ) {
            appendPointMask(
                flick->startTime, track, 0.7f, static_cast<int>(owners.size()));
        }
    }

    if ( snapshot->overlapMasks.empty() ) return;

    auto flattenOverlapMasks = [&]() {
        if ( snapshot->overlapMasks.size() < 2 ) return;

        std::vector<float> xs;
        std::vector<float> ys;
        xs.reserve(snapshot->overlapMasks.size() * 2);
        ys.reserve(snapshot->overlapMasks.size() * 2);

        for ( const auto& mask : snapshot->overlapMasks ) {
            if ( mask.w <= 0.0f || mask.h <= 0.0f ) continue;
            xs.push_back(mask.x);
            xs.push_back(mask.x + mask.w);
            ys.push_back(mask.y);
            ys.push_back(mask.y + mask.h);
        }

        auto uniqueCoords = [](std::vector<float>& coords) {
            std::sort(coords.begin(), coords.end());
            coords.erase(std::unique(coords.begin(),
                                     coords.end(),
                                     [](float a, float b) {
                                         return std::abs(a - b) < 0.01f;
                                     }),
                         coords.end());
        };

        uniqueCoords(xs);
        uniqueCoords(ys);
        if ( xs.size() < 2 || ys.size() < 2 ) return;

        std::vector<RenderSnapshot::OverlapMask> flattened;
        flattened.reserve(snapshot->overlapMasks.size());

        // 将重叠矩形拆成互不相交的扫描线小矩形，避免同一像素被红色滤镜重复覆盖。
        for ( size_t yIndex = 0; yIndex + 1 < ys.size(); ++yIndex ) {
            const float y0 = ys[yIndex];
            const float y1 = ys[yIndex + 1];
            if ( y1 <= y0 + 0.01f ) continue;

            bool  hasRun   = false;
            float runStart = 0.0f;
            int   runCount = 0;
            auto  flushRun = [&](float runEnd) {
                if ( !hasRun || runEnd <= runStart + 0.01f ) return;
                flattened.push_back(
                    { runStart, y0, runEnd - runStart, y1 - y0, runCount });
                hasRun   = false;
                runCount = 0;
            };

            for ( size_t xIndex = 0; xIndex + 1 < xs.size(); ++xIndex ) {
                const float x0 = xs[xIndex];
                const float x1 = xs[xIndex + 1];
                if ( x1 <= x0 + 0.01f ) continue;

                const float sampleX = (x0 + x1) * 0.5f;
                const float sampleY = (y0 + y1) * 0.5f;
                int         count   = 0;
                for ( const auto& mask : snapshot->overlapMasks ) {
                    if ( sampleX < mask.x || sampleX > mask.x + mask.w ||
                         sampleY < mask.y || sampleY > mask.y + mask.h ) {
                        continue;
                    }
                    count = std::max(count, mask.objectCount);
                }

                if ( count >= 2 ) {
                    if ( !hasRun ) {
                        hasRun   = true;
                        runStart = x0;
                        runCount = count;
                    } else if ( runCount != count ) {
                        flushRun(x0);
                        hasRun   = true;
                        runStart = x0;
                        runCount = count;
                    }
                } else {
                    flushRun(x0);
                }
            }

            flushRun(xs.back());
        }

        snapshot->overlapMasks = std::move(flattened);
    };

    flattenOverlapMasks();
    if ( snapshot->overlapMasks.empty() ) return;

    Batcher overlayBatcher(snapshot, &snapshot->overlayCmds);
    overlayBatcher.setScissor(leftX, topY, rightX - leftX, bottomY - topY);
    overlayBatcher.setTexture(TextureID::None);

    const glm::vec4 overlayColor{ 1.0f, 0.0f, 0.0f, 0.35f };
    for ( const auto& mask : snapshot->overlapMasks ) {
        overlayBatcher.pushQuad(
            mask.x, mask.y + mask.h, mask.w, mask.h, overlayColor);
    }
    overlayBatcher.flush();
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

    NoteComponent tempNote;
    tempNote.m_type         = brush.type;
    tempNote.m_timestamp    = brush.time;
    tempNote.m_duration     = brush.duration;
    tempNote.m_trackIndex   = brush.track;
    tempNote.m_dtrack       = brush.dtrack;
    tempNote.m_customColors = brush.customColors;

    glm::vec4 previewNote =
        resolveNoteColor(tempNote, NoteColorSlot::Tap, ctx.colorTap);
    glm::vec4 previewHead =
        resolveNoteColor(tempNote, NoteColorSlot::Head, ctx.colorHead);
    glm::vec4 previewBody =
        resolveNoteColor(tempNote, NoteColorSlot::Hold, ctx.colorHold);
    glm::vec4 previewEnd =
        resolveNoteColor(tempNote, NoteColorSlot::End, ctx.colorEnd);
    glm::vec4 previewNode =
        resolveNoteColor(tempNote, NoteColorSlot::Node, ctx.colorNode);
    glm::vec4 previewArrow =
        resolveNoteColor(tempNote, NoteColorSlot::FlickArrow, ctx.colorArrow);

    previewNote.a *= 0.5f;
    previewHead.a *= 0.5f;
    previewBody.a *= 0.5f;
    previewEnd.a *= 0.5f;
    previewNode.a *= 0.5f;
    previewArrow.a *= 0.5f;

    if ( brush.type == ::MMM::NoteType::NOTE ) {
        NoteRenderSystem::renderTap(batcher,
                                    tempNote,
                                    config,
                                    trackX + (singleTrackW - ctx.noteW) * 0.5f,
                                    screenY,
                                    ctx.noteW,
                                    ctx.noteH,
                                    ctx.baseAspect,
                                    previewNote);
    } else if ( brush.type == ::MMM::NoteType::HOLD ) {
        NoteRenderSystem::renderHold(batcher,
                                     tempNote,
                                     config,
                                     snapshot,
                                     trackX + (singleTrackW - ctx.noteW) * 0.5f,
                                     ctx.noteW,
                                     ctx.noteH,
                                     singleTrackW,
                                     previewHead,
                                     previewBody,
                                     previewEnd,
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
            previewHead,
            previewBody,
            previewArrow);
    } else if ( brush.type == ::MMM::NoteType::POLYLINE ) {
        tempNote.m_subNotes = brush.polylineSegments;
        if ( !tempNote.m_subNotes.empty() ) {
            tempNote.m_timestamp  = tempNote.m_subNotes.front().timestamp;
            tempNote.m_trackIndex = tempNote.m_subNotes.front().trackIndex;
        }

        float rightX  = leftX + snapshot->trackCount * singleTrackW;
        float topY    = 0.0f;
        float bottomY = judgmentLineY * 2.0f;  // 简单包围盒

        NoteRenderSystem::renderPolyline(ctx.cache,
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
                                         previewHead,
                                         previewBody,
                                         previewEnd,
                                         previewNode,
                                         previewArrow);
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
