#include "logic/ecs/system/CanvasComponentRenderSystem.h"

#include "common/CanvasComponentLayout.h"
#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include <cmath>
#include <cstdint>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <limits>
#include <string>
#include <vector>

namespace
{

/// @brief 为测试快照注入固定宽度 ASCII 字体度量和字形 UV。
/// @param snapshot 待初始化快照。
void configureAsciiFont(MMM::Logic::RenderSnapshot& snapshot)
{
    constexpr std::size_t tierIndex = 5U;
    auto&                 atlas     = snapshot.asciiFontAtlasMetrics;
    atlas.valid                     = true;
    auto& font                      = atlas.tiers[tierIndex];
    font.valid                      = true;
    font.ascender                   = 0.8f;
    font.lineHeight                 = 1.0f;
    for ( std::uint32_t code = MMM::Common::ASCII_GLYPH_FIRST;
          code <= MMM::Common::ASCII_GLYPH_LAST;
          ++code ) {
        auto& glyph     = font.glyphs[code - MMM::Common::ASCII_GLYPH_FIRST];
        glyph.available = true;
        glyph.hasBitmap = code != static_cast<std::uint32_t>(' ');
        glyph.width     = 0.5f;
        glyph.height    = 0.75f;
        glyph.bearingX  = 0.0f;
        glyph.bearingY  = 0.75f;
        glyph.advanceX  = 0.6f;
        if ( glyph.hasBitmap ) {
            const auto textureId = MMM::Logic::asciiGlyphTextureId(
                tierIndex, static_cast<char>(code));
            snapshot.uvMap.emplace(static_cast<std::uint32_t>(textureId),
                                   glm::vec4(0.0f, 0.0f, 0.01f, 0.01f));
        }
    }
}

/// @brief 创建只含基础视口信息的画布组件渲染上下文。
/// @param currentTime 当前判定线时间。
/// @return 800x600 测试视口上下文。
MMM::Logic::System::CanvasComponentRenderContext makeRenderContext(
    double currentTime)
{
    return {
        .currentTime    = currentTime,
        .viewportWidth  = 800.0f,
        .viewportHeight = 600.0f,
        .judgmentLineY  = 300.0f,
        .visibleTop     = 0.0f,
        .visibleBottom  = 600.0f,
        .renderScaleY   = 1.0f,
    };
}

/// @brief 验证关闭组件时不会生成覆盖层几何。
/// @return 快照保持为空时返回 true。
bool testHiddenComponentDoesNotRender()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    configureAsciiFont(snapshot);
    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, makeRenderContext(12.345), config);
    return snapshot.vertices.empty() && snapshot.indices.empty() &&
           snapshot.overlayCmds.empty() &&
           snapshot.canvasComponentInstances.empty();
}

/// @brief 验证启用时间组件后生成最终覆盖层且几何保持在配置边界内。
/// @return 覆盖命令、纹理类型和顶点范围均正确时返回 true。
bool testVisibleComponentRendersInOverlay()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    configureAsciiFont(snapshot);
    auto& placement         = config.judgmentLineTime;
    placement.visible       = true;
    placement.anchorX       = 0.25f;
    placement.anchorY       = 0.75f;
    placement.fontSizeRatio = 0.04f;
    placement.color         = { 0.2f, 0.4f, 0.6f, 0.8f };

    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, makeRenderContext(72.345), config);
    if ( snapshot.vertices.empty() || snapshot.indices.empty() ||
         snapshot.overlayCmds.empty() ) {
        XERROR("Visible canvas component did not produce overlay geometry");
        return false;
    }
    for ( const auto& command : snapshot.overlayCmds ) {
        if ( command.customTextureId ==
             static_cast<std::uint32_t>(MMM::Logic::TextureID::None) ) {
            XERROR("Canvas component did not use an ASCII glyph texture");
            return false;
        }
    }

    const auto text =
        MMM::Logic::System::CanvasComponentRenderSystem::formatJudgmentLineTime(
            72.345);
    const float fontHeight    = placement.fontSizeRatio * 600.0f;
    const auto  fontSelection = MMM::Common::selectAsciiFont(
        snapshot.asciiFontAtlasMetrics, fontHeight);
    if ( !fontSelection ) {
        XERROR("Canvas component did not select an ASCII font tier");
        return false;
    }
    const auto textSize = MMM::Common::measureAsciiText(
        *fontSelection.metrics, text.data(), fontHeight);
    const auto bounds = MMM::Logic::canvasComponentBounds(
        placement, 800.0f, 600.0f, textSize.width, textSize.height);
    constexpr float epsilon = 1e-4f;
    for ( const auto& vertex : snapshot.vertices ) {
        if ( vertex.pos.x < bounds.left - epsilon ||
             vertex.pos.x > bounds.right + epsilon ||
             vertex.pos.y < bounds.top - epsilon ||
             vertex.pos.y > bounds.bottom + epsilon ) {
            XERROR("Canvas component vertex escaped its configured bounds");
            return false;
        }
        if ( std::abs(vertex.color.r - placement.color[0]) > epsilon ||
             std::abs(vertex.color.g - placement.color[1]) > epsilon ||
             std::abs(vertex.color.b - placement.color[2]) > epsilon ||
             std::abs(vertex.color.a - placement.color[3]) > epsilon ) {
            XERROR("Canvas component vertex did not use configured color");
            return false;
        }
    }
    return true;
}

/// @brief 验证拍号会按整拍复制并保持在各自拍内布局区域。
/// @return 实例编号、区域边界、默认樱桃红色和字形几何均正确时返回 true。
bool testBeatNumbersRenderInsideEachBeat()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    configureAsciiFont(snapshot);
    config.beatNumber.visible = true;

    entt::registry timelineRegistry;
    const auto     bpmEntity = timelineRegistry.create();
    auto&          bpm =
        timelineRegistry.emplace<MMM::Logic::TimelineComponent>(bpmEntity);
    bpm.m_timestamp = 0.0;
    bpm.m_effect    = MMM::TimingEffect::BPM;
    bpm.m_value     = 120.0;

    MMM::Logic::System::ScrollCache cache;
    MMM::Config::EditorConfig       editorConfig;
    cache.rebuild(timelineRegistry, editorConfig, nullptr);
    std::vector<const MMM::Logic::TimelineComponent*> bpmEvents{ &bpm };

    auto context        = makeRenderContext(1.0);
    context.bpmEvents   = bpmEvents;
    context.scrollCache = &cache;
    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, context, config);

    if ( snapshot.canvasComponentInstances.size() != 2U ||
         snapshot.vertices.empty() || snapshot.overlayCmds.empty() ) {
        XERROR("Beat number component did not render visible whole beats");
        return false;
    }
    if ( snapshot.canvasComponentInstances[0].beatIndex != 2 ||
         snapshot.canvasComponentInstances[1].beatIndex != 3 ) {
        XERROR("Beat number component did not preserve one-based beat order");
        return false;
    }

    constexpr float epsilon = 1e-4f;
    for ( const auto& instance : snapshot.canvasComponentInstances ) {
        if ( instance.type != MMM::Config::CanvasComponentType::BeatNumber ||
             instance.beatIndex <= 0 || instance.regionLeft != 0.0f ||
             instance.regionRight != 800.0f ||
             instance.left < instance.regionLeft - epsilon ||
             instance.right > instance.regionRight + epsilon ||
             instance.top < instance.regionTop - epsilon ||
             instance.bottom > instance.regionBottom + epsilon ) {
            XERROR("Beat number escaped its per-beat layout region");
            return false;
        }
    }

    const auto& cherry = MMM::Config::DEFAULT_BEAT_NUMBER_PLACEMENT.color;
    for ( const auto& vertex : snapshot.vertices ) {
        if ( std::abs(vertex.color.r - cherry[0]) > epsilon ||
             std::abs(vertex.color.g - cherry[1]) > epsilon ||
             std::abs(vertex.color.b - cherry[2]) > epsilon ||
             std::abs(vertex.color.a - cherry[3]) > epsilon ) {
            XERROR("Beat number did not use the default cherry color");
            return false;
        }
    }
    return true;
}

/// @brief 验证判定线时间的固定精度格式与负值处理。
/// @return 常规、负值和非有限输入均符合约定时返回 true。
bool testJudgmentLineTimeFormatting()
{
    using System = MMM::Logic::System::CanvasComponentRenderSystem;
    return std::string(System::formatJudgmentLineTime(3723.456).data()) ==
               "01:02:03.456" &&
           std::string(System::formatJudgmentLineTime(-1.25).data()) ==
               "-00:00:01.250" &&
           std::string(System::formatJudgmentLineTime(
                           std::numeric_limits<double>::quiet_NaN())
                           .data()) == "00:00:00.000" &&
           std::string(System::formatBeatNumber(12345).data()) == "12345" &&
           std::string(System::formatBeatNumber(-5).data()) == "0";
}

}  // namespace

/// @brief 运行画布组件最终覆盖层回归测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testHiddenComponentDoesNotRender() &&
                   testVisibleComponentRendersInOverlay() &&
                   testBeatNumbersRenderInsideEachBeat() &&
                   testJudgmentLineTimeFormatting()
               ? 0
               : 1;
}
