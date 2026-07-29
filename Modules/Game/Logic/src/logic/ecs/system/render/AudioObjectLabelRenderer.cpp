#include "logic/ecs/system/render/AudioObjectLabelRenderer.h"

#include "common/AsciiFontData.h"
#include "config/skin/SkinConfig.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/system/render/Batcher.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>

namespace MMM::Logic::System
{

namespace
{

/// @brief 音频物件标签皮肤颜色键，避免热路径反复构造字符串。
const std::string AUDIO_OBJECT_LABEL_COLOR_KEY{ "bgm_tracks.text" };

/// @brief 判断皮肤颜色是否为缺省的洋红哨兵。
/// @param color 皮肤颜色。
/// @return 颜色为缺省哨兵时返回 true。
/// @warning 主画布热路径：保持纯数值判断，不得查询皮肤或分配内存。
bool isMissingSkinColor(const Config::Color& color)
{
    return color.r == 1.0F && color.g == 0.0F && color.b == 1.0F &&
           color.a == 1.0F;
}

/// @brief 将可能包含非 ASCII 字节的资源 ID 截断为可安全显示的 ASCII 文本。
/// @param output 输出缓冲区。
/// @param source 原资源 ID。
/// @warning 主画布热路径：只写入调用方提供的有界栈缓冲区。
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

/// @brief 计算单行 ASCII 文本的横向推进宽度。
/// @param font 当前字号对应的字体度量。
/// @param text 文本。
/// @param fontPixelHeight 字体像素高度。
/// @return 文本完整横向推进宽度。
/// @warning 主画布热路径：只允许线性扫描有界短文本。
float measureAsciiTextWidth(const Common::AsciiFontMetrics& font,
                            std::string_view text, float fontPixelHeight)
{
    float width = 0.0F;
    for ( const char character : text ) {
        const auto* glyph = font.glyph(character);
        if ( glyph && glyph->available ) {
            width += glyph->advanceX * fontPixelHeight;
        }
    }
    return width;
}

/// @brief 在水平裁剪区域内绘制一次 ASCII 文本。
/// @param batcher 目标批处理器。
/// @param selection 已选中的字体档位。
/// @param text 文本。
/// @param penX 文本起始笔位置。
/// @param y 文本区域上边界。
/// @param fontPixelHeight 字体像素高度。
/// @param clipLeft 水平裁剪左边界。
/// @param clipRight 水平裁剪右边界。
/// @param color 文字颜色。
/// @warning 主画布热路径：逐字形裁剪并写入几何，不得分配内存或切换裁剪命令。
void renderAsciiTextRunAt(Batcher&                          batcher,
                          const Common::AsciiFontSelection& selection,
                          std::string_view text, float penX, float y,
                          float fontPixelHeight, float clipLeft,
                          float clipRight, glm::vec4 color)
{
    if ( !selection || text.empty() || clipRight <= clipLeft ) return;

    const auto& font      = *selection.metrics;
    const float baselineY = y + font.ascender * fontPixelHeight;
    for ( const char character : text ) {
        const auto* glyph = font.glyph(character);
        if ( !glyph || !glyph->available ) continue;
        const float advance = glyph->advanceX * fontPixelHeight;
        if ( penX >= clipRight ) break;

        if ( glyph->hasBitmap ) {
            const auto textureId =
                asciiGlyphTextureId(selection.tierIndex, character);
            const auto uvIt = batcher.snapshot->uvMap.find(
                static_cast<std::uint32_t>(textureId));
            if ( textureId != TextureID::None &&
                 uvIt != batcher.snapshot->uvMap.end() ) {
                const float left = penX + glyph->bearingX * fontPixelHeight;
                const float top = baselineY - glyph->bearingY * fontPixelHeight;
                const float width        = glyph->width * fontPixelHeight;
                const float height       = glyph->height * fontPixelHeight;
                const float visibleLeft  = std::max(left, clipLeft);
                const float visibleRight = std::min(left + width, clipRight);
                if ( visibleRight > visibleLeft && width > 1e-6F ) {
                    const float leftRatio  = (visibleLeft - left) / width;
                    const float rightRatio = (visibleRight - left) / width;
                    const auto& uv         = uvIt->second;
                    batcher.setTexture(textureId);
                    batcher.pushUVQuad(
                        visibleLeft,
                        top + height,
                        visibleRight - visibleLeft,
                        height,
                        { uv.x + uv.z * leftRatio, uv.y },
                        { uv.x + uv.z * rightRatio, uv.y + uv.w },
                        color);
                }
            }
        }
        penX += advance;
    }
}

/// @brief 在空间不足时循环滚动 ASCII 文本，足够时保持居中静止。
/// @param batcher 目标批处理器。
/// @param text 文本。
/// @param x 文本区域左边界。
/// @param y 文本区域上边界。
/// @param fontPixelHeight 字体像素高度。
/// @param maxWidth 最大可用宽度。
/// @param color 文字颜色。
/// @param monotonicSeconds 单调时钟秒数。
/// @warning 主画布热路径：只绘制至多两份有界短文本，不得分配内存或创建
/// 独立 DrawCall 裁剪区。
void renderMarqueeAsciiTextAt(Batcher& batcher, std::string_view text, float x,
                              float y, float fontPixelHeight, float maxWidth,
                              glm::vec4 color, double monotonicSeconds)
{
    const auto selection = Common::selectAsciiFont(
        batcher.snapshot->asciiFontAtlasMetrics, fontPixelHeight);
    if ( !selection || text.empty() || maxWidth <= 0.0F ) return;

    const float textWidth =
        measureAsciiTextWidth(*selection.metrics, text, fontPixelHeight);
    if ( textWidth <= maxWidth ) {
        renderAsciiTextRunAt(batcher,
                             selection,
                             text,
                             x + (maxWidth - textWidth) * 0.5F,
                             y,
                             fontPixelHeight,
                             x,
                             x + maxWidth,
                             color);
        return;
    }

    constexpr double PAUSE_SECONDS  = 1.25;
    constexpr double SCROLL_SPEED   = 32.0;
    const float      gapWidth       = std::max(24.0F, fontPixelHeight * 2.0F);
    const double     scrollDistance = static_cast<double>(textWidth + gapWidth);
    const double     scrollSeconds  = scrollDistance / SCROLL_SPEED;
    const double     cycleSeconds   = PAUSE_SECONDS + scrollSeconds;
    const double     safeTime =
        std::isfinite(monotonicSeconds) && monotonicSeconds > 0.0
            ? monotonicSeconds
            : 0.0;
    const double phase = std::fmod(safeTime, cycleSeconds);
    const float  offset =
        phase <= PAUSE_SECONDS
            ? 0.0F
            : -static_cast<float>((phase - PAUSE_SECONDS) * SCROLL_SPEED);
    const float firstX  = x + offset;
    const float secondX = firstX + static_cast<float>(scrollDistance);
    if ( firstX < x + maxWidth && firstX + textWidth > x ) {
        renderAsciiTextRunAt(batcher,
                             selection,
                             text,
                             firstX,
                             y,
                             fontPixelHeight,
                             x,
                             x + maxWidth,
                             color);
    }
    if ( secondX < x + maxWidth && secondX + textWidth > x ) {
        renderAsciiTextRunAt(batcher,
                             selection,
                             text,
                             secondX,
                             y,
                             fontPixelHeight,
                             x,
                             x + maxWidth,
                             color);
    }
}

}  // namespace

glm::vec4 audioObjectLabelColor()
{
    const auto color =
        Config::SkinManager::instance().getColor(AUDIO_OBJECT_LABEL_COLOR_KEY);
    if ( isMissingSkinColor(color) ) {
        return { 0.9F, 0.96F, 1.0F, 0.96F };
    }
    return { color.r, color.g, color.b, color.a };
}

void renderCanvasAsciiText(Batcher& batcher, std::string_view text, float x,
                           float y, float fontPixelHeight, float maxWidth,
                           glm::vec4 color, bool centerHorizontally)
{
    const auto selection = Common::selectAsciiFont(
        batcher.snapshot->asciiFontAtlasMetrics, fontPixelHeight);
    if ( !selection || text.empty() || maxWidth <= 0.0F ) return;

    const float textWidth =
        measureAsciiTextWidth(*selection.metrics, text, fontPixelHeight);
    const float penX = x + (centerHorizontally && textWidth <= maxWidth
                                ? (maxWidth - textWidth) * 0.5F
                                : 0.0F);
    renderAsciiTextRunAt(batcher,
                         selection,
                         text,
                         penX,
                         y,
                         fontPixelHeight,
                         x,
                         x + maxWidth,
                         color);
}

void renderAudioObjectLabel(Batcher& batcher, std::string_view audioResourceId,
                            float volume, float laneLeftX, float objectTopY,
                            float laneWidth, float objectScaleY,
                            glm::vec4 color, double monotonicSeconds)
{
    if ( audioResourceId.empty() || laneWidth <= 4.0F ) return;

    std::array<char, 72> resourceBuffer{};
    copyDisplayResourceId(resourceBuffer, audioResourceId);
    const auto idLength = std::char_traits<char>::length(resourceBuffer.data());
    std::size_t resourceLength = idLength;
    if ( std::abs(volume - 1.0F) > 1e-3F &&
         resourceLength < resourceBuffer.size() - 1 ) {
        auto* const volumeBegin = resourceBuffer.data() + resourceLength;
        const auto  volumeResult =
            fmt::format_to_n(volumeBegin,
                             resourceBuffer.size() - resourceLength - 1,
                             " {:.0f}%",
                             static_cast<double>(volume) * 100.0);
        *volumeResult.out = '\0';
        resourceLength +=
            static_cast<std::size_t>(volumeResult.out - volumeBegin);
    }

    const float safeScale = std::isfinite(objectScaleY) && objectScaleY > 0.0F
                                ? objectScaleY
                                : 1.0F;
    const float fontPixelHeight =
        std::clamp(laneWidth * 0.18F, 13.0F, 18.0F) * safeScale;
    renderMarqueeAsciiTextAt(
        batcher,
        std::string_view(resourceBuffer.data(), resourceLength),
        laneLeftX + 2.0F,
        objectTopY - fontPixelHeight - 4.0F,
        fontPixelHeight,
        laneWidth - 4.0F,
        color,
        monotonicSeconds);
}

}  // namespace MMM::Logic::System
