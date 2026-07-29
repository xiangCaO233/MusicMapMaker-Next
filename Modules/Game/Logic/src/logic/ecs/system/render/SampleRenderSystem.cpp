#include "logic/ecs/system/SampleRenderSystem.h"

#include "config/EditorConfig.h"
#include "config/skin/SkinConfig.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/AudioObjectLabelRenderer.h"
#include "logic/ecs/system/render/Batcher.h"
#include "logic/session/context/SessionContext.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace MMM::Logic::System
{

namespace
{

/// @brief 判断皮肤颜色是否为缺省的洋红哨兵。
/// @param color 皮肤颜色。
/// @return 颜色为缺省哨兵时返回 true。
bool isMissingSkinColor(const Config::Color& color)
{
    return color.r == 1.0F && color.g == 0.0F && color.b == 1.0F &&
           color.a == 1.0F;
}

/// @brief 获取可回退的 BGM 轨道皮肤颜色。
/// @param key 皮肤颜色键。
/// @param fallback 缺失时使用的颜色。
/// @return 转换后的 GLM 颜色。
glm::vec4 bgmColor(std::string_view key, glm::vec4 fallback)
{
    const auto color =
        Config::SkinManager::instance().getColor(std::string(key));
    if ( isMissingSkinColor(color) ) return fallback;
    return { color.r, color.g, color.b, color.a };
}

/// @brief 获取自动采样锚点与实际触发点覆盖的时间区间。
/// @param sample 自动采样组件。
/// @return `[min,max]` 时间区间，单位秒。
std::pair<double, double> sampleTimeRange(const SampleComponent& sample)
{
    return std::minmax(sample.m_timestamp, sample.effectiveTime());
}

/// @brief 从排序索引收集当前可见时间范围内的自动采样候选。
/// @param registry 自动采样注册表。
/// @param sortedEntities 已按区间起点排序的实体。
/// @param maxEndPrefix 区间终点前缀最大值。
/// @param visibleRanges 当前滚动窗口映射出的可见时间范围。
/// @param result 输出实体列表。
/// @param seen 输出去重集合。
/// @warning 主画布热路径：只遍历索引命中的候选区间。
void collectVisibleSamples(
    entt::registry& registry, const std::vector<entt::entity>& sortedEntities,
    const std::vector<double>&                 maxEndPrefix,
    std::span<const std::pair<double, double>> visibleRanges,
    std::vector<entt::entity>& result, std::unordered_set<entt::entity>& seen)
{
    result.clear();
    seen.clear();
    if ( sortedEntities.empty() ||
         maxEndPrefix.size() != sortedEntities.size() ) {
        return;
    }

    for ( const auto& rawRange : visibleRanges ) {
        const double rangeStart =
            std::min(rawRange.first, rawRange.second) - 0.25;
        const double rangeEnd =
            std::max(rawRange.first, rawRange.second) + 0.25;
        const auto high = std::upper_bound(
            sortedEntities.begin(),
            sortedEntities.end(),
            rangeEnd,
            [&registry](double value, entt::entity entity) {
                if ( !registry.valid(entity) ||
                     !registry.all_of<SampleComponent>(entity) ) {
                    return false;
                }
                const auto range = sampleTimeRange(
                    registry.get<const SampleComponent>(entity));
                return value < range.first;
            });
        const auto highIndex =
            static_cast<std::size_t>(high - sortedEntities.begin());
        const auto low = std::lower_bound(
            maxEndPrefix.begin(), maxEndPrefix.begin() + highIndex, rangeStart);
        const auto lowIndex =
            static_cast<std::size_t>(low - maxEndPrefix.begin());

        for ( std::size_t index = lowIndex; index < highIndex; ++index ) {
            const auto entity = sortedEntities[index];
            if ( !registry.valid(entity) ||
                 !registry.all_of<SampleComponent>(entity) ) {
                continue;
            }
            const auto range =
                sampleTimeRange(registry.get<const SampleComponent>(entity));
            if ( range.second < rangeStart || range.first > rangeEnd ) continue;
            if ( seen.insert(entity).second ) {
                result.push_back(entity);
            }
        }
    }
}

}  // namespace

void SampleRenderSystem::renderLaneLayout(
    Batcher& batcher, const CanvasLaneProjection& projection,
    std::int32_t persistentBgmTrackCount, float viewportWidth, float topY,
    float bottomY)
{
    if ( !projection.valid || projection.bgmLaneCount == 0 ||
         bottomY <= topY ) {
        return;
    }
    const auto visibleRange = projection.visibleBgmRange(0.0F, viewportWidth);
    if ( !visibleRange ) return;

    const auto background =
        bgmColor("bgm_tracks.background", { 0.035F, 0.055F, 0.075F, 0.92F });
    const auto alternate =
        bgmColor("bgm_tracks.alternate", { 0.055F, 0.08F, 0.105F, 0.92F });
    const auto border =
        bgmColor("bgm_tracks.border", { 0.32F, 0.48F, 0.62F, 0.55F });
    const auto separator =
        bgmColor("bgm_tracks.separator", { 0.28F, 0.78F, 0.94F, 0.95F });
    const auto label =
        bgmColor("bgm_tracks.label", { 0.72F, 0.88F, 0.96F, 0.92F });

    const float visibleLeft  = std::max(0.0F, projection.bgmLeftX);
    const float visibleRight = std::min(viewportWidth, projection.bgmRightX);
    batcher.setScissor(
        visibleLeft, topY, visibleRight - visibleLeft, bottomY - topY);
    batcher.setTexture(TextureID::None);

    const auto [begin, end] = *visibleRange;
    for ( std::uint32_t index = begin; index < end; ++index ) {
        const auto bounds = projection.bounds({ CanvasLaneKind::Bgm, index });
        if ( !bounds ) continue;
        batcher.pushQuad(bounds->leftX,
                         bottomY,
                         projection.player.singleTrackWidth,
                         bottomY - topY,
                         index % 2 == 0 ? background : alternate);
        batcher.pushQuad(bounds->leftX, bottomY, 1.5F, bottomY - topY, border);

        std::array<char, 32> labelBuffer{};
        const bool appendLane = index == static_cast<std::uint32_t>(std::max(
                                             0, persistentBgmTrackCount));
        std::string_view labelText{ "BGM +" };
        if ( appendLane ) {
            labelText = "BGM +";
        } else {
            const auto result = fmt::format_to_n(labelBuffer.data(),
                                                 labelBuffer.size() - 1,
                                                 "BGM {}",
                                                 index + 1);
            *result.out       = '\0';
            labelText         = std::string_view(
                labelBuffer.data(),
                static_cast<std::size_t>(result.out - labelBuffer.data()));
        }
        renderCanvasAsciiText(batcher,
                              labelText,
                              bounds->leftX + 4.0F,
                              topY + 4.0F,
                              12.0F,
                              projection.player.singleTrackWidth - 8.0F,
                              label);
    }

    if ( projection.bgmLeftX >= 0.0F && projection.bgmLeftX <= viewportWidth ) {
        batcher.setTexture(TextureID::None);
        batcher.pushQuad(projection.bgmLeftX - 1.5F,
                         bottomY,
                         3.0F,
                         bottomY - topY,
                         separator);
    }
}

void SampleRenderSystem::renderSamples(
    entt::registry& registry, const std::vector<entt::entity>& sortedEntities,
    const std::vector<double>& maxEndPrefix, RenderSnapshot* snapshot,
    Batcher& batcher, const CanvasLaneProjection& projection,
    const ScrollCache* cache, const Config::EditorConfig& config,
    double currentTime, float judgmentLineY, float viewportWidth, float topY,
    float bottomY, float renderScaleY)
{
    if ( !snapshot || !cache || !projection.valid ||
         std::abs(renderScaleY) < 1e-6F ) {
        return;
    }
    const auto visibleLaneRange =
        projection.visibleBgmRange(0.0F, viewportWidth);

    const double currentAbsY = cache->getVisualAnchorAbsY(currentTime);
    const double topAbsY = currentAbsY + (judgmentLineY - topY) /
                                             static_cast<double>(renderScaleY);
    const double bottomAbsY =
        currentAbsY +
        (judgmentLineY - bottomY) / static_cast<double>(renderScaleY);
    const auto visibleTimeRanges = cache->getTimeRangesForAbsYWindow(
        std::min(topAbsY, bottomAbsY), std::max(topAbsY, bottomAbsY));
    snapshot->sampleQueryScratch.clear();
    snapshot->sampleQuerySeenScratch.clear();
    if ( visibleLaneRange ) {
        collectVisibleSamples(registry,
                              sortedEntities,
                              maxEndPrefix,
                              visibleTimeRanges,
                              snapshot->sampleQueryScratch,
                              snapshot->sampleQuerySeenScratch);
    }
    if ( const auto* pinned = registry.ctx().find<DragRenderPinnedEntities>();
         pinned && pinned->entities ) {
        for ( const auto entity : *pinned->entities ) {
            if ( !registry.valid(entity) ||
                 !registry.all_of<SampleComponent>(entity) ||
                 !snapshot->sampleQuerySeenScratch.insert(entity).second ) {
                continue;
            }
            snapshot->sampleQueryScratch.push_back(entity);
        }
    }
    if ( snapshot->sampleQueryScratch.empty() ) return;

    const auto sampleColor =
        bgmColor("bgm_tracks.sample", { 0.36F, 0.72F, 0.92F, 0.96F });
    const auto selectedColor =
        bgmColor("bgm_tracks.sample_selected", { 1.0F, 0.78F, 0.24F, 1.0F });
    const auto hoveredColor =
        bgmColor("bgm_tracks.sample_hovered", { 0.58F, 0.9F, 1.0F, 1.0F });
    const auto offsetColor =
        bgmColor("bgm_tracks.offset", { 0.96F, 0.56F, 0.28F, 0.92F });
    const auto textColor = audioObjectLabelColor();
    const auto noteTextureIt =
        snapshot->uvMap.find(static_cast<std::uint32_t>(TextureID::Note));
    const bool  hasNoteTexture = noteTextureIt != snapshot->uvMap.end() &&
                                 std::isfinite(noteTextureIt->second.z) &&
                                 std::isfinite(noteTextureIt->second.w) &&
                                 noteTextureIt->second.z > 1e-6F &&
                                 noteTextureIt->second.w > 1e-6F;
    const float noteTextureAspect =
        hasNoteTexture ? noteTextureIt->second.z / noteTextureIt->second.w
                       : 1.0F;
    const float sampleTextScale = std::isfinite(config.visual.noteScaleY) &&
                                          config.visual.noteScaleY > 0.0F
                                      ? config.visual.noteScaleY
                                      : 1.0F;

    batcher.setScissor(0.0F, topY, viewportWidth, bottomY - topY);

    for ( const auto entity : snapshot->sampleQueryScratch ) {
        if ( !registry.valid(entity) ||
             !registry.all_of<SampleComponent>(entity) ) {
            continue;
        }
        const auto& sample  = registry.get<const SampleComponent>(entity);
        const auto  address = CanvasLaneAddress::fromAbsoluteTrack(
            sample.m_track, projection.playerLaneCount);
        const auto* interaction =
            registry.try_get<const InteractionComponent>(entity);
        const bool isDragging = interaction && interaction->isDragging;
        if ( address.kind == CanvasLaneKind::Player && !isDragging ) {
            continue;
        }
        if ( address.kind == CanvasLaneKind::Bgm &&
             (!visibleLaneRange || address.index < visibleLaneRange->first ||
              address.index >= visibleLaneRange->second) ) {
            continue;
        }
        const auto bounds = projection.bounds(address);
        if ( !bounds || bounds->rightX <= 0.0F ||
             bounds->leftX >= viewportWidth ) {
            continue;
        }

        const float anchorY =
            judgmentLineY -
            static_cast<float>(cache->getDisplayDelta(
                sample.m_timestamp, currentAbsY, sample.m_timestamp)) *
                renderScaleY;
        const double effectiveTime = sample.effectiveTime();
        const float  effectiveY =
            judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                                effectiveTime, currentAbsY, effectiveTime)) *
                                renderScaleY;

        const float laneWidth = projection.player.singleTrackWidth;
        const float bodyWidth = laneWidth * config.visual.noteScaleX;
        const float bodyHeight =
            (laneWidth / noteTextureAspect) * config.visual.noteScaleY;
        const float verticalPadding =
            std::max(32.0F, bodyHeight * 0.5F + 24.0F);
        if ( std::max(anchorY, effectiveY) < topY - verticalPadding ||
             std::min(anchorY, effectiveY) > bottomY + verticalPadding ) {
            continue;
        }

        const float bodyX   = bounds->leftX + (laneWidth - bodyWidth) * 0.5F;
        const float centerX = bounds->leftX + laneWidth * 0.5F;
        const float offsetHandleSize =
            std::clamp(laneWidth * 0.12F, 8.0F, 14.0F);
        const float offsetHandleCenterX =
            bodyX + bodyWidth - offsetHandleSize * 0.5F;
        const float offsetHandleX =
            offsetHandleCenterX - offsetHandleSize * 0.5F;
        const bool showOffsetHandle =
            sample.m_offsetMs != 0 ||
            (interaction &&
             (interaction->isSelected || interaction->isHovered));

        glm::vec4 bodyColor = sampleColor;
        if ( interaction && interaction->isSelected ) {
            bodyColor = selectedColor;
        } else if ( interaction && interaction->isHovered ) {
            bodyColor = hoveredColor;
        }

        if ( sample.m_offsetMs != 0 ) {
            batcher.setTexture(TextureID::None);
            const float connectorTop =
                std::max(topY, std::min(anchorY, effectiveY));
            const float connectorBottom =
                std::min(bottomY, std::max(anchorY, effectiveY));
            if ( connectorBottom >= connectorTop ) {
                batcher.pushQuad(centerX - 1.0F,
                                 connectorBottom,
                                 2.0F,
                                 connectorBottom - connectorTop,
                                 offsetColor);
            }
            std::array<char, 32> offsetBuffer{};
            const auto offsetResult = fmt::format_to_n(offsetBuffer.data(),
                                                       offsetBuffer.size() - 1,
                                                       "{:+} ms",
                                                       sample.m_offsetMs);
            *offsetResult.out       = '\0';
            renderCanvasAsciiText(
                batcher,
                std::string_view(offsetBuffer.data(),
                                 static_cast<std::size_t>(offsetResult.out -
                                                          offsetBuffer.data())),
                bounds->leftX + 4.0F,
                effectiveY + 4.0F,
                11.0F * sampleTextScale,
                laneWidth - 8.0F,
                offsetColor);
            batcher.pushQuad(bounds->leftX + laneWidth * 0.18F,
                             effectiveY + 1.5F,
                             laneWidth * 0.64F,
                             3.0F,
                             offsetColor);
        }
        if ( showOffsetHandle ) {
            batcher.setTexture(TextureID::None);
            batcher.pushRoundedQuad(offsetHandleX,
                                    effectiveY + offsetHandleSize * 0.5F,
                                    offsetHandleSize,
                                    offsetHandleSize,
                                    offsetHandleSize * 0.25F,
                                    offsetColor);
        }

        if ( hasNoteTexture ) {
            batcher.setTexture(TextureID::Note);
            batcher.pushFilledQuad(bodyX,
                                   anchorY + bodyHeight * 0.5F,
                                   bodyWidth,
                                   bodyHeight,
                                   { noteTextureAspect, 1.0F },
                                   config.visual.noteFillMode,
                                   bodyColor);
        } else {
            batcher.setTexture(TextureID::None);
            batcher.pushRoundedQuad(bodyX,
                                    anchorY + bodyHeight * 0.5F,
                                    bodyWidth,
                                    bodyHeight,
                                    4.0F,
                                    bodyColor);
        }

        renderAudioObjectLabel(batcher,
                               sample.m_audioResourceId,
                               sample.m_volume,
                               bounds->leftX,
                               anchorY - bodyHeight * 0.5F,
                               laneWidth,
                               config.visual.noteScaleY,
                               textColor,
                               snapshot->snapshotSysTime);

        if ( !snapshot->isPlaying && snapshot->acceptsInteraction ) {
            snapshot->hitboxes.push_back({
                entity,
                HoverPart::SampleAnchor,
                -1,
                bodyX,
                anchorY - bodyHeight * 0.5F,
                bodyWidth,
                bodyHeight,
                ChartObjectKind::AudioSample,
            });
            if ( showOffsetHandle ) {
                snapshot->hitboxes.push_back({
                    entity,
                    HoverPart::SampleOffset,
                    -1,
                    offsetHandleX,
                    effectiveY - offsetHandleSize * 0.5F,
                    offsetHandleSize,
                    offsetHandleSize,
                    ChartObjectKind::AudioSample,
                });
            }
        }
    }
}

}  // namespace MMM::Logic::System
