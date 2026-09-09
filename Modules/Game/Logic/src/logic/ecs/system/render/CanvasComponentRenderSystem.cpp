#include "logic/ecs/system/CanvasComponentRenderSystem.h"

#include "common/AsciiFontData.h"
#include "common/CanvasComponentLayout.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"
#include "mmm/timing/BpmNormalization.h"
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

/// @brief 两位小时显示可容纳的最大毫秒数，即 99:59:59.999。
constexpr std::int64_t MAX_DISPLAY_MILLIS =
    ((99LL * 60LL + 59LL) * 60LL + 59LL) * 1000LL + 999LL;
/// @brief 单帧允许生成的重复文字实例上限，防止异常 BPM 数据放大热路径负载。
/// @note 每次网格文字调用独立计数，拍号与分拍时间不共用该额度。
constexpr std::size_t MAX_VISIBLE_REPEATED_TEXT_INSTANCES = 4096U;

/// @brief 绘制一行只包含 ASCII 字符的文本。
/// @param batcher 目标覆盖层批处理器。
/// @param text 以空字符结尾的 ASCII 文本。
/// @param placement 文本组件布局。
/// @param layoutRegion 当前实例允许布局的像素区域。
/// @param fontReferenceHeight 字号比例使用的固定画布参考高度。
/// @param outBounds 输出实际文字内容边界。
/// @param visibleRegion 可选的实例可见性判定区域。
/// @param outEffectiveLayoutRegion 输出应用文字尺寸偏移后的实际布局区域。
/// @param extendForBeatHeadCenter 为 true 时将布局区域向下扩展半个文字包围框
/// 高度，使其保留原上边界且底端位置可令文字中心对齐拍头线。
/// @return 至少生成一个可见字形时返回 true。
/// @warning
/// 热路径：组件启用时每次主画布快照生成调用；只扫描短文本并生成字形四边形。
/// @pre batcher.snapshot 非空，字体度量与 UV 表来自本帧可用的字体图集。
/// @note 返回 false 不保证输出边界未写入：完成布局后才进行可见性与字形检查。
/// @note 空串和纯空白内容可能没有绘制输出，调用方不能据此登记可见文字实例。
/// @note 本函数不设置 scissor；可见区域参数只用于整段文字的相交判断。
bool renderAsciiText(Batcher& batcher, const char* text,
                     const Config::CanvasComponentPlacement& placement,
                     CanvasComponentBounds                   layoutRegion,
                     float                        fontReferenceHeight,
                     CanvasComponentBounds&       outBounds,
                     const CanvasComponentBounds* visibleRegion      = nullptr,
                     CanvasComponentBounds* outEffectiveLayoutRegion = nullptr,
                     bool                   extendForBeatHeadCenter  = false)
{
    // 字号始终参照固定画布高度，不随单个节拍区间的压缩或伸展改变。
    const auto  sanitized       = sanitizeCanvasComponentPlacement(placement);
    const float fontPixelHeight = sanitized.fontSizeRatio * fontReferenceHeight;
    const auto  selection       = Common::selectAsciiFont(
        batcher.snapshot->asciiFontAtlasMetrics, fontPixelHeight);
    // 没有可用字号层级时直接跳过，不在快照生成中触发字体烘焙。
    if ( !selection || !text ) return false;

    // 先测量完整字符串以确定锚点位置，再逐字提交，避免居中位置依赖绘制进度。
    const auto& font    = *selection.metrics;
    const auto textSize = Common::measureAsciiText(font, text, fontPixelHeight);
    // 退化文字尺寸不参与锚点定位，避免登记没有内容面积的布局实例。
    if ( textSize.width <= 0.0f || textSize.height <= 0.0f ) return false;

    if ( extendForBeatHeadCenter ) {
        // 只扩展下边界，保留网格顶部基准；不能对称扩展而改变拍内布局区域。
        layoutRegion.bottom += textSize.height * 0.5f;
    }
    if ( outEffectiveLayoutRegion ) {
        // 编辑器必须得到扩展后的同一布局区域，否则拖动后的锚点换算会跳变。
        *outEffectiveLayoutRegion = layoutRegion;
    }

    const auto bounds = canvasComponentBoundsInRegion(
        sanitized, layoutRegion, textSize.width, textSize.height);
    // 内容边界和布局参考区域是不同概念：前者用于选择，后者用于相对定位。
    outBounds = bounds;
    if ( visibleRegion && (bounds.right < visibleRegion->left ||
                           bounds.left > visibleRegion->right ||
                           bounds.bottom < visibleRegion->top ||
                           bounds.top > visibleRegion->bottom) ) {
        return false;
    }
    // 这里只做整段文字的粗裁剪，边缘字形的精确裁剪仍由当前 scissor 完成。
    // 基线由字体上升部确定，不能直接将组件顶边作为字形基线。
    const float baselineY = bounds.top + font.ascender * fontPixelHeight;
    float       penX      = bounds.left;
    bool        rendered  = false;

    // 成功标志只由真正提交的位图字形置位，字体有该字符不等于已生成几何。
    for ( const char* cursor = text; *cursor != '\0'; ++cursor ) {
        const auto* glyph = font.glyph(*cursor);
        // 不可用字符不生成替代字形，也不推进画笔；调用方应提供受支持的 ASCII。
        if ( !glyph || !glyph->available ) continue;

        if ( glyph->hasBitmap ) {
            // 使用所选字号层级的字形纹理，度量与纹理不能混用不同层级。
            const auto textureId =
                asciiGlyphTextureId(selection.tierIndex, *cursor);
            const auto uvIt = batcher.snapshot->uvMap.find(
                static_cast<std::uint32_t>(textureId));
            if ( textureId != TextureID::None &&
                 uvIt != batcher.snapshot->uvMap.end() ) {
                // bearing 定位实际位图；advance
                // 定位下一字符，两者不能互相替代。
                const float left = penX + glyph->bearingX * fontPixelHeight;
                const float top = baselineY - glyph->bearingY * fontPixelHeight;
                const float width  = glyph->width * fontPixelHeight;
                const float height = glyph->height * fontPixelHeight;
                const auto& uv     = uvIt->second;

                // UV 表保存起点和尺寸，提交时转换为左上与右下端点。
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
        // 空格等无位图字符仍需占宽；有度量但 UV 缺失时也保留原有排版间距。
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
/// @note 此处不负责实例去重，重复组件的稳定索引由网格或轨道遍历方提供。
void appendComponentInstance(RenderSnapshot&              snapshot,
                             Config::CanvasComponentType  type,
                             std::int64_t                 instanceIndex,
                             const CanvasComponentBounds& bounds,
                             const CanvasComponentBounds& layoutRegion)
{
    // 不在此清空列表，允许时间、拍号和 KPS 等组件共同累积到同一快照。
    // 保存值而非局部边界对象的引用，调用方的栈上布局数据可在返回后释放。
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
    // 时间是单实例组件，布局相对整个画布，而不是判定线附近的窄条区域。
    const auto text =
        CanvasComponentRenderSystem::formatJudgmentLineTime(currentTime);
    const CanvasComponentBounds layoutRegion{
        0.0f, 0.0f, viewportWidth, viewportHeight
    };
    CanvasComponentBounds bounds;
    if ( renderAsciiText(batcher,
                         text.data(),
                         placement,
                         layoutRegion,
                         viewportHeight,
                         bounds) ) {
        // 无字形输出时不登记不可见的文字交互实例。
        appendComponentInstance(*batcher.snapshot,
                                Config::CanvasComponentType::JudgmentLineTime,
                                0,
                                bounds,
                                layoutRegion);
    }
}

/// @brief 按原值方向规整节拍网格文字使用的 BPM。
/// @param bpm 原始 BPM。
/// @param fallbackBpm 原值为 NaN 时使用的快照回退 BPM。
/// @return 处于安全范围内的 BPM。
/// @warning 网格热路径调用；只规范数值，不修改 BPM 事件或重建滚动缓存。
double normalizedGridTextBpm(double bpm, double fallbackBpm)
{
    // 与谱面其他 BPM 消费者共用边界回退规则，避免网格文字产生另一套拍长。
    return ::MMM::normalizeBpmValue(bpm, fallbackBpm);
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
/// @pre BPM 事件按时间升序提供，指针及滚动缓存在本次调用内保持有效。
/// @pre 视口、时间和缩放均为有效有限值；此函数不修复损坏的坐标上下文。
/// @note 实例序号从一开始，跨 BPM 段延续；文字是否实际绘出不影响编号。
/// @note formatter 返回的对象须提供以空字符结尾的 data()，供本轮立即读取。
/// @note 可见范围须与缓存的时间顺序一致，起点去重依赖候选按时间前进。
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
        // 零缩放无法反解像素窗口到滚动距离；缺缓存时不临时扫描时间事件补算。
        return;
    }

    // 查询与投影共享视觉锚点，避免平滑滚动期间用逻辑时间查询却按视觉时间绘制。
    const double currentAbsY = cache->getVisualAnchorAbsY(context.currentTime);
    const double topAbsY =
        currentAbsY + (context.judgmentLineY - context.visibleTop) /
                          static_cast<double>(context.renderScaleY);
    const double bottomAbsY =
        currentAbsY + (context.judgmentLineY - context.visibleBottom) /
                          static_cast<double>(context.renderScaleY);
    // SV 反转可能使一个画布窗口对应多个时间区间，不能只取一个时间端点范围。
    const auto visibleRanges = cache->getTimeRangesForAbsYWindow(
        std::min(topAbsY, bottomAbsY), std::max(topAbsY, bottomAbsY));
    if ( visibleRanges.empty() ) return;

    // 有符号纵向缩放可能交换上下端点，先排序再限制在画布内。
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
    // 文字允许跨越网格自身的边界，但不能越过当前轨道可见区域。
    batcher.setScissor(visibleRegion.left,
                       visibleRegion.top,
                       visibleRegion.width(),
                       visibleRegion.height());

    // 限制分拍密度，并保证后续拍长计算不会除以零。
    divisor = std::clamp(divisor, 1, 64);
    // 已完成段的网格数用于全曲编号，与本帧可见实例数量分开累积。
    std::int64_t completedGridCount = 0;
    std::size_t  renderedCount      = 0U;
    // 可见范围边界可能重复包含同一个网格，记录起点以避免重复文字和命中实例。
    double lastProcessedGridStart = -std::numeric_limits<double>::infinity();

    for ( std::size_t index = 0U; index < context.bpmEvents.size(); ++index ) {
        const auto* bpmEvent = context.bpmEvents[index];
        // 空指针不建立网格段，也不在这里修补输入序列。
        if ( !bpmEvent ) continue;

        // 每个 BPM 事件重新建立网格原点，而不是延续上一段剩余的拍内相位。
        const double bpmTime      = bpmEvent->m_timestamp;
        const double bpm          = normalizedGridTextBpm(bpmEvent->m_value,
                                                 batcher.snapshot->fallbackBpm);
        const double gridDuration = 60.0 / bpm / static_cast<double>(divisor);
        // 网格使用秒，与 BPM 事件时间戳和滚动缓存保持同一单位。
        // 最后一段没有有限终点，由可见时间范围约束候选遍历。
        const double nextBpmTime =
            index + 1U < context.bpmEvents.size() &&
                    context.bpmEvents[index + 1U]
                ? context.bpmEvents[index + 1U]->m_timestamp
                : std::numeric_limits<double>::infinity();

        for ( const auto& [rangeStart, rangeEnd] : visibleRanges ) {
            // 只处理当前 BPM 段和可见时间的交集，避免扫描整段历史网格。
            const double segmentStart = std::max(rangeStart, bpmTime);
            const double segmentEnd   = std::min(rangeEnd, nextBpmTime);
            if ( segmentEnd < segmentStart || segmentEnd < bpmTime ) continue;

            // 首个候选必须包含与布局视口相交但起点已越出视口的当前区间。
            // 实际文字是否可见继续由布局视口判定与 Vulkan scissor 决定。
            std::int64_t gridOffset = static_cast<std::int64_t>(
                std::floor((segmentStart - bpmTime) / gridDuration + 1e-6));
            gridOffset = std::max<std::int64_t>(0, gridOffset);
            // 不生成当前 BPM 原点之前的负网格序号。
            double gridStart =
                bpmTime + static_cast<double>(gridOffset) * gridDuration;
            // 上沿还需检查下一个区间：网格头线刚离开布局视口时，居中绘制的
            // 文字仍可能有一半处于 scissor 内。
            const double candidateEnd = segmentEnd + gridDuration;
            while ( gridStart <= candidateEnd + 1e-6 &&
                    gridStart < nextBpmTime ) {
                // 新 BPM 起点归属于下一段，此段必须使用严格小于的终点条件。
                if ( gridStart <= lastProcessedGridStart + 1e-6 ) {
                    // 直接按整数偏移重新计算时间，避免不断累加拍长引入累积误差。
                    ++gridOffset;
                    gridStart = bpmTime +
                                static_cast<double>(gridOffset) * gridDuration;
                    continue;
                }
                lastProcessedGridStart = gridStart;
                // BPM 切换可能截断最后一个网格，布局底边不能跨入下一段。
                const double gridEnd =
                    std::min(gridStart + gridDuration, nextBpmTime);
                if ( gridEnd <= gridStart + 1e-9 ) break;
                // 后续布局只处理有正时间跨度的候选，不为退化区间生成文字。

                // 两端分别按各自时间投影，不能用固定速度乘网格时长推导高度。
                const float startY = context.judgmentLineY -
                                     static_cast<float>(cache->getDisplayDelta(
                                         gridStart, currentAbsY, gridStart)) *
                                         context.renderScaleY;
                const float endY = context.judgmentLineY -
                                   static_cast<float>(cache->getDisplayDelta(
                                       gridEnd, currentAbsY, gridEnd)) *
                                       context.renderScaleY;
                // 布局使用有序像素边界，即使时间方向与屏幕方向相反也保持正高度。
                const CanvasComponentBounds layoutRegion{
                    0.0f,
                    std::min(startY, endY),
                    context.viewportWidth,
                    std::max(startY, endY),
                };

                if ( layoutRegion.height() > 0.5f ) {
                    // 极薄或折叠区间不排文字，避免密集叠字且没有可用的布局区域。
                    const std::int64_t instanceIndex =
                        completedGridCount + gridOffset + 1;
                    // 一基编号在这里统一生成，格式化器无需另行补一。
                    const auto text = formatter(instanceIndex, gridStart);
                    // 格式化结果在本轮立即消费，不把临时字符缓冲保存到快照。
                    CanvasComponentBounds bounds;
                    CanvasComponentBounds effectiveLayoutRegion;
                    // 画布高度决定字号，网格高度只控制实例布局区域。
                    if ( renderAsciiText(batcher,
                                         text.data(),
                                         placement,
                                         layoutRegion,
                                         context.viewportHeight,
                                         bounds,
                                         &visibleRegion,
                                         &effectiveLayoutRegion,
                                         true) ) {
                        // 拍头居中的扩展也要保存，供布局编辑反算同一参考区域。
                        appendComponentInstance(*batcher.snapshot,
                                                type,
                                                instanceIndex,
                                                bounds,
                                                effectiveLayoutRegion);
                        ++renderedCount;
                        // 上限针对成功输出的文字实例，不是 BPM
                        // 段数或候选循环次数。
                        if ( renderedCount >=
                             MAX_VISIBLE_REPEATED_TEXT_INSTANCES ) {
                            // 只结束当前重复文字类型，外层仍能绘制其他组件。
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
            // 即使该段完全不在视口内，也要累计其编号，避免滚动时可见拍号重置。
            // 每条新 BPM 红线都从新拍开始；不足一拍的旧段也必须占用一拍。
            const double beatDuration =
                gridDuration * static_cast<double>(divisor);
            const double segmentDuration = nextBpmTime - bpmTime;
            const auto   completedBeats  = static_cast<std::int64_t>(
                std::ceil(segmentDuration / beatDuration - 1e-6));
            // 先补齐整拍再乘分拍数，不延续旧段未完成的分拍。
            completedGridCount +=
                std::max<std::int64_t>(completedBeats, 1) * divisor;
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
    // 拍号只在整拍显示，独立于当前分拍线密度，防止调整分拍后编号意义改变。
    // 格式化回调忽略时间，只消费跨 BPM 段累计的一基编号。
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
    // 时间标签使用每条分拍线的真实起始时间；实例编号仅用于布局编辑身份。
    // 网格遍历负责限制分拍数，本入口原样传递当前配置。
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
    // 保留单位但去掉轨号，作为完整文本和纯数值之间的降级形式。
    // uint32 的十进制计数与单位能完整放入固定数组，不需要长度依赖的堆缓冲。
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
    // 最后一级仍显示真实计数，不以省略号替换数值。
    // 和其他格式共用数组大小，便于文本降级时直接按值替换。
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
/// @note 返回的文本不保存字体引用，后续绘制仍从同一快照选择字体。
std::array<char, 32> selectTrackKpsText(const RenderSnapshot& snapshot,
                                        std::int32_t          trackIndex,
                                        std::uint32_t         kps,
                                        float                 fontPixelHeight,
                                        float                 trackPixelWidth)
{
    auto text = CanvasComponentRenderSystem::formatTrackKps(trackIndex, kps);
    // 用与后续实际绘制相同的字号层级测宽，不按字符个数估计比例字体的占用。
    const auto selection = Common::selectAsciiFont(
        snapshot.asciiFontAtlasMetrics, fontPixelHeight);
    // 没有字体度量就无法判断是否放得下，保留完整语义，由绘制入口决定是否输出。
    if ( !selection ) return text;

    // 非有限宽度不表示无限空间；退化宽度会走向最简文本。
    // 宽度等于可用空间时仍保留较完整形式，不额外减去隐含内边距。
    const float availableWidth =
        std::isfinite(trackPixelWidth) ? std::max(0.0f, trackPixelWidth) : 0.0f;
    if ( Common::measureAsciiText(
             *selection.metrics, text.data(), fontPixelHeight)
             .width <= availableWidth ) {
        return text;
    }

    // 先保留单位，只有这一形式仍过宽才去掉单位，避免过早丢失上下文。
    text = formatTrackKpsWithoutTrack(kps);
    if ( Common::measureAsciiText(
             *selection.metrics, text.data(), fontPixelHeight)
             .width <= availableWidth ) {
        return text;
    }
    // 纯数值是最终兜底，不承诺必定塞进任意窄轨道，也不在这里缩小字号。
    return formatTrackKpsValue(kps);
}

/// @brief 绘制逐轨 KPS 与总 KPS。
/// @param batcher 目标覆盖层批处理器。
/// @param context 当前主画布与逐轨 KPS 上下文。
/// @param config 画布组件布局配置。
/// @warning 热路径：KPS 启用时每次主画布快照调用；只遍历当前轨道数量。
/// @note 本函数不更新时间窗口计数，context.trackKps 由播放侧预先准备。
void renderKps(Batcher& batcher, const CanvasComponentRenderContext& context,
               const Config::CanvasComponentLayoutConfig& config)
{
    // 拍号和分拍线时间会使用轨道视口裁剪，KPS 必须显式恢复全画布范围。
    batcher.setScissor(
        0.0f, 0.0f, context.viewportWidth, context.viewportHeight);

    // 负轨道数按空轨道处理；总计组件仍可显示零值。
    const auto trackCount = std::max<std::int32_t>(context.trackCount, 0);
    const CanvasComponentBounds layoutRegion{
        0.0f, 0.0f, context.viewportWidth, context.viewportHeight
    };
    // 轨道边界是画布宽度的比例，测宽前转换为单轨像素宽度。
    // 文字布局仍相对全画布；单轨宽度只用于决定是否省略轨号和单位。
    const float trackPixelWidth =
        trackCount > 0
            ? context.viewportWidth *
                  std::max(0.0f, context.trackRight - context.trackLeft) /
                  static_cast<float>(trackCount)
            : context.viewportWidth;

    // 累加使用较宽类型，最终显示时再饱和到格式化接口接受的范围。
    std::uint64_t totalKps = 0U;
    for ( std::int32_t trackIndex = 0; trackIndex < trackCount; ++trackIndex ) {
        std::uint32_t trackKps = 0U;
        // 停播时不显示上一轮播放残留的计数，缺少的轨道数据同样按零处理。
        if ( batcher.snapshot->isPlaying &&
             static_cast<std::size_t>(trackIndex) < context.trackKps.size() ) {
            trackKps = context.trackKps[static_cast<std::size_t>(trackIndex)];
        }
        totalKps += trackKps;

        // 同一 trackKps 同时用于逐轨显示和总和，停播回退不会导致两者不一致。
        // 逐轨解析布局，既支持共享默认位置，也保留独立实例的用户调整。
        const auto placement =
            config.resolvedPlacement(Config::CanvasComponentType::Kps,
                                     trackIndex,
                                     trackCount,
                                     context.trackLeft,
                                     context.trackRight);
        const float fontPixelHeight =
            sanitizeCanvasComponentPlacement(placement).fontSizeRatio *
            context.viewportHeight;
        // 测宽与绘制使用同一规范后的字号，避免按旧字号选择无法容纳的文本。
        const auto            text = selectTrackKpsText(*batcher.snapshot,
                                             trackIndex,
                                             trackKps,
                                             fontPixelHeight,
                                             trackPixelWidth);
        CanvasComponentBounds bounds;
        if ( renderAsciiText(batcher,
                             text.data(),
                             placement,
                             layoutRegion,
                             context.viewportHeight,
                             bounds) ) {
            appendComponentInstance(*batcher.snapshot,
                                    Config::CanvasComponentType::Kps,
                                    trackIndex,
                                    bounds,
                                    layoutRegion);
        }
    }

    // 总计覆盖当前轨道集合，不累加输入向量中可能残留的额外轨道。
    const auto text = CanvasComponentRenderSystem::formatTotalKps(
        static_cast<std::uint32_t>(std::min<std::uint64_t>(
            totalKps, std::numeric_limits<std::uint32_t>::max())));
    const auto placement =
        config.resolvedPlacement(Config::CanvasComponentType::Kps,
                                 Config::KPS_TOTAL_INSTANCE_INDEX,
                                 trackCount,
                                 context.trackLeft,
                                 context.trackRight);
    // 总计使用专门的实例索引，不与零基轨道索引重叠。
    // 总计不按单轨宽度省略前缀，保留 TOTAL 以区别独立轨道的计数。
    CanvasComponentBounds bounds;
    if ( renderAsciiText(batcher,
                         text.data(),
                         placement,
                         layoutRegion,
                         context.viewportHeight,
                         bounds) ) {
        appendComponentInstance(*batcher.snapshot,
                                Config::CanvasComponentType::Kps,
                                Config::KPS_TOTAL_INSTANCE_INDEX,
                                bounds,
                                layoutRegion);
    }
}

}  // namespace

/// @brief 将启用的文字组件追加到快照覆盖层，并登记其布局交互实例。
/// @param snapshot 字体、播放状态和输出容器所在的本帧快照。
/// @param context 视口、轨道、时间与只读节拍缓存上下文。
/// @pre 上下文中的借用数据在调用期间有效，不与生产者并发修改。
/// @param config 组件可见性及默认、逐实例布局配置。
/// @note 不清空既有图元或实例；快照初始化由调用方负责。
/// @warning
/// 每帧热路径：只消费已准备的字体与缓存，禁止资源加载、全实体扫描和阻塞等待。
void CanvasComponentRenderSystem::render(
    RenderSnapshot* snapshot, const CanvasComponentRenderContext& context,
    const Config::CanvasComponentLayoutConfig& config)
{
    if ( !snapshot || !snapshot->asciiFontAtlasMetrics.valid ||
         context.viewportWidth <= 0.0f || context.viewportHeight <= 0.0f ) {
        // 前置数据不足时保持输出不变，不创建空批次或补占位文字。
        return;
    }

    // 覆盖层命令与普通谱面命令分开保存，仍共享同一快照的顶点和索引存储。
    Batcher batcher(snapshot, &snapshot->overlayCmds);
    batcher.setScissor(
        0.0f, 0.0f, context.viewportWidth, context.viewportHeight);

    for ( Config::CanvasComponentType type : Config::CANVAS_COMPONENT_TYPES ) {
        // 按固定类型列表分派，保持同一组组件的输出次序稳定。
        const auto& placement = config.placement(type);
        // 类型级可见性是总开关，关闭时不解析该类型的逐实例布局。
        if ( !placement.visible ) continue;

        // 无谱面的处理按组件职责决定；拍号入口还会自行检查 BPM 上下文。
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
        case Config::CanvasComponentType::BackgroundSpectrum:
            // 频谱必须紧邻背景图片绘制，此处只保留统一类型遍历入口。
            break;
        case Config::CanvasComponentType::Count: break;
        }
    }
    // 提交最后一个纹理批次，避免最后一种组件的顶点存在却没有对应绘制命令。
    batcher.flush();
}

/// @brief 将秒数格式化为带毫秒、可带负号的固定宽度时间文本。
/// @param currentTime 时间秒数；非有限输入按零显示。
/// @note 只改变显示形式，不将限幅结果回写到播放或编辑状态。
/// @return 以空字符结尾的 HH:MM:SS.mmm 文本缓冲。
/// @pre 有限输入的毫秒绝对值应可由 llround 转为 int64，显示限幅发生在转换之后。
/// @warning 时间组件热路径调用；返回栈上固定数组，不进行动态字符串分配。
std::array<char, 16> CanvasComponentRenderSystem::formatJudgmentLineTime(
    double currentTime)
{
    std::array<char, 16> result{};
    // 小于半毫秒的负值不显示负号，避免正常归零附近出现负零文本。
    const bool   negative = std::isfinite(currentTime) && currentTime < -0.0005;
    const double finiteTime = std::isfinite(currentTime) ? currentTime : 0.0;
    // 符号与绝对时间分开处理，字段拆分不受负数取余规则影响。
    // 先统一舍入到毫秒再拆字段，保证秒与分钟进位来自同一个整数时间。
    const auto totalMillis = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(std::llround(std::abs(finiteTime) * 1000.0)),
        0LL,
        MAX_DISPLAY_MILLIS);
    const auto millis       = totalMillis % 1000LL;
    const auto totalSeconds = totalMillis / 1000LL;
    const auto seconds      = totalSeconds % 60LL;
    const auto totalMinutes = totalSeconds / 60LL;
    const auto minutes      = totalMinutes % 60LL;
    const auto hours        = totalMinutes / 60LL;

    // 字段都由同一个毫秒整数拆分，避免秒、分钟边界各自舍入而产生不一致进位。
    // 小时限制为两位，负号、毫秒及终止符都能放进固定缓冲。
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

/// @brief 将节拍序号格式化为带 # 前缀的十进制文本。
/// @param beatIndex 节拍序号；负值按零显示。
/// @note 不包含拍内分数，分拍定位仍由网格上下文负责。
/// @return 含结尾空字符的固定缓冲，转换失败时使用 #0。
/// @warning 可见拍号热路径调用；使用无分配的整数转换。
std::array<char, 24> CanvasComponentRenderSystem::formatBeatNumber(
    std::int64_t beatIndex)
{
    std::array<char, 24> result{};
    result[0] = '#';
    beatIndex = std::max<std::int64_t>(0, beatIndex);
    // 显示层不重新推导节拍顺序，零值可表示尚未定位到有效节拍。
    // 首位留给前缀，末位留给空字符；to_chars 本身不会追加字符串终止符。
    const auto conversion = std::to_chars(
        result.data() + 1, result.data() + result.size() - 1, beatIndex);
    if ( conversion.ec != std::errc{} ) {
        // 不消费失败转换留下的部分内容，恢复一个完整可显示的最小文本。
        result[1] = '0';
        result[2] = '\0';
    } else {
        // to_chars 返回末尾写指针，直接补终止符，无需扫描数字长度。
        *conversion.ptr = '\0';
    }
    return result;
}

/// @brief 生成含一基轨号和单位的完整单轨 KPS 文本。
/// @param zeroBasedTrackIndex 零基轨道序号；负值显示为第一轨。
/// @param kps 当前轨道的计数。
/// @note 文本降级由 selectTrackKpsText 决定，本函数保留完整信息。
/// @return K1 12 KPS 形式的固定字符缓冲。
/// @pre 轨道序号由实际轨道集合提供，增加一后仍在 int32 范围内。
/// @warning 每轨文字热路径调用；这里只格式化，不读取播放状态或计数器。
std::array<char, 32> CanvasComponentRenderSystem::formatTrackKps(
    std::int32_t zeroBasedTrackIndex, std::uint32_t kps)
{
    // 可见轨号从一开始，内部布局实例仍使用零基索引，不在此修改调用方状态。
    std::array<char, 32> result{};
    std::snprintf(result.data(),
                  result.size(),
                  "K%d %u KPS",
                  std::max(zeroBasedTrackIndex, 0) + 1,
                  static_cast<unsigned int>(kps));
    return result;
}

/// @brief 将已经汇总的 KPS 生成总计文本。
/// @param kps 调用方完成累加与限幅后的总计数。
/// @note 零值仍生成完整总计文本，不把停播状态表示为空串。
/// @return TOTAL 12 KPS 形式的固定字符缓冲。
/// @warning 总计组件热路径调用；不在格式化层重复遍历轨道。
std::array<char, 32> CanvasComponentRenderSystem::formatTotalKps(
    std::uint32_t kps)
{
    // 固定前缀使总计即使移到逐轨文字附近，也能从文本上区分。
    std::array<char, 32> result{};
    std::snprintf(result.data(),
                  result.size(),
                  "TOTAL %u KPS",
                  static_cast<unsigned int>(kps));
    return result;
}

}  // namespace MMM::Logic::System
