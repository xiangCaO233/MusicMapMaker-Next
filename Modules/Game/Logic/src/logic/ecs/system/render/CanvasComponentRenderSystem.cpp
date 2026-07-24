#include "logic/ecs/system/CanvasComponentRenderSystem.h"

#include "common/AsciiFontData.h"
#include "common/CanvasComponentLayout.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/system/render/Batcher.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace MMM::Logic::System
{

namespace
{

constexpr std::int64_t MAX_DISPLAY_MILLIS =
    ((99LL * 60LL + 59LL) * 60LL + 59LL) * 1000LL + 999LL;

/// @brief 绘制一行只包含 ASCII 字符的文本。
/// @param batcher 目标覆盖层批处理器。
/// @param text 以空字符结尾的 ASCII 文本。
/// @param placement 文本组件布局。
/// @param viewportWidth 主画布宽度。
/// @param viewportHeight 主画布高度。
/// @warning
/// 热路径：组件启用时每次主画布快照生成调用；只扫描短文本并生成字形四边形。
void renderAsciiText(Batcher& batcher, const char* text,
                     const Config::CanvasComponentPlacement& placement,
                     float viewportWidth, float viewportHeight)
{
    const auto  sanitized       = sanitizeCanvasComponentPlacement(placement);
    const float fontPixelHeight = sanitized.fontSizeRatio * viewportHeight;
    const auto  selection       = Common::selectAsciiFont(
        batcher.snapshot->asciiFontAtlasMetrics, fontPixelHeight);
    if ( !selection || !text ) return;

    const auto& font    = *selection.metrics;
    const auto textSize = Common::measureAsciiText(font, text, fontPixelHeight);
    if ( textSize.width <= 0.0f || textSize.height <= 0.0f ) return;

    const auto  bounds    = canvasComponentBounds(sanitized,
                                              viewportWidth,
                                              viewportHeight,
                                              textSize.width,
                                              textSize.height);
    const float baselineY = bounds.top + font.ascender * fontPixelHeight;
    float       penX      = bounds.left;

    for ( const char* cursor = text; *cursor != '\0'; ++cursor ) {
        const auto* glyph = font.glyph(*cursor);
        if ( !glyph || !glyph->available ) continue;

        if ( glyph->hasBitmap ) {
            const auto textureId =
                asciiGlyphTextureId(selection.tierIndex, *cursor);
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
                                   { 1.0f, 1.0f, 1.0f, 1.0f });
            }
        }
        penX += glyph->advanceX * fontPixelHeight;
    }
}

/// @brief 绘制当前判定线时间组件。
/// @param batcher 目标覆盖层批处理器。
/// @param currentTime 当前判定线时间。
/// @param viewportWidth 主画布宽度。
/// @param viewportHeight 主画布高度。
/// @param placement 组件布局。
/// @warning 热路径：组件启用时每次主画布快照生成调用；只生成固定长度 ASCII
/// 文本。
void renderJudgmentLineTime(Batcher& batcher, double currentTime,
                            float viewportWidth, float viewportHeight,
                            const Config::CanvasComponentPlacement& placement)
{
    const auto text =
        CanvasComponentRenderSystem::formatJudgmentLineTime(currentTime);
    renderAsciiText(
        batcher, text.data(), placement, viewportWidth, viewportHeight);
}

}  // namespace

void CanvasComponentRenderSystem::render(
    RenderSnapshot* snapshot, double currentTime, float viewportWidth,
    float viewportHeight, const Config::CanvasComponentLayoutConfig& config)
{
    if ( !snapshot || !snapshot->asciiFontAtlasMetrics.valid ||
         viewportWidth <= 0.0f || viewportHeight <= 0.0f ) {
        return;
    }

    Batcher batcher(snapshot, &snapshot->overlayCmds);
    batcher.setScissor(0.0f, 0.0f, viewportWidth, viewportHeight);

    for ( Config::CanvasComponentType type : Config::CANVAS_COMPONENT_TYPES ) {
        const auto& placement = config.placement(type);
        if ( !placement.visible ) continue;

        switch ( type ) {
        case Config::CanvasComponentType::JudgmentLineTime:
            renderJudgmentLineTime(
                batcher, currentTime, viewportWidth, viewportHeight, placement);
            break;
        case Config::CanvasComponentType::Count: break;
        }
    }
    batcher.flush();
}

std::array<char, 16> CanvasComponentRenderSystem::formatJudgmentLineTime(
    double currentTime)
{
    std::array<char, 16> result{};
    const bool   negative = std::isfinite(currentTime) && currentTime < -0.0005;
    const double finiteTime  = std::isfinite(currentTime) ? currentTime : 0.0;
    const auto   totalMillis = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(std::llround(std::abs(finiteTime) * 1000.0)),
        0LL,
        MAX_DISPLAY_MILLIS);
    const auto millis       = totalMillis % 1000LL;
    const auto totalSeconds = totalMillis / 1000LL;
    const auto seconds      = totalSeconds % 60LL;
    const auto totalMinutes = totalSeconds / 60LL;
    const auto minutes      = totalMinutes % 60LL;
    const auto hours        = totalMinutes / 60LL;

    std::snprintf(result.data(),
                  result.size(),
                  negative ? "-%02lld:%02lld:%02lld.%03lld"
                           : "%02lld:%02lld:%02lld.%03lld",
                  static_cast<long long>(hours),
                  static_cast<long long>(minutes),
                  static_cast<long long>(seconds),
                  static_cast<long long>(millis));
    return result;
}

}  // namespace MMM::Logic::System
