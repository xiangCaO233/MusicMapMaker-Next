#include "logic/ecs/system/CanvasComponentRenderSystem.h"

#include "common/CanvasComponentLayout.h"
#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/HitFXSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include <algorithm>
#include <array>
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
    snapshot.hasBeatmap     = true;
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

/// @brief 验证未加载谱面时不会绘制当前判定线时间。
/// @return 时间组件已启用但快照保持无覆盖层几何时返回 true。
bool testJudgmentLineTimeDoesNotRenderWithoutBeatmap()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    configureAsciiFont(snapshot);
    config.judgmentLineTime.visible = true;

    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, makeRenderContext(-0.035), config);
    return snapshot.vertices.empty() && snapshot.indices.empty() &&
           snapshot.overlayCmds.empty() &&
           snapshot.canvasComponentInstances.empty();
}

/// @brief 验证拍号会按整拍复制并保持在各自拍内布局区域。
/// @return 实例编号、区域边界、默认暗橙色和字形几何均正确时返回 true。
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
    if ( snapshot.canvasComponentInstances[0].instanceIndex != 2 ||
         snapshot.canvasComponentInstances[1].instanceIndex != 3 ) {
        XERROR("Beat number component did not preserve one-based beat order");
        return false;
    }

    constexpr float epsilon = 1e-4f;
    for ( const auto& instance : snapshot.canvasComponentInstances ) {
        if ( instance.type != MMM::Config::CanvasComponentType::BeatNumber ||
             instance.instanceIndex <= 0 || instance.regionLeft != 0.0f ||
             instance.regionRight != 800.0f ||
             instance.left < instance.regionLeft - epsilon ||
             instance.right > instance.regionRight + epsilon ||
             instance.top < instance.regionTop - epsilon ||
             instance.bottom > instance.regionBottom + epsilon ) {
            XERROR("Beat number escaped its per-beat layout region");
            return false;
        }
    }

    const auto& darkOrange = MMM::Config::DEFAULT_BEAT_NUMBER_PLACEMENT.color;
    if ( std::abs(darkOrange[0] - 1.0f) > epsilon ||
         std::abs(darkOrange[1] - 140.0f / 255.0f) > epsilon ||
         std::abs(darkOrange[2]) > epsilon ||
         std::abs(darkOrange[3] - 1.0f) > epsilon ) {
        XERROR("Beat number default color is not #FF8C00");
        return false;
    }
    for ( const auto& vertex : snapshot.vertices ) {
        if ( std::abs(vertex.color.r - darkOrange[0]) > epsilon ||
             std::abs(vertex.color.g - darkOrange[1]) > epsilon ||
             std::abs(vertex.color.b - darkOrange[2]) > epsilon ||
             std::abs(vertex.color.a - darkOrange[3]) > epsilon ) {
            XERROR("Beat number did not use the default dark orange color");
            return false;
        }
    }
    return true;
}

/// @brief 验证拍起点越过判定线后，拍号会保留到文字离开轨道布局视口。
/// @return 当前拍文字仍被生成且绘制命令使用布局视口 scissor 时返回 true。
bool testBeatNumberRendersUntilLayoutViewportExit()
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

    auto context           = makeRenderContext(1.4);
    context.viewportHeight = 650.0f;
    context.judgmentLineY  = 500.0f;
    context.visibleTop     = 25.0f;
    context.visibleBottom  = 600.0f;
    context.bpmEvents      = bpmEvents;
    context.scrollCache    = &cache;
    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, context, config);

    const auto instance = std::find_if(
        snapshot.canvasComponentInstances.begin(),
        snapshot.canvasComponentInstances.end(),
        [](const auto& candidate) { return candidate.instanceIndex == 3; });
    if ( instance == snapshot.canvasComponentInstances.end() ||
         instance->regionBottom <= context.visibleBottom ||
         instance->top <= context.judgmentLineY ||
         instance->top >= context.visibleBottom ||
         instance->bottom <= context.visibleBottom ) {
        XERROR("Beat number disappeared before leaving the layout viewport");
        return false;
    }

    for ( const auto& command : snapshot.overlayCmds ) {
        if ( command.scissor.offset.x != 0 || command.scissor.offset.y != 25 ||
             command.scissor.extent.width != 800U ||
             command.scissor.extent.height != 575U ) {
            XERROR("Beat number did not use the layout viewport scissor");
            return false;
        }
    }
    return !snapshot.overlayCmds.empty();
}

/// @brief 验证拍内可移动区域保留上边界并按当前文字半高向下扩展。
/// @return 上方空间、下方扩展量和拍头线文字中心均正确时返回 true。
bool testBeatNumberLayoutRegionCentersOnBeatHead()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    configureAsciiFont(snapshot);
    config.beatNumber.visible = true;
    config.beatNumber.anchorY = 1.0f;

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

    auto context           = makeRenderContext(1.08);
    context.viewportHeight = 750.0f;
    context.judgmentLineY  = 300.0f;
    context.visibleTop     = 100.0f;
    context.visibleBottom  = 700.0f;
    context.bpmEvents      = bpmEvents;
    context.scrollCache    = &cache;
    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, context, config);

    const auto instance = std::find_if(
        snapshot.canvasComponentInstances.begin(),
        snapshot.canvasComponentInstances.end(),
        [](const auto& candidate) { return candidate.instanceIndex == 4; });
    if ( instance == snapshot.canvasComponentInstances.end() ) {
        XERROR("Beat number for beat-head alignment was not rendered");
        return false;
    }

    constexpr float rawRegionTop    = -160.0f;
    constexpr float rawRegionBottom = 90.0f;
    const float     fontHeight =
        config.beatNumber.fontSizeRatio * context.viewportHeight;
    const auto selection = MMM::Common::selectAsciiFont(
        snapshot.asciiFontAtlasMetrics, fontHeight);
    const auto text =
        MMM::Logic::System::CanvasComponentRenderSystem::formatBeatNumber(4);
    if ( !selection ) {
        XERROR("Beat number did not select a font for layout alignment");
        return false;
    }
    const auto textSize = MMM::Common::measureAsciiText(
        *selection.metrics, text.data(), fontHeight);
    const float     expectedOffset = textSize.height * 0.5f;
    const float     contentCenterY = (instance->top + instance->bottom) * 0.5f;
    constexpr float epsilon        = 1e-4f;
    if ( std::abs(instance->regionTop - rawRegionTop) > epsilon ||
         std::abs(instance->regionBottom - (rawRegionBottom + expectedOffset)) >
             epsilon ||
         std::abs(contentCenterY - rawRegionBottom) > epsilon ||
         instance->top >= context.visibleTop ||
         instance->bottom <= context.visibleTop ) {
        XERROR("Beat number layout range did not center on the beat-head line");
        return false;
    }
    return true;
}

/// @brief 验证分拍线时间逐分拍绘制并限制在单个分拍扩展区域内。
/// @return 时间、实例序号、独立颜色及向下半高扩展均正确时返回 true。
bool testBeatLineTimesRenderInsideEachSubdivision()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    configureAsciiFont(snapshot);
    snapshot.hasBeatmap         = true;
    config.beatLineTime.visible = true;
    config.beatLineTime.anchorY = 1.0f;
    config.beatLineTime.color   = { 0.2f, 0.7f, 0.9f, 0.8f };

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
    context.beatDivisor = 4;
    context.bpmEvents   = bpmEvents;
    context.scrollCache = &cache;
    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, context, config);

    const auto instance = std::find_if(
        snapshot.canvasComponentInstances.begin(),
        snapshot.canvasComponentInstances.end(),
        [](const auto& candidate) {
            return candidate.type ==
                       MMM::Config::CanvasComponentType::BeatLineTime &&
                   candidate.instanceIndex == 9;
        });
    if ( instance == snapshot.canvasComponentInstances.end() ) {
        XERROR("Beat line time at 1.000 seconds was not rendered");
        return false;
    }

    constexpr float rawRegionTop    = 237.5f;
    constexpr float rawRegionBottom = 300.0f;
    const float     fontHeight =
        config.beatLineTime.fontSizeRatio * context.viewportHeight;
    const auto selection = MMM::Common::selectAsciiFont(
        snapshot.asciiFontAtlasMetrics, fontHeight);
    const auto text =
        MMM::Logic::System::CanvasComponentRenderSystem::formatJudgmentLineTime(
            1.0);
    if ( !selection ) {
        XERROR("Beat line time did not select an ASCII font");
        return false;
    }
    const auto textSize = MMM::Common::measureAsciiText(
        *selection.metrics, text.data(), fontHeight);
    const float     expectedOffset = textSize.height * 0.5f;
    const float     contentCenterY = (instance->top + instance->bottom) * 0.5f;
    constexpr float epsilon        = 1e-4f;
    if ( std::abs(instance->regionTop - rawRegionTop) > epsilon ||
         std::abs(instance->regionBottom - (rawRegionBottom + expectedOffset)) >
             epsilon ||
         std::abs(contentCenterY - rawRegionBottom) > epsilon ||
         std::abs((instance->right - instance->left) - textSize.width) >
             epsilon ) {
        XERROR("Beat line time escaped its subdivision layout region");
        return false;
    }

    MMM::Logic::RenderSnapshot stretchedSnapshot;
    configureAsciiFont(stretchedSnapshot);
    stretchedSnapshot.hasBeatmap  = true;
    auto stretchedContext         = context;
    stretchedContext.renderScaleY = 5.0f;
    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &stretchedSnapshot, stretchedContext, config);
    const auto stretchedInstance = std::find_if(
        stretchedSnapshot.canvasComponentInstances.begin(),
        stretchedSnapshot.canvasComponentInstances.end(),
        [](const auto& candidate) {
            return candidate.type ==
                       MMM::Config::CanvasComponentType::BeatLineTime &&
                   candidate.instanceIndex == 9;
        });
    if ( stretchedInstance ==
             stretchedSnapshot.canvasComponentInstances.end() ||
         std::abs((stretchedInstance->right - stretchedInstance->left) -
                  (instance->right - instance->left)) > epsilon ||
         std::abs((stretchedInstance->bottom - stretchedInstance->top) -
                  (instance->bottom - instance->top)) > epsilon ) {
        XERROR("Beat line time size changed with the visual grid height");
        return false;
    }

    for ( const auto& vertex : snapshot.vertices ) {
        if ( std::abs(vertex.color.r - config.beatLineTime.color[0]) >
                 epsilon ||
             std::abs(vertex.color.g - config.beatLineTime.color[1]) >
                 epsilon ||
             std::abs(vertex.color.b - config.beatLineTime.color[2]) >
                 epsilon ||
             std::abs(vertex.color.a - config.beatLineTime.color[3]) >
                 epsilon ) {
            XERROR("Beat line time did not use its independent color");
            return false;
        }
    }
    return !snapshot.vertices.empty() && !snapshot.overlayCmds.empty();
}

/// @brief 验证逐轨及总 KPS 实例，并覆盖单轨 KPS 位于轨道外的场景。
/// @return 实例数量、索引、全画布布局与裁剪以及颜色均正确时返回 true。
bool testKpsRendersPerTrackAndTotal()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    configureAsciiFont(snapshot);
    snapshot.hasBeatmap       = true;
    snapshot.isPlaying        = true;
    config.beatNumber.visible = true;
    config.kps.visible        = true;
    config.kps.anchorY        = 0.05f;
    config.kps.color          = { 0.3f, 0.8f, 0.9f, 0.75f };
    auto& secondTrack         = config.editablePlacement(
        MMM::Config::CanvasComponentType::Kps, 1, 3, 0.2f, 0.8f);
    secondTrack.anchorX       = 0.1f;
    secondTrack.anchorY       = 0.05f;
    secondTrack.fontSizeRatio = 0.04f;

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

    const std::array<std::uint32_t, 3> kps{ 2U, 4U, 6U };
    auto                               context = makeRenderContext(1.0);
    context.trackCount                         = 3;
    context.trackLeft                          = 0.2f;
    context.trackRight                         = 0.8f;
    context.trackKps                           = kps;
    context.bpmEvents                          = bpmEvents;
    context.scrollCache                        = &cache;
    context.visibleTop                         = 100.0f;
    context.visibleBottom                      = 500.0f;
    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, context, config);

    const auto kpsInstanceCount = std::count_if(
        snapshot.canvasComponentInstances.begin(),
        snapshot.canvasComponentInstances.end(),
        [](const auto& instance) {
            return instance.type == MMM::Config::CanvasComponentType::Kps;
        });
    if ( kpsInstanceCount != 4 ) {
        XERROR("KPS component did not produce three tracks and one total");
        return false;
    }
    const auto findInstance = [&snapshot](std::int64_t instanceIndex) {
        return std::find_if(
            snapshot.canvasComponentInstances.begin(),
            snapshot.canvasComponentInstances.end(),
            [instanceIndex](const auto& instance) {
                return instance.type == MMM::Config::CanvasComponentType::Kps &&
                       instance.instanceIndex == instanceIndex;
            });
    };
    const auto total  = findInstance(MMM::Config::KPS_TOTAL_INSTANCE_INDEX);
    const auto first  = findInstance(0);
    const auto second = findInstance(1);
    const auto third  = findInstance(2);
    if ( total == snapshot.canvasComponentInstances.end() ||
         first == snapshot.canvasComponentInstances.end() ||
         second == snapshot.canvasComponentInstances.end() ||
         third == snapshot.canvasComponentInstances.end() ) {
        XERROR("KPS component instance indices were not stable");
        return false;
    }

    constexpr float epsilon       = 1e-4f;
    const float     secondCenterX = (second->left + second->right) * 0.5f;
    const float     secondCenterY = (second->top + second->bottom) * 0.5f;
    if ( std::abs(secondCenterX - 80.0f) > epsilon ||
         std::abs(secondCenterY - 30.0f) > epsilon ||
         second->right >= context.trackLeft * context.viewportWidth ||
         second->bottom >= context.visibleTop ||
         total->bottom >= context.visibleTop || second->regionLeft != 0.0f ||
         second->regionTop != 0.0f || second->regionRight != 800.0f ||
         second->regionBottom != 600.0f ) {
        XERROR("Per-track KPS override was constrained to the track region");
        return false;
    }
    if ( snapshot.overlayCmds.empty() ) {
        XERROR("KPS component did not produce an overlay command");
        return false;
    }
    const auto& kpsCommand = snapshot.overlayCmds.back();
    if ( kpsCommand.scissor.offset.x != 0 || kpsCommand.scissor.offset.y != 0 ||
         kpsCommand.scissor.extent.width != 800U ||
         kpsCommand.scissor.extent.height != 600U ) {
        XERROR("KPS inherited the preceding beat component scissor");
        return false;
    }
    const auto commandIndexEnd = kpsCommand.indexOffset + kpsCommand.indexCount;
    for ( std::uint32_t index = kpsCommand.indexOffset; index < commandIndexEnd;
          ++index ) {
        const auto& vertex = snapshot.vertices[snapshot.indices[index]];
        if ( std::abs(vertex.color.r - config.kps.color[0]) > epsilon ||
             std::abs(vertex.color.g - config.kps.color[1]) > epsilon ||
             std::abs(vertex.color.b - config.kps.color[2]) > epsilon ||
             std::abs(vertex.color.a - config.kps.color[3]) > epsilon ) {
            XERROR("KPS instances did not share the configured group color");
            return false;
        }
    }
    using System = MMM::Logic::System::CanvasComponentRenderSystem;
    return std::string(System::formatTrackKps(1, 4U).data()) == "K2 4 KPS" &&
           std::string(System::formatTotalKps(12U).data()) == "TOTAL 12 KPS";
}

/// @brief 验证轨道宽度不足时依次隐藏轨道编号与 KPS 后缀。
/// @return 三条轨道分别使用完整、无编号和纯数值文本时返回 true。
bool testKpsTextAdaptsToTrackWidth()
{
    MMM::Logic::RenderSnapshot               snapshot;
    MMM::Config::CanvasComponentLayoutConfig config;
    configureAsciiFont(snapshot);
    snapshot.hasBeatmap = true;
    snapshot.isPlaying  = true;
    config.kps.visible  = true;

    constexpr std::int32_t              trackCount = 3;
    constexpr float                     trackLeft  = 0.2f;
    constexpr float                     trackRight = 0.8f;
    const std::array<float, trackCount> fontSizeRatios{ 0.035f, 0.06f, 0.1f };
    for ( std::int32_t trackIndex = 0; trackIndex < trackCount; ++trackIndex ) {
        auto& placement =
            config.editablePlacement(MMM::Config::CanvasComponentType::Kps,
                                     trackIndex,
                                     trackCount,
                                     trackLeft,
                                     trackRight);
        placement.fontSizeRatio =
            fontSizeRatios[static_cast<std::size_t>(trackIndex)];
    }

    const std::array<std::uint32_t, trackCount> kps{ 1U, 2U, 3U };
    auto context       = makeRenderContext(1.0);
    context.trackCount = trackCount;
    context.trackLeft  = trackLeft;
    context.trackRight = trackRight;
    context.trackKps   = kps;
    MMM::Logic::System::CanvasComponentRenderSystem::render(
        &snapshot, context, config);

    const std::array<const char*, trackCount> expectedTexts{ "K1 1 KPS",
                                                             "2 KPS",
                                                             "3" };
    constexpr float                           epsilon = 1e-4f;
    for ( std::int32_t trackIndex = 0; trackIndex < trackCount; ++trackIndex ) {
        const auto instance =
            std::find_if(snapshot.canvasComponentInstances.begin(),
                         snapshot.canvasComponentInstances.end(),
                         [trackIndex](const auto& candidate) {
                             return candidate.type ==
                                        MMM::Config::CanvasComponentType::Kps &&
                                    candidate.instanceIndex == trackIndex;
                         });
        if ( instance == snapshot.canvasComponentInstances.end() ) {
            XERROR("Adaptive KPS text did not render track {}", trackIndex);
            return false;
        }

        const float fontPixelHeight =
            fontSizeRatios[static_cast<std::size_t>(trackIndex)] *
            context.viewportHeight;
        const auto selection = MMM::Common::selectAsciiFont(
            snapshot.asciiFontAtlasMetrics, fontPixelHeight);
        if ( !selection ) {
            XERROR("Adaptive KPS text did not select an ASCII font");
            return false;
        }
        const float expectedWidth =
            MMM::Common::measureAsciiText(
                *selection.metrics,
                expectedTexts[static_cast<std::size_t>(trackIndex)],
                fontPixelHeight)
                .width;
        if ( std::abs((instance->right - instance->left) - expectedWidth) >
             epsilon ) {
            XERROR("Adaptive KPS text selected the wrong compact level");
            return false;
        }
    }
    return true;
}

/// @brief 验证 HitEffect 消费事件形成逐轨一秒滚动 KPS。
/// @return 新事件、过期事件和清空路径的计数均正确时返回 true。
bool testHitEffectKpsRollingWindow()
{
    using HitSystem      = MMM::Logic::System::HitFXSystem;
    const auto makeEvent = [](double timestamp, int trackIndex) {
        return HitSystem::HitEvent{ timestamp,
                                    MMM::NoteType::NOTE,
                                    HitSystem::HitEvent::Role::None,
                                    1,
                                    trackIndex,
                                    0,
                                    0.0,
                                    false };
    };

    HitSystem                 system;
    MMM::Config::EditorConfig config;
    config.visual.enableHitEffects             = false;
    config.visual.canvasComponents.kps.visible = true;
    auto offsetEvent                           = makeEvent(0.8, 0);
    offsetEvent.type                           = MMM::NoteType::FLICK;
    offsetEvent.trackOffset                    = 2;
    system.update(
        1.0, { makeEvent(0.4, 0), makeEvent(1.0, 0), offsetEvent }, 3, config);
    auto kps = system.trackKps();
    if ( kps.size() != 3U || kps[0] != 2U || kps[1] != 0U || kps[2] != 1U ) {
        XERROR("Initial per-track KPS counts were incorrect");
        return false;
    }

    system.update(1.5, { makeEvent(1.5, 1) }, 3, config);
    kps = system.trackKps();
    if ( kps[0] != 1U || kps[1] != 1U || kps[2] != 1U ) {
        XERROR("KPS rolling window did not expire the oldest event");
        return false;
    }

    system.update(2.01, {}, 3, config);
    kps = system.trackKps();
    if ( kps[0] != 0U || kps[1] != 1U || kps[2] != 0U ) {
        XERROR("KPS rolling window did not retain only the last second");
        return false;
    }

    system.clearActiveEffects();
    kps = system.trackKps();
    return kps.size() == 3U && kps[0] == 0U && kps[1] == 0U && kps[2] == 0U;
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
           std::string(System::formatBeatNumber(12345).data()) == "#12345" &&
           std::string(System::formatBeatNumber(-5).data()) == "#0";
}

}  // namespace

/// @brief 运行画布组件最终覆盖层回归测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testHiddenComponentDoesNotRender() &&
                   testVisibleComponentRendersInOverlay() &&
                   testJudgmentLineTimeDoesNotRenderWithoutBeatmap() &&
                   testBeatNumbersRenderInsideEachBeat() &&
                   testBeatNumberRendersUntilLayoutViewportExit() &&
                   testBeatNumberLayoutRegionCentersOnBeatHead() &&
                   testBeatLineTimesRenderInsideEachSubdivision() &&
                   testKpsRendersPerTrackAndTotal() &&
                   testKpsTextAdaptsToTrackWidth() &&
                   testHitEffectKpsRollingWindow() &&
                   testJudgmentLineTimeFormatting()
               ? 0
               : 1;
}
