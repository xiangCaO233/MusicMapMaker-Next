#include "logic/ecs/system/NoteRenderSystem.h"

#include "config/skin/SkinConfig.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
#include "logic/session/context/SessionContext.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

#include "logic/ecs/system/HitFXSystem.h"

namespace MMM::Logic::System
{

namespace
{

/// @brief 计算自动显示模式下指定分拍线的附加不透明度。
/// @param distanceToCursor 分拍线到光标中心的垂直像素距离。
/// @param viewportHeight 当前画布垂直范围。
/// @param visibleRatio 完全显示区域占画布垂直范围的比例。
/// @param fadeRatio 两侧渐隐区域合计占画布垂直范围的比例。
/// @return 范围在 0 到 1 之间的平滑不透明度倍率。
/// @warning 分拍线热路径逐线调用；仅允许常数次浮点运算。
float calculateCursorRevealAlpha(float distanceToCursor, float viewportHeight,
                                 float visibleRatio, float fadeRatio)
{
    const float visibleHalfSpan =
        viewportHeight * std::clamp(visibleRatio, 0.05f, 0.50f) * 0.5f;
    const float fadeHalfSpan =
        viewportHeight * std::clamp(fadeRatio, 0.02f, 0.40f) * 0.5f;
    if ( distanceToCursor <= visibleHalfSpan ) return 1.0f;
    if ( distanceToCursor >= visibleHalfSpan + fadeHalfSpan ) return 0.0f;

    const float progress = (distanceToCursor - visibleHalfSpan) / fadeHalfSpan;
    const float smoothProgress = progress * progress * (3.0f - 2.0f * progress);
    return 1.0f - smoothProgress;
}

}  // namespace

void NoteRenderSystem::renderTrackLayout(
    Batcher& batcher, float viewportWidth, float viewportHeight,
    float judgmentLineY, int32_t trackCount, const Config::EditorConfig& config,
    const entt::registry& timelineRegistry, double currentTime,
    const ScrollCache* cache, float& leftX, float& rightX, float& topY,
    float& bottomY, float& trackAreaW, float& singleTrackW, float renderScaleY)
{
    // 1. 基础布局计算 (确保范围有效，防止后续计算出现无限循环)
    float l = config.visual.trackLayout.left;
    float r = config.visual.trackLayout.right;
    float t = config.visual.trackLayout.top;
    float b = config.visual.trackLayout.bottom;

    // 强制保证 Left < Right, Top < Bottom
    if ( l >= r ) {
        r = l + 0.01f;
    }
    if ( t >= b ) {
        b = t + 0.01f;
    }

    const float horizontalOffsetX =
        batcher.snapshot ? batcher.snapshot->canvasHorizontalOffsetX : 0.0F;
    leftX        = viewportWidth * l + horizontalOffsetX;
    rightX       = viewportWidth * r + horizontalOffsetX;
    topY         = viewportHeight * t;
    bottomY      = viewportHeight * b;
    trackAreaW   = rightX - leftX;
    singleTrackW = trackAreaW / std::max(1.0f, static_cast<float>(trackCount));

    // 2. 绘制轨道底板
    NoteRenderSystem::drawTrackBackground(
        batcher, trackCount, leftX, topY, bottomY, singleTrackW);

    // 3. 绘制轨道包围框
    batcher.setTexture(TextureID::None);
    batcher.pushStrokeRect(leftX,
                           topY,
                           rightX,
                           bottomY,
                           config.visual.trackBoxLineWidth,
                           { 0.5f, 0.5f, 0.5f, 1.0f });

    // 4. 绘制判定区域
    NoteRenderSystem::drawJudgmentArea(batcher,
                                       trackCount,
                                       leftX,
                                       judgmentLineY,
                                       singleTrackW,
                                       trackAreaW,
                                       config);
}

void NoteRenderSystem::drawTrackBackground(Batcher& batcher, int32_t trackCount,
                                           float leftX, float topY,
                                           float bottomY, float singleTrackW)
{
    if ( trackCount <= 0 || singleTrackW <= 0.001f ) return;

    batcher.setTexture(TextureID::Track);
    auto uvIt =
        batcher.snapshot->uvMap.find(static_cast<uint32_t>(TextureID::Track));
    if ( uvIt == batcher.snapshot->uvMap.end() ) return;

    float texW_px = uvIt->second.z * 2048.0f;
    float texH_px = uvIt->second.w * 2048.0f;

    if ( texW_px <= 0 || texH_px <= 0 ) return;

    float texAspect = texW_px / texH_px;
    float drawH     = singleTrackW / texAspect;

    // 严防无限循环：若平铺高度过小，则跳过绘制
    if ( drawH <= 1.0f ) return;

    const float halfPixelU = 0.5f / 2048.0f;
    const float halfPixelV = 0.5f / 2048.0f;

    float uMin = uvIt->second.x + halfPixelU;
    float uMax = uvIt->second.x + uvIt->second.z - halfPixelU;

    for ( int i = 0; i < trackCount; ++i ) {
        float trackX   = leftX + i * singleTrackW;
        float currentY = bottomY;
        float drawW    = singleTrackW + 0.5f;

        while ( currentY > topY ) {
            float remainH     = currentY - topY;
            float actualDrawH = std::min(drawH, remainH);

            float vMax = 1.0f;
            float vMin = 1.0f - (actualDrawH / drawH);

            float finalVMin =
                uvIt->second.y + vMin * uvIt->second.w + halfPixelV;
            float finalVMax =
                uvIt->second.y + vMax * uvIt->second.w - halfPixelV;

            batcher.pushUVQuad(trackX,
                               currentY,
                               drawW,
                               actualDrawH + 0.5f,
                               glm::vec2(uMin, finalVMin),
                               glm::vec2(uMax, finalVMax),
                               { 1.0f, 1.0f, 1.0f, 1.0f });

            currentY -= drawH;
        }
    }
}

void NoteRenderSystem::drawJudgmentArea(Batcher& batcher, int32_t trackCount,
                                        float leftX, float judgmentLineY,
                                        float singleTrackW, float trackAreaW,
                                        const Config::EditorConfig& config)
{
    batcher.setTexture(TextureID::JudgeArea);
    auto judgeUvIt = batcher.snapshot->uvMap.find(
        static_cast<uint32_t>(TextureID::JudgeArea));

    if ( judgeUvIt != batcher.snapshot->uvMap.end() ) {
        float texW = judgeUvIt->second.z * 2048.0f;
        float texH = judgeUvIt->second.w * 2048.0f;
        if ( texW > 0 && texH > 0 ) {
            float aspect = texW / texH;
            float drawW  = singleTrackW * config.visual.noteScaleX;
            float drawH  = (singleTrackW / aspect) * config.visual.noteScaleY;

            const float halfPixelU = 0.5f / 2048.0f;
            const float halfPixelV = 0.5f / 2048.0f;

            for ( int i = 0; i < trackCount; ++i ) {
                float trackCenterX =
                    leftX + i * singleTrackW + singleTrackW * 0.5f;
                float drawX = trackCenterX - drawW * 0.5f;

                batcher.pushUVQuad(
                    drawX,
                    judgmentLineY + drawH * 0.5f,
                    drawW,
                    drawH,
                    glm::vec2(judgeUvIt->second.x + halfPixelU,
                              judgeUvIt->second.y + halfPixelV),
                    glm::vec2(
                        judgeUvIt->second.x + judgeUvIt->second.z - halfPixelU,
                        judgeUvIt->second.y + judgeUvIt->second.w - halfPixelV),
                    { 1.0f, 1.0f, 1.0f, 1.0f });
            }
        }
    } else {
        batcher.setTexture(TextureID::None);
        batcher.pushQuad(leftX,
                         judgmentLineY + 2.0f * 0.5f,
                         trackAreaW,
                         2.0f,
                         { 1.0f, 1.0f, 1.0f, 1.0f });
    }
}

void NoteRenderSystem::drawBeatLines(
    Batcher& batcher, float viewportHeight, float judgmentLineY,
    const Config::EditorConfig&                  config,
    const std::vector<const TimelineComponent*>& bpmEvents, double currentTime,
    const ScrollCache* cache, float leftX, float topY, float bottomY,
    float trackAreaW, float renderScaleY, bool revealNearCursor,
    float opacityScale, bool allowHoverSubdivisionPreview)
{
    if ( !cache ) return;
    if ( revealNearCursor && !batcher.snapshot->isHoveringCanvas ) return;

    int beatDivisor = config.settings.beatDivisor;
    if ( beatDivisor <= 0 ) beatDivisor = 4;

    if ( bpmEvents.empty() ) return;

    double currentAbsY = cache->getVisualAnchorAbsY(currentTime);
    if ( std::abs(renderScaleY) < 1e-6f ) return;
    const float cursorY =
        revealNearCursor
            ? judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                                  batcher.snapshot->hoveredTime,
                                  currentAbsY,
                                  batcher.snapshot->hoveredTime)) *
                                  renderScaleY
            : judgmentLineY;
    double topAbsY = currentAbsY +
                     (judgmentLineY - topY) / static_cast<double>(renderScaleY);
    double bottomAbsY    = currentAbsY + (judgmentLineY - bottomY) /
                                             static_cast<double>(renderScaleY);
    auto   visibleRanges = cache->getTimeRangesForAbsYWindow(
        std::min(topAbsY, bottomAbsY), std::max(topAbsY, bottomAbsY));

    batcher.setTexture(TextureID::None);

    float visibleTop    = std::min(topY, bottomY);
    float visibleBottom = std::max(topY, bottomY);
    int   rowCount      = std::max(
        1, static_cast<int>(std::ceil(visibleBottom - visibleTop)) + 1);
    std::vector<uint8_t> occupiedRows(static_cast<size_t>(rowCount), 0);
    int                  occupiedRowCount = 0;
    auto                 occupyRow        = [&](float y) {
        if ( y < visibleTop || y > visibleBottom ) return false;
        int row = static_cast<int>(std::floor(y - visibleTop));
        row     = std::clamp(row, 0, rowCount - 1);
        if ( occupiedRows[static_cast<size_t>(row)] != 0 ) return false;
        occupiedRows[static_cast<size_t>(row)] = 1;
        ++occupiedRowCount;
        return true;
    };

    auto& skin = Config::SkinManager::instance();
    float globalAlpha =
        config.visual.beatLineAlpha * std::clamp(opacityScale, 0.0F, 1.0F);
    auto getBeatLineConfig =
        [&skin, &config, globalAlpha](
            int denominator) -> std::pair<glm::vec4, float> {
        std::string   key = "beat_lines.beat_" + std::to_string(denominator);
        Config::Color c   = skin.getColor(key);
        if ( c.r == 1.0f && c.g == 0.0f && c.b == 1.0f && c.a == 1.0f ) {
            c   = skin.getColor("beat_lines.default");
            key = "beat_lines_width.default";
        } else {
            key = "beat_lines_width.beat_" + std::to_string(denominator);
        }
        float width =
            skin.getValue(key, skin.getValue("beat_lines_width.default", 2.0f));
        if ( config.visual.overrideBeatLineColors ) {
            const auto& overrideColor =
                config.visual.beatLineColors[Config::beatLineColorPaletteSlot(
                    denominator)];
            c = { overrideColor[0],
                  overrideColor[1],
                  overrideColor[2],
                  overrideColor[3] };
        }
        return { glm::vec4(c.r, c.g, c.b, c.a * globalAlpha), width };
    };

    const auto& subdivisionPreview = batcher.snapshot->hoverSubdivisionPreview;
    const auto  commonBeatDivisorMask =
        subdivisionPreview.commonBeatDivisorMask &
        Config::COMMON_BEAT_DIVISOR_MASK_ALL;
    const bool usesCommonBeatDivisors = commonBeatDivisorMask != 0U;
    const bool hasSubdivisionPreview =
        allowHoverSubdivisionPreview && subdivisionPreview.show &&
        batcher.snapshot->trackCount > 0 &&
        subdivisionPreview.track >= -batcher.snapshot->trackCount &&
        subdivisionPreview.track < batcher.snapshot->trackCount &&
        (usesCommonBeatDivisors || subdivisionPreview.denominator > 1) &&
        subdivisionPreview.denominator <= 128 &&
        subdivisionPreview.beatEndTime > subdivisionPreview.beatStartTime &&
        subdivisionPreview.beatDuration > 0.0;
    const float subdivisionTrackWidth =
        hasSubdivisionPreview
            ? trackAreaW / static_cast<float>(batcher.snapshot->trackCount)
            : 0.0F;
    const float subdivisionLeft =
        leftX +
        static_cast<float>(subdivisionPreview.track) * subdivisionTrackWidth;
    const float subdivisionRight = subdivisionLeft + subdivisionTrackWidth;
    const float subdivisionLineExtensionRatio = std::clamp(
        config.visual.hoverSubdivisionLineExtensionRatio, 0.0F, 1.0F);
    const float subdivisionLineLeft =
        std::max(leftX,
                 subdivisionLeft -
                     subdivisionTrackWidth * subdivisionLineExtensionRatio);
    const float subdivisionLineRight =
        std::min(leftX + trackAreaW,
                 subdivisionRight +
                     subdivisionTrackWidth * subdivisionLineExtensionRatio);
    const auto timeToCanvasY = [&](double time) {
        return judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                                   time, currentAbsY, time)) *
                                   renderScaleY;
    };
    const auto revealAlphaAt = [&](float y) {
        if ( !revealNearCursor ) return 1.0F;
        return calculateCursorRevealAlpha(
            std::abs(y - cursorY),
            viewportHeight,
            config.visual.beatLineCursorVisibleRatio,
            config.visual.beatLineCursorFadeRatio);
    };
    // 分段绘制允许只挖掉目标轨道该拍内的旧分拍线，同时保留其它轨道。
    const auto drawBeatLineSegment = [&](float     x,
                                         float     segmentWidth,
                                         float     y,
                                         float     lineWidth,
                                         glm::vec4 color,
                                         bool      glow) {
        if ( segmentWidth <= 0.001F ) return;
        if ( glow ) {
            glm::vec4 glowColor = color;
            glowColor.a *= 0.6F;
            batcher.pushQuad(x,
                             y + (lineWidth + 4.0F) * 0.5F,
                             segmentWidth,
                             lineWidth + 4.0F,
                             glowColor);
            glowColor.a *= 0.5F;
            batcher.pushQuad(x,
                             y + (lineWidth + 10.0F) * 0.5F,
                             segmentWidth,
                             lineWidth + 10.0F,
                             glowColor);
            glowColor.a *= 0.5F;
            batcher.pushQuad(x,
                             y + (lineWidth + 20.0F) * 0.5F,
                             segmentWidth,
                             lineWidth + 20.0F,
                             glowColor);
        }
        batcher.pushQuad(
            x, y + lineWidth * 0.5F, segmentWidth, lineWidth, color);
    };

    if ( hasSubdivisionPreview ) {
        const float beatStartY =
            timeToCanvasY(subdivisionPreview.beatStartTime);
        const float beatEndY = timeToCanvasY(subdivisionPreview.beatEndTime);
        const float highlightTop =
            std::max(visibleTop, std::min(beatStartY, beatEndY));
        const float highlightBottom =
            std::min(visibleBottom, std::max(beatStartY, beatEndY));
        if ( highlightBottom > highlightTop ) {
            auto [highlightColor, outlineWidth] =
                getBeatLineConfig(subdivisionPreview.denominator);
            const double inspectedTime =
                std::clamp(subdivisionPreview.focusTime,
                           subdivisionPreview.beatStartTime,
                           subdivisionPreview.beatEndTime);
            const float highlightReveal =
                revealAlphaAt(timeToCanvasY(inspectedTime));
            glm::vec4 outlineColor = highlightColor;
            highlightColor.a *= 0.38F * highlightReveal;
            outlineColor.a *= 0.95F * highlightReveal;
            batcher.pushQuad(subdivisionLeft,
                             highlightBottom,
                             subdivisionTrackWidth,
                             highlightBottom - highlightTop,
                             highlightColor);
            batcher.pushStrokeRect(subdivisionLeft,
                                   highlightTop,
                                   subdivisionRight,
                                   highlightBottom,
                                   std::max(2.0F, outlineWidth),
                                   outlineColor);
        }

        const auto drawSubdivisionLine = [&](int step, int divisor) {
            const int    common      = std::gcd(step, divisor);
            const int    numerator   = step / common;
            const int    denominator = divisor / common;
            const double lineTime    = subdivisionPreview.beatStartTime +
                                       subdivisionPreview.beatDuration *
                                           static_cast<double>(numerator) /
                                           static_cast<double>(denominator);
            if ( lineTime >= subdivisionPreview.beatEndTime - 1e-6 ) return;
            const float y = timeToCanvasY(lineTime);
            if ( y < visibleTop || y > visibleBottom ) return;

            auto [color, width] = getBeatLineConfig(denominator);
            color.a *= revealAlphaAt(y);
            drawBeatLineSegment(subdivisionLineLeft,
                                subdivisionLineRight - subdivisionLineLeft,
                                y,
                                width,
                                color,
                                false);
        };

        if ( usesCommonBeatDivisors ) {
            std::array<std::array<bool, Config::COMMON_BEAT_DIVISOR_MAX + 1>,
                       Config::COMMON_BEAT_DIVISOR_MAX + 1>
                renderedFractions{};
            for ( int divisor = Config::COMMON_BEAT_DIVISOR_MIN;
                  divisor <= Config::COMMON_BEAT_DIVISOR_MAX;
                  ++divisor ) {
                if ( !Config::isCommonBeatDivisorEnabled(commonBeatDivisorMask,
                                                         divisor) ) {
                    continue;
                }
                for ( int step = 1; step < divisor; ++step ) {
                    const int common      = std::gcd(step, divisor);
                    const int numerator   = step / common;
                    const int denominator = divisor / common;
                    if ( renderedFractions[denominator][numerator] ) continue;
                    renderedFractions[denominator][numerator] = true;
                    drawSubdivisionLine(step, divisor);
                }
            }
        } else {
            for ( int step = 1; step < subdivisionPreview.denominator;
                  ++step ) {
                drawSubdivisionLine(step, subdivisionPreview.denominator);
            }
        }
    }

    for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
        const auto* currentBPM = bpmEvents[i];
        double      bpmTime    = currentBPM->m_timestamp;
        double      bpmVal     = currentBPM->m_value;
        if ( bpmVal <= 0.0 ) {
            bpmVal = batcher.snapshot->fallbackBpm;
        }

        // 限制极端 BPM 导致的无限循环 (例如 osu! 谱面中的 6E-96 ms_per_beat)
        if ( bpmVal > 10000.0 ) bpmVal = 10000.0;
        if ( bpmVal <= 0.0 ) bpmVal = 120.0;

        double nextBpmTime = (i + 1 < bpmEvents.size())
                                 ? bpmEvents[i + 1]->m_timestamp
                                 : std::numeric_limits<double>::infinity();

        double beatDuration = 60.0 / bpmVal;
        double stepDuration = beatDuration / beatDivisor;

        for ( const auto& [startTime, endTime] : visibleRanges ) {
            if ( nextBpmTime <= startTime ) continue;
            double segmentStartTime = bpmTime;
            if ( i == 0 && config.visual.drawBeatLinesBeforeFirstTiming ) {
                segmentStartTime = startTime;
            }
            if ( segmentStartTime >= endTime ) continue;

            double  startCalcTime = std::max(segmentStartTime, startTime);
            int64_t stepOffset    = 0;
            if ( startCalcTime > bpmTime ) {
                stepOffset = static_cast<int64_t>(
                    std::ceil((startCalcTime - bpmTime) / stepDuration - 1e-4));
            } else if ( startCalcTime < bpmTime ) {
                stepOffset = static_cast<int64_t>(std::floor(
                    (startCalcTime - bpmTime) / stepDuration + 1e-4));
            }

            double t = bpmTime + stepOffset * stepDuration;
            while ( t < startCalcTime - 1e-4 ) {
                stepOffset++;
                t = bpmTime + stepOffset * stepDuration;
            }
            while ( t < nextBpmTime && t <= endTime ) {
                int beatIndex = static_cast<int>(stepOffset % beatDivisor);
                if ( beatIndex < 0 ) beatIndex += beatDivisor;
                int denominator = 1;
                if ( beatIndex != 0 ) {
                    int gcd     = std::gcd(beatIndex, beatDivisor);
                    denominator = beatDivisor / gcd;
                }

                float y =
                    judgmentLineY - static_cast<float>(cache->getDisplayDelta(
                                        t, currentAbsY, t)) *
                                        renderScaleY;

                float cursorRevealAlpha = 1.0f;
                if ( revealNearCursor ) {
                    cursorRevealAlpha = calculateCursorRevealAlpha(
                        std::abs(y - cursorY),
                        viewportHeight,
                        config.visual.beatLineCursorVisibleRatio,
                        config.visual.beatLineCursorFadeRatio);
                    if ( cursorRevealAlpha <= 0.0f ) {
                        stepOffset++;
                        t = bpmTime + stepOffset * stepDuration;
                        continue;
                    }
                }

                if ( y >= visibleTop && y <= visibleBottom && occupyRow(y) ) {
                    auto [color, width] = getBeatLineConfig(denominator);
                    color.a *= cursorRevealAlpha;
                    const bool glow =
                        batcher.snapshot->isSnapped &&
                        std::abs(t - batcher.snapshot->snappedTime) < 1e-6;
                    const bool replaceTargetTrack =
                        hasSubdivisionPreview &&
                        t > subdivisionPreview.beatStartTime + 1e-6 &&
                        t < subdivisionPreview.beatEndTime - 1e-6;
                    if ( replaceTargetTrack ) {
                        drawBeatLineSegment(leftX,
                                            subdivisionLeft - leftX,
                                            y,
                                            width,
                                            color,
                                            glow);
                        drawBeatLineSegment(
                            subdivisionRight,
                            leftX + trackAreaW - subdivisionRight,
                            y,
                            width,
                            color,
                            glow);
                    } else {
                        drawBeatLineSegment(
                            leftX, trackAreaW, y, width, color, glow);
                    }
                    if ( occupiedRowCount >= rowCount ) return;
                }
                stepOffset++;
                t = bpmTime + stepOffset * stepDuration;
            }
        }
    }
}

void NoteRenderSystem::drawTimingLines(Batcher& batcher, float viewportHeight,
                                       float judgmentLineY,
                                       const Config::EditorConfig& config,
                                       double                      currentTime,
                                       const ScrollCache* cache, float leftX,
                                       float topY, float bottomY,
                                       float trackAreaW, float renderScaleY)
{
    if ( !cache ) return;

    double currentAbsY = cache->getVisualAnchorAbsY(currentTime);
    if ( std::abs(renderScaleY) < 1e-6f ) return;
    batcher.setTexture(TextureID::None);

    float visibleTop    = std::min(topY, bottomY);
    float visibleBottom = std::max(topY, bottomY);
    int   rowCount      = std::max(
        1, static_cast<int>(std::ceil(visibleBottom - visibleTop)) + 1);
    std::vector<uint8_t> occupiedRows(static_cast<size_t>(rowCount), 0);
    int                  occupiedRowCount = 0;
    auto                 occupyRow        = [&](float y) {
        if ( y < visibleTop || y > visibleBottom ) return false;
        int row = static_cast<int>(std::floor(y - visibleTop));
        row     = std::clamp(row, 0, rowCount - 1);
        if ( occupiedRows[static_cast<size_t>(row)] != 0 ) return false;
        occupiedRows[static_cast<size_t>(row)] = 1;
        ++occupiedRowCount;
        return true;
    };

    for ( const auto& seg : cache->getSegments() ) {
        if ( seg.effects == 0 ) continue;  // 忽略没有效果的段（通常是第0段）

        const double segmentAbsY = seg.absY * cache->getAnimatedZoomScale();
        float y = judgmentLineY -
                  static_cast<float>((segmentAbsY - currentAbsY) * seg.hs) *
                      renderScaleY;

        if ( occupyRow(y) ) {
            glm::vec4 color = { 1.0f, 1.0f, 1.0f, 0.5f };
            if ( (seg.effects & SCROLL_EFFECT_BPM) &&
                 (seg.effects & SCROLL_EFFECT_SCROLL) ) {
                color = { 1.0f, 0.5f, 0.0f, 0.8f };
            } else if ( seg.effects & SCROLL_EFFECT_BPM ) {
                color = { 1.0f, 0.2f, 0.2f, 0.8f };
            } else if ( seg.effects & SCROLL_EFFECT_JUMP ) {
                color = { 0.2f, 0.45f, 1.0f, 0.8f };
            } else if ( seg.effects & SCROLL_EFFECT_HS ) {
                color = { 1.0f, 0.85f, 0.2f, 0.8f };
            } else if ( seg.effects & SCROLL_EFFECT_SCROLL ) {
                color = { 0.2f, 1.0f, 0.2f, 0.8f };
            }

            batcher.pushQuad(leftX, y + 1.0f, trackAreaW, 2.0f, color);
            if ( occupiedRowCount >= rowCount ) return;
        }
    }
}

}  // namespace MMM::Logic::System
