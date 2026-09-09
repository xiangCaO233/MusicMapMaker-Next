#include "logic/ecs/system/render/AudioObjectLabelRenderer.h"

#include "common/AsciiFontData.h"
#include "common/UnicodeFontData.h"
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

/// @brief 将资源 ID 按完整 UTF-8 序列截断到固定输出缓冲区。
/// @param output 输出缓冲区。
/// @param source 原资源 ID。
/// @note 输出非空时始终以零结尾；不添加省略号，也不修改原始资源名。
/// @warning 主画布热路径：只写入调用方提供的有界栈缓冲区。
void copyDisplayResourceId(std::span<char> output, std::string_view source)
{
    if ( output.empty() ) return;
    // 始终预留字符串终止符空间，截断按码点而不是任意字节发生。
    std::size_t sourceOffset = 0U;
    std::size_t outputOffset = 0U;
    while ( sourceOffset < source.size() &&
            outputOffset < output.size() - 1U ) {
        const std::size_t sequenceStart = sourceOffset;
        const auto        codepoint =
            Common::decodeNextUtf8Codepoint(source, sourceOffset);
        const std::size_t sequenceLength = sourceOffset - sequenceStart;
        // 合法的替换字符本身占多个字节，不应被当成损坏序列改写。
        const bool isInvalidSequence =
            codepoint == Common::UNICODE_REPLACEMENT_CODEPOINT &&
            sequenceLength == 1U &&
            static_cast<unsigned char>(source[sequenceStart]) >= 0x80U;
        if ( isInvalidSequence ) {
            // 非法高位单字节以可显示的 ASCII 替代，仍继续处理后续内容。
            output[outputOffset++] = '?';
            continue;
        }
        if ( outputOffset + sequenceLength >= output.size() ) break;
        // 剩余空间放不下完整序列时停止，不写入半个 UTF-8 字符。
        std::copy_n(source.data() + sequenceStart,
                    sequenceLength,
                    output.data() + outputOffset);
        outputOffset += sequenceLength;
    }
    output[outputOffset] = '\0';
}

/// @brief 按码点取得当前画布字体字形和纹理 ID。
/// @param selection 当前 ASCII 字号档位。
/// @param snapshot 提供当前 Unicode 字形度量，并收集缺字请求。
/// @param codepoint Unicode 码点。
/// @param textureId 接收字形纹理 ID。
/// @return 可用字形度量；缺字时回退到 ASCII 问号。
/// @pre selection 有效，返回指针仅在对应字体度量数据有效期间使用。
/// @warning 主画布热路径：Unicode 路径只执行二分查找，不得分配内存。
const Common::AsciiGlyphMetrics* resolveCanvasGlyph(
    RenderSnapshot& snapshot, const Common::AsciiFontSelection& selection,
    std::uint32_t codepoint, TextureID& textureId)
{
    textureId = TextureID::None;
    // 先查询当前 ASCII 档位，Unicode 则使用快照内按码点组织的度量。
    if ( codepoint <= Common::ASCII_GLYPH_LAST ) {
        const char  character = static_cast<char>(codepoint);
        const auto* glyph     = selection.metrics->glyph(character);
        if ( glyph && glyph->available ) {
            textureId = asciiGlyphTextureId(selection.tierIndex, character);
            return glyph;
        }
    } else if ( const auto* glyph =
                    snapshot.unicodeFontMetrics.glyph(codepoint);
                glyph && glyph->available ) {
        textureId = unicodeGlyphTextureId(codepoint);
        return glyph;
    } else {
        // 这里只登记请求，不同步生成字形；本帧先使用可用的替代字符。
        snapshot.requestUnicodeGlyph(codepoint);
    }

    const auto* fallback = selection.metrics->glyph('?');
    // 字形度量与纹理标识必须一起回退，不能以原码点纹理绘制问号尺寸。
    if ( fallback && fallback->available ) {
        textureId = asciiGlyphTextureId(selection.tierIndex, '?');
        return fallback;
    }
    return nullptr;
}

/// @brief 计算单行 UTF-8 文本的横向推进宽度。
/// @param selection 当前字号对应的 ASCII 字体档位。
/// @param snapshot 当前渲染快照，用于读取并回报按需 Unicode 字形。
/// @param text 文本。
/// @param fontPixelHeight 字体像素高度。
/// @return 文本完整横向推进宽度。
/// @note 缺失且没有替代字形的码点不贡献宽度，与绘制阶段保持一致。
/// @warning 主画布热路径：只允许线性扫描有界短文本。
float measureCanvasTextWidth(const Common::AsciiFontSelection& selection,
                             RenderSnapshot& snapshot, std::string_view text,
                             float fontPixelHeight)
{
    float width = 0.0F;
    // 使用排版推进量而非位图宽度，空格与左右留白也参与整体宽度。
    std::size_t offset = 0U;
    while ( offset < text.size() ) {
        const auto  codepoint = Common::decodeNextUtf8Codepoint(text, offset);
        TextureID   textureId = TextureID::None;
        const auto* glyph =
            resolveCanvasGlyph(snapshot, selection, codepoint, textureId);
        if ( glyph && glyph->available ) {
            // 字体度量按字号比例缩放；测量和绘制必须采用相同的字形回退规则。
            width += glyph->advanceX * fontPixelHeight;
        }
    }
    return width;
}

/// @brief 在水平裁剪区域内绘制一次 UTF-8 文本。
/// @param batcher 目标批处理器。
/// @param selection 已选中的字体档位。
/// @param text 文本。
/// @param penX 文本起始笔位置。
/// @param y 文本区域上边界。
/// @param fontPixelHeight 字体像素高度。
/// @param clipLeft 水平裁剪左边界。
/// @param clipRight 水平裁剪右边界。
/// @pre 输入为单行文本；这里不执行换行、双向排版或组合字形塑形。
/// @param color 文字颜色。
/// @warning 主画布热路径：逐字形裁剪并写入几何，不得分配内存或切换裁剪命令。
void renderCanvasTextRunAt(Batcher&                          batcher,
                           const Common::AsciiFontSelection& selection,
                           std::string_view text, float penX, float y,
                           float fontPixelHeight, float clipLeft,
                           float clipRight, glm::vec4 color)
{
    if ( !selection || text.empty() || clipRight <= clipLeft ) return;

    const auto& font = *selection.metrics;
    // 外部传入文本上边界，基线由字体升部度量换算得到。
    const float baselineY = y + font.ascender * fontPixelHeight;
    // Unicode 字形也沿用该基线，混合文本不按单字位图高度重新对齐。
    std::size_t offset = 0U;
    while ( offset < text.size() ) {
        const auto  codepoint = Common::decodeNextUtf8Codepoint(text, offset);
        TextureID   textureId = TextureID::None;
        const auto* glyph     = resolveCanvasGlyph(
            *batcher.snapshot, selection, codepoint, textureId);
        if ( !glyph || !glyph->available ) continue;
        const float advance = glyph->advanceX * fontPixelHeight;
        // 后续字符继续向右推进，笔位置越过右边界后无需再解码剩余文本。
        if ( penX >= clipRight ) break;

        if ( glyph->hasBitmap ) {
            // 空格等字形可能只有推进量而没有位图，保留排版但不生成四边形。
            const auto uvIt = batcher.snapshot->uvMap.find(
                static_cast<std::uint32_t>(textureId));
            if ( textureId != TextureID::None &&
                 uvIt != batcher.snapshot->uvMap.end() ) {
                const float left = penX + glyph->bearingX * fontPixelHeight;
                // bearing 决定位图相对笔位置和基线的偏移，不能用字符框代替。
                const float top = baselineY - glyph->bearingY * fontPixelHeight;
                const float width        = glyph->width * fontPixelHeight;
                const float height       = glyph->height * fontPixelHeight;
                const float visibleLeft  = std::max(left, clipLeft);
                const float visibleRight = std::min(left + width, clipRight);
                if ( visibleRight > visibleLeft && width > 1e-6F ) {
                    // 在 CPU 同时裁剪几何与 UV，避免为每个标签切换 Scissor。
                    // 只裁横向范围，纵向仍由外层画布的裁剪状态负责。
                    const float leftRatio  = (visibleLeft - left) / width;
                    const float rightRatio = (visibleRight - left) / width;
                    // 裁剪只改变横向 UV，保留完整高度和纵向采样范围。
                    const auto& uv = uvIt->second;
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
        // 即使位图未就绪或完全被裁剪，已有字形的推进量仍然生效。
    }
}

/// @brief 在空间不足时循环滚动 UTF-8 文本，足够时保持居中静止。
/// @param batcher 目标批处理器。
/// @param text 文本。
/// @param x 文本区域左边界。
/// @param y 文本区域上边界。
/// @param fontPixelHeight 字体像素高度。
/// @param maxWidth 最大可用宽度。
/// @param color 文字颜色。
/// @param monotonicSeconds 单调时钟秒数。
/// @param centerHorizontally 文本可完整显示时是否水平居中。
/// @note 不保存逐标签相位，相同宽度的标签可通过共用时钟保持同步。
/// @warning 主画布热路径：只绘制至多两份有界短文本，不得分配内存或创建
/// 独立 DrawCall 裁剪区。
void renderMarqueeCanvasTextAt(Batcher& batcher, std::string_view text, float x,
                               float y, float fontPixelHeight, float maxWidth,
                               glm::vec4 color, double monotonicSeconds,
                               bool centerHorizontally)
{
    const auto selection = Common::selectAsciiFont(
        batcher.snapshot->asciiFontAtlasMetrics, fontPixelHeight);
    if ( !selection || text.empty() || maxWidth <= 0.0F ) return;

    const float textWidth = measureCanvasTextWidth(
        selection, *batcher.snapshot, text, fontPixelHeight);
    if ( textWidth <= maxWidth ) {
        // 能完整显示时不应用滚动相位，避免短标签随时间移动。
        renderCanvasTextRunAt(
            batcher,
            selection,
            text,
            x + (centerHorizontally ? (maxWidth - textWidth) * 0.5F : 0.0F),
            y,
            fontPixelHeight,
            x,
            x + maxWidth,
            color);
        return;
    }

    constexpr double PAUSE_SECONDS = 1.25;
    // 停顿只是相位计算的一部分，绝不暂停线程或延迟其他标签绘制。
    constexpr double SCROLL_SPEED = 32.0;
    const float      gapWidth     = std::max(24.0F, fontPixelHeight * 2.0F);
    // 副本间留白随字号增大，区分循环接缝与资源名称本身。
    const double scrollDistance = static_cast<double>(textWidth + gapWidth);
    const double scrollSeconds  = scrollDistance / SCROLL_SPEED;
    const double cycleSeconds   = PAUSE_SECONDS + scrollSeconds;
    // 非法或负时间从周期起点开始，防止 fmod 产生不可用的位置。
    const double safeTime =
        std::isfinite(monotonicSeconds) && monotonicSeconds > 0.0
            ? monotonicSeconds
            : 0.0;
    const double phase = std::fmod(safeTime, cycleSeconds);
    // 不累计帧间位移，掉帧后直接以当前时间求位置，不追补丢失帧。
    const float offset =
        phase <= PAUSE_SECONDS
            ? 0.0F
            : -static_cast<float>((phase - PAUSE_SECONDS) * SCROLL_SPEED);
    const float firstX = x + offset;
    // 第二份文本隔一个完整滚动距离跟随，末尾离开后可无缝回到首份。
    const float secondX = firstX + static_cast<float>(scrollDistance);
    if ( firstX < x + maxWidth && firstX + textWidth > x ) {
        // 两份文本分别测试相交，完全离开可视区域的副本不生成字形。
        renderCanvasTextRunAt(batcher,
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
        renderCanvasTextRunAt(batcher,
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

/// @brief 获取音频物件标签乘色，皮肤未定义时使用浅色默认值。
/// @return 可直接用于画布顶点的 RGBA，不额外修改皮肤透明度。
/// @note 该接口不按物件音量调暗文字；音量信息通过文本后缀表达。
/// @warning 主画布热路径只查询现有皮肤颜色，不触发皮肤重载。
glm::vec4 audioObjectLabelColor()
{
    const auto color =
        Config::SkinManager::instance().getColor(AUDIO_OBJECT_LABEL_COLOR_KEY);
    if ( isMissingSkinColor(color) ) {
        // 哨兵表示缺少配置，不将诊断用洋红色直接显示为标签颜色。
        return { 0.9F, 0.96F, 1.0F, 0.96F };
    }
    return { color.r, color.g, color.b, color.a };
}

/// @brief 绘制单行静态画布文本，沿用历史 ASCII 接口名但支持 UTF-8。
/// @param batcher 接收字形图元的当前批处理器。
/// @param text 借用文本，在本次调用内消费完毕。
/// @param x 文本可视区域左边界。
/// @param y 文本区域上边界，不是字形基线。
/// @param fontPixelHeight 请求的字体像素高度。
/// @param maxWidth 水平可视区域宽度，超长文本只裁剪不滚动。
/// @param color 字形统一乘色。
/// @param centerHorizontally 仅在文本可完整显示时启用居中。
/// @note 静态入口不会在超宽时添加省略号，只显示左侧可见部分。
/// @warning 热路径测量后直接写字形几何，不加载字体或创建独立裁剪命令。
void renderCanvasAsciiText(Batcher& batcher, std::string_view text, float x,
                           float y, float fontPixelHeight, float maxWidth,
                           glm::vec4 color, bool centerHorizontally)
{
    const auto selection = Common::selectAsciiFont(
        batcher.snapshot->asciiFontAtlasMetrics, fontPixelHeight);
    if ( !selection || text.empty() || maxWidth <= 0.0F ) return;

    const float textWidth = measureCanvasTextWidth(
        selection, *batcher.snapshot, text, fontPixelHeight);
    // 超宽时从左侧开始显示，不能居中后同时裁掉两端。
    const float penX = x + (centerHorizontally && textWidth <= maxWidth
                                ? (maxWidth - textWidth) * 0.5F
                                : 0.0F);
    renderCanvasTextRunAt(batcher,
                          selection,
                          text,
                          penX,
                          y,
                          fontPixelHeight,
                          x,
                          x + maxWidth,
                          color);
}

/// @brief 绘制可循环滚动的画布文本，供音频标签等短文本共用。
/// @param batcher 当前快照批处理器。
/// @param text UTF-8 文本，长于可视宽度时才滚动。
/// @param x 可视区域左边界。
/// @param y 文本上边界。
/// @param fontPixelHeight 字体像素高度。
/// @param maxWidth 水平可视区域宽度。
/// @param color 字形乘色。
/// @param monotonicSeconds 单调时钟时间，用于非阻塞周期计算。
/// @param centerHorizontally 短文本是否在可视区域内居中。
/// @pre 调用方提供有界短文本，避免无上限的每帧测量成本。
/// @warning 热路径委托统一跑马灯逻辑，不维护逐标签动画对象。
void renderMarqueeCanvasAsciiText(Batcher& batcher, std::string_view text,
                                  float x, float y, float fontPixelHeight,
                                  float maxWidth, glm::vec4 color,
                                  double monotonicSeconds,
                                  bool   centerHorizontally)
{
    renderMarqueeCanvasTextAt(batcher,
                              text,
                              x,
                              y,
                              fontPixelHeight,
                              maxWidth,
                              color,
                              monotonicSeconds,
                              centerHorizontally);
}

/// @brief 在音频物件上方显示资源名及非默认音量。
/// @param batcher 标签图元输出目标。
/// @param audioResourceId 原始资源 ID，仅显示截断副本，不修改资源引用。
/// @param volume 线性音量倍率，显示时转换为百分数。
/// @param laneLeftX 物件所在轨道的左边界。
/// @param objectTopY 物件上边界，标签放在其上方。
/// @param laneWidth 当前轨道宽度，决定字号基准与水平裁剪区。
/// @param objectScaleY 标签整体字号缩放，无效值回退为 1。
/// @param color 标签文字乘色，由调用方选择。
/// @param monotonicSeconds 驱动超长标签滚动的单调秒数。
/// @note 音量后缀优先使用截断 ID 后的剩余空间，不反向缩短 ID 腾出容量。
/// @warning 逐音频物件热路径使用固定栈缓冲，不构造堆上显示字符串。
void renderAudioObjectLabel(Batcher& batcher, std::string_view audioResourceId,
                            float volume, float laneLeftX, float objectTopY,
                            float laneWidth, float objectScaleY,
                            glm::vec4 color, double monotonicSeconds)
{
    if ( audioResourceId.empty() || laneWidth <= 4.0F ) return;
    // 极窄轨道扣除左右留白后没有文字空间，不请求字形也不测量标签。

    // 显示上限包含终止符；长 ID 截断不会改变真实绑定的资源名称。
    std::array<char, 72> resourceBuffer{};
    copyDisplayResourceId(resourceBuffer, audioResourceId);
    const auto idLength = std::char_traits<char>::length(resourceBuffer.data());
    std::size_t resourceLength = idLength;
    // 单位音量省略后缀，保留有限显示空间给资源 ID。
    if ( std::abs(volume - 1.0F) > 1e-3F &&
         resourceLength < resourceBuffer.size() - 1 ) {
        auto* const volumeBegin = resourceBuffer.data() + resourceLength;
        // 格式化只写剩余容量，末尾始终为终止符留一字节。
        const auto volumeResult =
            fmt::format_to_n(volumeBegin,
                             resourceBuffer.size() - resourceLength - 1,
                             " {:.0f}%",
                             static_cast<double>(volume) * 100.0);
        *volumeResult.out = '\0';
        // 后缀由 ASCII 构成，容量截断不会造成新的 UTF-8 半字符。
        // 按实际写入量推进，不使用格式化期望长度，避免视图越过缓冲区。
        resourceLength +=
            static_cast<std::size_t>(volumeResult.out - volumeBegin);
    }

    // 非法缩放只回退显示字号，不改动原物件布局。
    const float safeScale = std::isfinite(objectScaleY) && objectScaleY > 0.0F
                                ? objectScaleY
                                : 1.0F;
    // 先限制轨宽导出的基础字号，再应用物件缩放，不抵消用户缩放效果。
    const float fontPixelHeight =
        std::clamp(laneWidth * 0.18F, 13.0F, 18.0F) * safeScale;
    // 左右各保留两单位内边距，标签底部与物件之间另留四单位间隔。
    renderMarqueeCanvasTextAt(
        batcher,
        std::string_view(resourceBuffer.data(), resourceLength),
        laneLeftX + 2.0F,
        objectTopY - fontPixelHeight - 4.0F,
        fontPixelHeight,
        laneWidth - 4.0F,
        color,
        monotonicSeconds,
        true);
}

}  // namespace MMM::Logic::System
