#include "logic/ecs/system/CanvasComponentRenderSystem.h"

#include "common/CanvasComponentLayout.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/system/render/Batcher.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <glm/glm.hpp>

namespace MMM::Logic::System
{

namespace
{

constexpr std::uint8_t SEGMENT_TOP         = 1U << 0U;
constexpr std::uint8_t SEGMENT_UPPER_RIGHT = 1U << 1U;
constexpr std::uint8_t SEGMENT_LOWER_RIGHT = 1U << 2U;
constexpr std::uint8_t SEGMENT_BOTTOM      = 1U << 3U;
constexpr std::uint8_t SEGMENT_LOWER_LEFT  = 1U << 4U;
constexpr std::uint8_t SEGMENT_UPPER_LEFT  = 1U << 5U;
constexpr std::uint8_t SEGMENT_MIDDLE      = 1U << 6U;
constexpr std::int64_t MAX_DISPLAY_MILLIS =
    ((99LL * 60LL + 59LL) * 60LL + 59LL) * 1000LL + 999LL;

/// @brief 取得七段数字对应的点亮笔画掩码。
/// @param digit ASCII 数字。
/// @return 七段笔画位掩码；非数字返回 0。
[[nodiscard]] constexpr std::uint8_t digitSegmentMask(char digit)
{
    switch ( digit ) {
    case '0':
        return SEGMENT_TOP | SEGMENT_UPPER_RIGHT | SEGMENT_LOWER_RIGHT |
               SEGMENT_BOTTOM | SEGMENT_LOWER_LEFT | SEGMENT_UPPER_LEFT;
    case '1': return SEGMENT_UPPER_RIGHT | SEGMENT_LOWER_RIGHT;
    case '2':
        return SEGMENT_TOP | SEGMENT_UPPER_RIGHT | SEGMENT_MIDDLE |
               SEGMENT_LOWER_LEFT | SEGMENT_BOTTOM;
    case '3':
        return SEGMENT_TOP | SEGMENT_UPPER_RIGHT | SEGMENT_MIDDLE |
               SEGMENT_LOWER_RIGHT | SEGMENT_BOTTOM;
    case '4':
        return SEGMENT_UPPER_LEFT | SEGMENT_MIDDLE | SEGMENT_UPPER_RIGHT |
               SEGMENT_LOWER_RIGHT;
    case '5':
        return SEGMENT_TOP | SEGMENT_UPPER_LEFT | SEGMENT_MIDDLE |
               SEGMENT_LOWER_RIGHT | SEGMENT_BOTTOM;
    case '6':
        return SEGMENT_TOP | SEGMENT_UPPER_LEFT | SEGMENT_MIDDLE |
               SEGMENT_LOWER_LEFT | SEGMENT_LOWER_RIGHT | SEGMENT_BOTTOM;
    case '7': return SEGMENT_TOP | SEGMENT_UPPER_RIGHT | SEGMENT_LOWER_RIGHT;
    case '8':
        return SEGMENT_TOP | SEGMENT_UPPER_RIGHT | SEGMENT_LOWER_RIGHT |
               SEGMENT_BOTTOM | SEGMENT_LOWER_LEFT | SEGMENT_UPPER_LEFT |
               SEGMENT_MIDDLE;
    case '9':
        return SEGMENT_TOP | SEGMENT_UPPER_RIGHT | SEGMENT_LOWER_RIGHT |
               SEGMENT_BOTTOM | SEGMENT_UPPER_LEFT | SEGMENT_MIDDLE;
    default: return 0;
    }
}

/// @brief 向批处理器推送使用顶部坐标描述的矩形。
/// @param batcher 目标覆盖层批处理器。
/// @param left 左侧坐标。
/// @param top 顶部坐标。
/// @param width 宽度。
/// @param height 高度。
/// @param color 颜色。
void pushTopRect(Batcher& batcher, float left, float top, float width,
                 float height, glm::vec4 color)
{
    batcher.pushQuad(left, top + height, width, height, color);
}

/// @brief 取得单个时间字符的横向步进。
/// @param character 时间字符。
/// @param digitWidth 数字宽度。
/// @param thickness 笔画厚度。
/// @param gap 字符间距。
/// @return 字符绘制完成后的横向步进。
[[nodiscard]] float characterAdvance(char character, float digitWidth,
                                     float thickness, float gap)
{
    if ( character == ':' || character == '.' ) {
        return thickness * 2.0f + gap;
    }
    if ( character == '-' ) {
        return digitWidth * 0.75f + gap;
    }
    return digitWidth + gap;
}

/// @brief 绘制单个七段时间字符。
/// @param batcher 目标覆盖层批处理器。
/// @param character 时间字符。
/// @param left 字符左侧坐标。
/// @param top 字符顶部坐标。
/// @param digitWidth 数字宽度。
/// @param digitHeight 数字高度。
/// @param thickness 笔画厚度。
/// @param color 笔画颜色。
void drawTimeCharacter(Batcher& batcher, char character, float left, float top,
                       float digitWidth, float digitHeight, float thickness,
                       glm::vec4 color)
{
    const float middleY = top + (digitHeight - thickness) * 0.5f;
    if ( character == ':' ) {
        const float dotSize = thickness * 1.35f;
        const float dotX    = left + thickness - dotSize * 0.5f;
        pushTopRect(batcher,
                    dotX,
                    top + digitHeight * 0.27f - dotSize * 0.5f,
                    dotSize,
                    dotSize,
                    color);
        pushTopRect(batcher,
                    dotX,
                    top + digitHeight * 0.73f - dotSize * 0.5f,
                    dotSize,
                    dotSize,
                    color);
        return;
    }
    if ( character == '.' ) {
        const float dotSize = thickness * 1.35f;
        pushTopRect(batcher,
                    left + thickness - dotSize * 0.5f,
                    top + digitHeight - dotSize,
                    dotSize,
                    dotSize,
                    color);
        return;
    }
    if ( character == '-' ) {
        pushTopRect(
            batcher, left, middleY, digitWidth * 0.75f, thickness, color);
        return;
    }

    const std::uint8_t mask = digitSegmentMask(character);
    const float        verticalHeight =
        std::max(thickness, (digitHeight - 3.0f * thickness) * 0.5f);
    if ( (mask & SEGMENT_TOP) != 0U ) {
        pushTopRect(batcher, left, top, digitWidth, thickness, color);
    }
    if ( (mask & SEGMENT_UPPER_RIGHT) != 0U ) {
        pushTopRect(batcher,
                    left + digitWidth - thickness,
                    top + thickness,
                    thickness,
                    verticalHeight,
                    color);
    }
    if ( (mask & SEGMENT_LOWER_RIGHT) != 0U ) {
        pushTopRect(batcher,
                    left + digitWidth - thickness,
                    middleY + thickness,
                    thickness,
                    verticalHeight,
                    color);
    }
    if ( (mask & SEGMENT_BOTTOM) != 0U ) {
        pushTopRect(batcher,
                    left,
                    top + digitHeight - thickness,
                    digitWidth,
                    thickness,
                    color);
    }
    if ( (mask & SEGMENT_LOWER_LEFT) != 0U ) {
        pushTopRect(batcher,
                    left,
                    middleY + thickness,
                    thickness,
                    verticalHeight,
                    color);
    }
    if ( (mask & SEGMENT_UPPER_LEFT) != 0U ) {
        pushTopRect(
            batcher, left, top + thickness, thickness, verticalHeight, color);
    }
    if ( (mask & SEGMENT_MIDDLE) != 0U ) {
        pushTopRect(batcher, left, middleY, digitWidth, thickness, color);
    }
}

/// @brief 绘制当前判定线时间组件。
/// @param batcher 目标覆盖层批处理器。
/// @param currentTime 当前判定线时间。
/// @param viewportWidth 主画布宽度。
/// @param viewportHeight 主画布高度。
/// @param placement 组件布局。
/// @warning 热路径：组件启用时每次主画布快照生成调用；只生成常量数量几何。
void renderJudgmentLineTime(Batcher& batcher, double currentTime,
                            float viewportWidth, float viewportHeight,
                            const Config::CanvasComponentPlacement& placement)
{
    const auto bounds =
        canvasComponentBounds(Config::CanvasComponentType::JudgmentLineTime,
                              placement,
                              viewportWidth,
                              viewportHeight);
    if ( bounds.width() <= 0.0f || bounds.height() <= 0.0f ) return;

    const float rounding = bounds.height() * 0.18f;
    batcher.pushRoundedQuad(bounds.left,
                            bounds.bottom,
                            bounds.width(),
                            bounds.height(),
                            rounding,
                            { 0.025f, 0.04f, 0.055f, 0.82f });
    batcher.pushRoundedStrokeRect(bounds.left,
                                  bounds.bottom,
                                  bounds.width(),
                                  bounds.height(),
                                  rounding,
                                  1.5f,
                                  { 0.25f, 0.78f, 1.0f, 0.9f });

    const auto text =
        CanvasComponentRenderSystem::formatJudgmentLineTime(currentTime);
    const float digitHeight = bounds.height() * 0.56f;
    const float digitWidth  = digitHeight * 0.55f;
    const float thickness   = std::max(1.25f, digitHeight * 0.11f);
    const float gap         = digitHeight * 0.10f;
    float       textWidth   = 0.0f;
    for ( std::size_t i = 0; i < text.size() && text[i] != '\0'; ++i ) {
        textWidth += characterAdvance(text[i], digitWidth, thickness, gap);
    }
    if ( textWidth > 0.0f ) textWidth -= gap;

    float           cursorX = bounds.left + (bounds.width() - textWidth) * 0.5f;
    const float     top = bounds.top + (bounds.height() - digitHeight) * 0.5f;
    const glm::vec4 color{ 0.88f, 0.97f, 1.0f, 0.98f };
    for ( std::size_t i = 0; i < text.size() && text[i] != '\0'; ++i ) {
        drawTimeCharacter(batcher,
                          text[i],
                          cursorX,
                          top,
                          digitWidth,
                          digitHeight,
                          thickness,
                          color);
        cursorX += characterAdvance(text[i], digitWidth, thickness, gap);
    }
}

}  // namespace

void CanvasComponentRenderSystem::render(
    RenderSnapshot* snapshot, double currentTime, float viewportWidth,
    float viewportHeight, const Config::CanvasComponentLayoutConfig& config)
{
    if ( !snapshot || viewportWidth <= 0.0f || viewportHeight <= 0.0f ) return;

    Batcher batcher(snapshot, &snapshot->overlayCmds);
    batcher.setScissor(0.0f, 0.0f, viewportWidth, viewportHeight);
    batcher.setTexture(TextureID::None);

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
