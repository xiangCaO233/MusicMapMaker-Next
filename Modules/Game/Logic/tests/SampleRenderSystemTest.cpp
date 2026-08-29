#include "logic/ecs/system/SampleRenderSystem.h"

#include "common/AsciiFontData.h"
#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/system/render/Batcher.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

/// @brief 使用小容差比较纹理坐标。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个坐标足够接近时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-4F;
}

constexpr glm::vec4 NOTE_UV{ 0.25F, 0.35F, 0.2F, 0.1F };

/// @brief 为采样标签测试注入固定宽度 ASCII 字体度量和字形 UV。
/// @param snapshot 待初始化快照。
void configureAsciiFont(MMM::Logic::RenderSnapshot& snapshot)
{
    constexpr std::size_t tierIndex = 3U;
    auto&                 atlas     = snapshot.asciiFontAtlasMetrics;
    atlas.valid                     = true;
    auto& font                      = atlas.tiers[tierIndex];
    font.valid                      = true;
    font.ascender                   = 0.8F;
    font.lineHeight                 = 1.0F;
    for ( std::uint32_t code = MMM::Common::ASCII_GLYPH_FIRST;
          code <= MMM::Common::ASCII_GLYPH_LAST;
          ++code ) {
        auto& glyph     = font.glyphs[code - MMM::Common::ASCII_GLYPH_FIRST];
        glyph.available = true;
        glyph.hasBitmap = code != static_cast<std::uint32_t>(' ');
        glyph.width     = 0.5F;
        glyph.height    = 0.75F;
        glyph.bearingX  = 0.0F;
        glyph.bearingY  = 0.75F;
        glyph.advanceX  = 0.6F;
        if ( glyph.hasBitmap ) {
            const auto textureId = MMM::Logic::asciiGlyphTextureId(
                tierIndex, static_cast<char>(code));
            const float glyphU =
                0.6F +
                static_cast<float>(code - MMM::Common::ASCII_GLYPH_FIRST) *
                    0.001F;
            snapshot.uvMap.emplace(static_cast<std::uint32_t>(textureId),
                                   glm::vec4{ glyphU, 0.7F, 0.0008F, 0.01F });
        }
    }
}

/// @brief 为采样标签测试注入“初音”两个 CJK 字形及其 UV。
/// @param snapshot 待初始化快照。
void configureUnicodeFont(MMM::Logic::RenderSnapshot& snapshot)
{
    constexpr std::array<std::uint32_t, 2> codepoints{ 0x521DU, 0x97F3U };
    auto& unicodeFont      = snapshot.unicodeFontMetrics;
    unicodeFont.valid      = true;
    unicodeFont.ascender   = 0.88F;
    unicodeFont.lineHeight = 1.0F;
    for ( std::size_t index = 0U; index < codepoints.size(); ++index ) {
        MMM::Common::UnicodeGlyphMetrics entry;
        entry.codepoint         = codepoints[index];
        entry.metrics.available = true;
        entry.metrics.hasBitmap = true;
        entry.metrics.width     = 0.9F;
        entry.metrics.height    = 0.9F;
        entry.metrics.bearingX  = 0.0F;
        entry.metrics.bearingY  = 0.85F;
        entry.metrics.advanceX  = 1.0F;
        const auto textureId =
            MMM::Logic::unicodeGlyphTextureId(entry.codepoint);
        snapshot.uvMap.emplace(
            static_cast<std::uint32_t>(textureId),
            glm::vec4{ 0.82F + static_cast<float>(index) * 0.01F,
                       0.84F,
                       0.008F,
                       0.012F });
        unicodeFont.glyphs.push_back(entry);
    }
}

/// @brief 验证 BGM 轨道底色纹理与标题字号。
/// @return 底色未复用标签 UV 且标题按 16 像素字号绘制时返回 true。
bool testLaneLayoutVisuals()
{
    MMM::Logic::RenderSnapshot snapshot;
    constexpr glm::vec4        solidUv{ 0.1F, 0.2F, 0.04F, 0.06F };
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::None), solidUv);
    configureAsciiFont(snapshot);

    constexpr float        viewportWidth           = 800.0F;
    constexpr float        topY                    = 10.0F;
    constexpr float        bottomY                 = 590.0F;
    constexpr std::int32_t persistentBgmTrackCount = 3;
    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        viewportWidth, 4, persistentBgmTrackCount, 0.1F, 0.5F, 0.0F);
    MMM::Logic::System::Batcher batcher(&snapshot);
    MMM::Logic::System::SampleRenderSystem::renderLaneLayout(
        batcher,
        projection,
        persistentBgmTrackCount,
        viewportWidth,
        topY,
        bottomY);
    batcher.flush();

    const float expectedU = solidUv.x + solidUv.z * 0.5F;
    const float expectedV = solidUv.y + solidUv.w * 0.5F;
    for ( std::uint32_t laneIndex = 0U; laneIndex < projection.bgmLaneCount;
          ++laneIndex ) {
        const auto bounds =
            projection.bounds({ MMM::Logic::CanvasLaneKind::Bgm, laneIndex });
        if ( !bounds ) {
            XERROR("Unable to resolve BGM lane {} bounds", laneIndex);
            return false;
        }

        bool foundBackground = false;
        for ( std::size_t vertexIndex = 0U;
              vertexIndex + 3U < snapshot.vertices.size();
              vertexIndex += 4U ) {
            const auto* quad = snapshot.vertices.data() + vertexIndex;
            if ( !near(quad[0].pos.x, bounds->leftX) ||
                 !near(quad[0].pos.y, bottomY) ||
                 !near(quad[1].pos.x, bounds->rightX) ||
                 !near(quad[1].pos.y, bottomY) ||
                 !near(quad[2].pos.x, bounds->rightX) ||
                 !near(quad[2].pos.y, topY) ||
                 !near(quad[3].pos.x, bounds->leftX) ||
                 !near(quad[3].pos.y, topY) ) {
                continue;
            }

            foundBackground = true;
            for ( std::size_t corner = 0U; corner < 4U; ++corner ) {
                if ( !near(quad[corner].uv.u, expectedU) ||
                     !near(quad[corner].uv.v, expectedV) ) {
                    XERROR("BGM lane {} background reused a label glyph UV",
                           laneIndex);
                    return false;
                }
            }
            break;
        }
        if ( !foundBackground ) {
            XERROR("BGM lane {} background quad was not generated", laneIndex);
            return false;
        }
    }

    float maxLabelGlyphHeight = 0.0F;
    for ( std::size_t vertexIndex = 0U;
          vertexIndex + 3U < snapshot.vertices.size();
          vertexIndex += 4U ) {
        const auto* quad = snapshot.vertices.data() + vertexIndex;
        if ( quad[0].uv.u < 0.6F ) continue;
        maxLabelGlyphHeight = std::max(maxLabelGlyphHeight,
                                       std::abs(quad[0].pos.y - quad[3].pos.y));
    }
    // 测试字形高度为字号的 0.75，16 像素字号应生成 12 像素字形。
    if ( !near(maxLabelGlyphHeight, 12.0F) ) {
        XERROR("BGM lane label font size is too small: glyph height={}",
               maxLabelGlyphHeight);
        return false;
    }
    return true;
}

/// @brief 验证窄 BGM 轨道标题会在轨道范围内自动滚动。
/// @return 同一字形随单调时钟左移且始终受轨道宽度裁剪时返回 true。
bool testNarrowLaneLabelMarquee()
{
    constexpr float viewportWidth = 800.0F;
    const auto      projection    = MMM::Logic::calculateCanvasLaneProjection(
        viewportWidth, 4, 3, 0.1F, 0.3F, 0.0F);
    const auto firstLane =
        projection.bounds({ MMM::Logic::CanvasLaneKind::Bgm, 0U });
    if ( !firstLane ) {
        XERROR("Unable to resolve narrow BGM lane bounds");
        return false;
    }

    const auto renderGlyphLeft =
        [&](double monotonicSeconds) -> std::optional<float> {
        MMM::Logic::RenderSnapshot snapshot;
        snapshot.snapshotSysTime = monotonicSeconds;
        snapshot.uvMap.emplace(
            static_cast<std::uint32_t>(MMM::Logic::TextureID::None),
            glm::vec4{ 0.1F, 0.2F, 0.04F, 0.06F });
        configureAsciiFont(snapshot);

        MMM::Logic::System::Batcher batcher(&snapshot);
        MMM::Logic::System::SampleRenderSystem::renderLaneLayout(
            batcher, projection, 3, firstLane->rightX, 10.0F, 590.0F);
        batcher.flush();

        constexpr float glyphU =
            0.6F +
            static_cast<float>('G' - MMM::Common::ASCII_GLYPH_FIRST) * 0.001F;
        for ( std::size_t vertexIndex = 0U;
              vertexIndex + 3U < snapshot.vertices.size();
              vertexIndex += 4U ) {
            const auto* quad = snapshot.vertices.data() + vertexIndex;
            if ( !near(quad[0].uv.u, glyphU) ) continue;
            const float glyphLeft  = quad[0].pos.x;
            const float glyphRight = quad[1].pos.x;
            if ( glyphLeft < firstLane->leftX + 4.0F ||
                 glyphRight > firstLane->rightX - 4.0F ) {
                XERROR("Scrolling BGM lane label escaped its lane");
                return std::nullopt;
            }
            return glyphLeft;
        }
        return std::nullopt;
    };

    const auto pausedX   = renderGlyphLeft(1.0);
    const auto scrolledX = renderGlyphLeft(1.5);
    if ( !pausedX || !scrolledX || !(*scrolledX < *pausedX) ) {
        XERROR("Narrow BGM lane label did not scroll with the monotonic clock");
        return false;
    }
    return true;
}

/// @brief 为单个零 offset 自动采样生成测试快照。
/// @param snapshot 输出快照。
/// @param resourceId 音频资源 ID。
/// @param snapshotSysTime 单调时钟秒数。
/// @param withAsciiFont 是否注入标签字体。
/// @param noteScaleX 物件横向缩放。
/// @param noteScaleY 物件纵向缩放。
/// @param erasing 是否启用待删除预览。
/// @param hovered 是否启用悬浮状态。
/// @param selected 是否启用选中状态。
void renderSingleSample(MMM::Logic::RenderSnapshot& snapshot,
                        std::string_view resourceId, double snapshotSysTime,
                        bool withAsciiFont, float noteScaleX = 1.2F,
                        float noteScaleY = 1.2F, bool erasing = false,
                        bool hovered = false, bool selected = false)
{
    entt::registry timelineRegistry;
    const auto     bpmEntity = timelineRegistry.create();
    timelineRegistry.emplace<MMM::Logic::TimelineComponent>(
        bpmEntity,
        MMM::Logic::TimelineComponent{
            .m_timestamp = 0.0,
            .m_effect    = MMM::TimingEffect::BPM,
            .m_value     = 120.0,
        });

    MMM::Config::EditorConfig config;
    config.visual.noteScaleX = noteScaleX;
    config.visual.noteScaleY = noteScaleY;
    MMM::Logic::System::ScrollCache cache;
    cache.rebuild(timelineRegistry, config, nullptr);

    entt::registry sampleRegistry;
    const auto     sampleEntity = sampleRegistry.create();
    sampleRegistry.emplace<MMM::Logic::SampleComponent>(
        sampleEntity,
        MMM::Logic::SampleComponent{
            .m_timestamp       = 0.0,
            .m_track           = 4,
            .m_audioResourceId = std::string(resourceId),
        });
    sampleRegistry.emplace<MMM::Logic::InteractionComponent>(
        sampleEntity,
        MMM::Logic::InteractionComponent{
            .isHovered  = hovered,
            .isSelected = selected,
        });
    const std::vector<entt::entity> sortedEntities{ sampleEntity };
    const std::vector<double>       maxEndPrefix{ 0.0 };
    if ( erasing ) {
        snapshot.erasingObjectKind = MMM::Logic::ChartObjectKind::AudioSample;
        snapshot.erasingEntities.insert(sampleEntity);
    }

    snapshot.snapshotSysTime = snapshotSysTime;
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::None),
        glm::vec4{ 0.0F, 0.0F, 0.01F, 0.01F });
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::Note), NOTE_UV);
    if ( withAsciiFont ) configureAsciiFont(snapshot);

    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        800.0F, 4, 1, 0.1F, 0.5F, 0.0F);
    MMM::Logic::System::Batcher batcher(&snapshot);
    MMM::Logic::System::SampleRenderSystem::renderSamples(sampleRegistry,
                                                          sortedEntities,
                                                          maxEndPrefix,
                                                          &snapshot,
                                                          batcher,
                                                          projection,
                                                          &cache,
                                                          config,
                                                          0.0,
                                                          300.0F,
                                                          800.0F,
                                                          0.0F,
                                                          600.0F,
                                                          1.0F);
    batcher.flush();
}

/// @brief 为 BGM 区画笔预览生成不含正式采样的快照。
/// @param snapshot 输出快照。
void renderSampleBrushPreview(MMM::Logic::RenderSnapshot& snapshot)
{
    entt::registry timelineRegistry;
    const auto     bpmEntity = timelineRegistry.create();
    timelineRegistry.emplace<MMM::Logic::TimelineComponent>(
        bpmEntity,
        MMM::Logic::TimelineComponent{
            .m_timestamp = 0.0,
            .m_effect    = MMM::TimingEffect::BPM,
            .m_value     = 120.0,
        });

    MMM::Config::EditorConfig       config;
    MMM::Logic::System::ScrollCache cache;
    cache.rebuild(timelineRegistry, config, nullptr);

    entt::registry sampleRegistry;
    snapshot.brush.isActive           = true;
    snapshot.brush.createsAudioSample = true;
    snapshot.brush.time               = 0.0;
    snapshot.brush.track              = 4;
    snapshot.brush.audioResourceId    = "preview.wav";
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::None),
        glm::vec4{ 0.0F, 0.0F, 0.01F, 0.01F });
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::Note), NOTE_UV);

    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        800.0F, 4, 1, 0.1F, 0.5F, 0.0F);
    MMM::Logic::System::Batcher batcher(&snapshot);
    MMM::Logic::System::SampleRenderSystem::renderSamples(sampleRegistry,
                                                          {},
                                                          {},
                                                          &snapshot,
                                                          batcher,
                                                          projection,
                                                          &cache,
                                                          config,
                                                          0.0,
                                                          300.0F,
                                                          800.0F,
                                                          0.0F,
                                                          600.0F,
                                                          1.0F);
    batcher.flush();
}

/// @brief 验证自动采样本体与玩家 Tap 使用相同纹理和尺寸公式。
/// @return 采样本体的图集 UV、宽高与玩家 Tap 一致时返回 true。
bool testSampleBodyMatchesTapTextureAndSize()
{
    MMM::Logic::RenderSnapshot snapshot;
    renderSingleSample(snapshot, "sample.wav", 0.0, false);
    MMM::Config::EditorConfig config;
    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        800.0F, 4, 1, 0.1F, 0.5F, 0.0F);

    if ( snapshot.vertices.size() != 4U || snapshot.indices.size() != 6U ) {
        XERROR("Zero-offset sample rendered decorations over its Note texture");
        return false;
    }

    bool  foundNoteTopLeft     = false;
    bool  foundNoteBottomRight = false;
    float minX                 = snapshot.vertices.front().pos.x;
    float maxX                 = minX;
    float minY                 = snapshot.vertices.front().pos.y;
    float maxY                 = minY;
    for ( const auto& vertex : snapshot.vertices ) {
        foundNoteTopLeft = foundNoteTopLeft || (near(vertex.uv.u, NOTE_UV.x) &&
                                                near(vertex.uv.v, NOTE_UV.y));
        foundNoteBottomRight =
            foundNoteBottomRight || (near(vertex.uv.u, NOTE_UV.x + NOTE_UV.z) &&
                                     near(vertex.uv.v, NOTE_UV.y + NOTE_UV.w));
        minX = std::min(minX, vertex.pos.x);
        maxX = std::max(maxX, vertex.pos.x);
        minY = std::min(minY, vertex.pos.y);
        maxY = std::max(maxY, vertex.pos.y);
    }
    if ( !foundNoteTopLeft || !foundNoteBottomRight ) {
        XERROR("Sample body did not use the configured Note texture UV");
        return false;
    }

    const float laneWidth     = projection.player.singleTrackWidth;
    const float expectedWidth = laneWidth * config.visual.noteScaleX;
    const float expectedHeight =
        laneWidth / (NOTE_UV.z / NOTE_UV.w) * config.visual.noteScaleY;
    if ( !near(maxX - minX, expectedWidth) ||
         !near(maxY - minY, expectedHeight) ) {
        XERROR("Sample body size diverged from the player Tap size");
        return false;
    }
    const auto& sampleVertex = snapshot.vertices.front();
    if ( !near(sampleVertex.color.r, 0.36F) ||
         !near(sampleVertex.color.g, 0.72F) ||
         !near(sampleVertex.color.b, 0.92F) ||
         !near(sampleVertex.color.a, 0.96F) ) {
        XERROR("Sample body lost its distinct BGM object color");
        return false;
    }
    return true;
}

/// @brief 验证 BGM 画笔按下后立即绘制半透明自动采样并跟随笔刷位置。
/// @return 预览使用采样颜色、Note 纹理且不生成可拾取正式实体时返回 true。
bool testSampleBrushPreview()
{
    MMM::Logic::RenderSnapshot snapshot;
    renderSampleBrushPreview(snapshot);
    if ( snapshot.vertices.size() != 4U || snapshot.indices.size() != 6U ||
         !snapshot.hitboxes.empty() ) {
        XERROR("Sample brush preview did not render as one transient object");
        return false;
    }
    const auto& vertex = snapshot.vertices.front();
    if ( !near(vertex.color.r, 0.36F) || !near(vertex.color.g, 0.72F) ||
         !near(vertex.color.b, 0.92F) || !near(vertex.color.a, 0.48F) ||
         !near(vertex.uv.u, NOTE_UV.x) ||
         (!near(vertex.uv.v, NOTE_UV.y) &&
          !near(vertex.uv.v, NOTE_UV.y + NOTE_UV.w)) ) {
        XERROR(
            "Sample brush preview lost styling: color=({:.3f},{:.3f},{:.3f},"
            "{:.3f}) uv=({:.3f},{:.3f})",
            vertex.color.r,
            vertex.color.g,
            vertex.color.b,
            vertex.color.a,
            vertex.uv.u,
            vertex.uv.v);
        return false;
    }
    return true;
}

/// @brief 验证右键按住自动采样时显示红色半透明待删除效果。
/// @return 采样本体切换为红色半透明且不影响纹理时返回 true。
bool testSampleErasePreview()
{
    MMM::Logic::RenderSnapshot snapshot;
    renderSingleSample(snapshot, "erase.wav", 0.0, false, 1.2F, 1.2F, true);
    if ( snapshot.vertices.size() != 4U ) {
        XERROR("Sample erase preview rendered unexpected decorations");
        return false;
    }
    const auto& vertex = snapshot.vertices.front();
    if ( !near(vertex.color.r, 1.0F) || !near(vertex.color.g, 0.2F) ||
         !near(vertex.color.b, 0.2F) || !near(vertex.color.a, 0.5F) ) {
        XERROR("Sample erase preview was not red and translucent");
        return false;
    }
    return true;
}

/// @brief 验证自动采样悬浮和选中状态沿用本体颜色并写入发光层。
/// @return 两种交互状态均只增加同色发光命令时返回 true。
bool testSampleInteractionGlow()
{
    MMM::Logic::RenderSnapshot hoveredSnapshot;
    MMM::Logic::RenderSnapshot selectedSnapshot;
    renderSingleSample(hoveredSnapshot,
                       "hovered.wav",
                       0.0,
                       false,
                       1.2F,
                       1.2F,
                       false,
                       true,
                       false);
    renderSingleSample(selectedSnapshot,
                       "selected.wav",
                       0.0,
                       false,
                       1.2F,
                       1.2F,
                       false,
                       false,
                       true);

    for ( const auto* snapshot : { &hoveredSnapshot, &selectedSnapshot } ) {
        if ( snapshot->glowCmds.size() != 1U ||
             snapshot->glowCmds.front().indexCount != 6U ) {
            XERROR("Interactive sample did not generate one body glow command");
            return false;
        }

        std::size_t noteVertexCount = 0U;
        for ( const auto& vertex : snapshot->vertices ) {
            const bool usesNoteUv =
                vertex.uv.u >= NOTE_UV.x - 1e-4F &&
                vertex.uv.u <= NOTE_UV.x + NOTE_UV.z + 1e-4F &&
                vertex.uv.v >= NOTE_UV.y - 1e-4F &&
                vertex.uv.v <= NOTE_UV.y + NOTE_UV.w + 1e-4F;
            if ( !usesNoteUv ) continue;
            ++noteVertexCount;
            if ( !near(vertex.color.r, 0.36F) || !near(vertex.color.g, 0.72F) ||
                 !near(vertex.color.b, 0.92F) ||
                 !near(vertex.color.a, 0.96F) ) {
                XERROR(
                    "Interactive sample replaced its base color instead of "
                    "reusing it for glow");
                return false;
            }
        }
        if ( noteVertexCount != 8U ) {
            XERROR(
                "Interactive sample base and glow geometry did not both use "
                "the Note texture");
            return false;
        }
    }
    return true;
}

/// @brief 判断两份快照的标签几何是否完全一致。
/// @param lhs 第一份快照。
/// @param rhs 第二份快照。
/// @return 忽略前四个物件本体顶点后，标签位置与 UV 均一致时返回 true。
bool labelGeometryEqual(const MMM::Logic::RenderSnapshot& lhs,
                        const MMM::Logic::RenderSnapshot& rhs)
{
    if ( lhs.vertices.size() != rhs.vertices.size() ||
         lhs.vertices.size() <= 4U ) {
        return false;
    }
    for ( std::size_t index = 4U; index < lhs.vertices.size(); ++index ) {
        const auto& left  = lhs.vertices[index];
        const auto& right = rhs.vertices[index];
        if ( !near(left.pos.x, right.pos.x) || !near(left.pos.y, right.pos.y) ||
             !near(left.uv.u, right.uv.u) || !near(left.uv.v, right.uv.v) ) {
            return false;
        }
    }
    return true;
}

/// @brief 计算忽略物件本体后的标签字形几何高度。
/// @param snapshot 待检查快照。
/// @return 标签字形覆盖高度；没有标签几何时返回零。
float labelGlyphHeight(const MMM::Logic::RenderSnapshot& snapshot)
{
    if ( snapshot.vertices.size() <= 4U ) return 0.0F;
    float minY = snapshot.vertices[4U].pos.y;
    float maxY = minY;
    for ( std::size_t index = 5U; index < snapshot.vertices.size(); ++index ) {
        minY = std::min(minY, snapshot.vertices[index].pos.y);
        maxY = std::max(maxY, snapshot.vertices[index].pos.y);
    }
    return maxY - minY;
}

/// @brief 验证短标签保持居中静止，长标签在轨道宽度内循环滚动。
/// @return 标签静止、滚动和 CPU 侧水平裁剪均符合预期时返回 true。
bool testSampleLabelMarquee()
{
    MMM::Logic::RenderSnapshot shortStart;
    MMM::Logic::RenderSnapshot shortLater;
    renderSingleSample(shortStart, "fx.wav", 0.0, true);
    renderSingleSample(shortLater, "fx.wav", 2.25, true);
    if ( !labelGeometryEqual(shortStart, shortLater) ) {
        XERROR("Short sample label moved despite fitting inside the object");
        return false;
    }

    MMM::Logic::RenderSnapshot longStart;
    MMM::Logic::RenderSnapshot longLater;
    renderSingleSample(
        longStart, "very_long_sample_resource_name.wav", 0.0, true);
    renderSingleSample(
        longLater, "very_long_sample_resource_name.wav", 2.25, true);
    if ( longStart.vertices.size() <= 4U || longLater.vertices.size() <= 4U ||
         labelGeometryEqual(longStart, longLater) ) {
        XERROR("Long sample label did not advance with the monotonic clock");
        return false;
    }

    const auto projection = MMM::Logic::calculateCanvasLaneProjection(
        800.0F, 4, 1, 0.1F, 0.5F, 0.0F);
    const auto laneBounds =
        projection.bounds({ MMM::Logic::CanvasLaneKind::Bgm, 0U });
    if ( !laneBounds ) {
        XERROR("Unable to resolve the first BGM lane bounds");
        return false;
    }
    const float labelLeft  = laneBounds->leftX + 2.0F;
    const float labelRight = laneBounds->rightX - 2.0F;
    for ( const auto* snapshot : { &longStart, &longLater } ) {
        for ( std::size_t index = 4U; index < snapshot->vertices.size();
              ++index ) {
            const float x = snapshot->vertices[index].pos.x;
            if ( x < labelLeft - 1e-4F || x > labelRight + 1e-4F ) {
                XERROR("Scrolling sample label escaped its lane width");
                return false;
            }
        }
    }
    return true;
}

/// @brief 验证标签字号跟随纵向物件缩放，裁剪范围不跟随横向缩放。
/// @return 字号比例和固定轨道宽度裁剪均符合预期时返回 true。
bool testSampleLabelScaleAndFixedLaneWidth()
{
    MMM::Logic::RenderSnapshot narrowBody;
    MMM::Logic::RenderSnapshot wideBody;
    renderSingleSample(narrowBody,
                       "very_long_sample_resource_name.wav",
                       2.25,
                       true,
                       0.5F,
                       1.0F);
    renderSingleSample(
        wideBody, "very_long_sample_resource_name.wav", 2.25, true, 3.0F, 1.0F);
    if ( !labelGeometryEqual(narrowBody, wideBody) ) {
        XERROR("Sample label bounds changed with horizontal object scale");
        return false;
    }

    MMM::Logic::RenderSnapshot shortBody;
    MMM::Logic::RenderSnapshot tallBody;
    renderSingleSample(shortBody, "fx.wav", 0.0, true, 1.0F, 0.5F);
    renderSingleSample(tallBody, "fx.wav", 0.0, true, 1.0F, 2.0F);
    const float shortGlyphHeight = labelGlyphHeight(shortBody);
    const float tallGlyphHeight  = labelGlyphHeight(tallBody);
    if ( shortGlyphHeight <= 0.0F ||
         !near(tallGlyphHeight / shortGlyphHeight, 4.0F) ) {
        XERROR("Sample label font size did not follow vertical object scale");
        return false;
    }
    return true;
}

/// @brief 验证采样标签按 UTF-8 码点使用按需加载的 CJK 字形。
/// @return “初音”均使用 Unicode 图集纹理且没有被替换成问号时返回 true。
bool testSampleLabelCjkGlyphs()
{
    MMM::Logic::RenderSnapshot snapshot;
    configureUnicodeFont(snapshot);
    renderSingleSample(snapshot, "初音.wav", 0.0, true);

    if ( snapshot.requestedUnicodeGlyphCount != 0U ) {
        XERROR("Loaded CJK sample label requested a redundant atlas refresh");
        return false;
    }

    bool foundFirst  = false;
    bool foundSecond = false;
    for ( std::size_t index = 4U; index < snapshot.vertices.size(); ++index ) {
        const float u = snapshot.vertices[index].uv.u;
        foundFirst    = foundFirst || near(u, 0.82F);
        foundSecond   = foundSecond || near(u, 0.83F);
    }
    if ( !foundFirst || !foundSecond ) {
        XERROR("CJK sample label did not use the Unicode glyph atlas");
        return false;
    }
    return true;
}

/// @brief 验证可见 CJK 标签会回报当前图集缺失的码点。
/// @return 截图项目的日文资源名各码点只回报一次时返回 true。
bool testMissingCjkGlyphRequestsAtlasRefresh()
{
    MMM::Logic::RenderSnapshot snapshot;
    renderSingleSample(
        snapshot, "ぴょん (feat. 初音ミク & 重音テト).mp3", 0.0, true);
    constexpr std::array<std::uint32_t, 10> expectedCodepoints{
        0x3074U, 0x3087U, 0x3093U, 0x521DU, 0x97F3U,
        0x30DFU, 0x30AFU, 0x91CDU, 0x30C6U, 0x30C8U
    };
    if ( snapshot.requestedUnicodeGlyphCount != expectedCodepoints.size() ) {
        XERROR("Missing CJK label glyphs were not reported for atlas refresh");
        return false;
    }
    for ( std::size_t index = 0U; index < expectedCodepoints.size(); ++index ) {
        if ( snapshot.requestedUnicodeGlyphs[index] !=
             expectedCodepoints[index] ) {
            XERROR("CJK atlas refresh request lost the screenshot resource ID");
            return false;
        }
    }
    return true;
}

}  // namespace

/// @brief 运行自动采样渲染系统回归测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testLaneLayoutVisuals() && testNarrowLaneLabelMarquee() &&
                   testSampleBodyMatchesTapTextureAndSize() &&
                   testSampleBrushPreview() && testSampleErasePreview() &&
                   testSampleInteractionGlow() && testSampleLabelMarquee() &&
                   testSampleLabelScaleAndFixedLaneWidth() &&
                   testSampleLabelCjkGlyphs() &&
                   testMissingCjkGlyphRequestsAtlasRefresh()
               ? 0
               : 1;
}
