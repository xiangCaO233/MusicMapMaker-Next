#include "logic/ecs/system/CanvasComponentRenderSystem.h"

#include "common/AsciiFontData.h"
#include "common/CanvasComponentLayout.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <system_error>

namespace MMM::Logic::System
{

namespace
{

constexpr std::int64_t MAX_DISPLAY_MILLIS =
    ((99LL * 60LL + 59LL) * 60LL + 59LL) * 1000LL + 999LL;
/// @brief 单帧允许生成的重复文字实例上限，防止异常 BPM 数据放大热路径负载。
constexpr std::size_t MAX_VISIBLE_REPEATED_TEXT_INSTANCES = 4096U;

/// @brief 绘制一行只包含 ASCII 字符的文本。
/// @param batcher 目标覆盖层批处理器。
/// @param text 以空字符结尾的 ASCII 文本。
/// @param placement 文本组件布局。
/// @param layoutRegion 当前实例允许布局的像素区域。
/// @param outBounds 输出实际文字内容边界。
/// @param visibleRegion 可选的实例可见性判定区域。
/// @param outEffectiveLayoutRegion 输出应用文字尺寸偏移后的实际布局区域。
/// @param extendForBeatHeadCenter 为 true 时将布局区域向下扩展半个文字包围框
/// 高度，使其保留原上边界且底端位置可令文字中心对齐拍头线。
/// @return 至少生成一个可见字形时返回 true。
/// @warning
/// 热路径：组件启用时每次主画布快照生成调用；只扫描短文本并生成字形四边形。
bool renderAsciiText(Batcher& batcher, const char* text,
                     const Config::CanvasComponentPlacement& placement,
                     CanvasComponentBounds                   layoutRegion,
                     CanvasComponentBounds&                  outBounds,
                     const CanvasComponentBounds* visibleRegion      = nullptr,
                     CanvasComponentBounds* outEffectiveLayoutRegion = nullptr,
                     bool                   extendForBeatHeadCenter  = false)
{
    const auto  sanitized = sanitizeCanvasComponentPlacement(placement);
    const float fontPixelHeight =
        sanitized.fontSizeRatio * layoutRegion.height();
    const auto selection = Common::selectAsciiFont(
        batcher.snapshot->asciiFontAtlasMetrics, fontPixelHeight);
    if ( !selection || !text ) return false;

    const auto& font    = *selection.metrics;
    const auto textSize = Common::measureAsciiText(font, text, fontPixelHeight);
    if ( textSize.width <= 0.0f || textSize.height <= 0.0f ) return false;

    if ( extendForBeatHeadCenter ) {
        layoutRegion.bottom += textSize.height * 0.5f;
    }
    if ( outEffectiveLayoutRegion ) {
        *outEffectiveLayoutRegion = layoutRegion;
    }

    const auto bounds = canvasComponentBoundsInRegion(
        sanitized, layoutRegion, textSize.width, textSize.height);
    outBounds = bounds;
    if ( visibleRegion && (bounds.right < visibleRegion->left ||
                           bounds.left > visibleRegion->right ||
                           bounds.bottom < visibleRegion->top ||
                           bounds.top > visibleRegion->bottom) ) {
        return false;
    }
    const float baselineY = bounds.top + font.ascender * fontPixelHeight;
    float       penX      = bounds.left;
    bool        rendered  = false;

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
                                   { sanitized.color[0],
                                     sanitized.color[1],
                                     sanitized.color[2],
                                     sanitized.color[3] });
                rendered = true;
            }
        }
        penX += glyph->advanceX * fontPixelHeight;
    }
    return rendered;
}

/// @brief 记录一个与实际 Vulkan 文字几何一致的布局编辑实例。
/// @param snapshot 目标渲染快照。
/// @param type 组件类型。
/// @param instanceIndex 重复组件实例序号；非重复组件为 0。
/// @param bounds 实际文字内容边界。
/// @param layoutRegion 当前实例允许布局的区域。
/// @warning 热路径：每个已绘制组件实例调用一次，只追加到复用向量。
void appendComponentInstance(RenderSnapshot&              snapshot,
                             Config::CanvasComponentType  type,
                             std::int64_t                 instanceIndex,
                             const CanvasComponentBounds& bounds,
                             const CanvasComponentBounds& layoutRegion)
{
    snapshot.canvasComponentInstances.push_back({ type,
                                                  instanceIndex,
                                                  bounds.left,
                                                  bounds.top,
                                                  bounds.right,
                                                  bounds.bottom,
                                                  layoutRegion.left,
                                                  layoutRegion.top,
                                                  layoutRegion.right,
                                                  layoutRegion.bottom });
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
    const CanvasComponentBounds layoutRegion{
        0.0f, 0.0f, viewportWidth, viewportHeight
    };
    CanvasComponentBounds bounds;
    if ( renderAsciiText(
             batcher, text.data(), placement, layoutRegion, bounds) ) {
        appendComponentInstance(*batcher.snapshot,
                                Config::CanvasComponentType::JudgmentLineTime,
                                0,
                                bounds,
                                layoutRegion);
    }
}

/// @brief 规整节拍网格文字使用的 BPM。
/// @param bpm 原始 BPM。
/// @param fallbackBpm 无效值的快照回退 BPM。
/// @return 处于安全范围内的 BPM。
double normalizedGridTextBpm(double bpm, double fallbackBpm)
{
    if ( !std::isfinite(bpm) || bpm <= 0.0 ) {
        bpm = fallbackBpm;
    }
    if ( !std::isfinite(bpm) || bpm <= 0.0 ) {
        bpm = 120.0;
    }
    return std::min(bpm, 10000.0);
}

/// @brief 在每个可见节拍网格区间内绘制一个重复文字实例。
/// @tparam TextFormatter 根据实例序号与区间起始时间生成 ASCII 文本的函数类型。
/// @param batcher 目标覆盖层批处理器。
/// @param context 当前主画布节拍坐标上下文。
/// @param placement 组件在单个网格区间内的布局。
/// @param type 组件类型。
/// @param divisor 每拍划分的网格区间数量。
/// @param formatter 文字生成函数。
/// @warning 热路径：重复组件启用时每个主画布快照调用；只遍历缓存 BPM
/// 事件、可见时间区间与可见网格，不访问 ECS 或文件系统。
template<typename TextFormatter>
void renderBeatGridTexts(Batcher&                                batcher,
                         const CanvasComponentRenderContext&     context,
                         const Config::CanvasComponentPlacement& placement,
                         Config::CanvasComponentType type, int divisor,
                         TextFormatter&& formatter)
{
    const auto* cache = context.scrollCache;
    if ( !cache || context.bpmEvents.empty() ||
         std::abs(context.renderScaleY) < 1e-6f ) {
        return;
    }

    const double currentAbsY = cache->getVisualAnchorAbsY(context.currentTime);
    const double topAbsY =
        currentAbsY + (context.judgmentLineY - context.visibleTop) /
                          static_cast<double>(context.renderScaleY);
    const double bottomAbsY =
        currentAbsY + (context.judgmentLineY - context.visibleBottom) /
                          static_cast<double>(context.renderScaleY);
    const auto visibleRanges = cache->getTimeRangesForAbsYWindow(
        std::min(topAbsY, bottomAbsY), std::max(topAbsY, bottomAbsY));
    if ( visibleRanges.empty() ) return;

    const float visibleTop =
        std::clamp(std::min(context.visibleTop, context.visibleBottom),
                   0.0f,
                   context.viewportHeight);
    const float visibleBottom =
        std::clamp(std::max(context.visibleTop, context.visibleBottom),
                   0.0f,
                   context.viewportHeight);
    const CanvasComponentBounds visibleRegion{
        0.0f, visibleTop, context.viewportWidth, visibleBottom
    };
    if ( visibleRegion.height() <= 0.0f ) return;
    batcher.setScissor(visibleRegion.left,
                       visibleRegion.top,
                       visibleRegion.width(),
                       visibleRegion.height());

    divisor                         = std::clamp(divisor, 1, 64);
    std::int64_t completedGridCount = 0;
    std::size_t  renderedCount      = 0U;
    double lastProcessedGridStart   = -std::numeric_limits<double>::infinity();

    for ( std::size_t index = 0U; index < context.bpmEvents.size(); ++index ) {
        const auto* bpmEvent = context.bpmEvents[index];
        if ( !bpmEvent ) continue;

        const double bpmTime      = bpmEvent->m_timestamp;
        const double bpm          = normalizedGridTextBpm(bpmEvent->m_value,
                                                 batcher.snapshot->fallbackBpm);
        const double gridDuration = 60.0 / bpm / static_cast<double>(divisor);
        const double nextBpmTime =
            index + 1U < context.bpmEvents.size() &&
                    context.bpmEvents[index + 1U]
                ? context.bpmEvents[index + 1U]->m_timestamp
                : std::numeric_limits<double>::infinity();

        for ( const auto& [rangeStart, rangeEnd] : visibleRanges ) {
            const double segmentStart = std::max(rangeStart, bpmTime);
            const double segmentEnd   = std::min(rangeEnd, nextBpmTime);
            if ( segmentEnd < segmentStart || segmentEnd < bpmTime ) continue;

            // 首个候选必须包含与布局视口相交但起点已越出视口的当前区间。
            // 实际文字是否可见继续由布局视口判定与 Vulkan scissor 决定。
            std::int64_t gridOffset = static_cast<std::int64_t>(
                std::floor((segmentStart - bpmTime) / gridDuration + 1e-6));
            gridOffset = std::max<std::int64_t>(0, gridOffset);
            double gridStart =
                bpmTime + static_cast<double>(gridOffset) * gridDuration;
            // 上沿还需检查下一个区间：网格头线刚离开布局视口时，居中绘制的
            // 文字仍可能有一半处于 scissor 内。
            const double candidateEnd = segmentEnd + gridDuration;
            while ( gridStart <= candidateEnd + 1e-6 &&
                    gridStart < nextBpmTime ) {
                if ( gridStart <= lastProcessedGridStart + 1e-6 ) {
                    ++gridOffset;
                    gridStart = bpmTime +
                                static_cast<double>(gridOffset) * gridDuration;
                    continue;
                }
                lastProcessedGridStart = gridStart;
                const double gridEnd =
                    std::min(gridStart + gridDuration, nextBpmTime);
                if ( gridEnd <= gridStart + 1e-9 ) break;

                const float startY = context.judgmentLineY -
                                     static_cast<float>(cache->getDisplayDelta(
                                         gridStart, currentAbsY, gridStart)) *
                                         context.renderScaleY;
                const float endY = context.judgmentLineY -
                                   static_cast<float>(cache->getDisplayDelta(
                                       gridEnd, currentAbsY, gridEnd)) *
                                       context.renderScaleY;
                const CanvasComponentBounds layoutRegion{
                    0.0f,
                    std::min(startY, endY),
                    context.viewportWidth,
                    std::max(startY, endY),
                };

                if ( layoutRegion.height() > 0.5f ) {
                    const std::int64_t instanceIndex =
                        completedGridCount + gridOffset + 1;
                    const auto text = formatter(instanceIndex, gridStart);
                    CanvasComponentBounds bounds;
                    CanvasComponentBounds effectiveLayoutRegion;
                    if ( renderAsciiText(batcher,
                                         text.data(),
                                         placement,
                                         layoutRegion,
                                         bounds,
                                         &visibleRegion,
                                         &effectiveLayoutRegion,
                                         true) ) {
                        appendComponentInstance(*batcher.snapshot,
                                                type,
                                                instanceIndex,
                                                bounds,
                                                effectiveLayoutRegion);
                        ++renderedCount;
                        if ( renderedCount >=
                             MAX_VISIBLE_REPEATED_TEXT_INSTANCES ) {
                            return;
                        }
                    }
                }

                ++gridOffset;
                gridStart =
                    bpmTime + static_cast<double>(gridOffset) * gridDuration;
            }
        }

        if ( std::isfinite(nextBpmTime) && nextBpmTime > bpmTime ) {
            completedGridCount += static_cast<std::int64_t>(
                std::llround((nextBpmTime - bpmTime) / gridDuration));
        }
    }
}

/// @brief 绘制全部可见整拍的拍号组件。
/// @param batcher 目标覆盖层批处理器。
/// @param context 当前主画布节拍坐标上下文。
/// @param placement 拍号组件的拍内布局。
/// @warning 热路径：拍号启用时每个主画布快照调用；只转发到缓存网格遍历。
void renderBeatNumbers(Batcher&                                batcher,
                       const CanvasComponentRenderContext&     context,
                       const Config::CanvasComponentPlacement& placement)
{
    renderBeatGridTexts(
        batcher,
        context,
        placement,
        Config::CanvasComponentType::BeatNumber,
        1,
        [](std::int64_t instanceIndex, double) {
            return CanvasComponentRenderSystem::formatBeatNumber(instanceIndex);
        });
}

/// @brief 在每条可见分拍线上绘制其谱面时间。
/// @param batcher 目标覆盖层批处理器。
/// @param context 当前主画布节拍坐标上下文。
/// @param placement 分拍线时间组件的分拍内布局。
/// @warning 热路径：组件启用时每个主画布快照调用；只转发到缓存网格遍历。
void renderBeatLineTimes(Batcher&                                batcher,
                         const CanvasComponentRenderContext&     context,
                         const Config::CanvasComponentPlacement& placement)
{
    renderBeatGridTexts(
        batcher,
        context,
        placement,
        Config::CanvasComponentType::BeatLineTime,
        context.beatDivisor,
        [](std::int64_t, double gridStart) {
            return CanvasComponentRenderSystem::formatJudgmentLineTime(
                gridStart);
        });
}

/// @brief 将单轨 KPS 格式化为不含轨道编号的紧凑文本。
/// @param kps 最近一秒触发次数。
/// @return `12 KPS` 格式文本。
/// @warning 热路径：单轨完整文本超过轨道宽度时调用；不得引入堆分配。
std::array<char, 32> formatTrackKpsWithoutTrack(std::uint32_t kps)
{
    std::array<char, 32> result{};
    std::snprintf(
        result.data(), result.size(), "%u KPS", static_cast<unsigned int>(kps));
    return result;
}

/// @brief 将单轨 KPS 格式化为只含数值的最简文本。
/// @param kps 最近一秒触发次数。
/// @return `12` 格式文本。
/// @warning 热路径：不含轨道编号的文本仍超过轨道宽度时调用；不得引入堆分配。
std::array<char, 32> formatTrackKpsValue(std::uint32_t kps)
{
    std::array<char, 32> result{};
    std::snprintf(
        result.data(), result.size(), "%u", static_cast<unsigned int>(kps));
    return result;
}

/// @brief 按单轨可用宽度选择 KPS 完整或紧凑文本。
/// @param snapshot 当前渲染快照。
/// @param trackIndex 从零开始的轨道序号。
/// @param kps 最近一秒触发次数。
/// @param fontPixelHeight 当前单轨 KPS 字号的像素高度。
/// @param trackPixelWidth 单条轨道的像素宽度。
/// @return 优先保留完整文本，其次移除轨道编号，最后只保留数值。
/// @warning 热路径：每条轨道每次快照生成调用；只测量最多三条短 ASCII 文本。
std::array<char, 32> selectTrackKpsText(const RenderSnapshot& snapshot,
                                        std::int32_t          trackIndex,
                                        std::uint32_t         kps,
                                        float                 fontPixelHeight,
                                        float                 trackPixelWidth)
{
    auto text = CanvasComponentRenderSystem::formatTrackKps(trackIndex, kps);
    const auto selection = Common::selectAsciiFont(
        snapshot.asciiFontAtlasMetrics, fontPixelHeight);
    if ( !selection ) return text;

    const float availableWidth =
        std::isfinite(trackPixelWidth) ? std::max(0.0f, trackPixelWidth) : 0.0f;
    if ( Common::measureAsciiText(
             *selection.metrics, text.data(), fontPixelHeight)
             .width <= availableWidth ) {
        return text;
    }

    text = formatTrackKpsWithoutTrack(kps);
    if ( Common::measureAsciiText(
             *selection.metrics, text.data(), fontPixelHeight)
             .width <= availableWidth ) {
        return text;
    }
    return formatTrackKpsValue(kps);
}

/// @brief 绘制逐轨 KPS 与总 KPS。
/// @param batcher 目标覆盖层批处理器。
/// @param context 当前主画布与逐轨 KPS 上下文。
/// @param config 画布组件布局配置。
/// @warning 热路径：KPS 启用时每次主画布快照调用；只遍历当前轨道数量。
void renderKps(Batcher& batcher, const CanvasComponentRenderContext& context,
               const Config::CanvasComponentLayoutConfig& config)
{
    // 拍号和分拍线时间会使用轨道视口裁剪，KPS 必须显式恢复全画布范围。
    batcher.setScissor(
        0.0f, 0.0f, context.viewportWidth, context.viewportHeight);

    const auto trackCount = std::max<std::int32_t>(context.trackCount, 0);
    const CanvasComponentBounds layoutRegion{
        0.0f, 0.0f, context.viewportWidth, context.viewportHeight
    };
    const float trackPixelWidth =
        trackCount > 0
            ? context.viewportWidth *
                  std::max(0.0f, context.trackRight - context.trackLeft) /
                  static_cast<float>(trackCount)
            : context.viewportWidth;

    std::uint64_t totalKps = 0U;
    for ( std::int32_t trackIndex = 0; trackIndex < trackCount; ++trackIndex ) {
        std::uint32_t trackKps = 0U;
        if ( batcher.snapshot->isPlaying &&
             static_cast<std::size_t>(trackIndex) < context.trackKps.size() ) {
            trackKps = context.trackKps[static_cast<std::size_t>(trackIndex)];
        }
        totalKps += trackKps;

        const auto placement =
            config.resolvedPlacement(Config::CanvasComponentType::Kps,
                                     trackIndex,
                                     trackCount,
                                     context.trackLeft,
                                     context.trackRight);
        const float fontPixelHeight =
            sanitizeCanvasComponentPlacement(placement).fontSizeRatio *
            context.viewportHeight;
        const auto            text = selectTrackKpsText(*batcher.snapshot,
                                                        trackIndex,
                                                        trackKps,
                                                        fontPixelHeight,
                                                        trackPixelWidth);
        CanvasComponentBounds bounds;
        if ( renderAsciiText(
                 batcher, text.data(), placement, layoutRegion, bounds) ) {
            appendComponentInstance(*batcher.snapshot,
                                    Config::CanvasComponentType::Kps,
                                    trackIndex,
                                    bounds,
                                    layoutRegion);
        }
    }

    const auto text = CanvasComponentRenderSystem::formatTotalKps(
        static_cast<std::uint32_t>(std::min<std::uint64_t>(
            totalKps, std::numeric_limits<std::uint32_t>::max())));
    const auto placement =
        config.resolvedPlacement(Config::CanvasComponentType::Kps,
                                 Config::KPS_TOTAL_INSTANCE_INDEX,
                                 trackCount,
                                 context.trackLeft,
                                 context.trackRight);
    CanvasComponentBounds bounds;
    if ( renderAsciiText(
             batcher, text.data(), placement, layoutRegion, bounds) ) {
        appendComponentInstance(*batcher.snapshot,
                                Config::CanvasComponentType::Kps,
                                Config::KPS_TOTAL_INSTANCE_INDEX,
                                bounds,
                                layoutRegion);
    }
}

}  // namespace

void CanvasComponentRenderSystem::render(
    RenderSnapshot* snapshot, const CanvasComponentRenderContext& context,
    const Config::CanvasComponentLayoutConfig& config)
{
    if ( !snapshot || !snapshot->asciiFontAtlasMetrics.valid ||
         context.viewportWidth <= 0.0f || context.viewportHeight <= 0.0f ) {
        return;
    }

    Batcher batcher(snapshot, &snapshot->overlayCmds);
    batcher.setScissor(
        0.0f, 0.0f, context.viewportWidth, context.viewportHeight);

    for ( Config::CanvasComponentType type : Config::CANVAS_COMPONENT_TYPES ) {
        const auto& placement = config.placement(type);
        if ( !placement.visible ) continue;

        switch ( type ) {
        case Config::CanvasComponentType::JudgmentLineTime:
            if ( !snapshot->hasBeatmap ) break;
            renderJudgmentLineTime(batcher,
                                   context.currentTime,
                                   context.viewportWidth,
                                   context.viewportHeight,
                                   placement);
            break;
        case Config::CanvasComponentType::BeatNumber:
            renderBeatNumbers(batcher, context, placement);
            break;
        case Config::CanvasComponentType::BeatLineTime:
            if ( !snapshot->hasBeatmap ) break;
            renderBeatLineTimes(batcher, context, placement);
            break;
        case Config::CanvasComponentType::Kps:
            if ( !snapshot->hasBeatmap ) break;
            renderKps(batcher, context, config);
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

std::array<char, 24> CanvasComponentRenderSystem::formatBeatNumber(
    std::int64_t beatIndex)
{
    std::array<char, 24> result{};
    result[0]             = '#';
    beatIndex             = std::max<std::int64_t>(0, beatIndex);
    const auto conversion = std::to_chars(
        result.data() + 1, result.data() + result.size() - 1, beatIndex);
    if ( conversion.ec != std::errc{} ) {
        result[1] = '0';
        result[2] = '\0';
    } else {
        *conversion.ptr = '\0';
    }
    return result;
}

std::array<char, 32> CanvasComponentRenderSystem::formatTrackKps(
    std::int32_t zeroBasedTrackIndex, std::uint32_t kps)
{
    std::array<char, 32> result{};
    std::snprintf(result.data(),
                  result.size(),
                  "K%d %u KPS",
                  std::max(zeroBasedTrackIndex, 0) + 1,
                  static_cast<unsigned int>(kps));
    return result;
}

std::array<char, 32> CanvasComponentRenderSystem::formatTotalKps(
    std::uint32_t kps)
{
    std::array<char, 32> result{};
    std::snprintf(result.data(),
                  result.size(),
                  "TOTAL %u KPS",
                  static_cast<unsigned int>(kps));
    return result;
}

}  // namespace MMM::Logic::System
