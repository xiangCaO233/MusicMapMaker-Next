#include "logic/ecs/system/SampleRenderSystem.h"

#include "common/AsciiFontData.h"
#include "config/skin/SkinConfig.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/ecs/system/ScrollCache.h"
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

/// @brief 将可能包含非 ASCII 字节的资源 ID 截断为可安全显示的 ASCII 文本。
/// @param output 输出缓冲区。
/// @param source 原资源 ID。
void copyDisplayResourceId(std::span<char> output, std::string_view source)
{
    if ( output.empty() ) return;
    const std::size_t count =
        std::min(source.size(), output.size() - std::size_t{ 1 });
    for ( std::size_t i = 0; i < count; ++i ) {
        const auto value = static_cast<unsigned char>(source[i]);
        output[i]        = value >= Common::ASCII_GLYPH_FIRST &&
                                   value <= Common::ASCII_GLYPH_LAST
                               ? static_cast<char>(value)
                               : '?';
    }
    output[count] = '\0';
}

/// @brief 在指定位置绘制单行 ASCII 文本。
/// @param batcher 目标批处理器。
/// @param text 文本。
/// @param x 左上角横坐标。
/// @param y 左上角纵坐标。
/// @param fontPixelHeight 字体像素高度。
/// @param maxWidth 最大可用宽度。
/// @param color 文字颜色。
/// @warning 主画布热路径：只处理有界短文本，不得加载字体或分配 GPU 资源。
void renderAsciiTextAt(Batcher& batcher, std::string_view text, float x,
                       float y, float fontPixelHeight, float maxWidth,
                       glm::vec4 color)
{
    const auto selection = Common::selectAsciiFont(
        batcher.snapshot->asciiFontAtlasMetrics, fontPixelHeight);
    if ( !selection || text.empty() || maxWidth <= 0.0F ) return;

    const auto& font      = *selection.metrics;
    const float baselineY = y + font.ascender * fontPixelHeight;
    float       penX      = x;
    for ( const char character : text ) {
        const auto* glyph = font.glyph(character);
        if ( !glyph || !glyph->available ) continue;
        const float advance = glyph->advanceX * fontPixelHeight;
        if ( penX + advance > x + maxWidth ) break;

        if ( glyph->hasBitmap ) {
            const auto textureId =
                asciiGlyphTextureId(selection.tierIndex, character);
            const auto uvIt = batcher.snapshot->uvMap.find(
                static_cast<std::uint32_t>(textureId));
            if ( textureId != TextureID::None &&
                 uvIt != batcher.snapshot->uvMap.end() ) {
                const float left = penX + glyph->bearingX * fontPixelHeight;
                const float top = baselineY - glyph->bearingY * fontPixelHeight;
                const float width  = glyph->width * fontPixelHeight;
                const float height = glyph->height * fontPixelHeight;
                const auto& uv     = uvIt->second;
                batcher.setTexture(textureId);
                batcher.pushUVQuad(left,
                                   top + height,
                                   width,
                                   height,
                                   { uv.x, uv.y },
                                   { uv.x + uv.z, uv.y + uv.w },
                                   color);
            }
        }
        penX += advance;
    }
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
        renderAsciiTextAt(batcher,
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
    const ScrollCache* cache, double currentTime, float judgmentLineY,
    float viewportWidth, float topY, float bottomY, float renderScaleY)
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
    const auto textColor =
        bgmColor("bgm_tracks.text", { 0.9F, 0.96F, 1.0F, 0.96F });

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
        if ( std::max(anchorY, effectiveY) < topY - 32.0F ||
             std::min(anchorY, effectiveY) > bottomY + 32.0F ) {
            continue;
        }

        const float laneWidth  = projection.player.singleTrackWidth;
        const float bodyWidth  = std::max(12.0F, laneWidth * 0.78F);
        const float bodyHeight = std::clamp(laneWidth * 0.24F, 16.0F, 28.0F);
        const float bodyX      = bounds->leftX + (laneWidth - bodyWidth) * 0.5F;
        const float centerX    = bounds->leftX + laneWidth * 0.5F;
        const float offsetHandleSize =
            std::clamp(laneWidth * 0.12F, 8.0F, 14.0F);
        const float offsetHandleCenterX = bounds->leftX + laneWidth * 0.82F;
        const float offsetHandleX =
            offsetHandleCenterX - offsetHandleSize * 0.5F;

        glm::vec4 bodyColor = sampleColor;
        if ( interaction && interaction->isSelected ) {
            bodyColor = selectedColor;
        } else if ( interaction && interaction->isHovered ) {
            bodyColor = hoveredColor;
        }

        batcher.setTexture(TextureID::None);
        if ( sample.m_offsetMs != 0 ) {
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
            renderAsciiTextAt(
                batcher,
                std::string_view(offsetBuffer.data(),
                                 static_cast<std::size_t>(offsetResult.out -
                                                          offsetBuffer.data())),
                bounds->leftX + 4.0F,
                effectiveY + 4.0F,
                11.0F,
                laneWidth - 8.0F,
                offsetColor);
        }
        batcher.pushQuad(bounds->leftX + laneWidth * 0.18F,
                         effectiveY + 1.5F,
                         laneWidth * 0.64F,
                         3.0F,
                         offsetColor);
        batcher.pushRoundedQuad(offsetHandleX,
                                effectiveY + offsetHandleSize * 0.5F,
                                offsetHandleSize,
                                offsetHandleSize,
                                offsetHandleSize * 0.25F,
                                offsetColor);

        batcher.setTexture(TextureID::None);
        batcher.pushRoundedQuad(bodyX,
                                anchorY + bodyHeight * 0.5F,
                                bodyWidth,
                                bodyHeight,
                                4.0F,
                                bodyColor);
        batcher.pushRoundedStrokeRect(bodyX,
                                      anchorY + bodyHeight * 0.5F,
                                      bodyWidth,
                                      bodyHeight,
                                      4.0F,
                                      1.5F,
                                      { 0.9F, 0.96F, 1.0F, 0.9F });

        std::array<char, 72> resourceBuffer{};
        copyDisplayResourceId(resourceBuffer, sample.m_audioResourceId);
        const auto idLength =
            std::char_traits<char>::length(resourceBuffer.data());
        std::size_t resourceLength = idLength;
        if ( resourceLength < resourceBuffer.size() - 1 ) {
            auto* const volumeBegin = resourceBuffer.data() + resourceLength;
            const auto  volumeResult =
                fmt::format_to_n(volumeBegin,
                                 resourceBuffer.size() - resourceLength - 1,
                                 " {:.0f}%",
                                 static_cast<double>(sample.m_volume) * 100.0);
            *volumeResult.out = '\0';
            resourceLength +=
                static_cast<std::size_t>(volumeResult.out - volumeBegin);
        }
        renderAsciiTextAt(
            batcher,
            std::string_view(resourceBuffer.data(), resourceLength),
            bodyX + 4.0F,
            anchorY - 6.0F,
            11.0F,
            bodyWidth - 8.0F,
            textColor);

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

}  // namespace MMM::Logic::System
