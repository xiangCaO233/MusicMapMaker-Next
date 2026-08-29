#include "logic/ecs/system/NoteRenderSystem.h"

#include "common/AsciiFontData.h"
#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

constexpr float VIEWPORT_WIDTH  = 800.0F;
constexpr float VIEWPORT_HEIGHT = 600.0F;
constexpr float PLAYER_LEFT     = 80.0F;
constexpr float LANE_WIDTH      = 80.0F;
constexpr float LABEL_LEFT      = PLAYER_LEFT + 2.0F;
constexpr float LABEL_RIGHT     = PLAYER_LEFT + LANE_WIDTH - 2.0F;

/// @brief 使用小容差比较测试几何数值。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-4F;
}

/// @brief 为标签测试注入固定宽度 ASCII 字体度量和字形 UV。
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

/// @brief 记录一个标签字形顶点的屏幕位置与 UV。
struct GlyphVertex {
    float x;
    float y;
    float u;
    float v;
};

/// @brief 收集首个玩家轨道范围内的 ASCII 字形顶点。
/// @param snapshot 待检查快照。
/// @return 绑定音效标签使用的字形顶点。
std::vector<GlyphVertex> collectFirstLaneGlyphs(
    const MMM::Logic::RenderSnapshot& snapshot)
{
    std::vector<GlyphVertex> result;
    for ( const auto& vertex : snapshot.vertices ) {
        if ( vertex.uv.u < 0.6F || vertex.uv.v < 0.69F ||
             vertex.pos.x < LABEL_LEFT - 1e-4F ||
             vertex.pos.x > LABEL_RIGHT + 1e-4F ) {
            continue;
        }
        result.push_back(
            { vertex.pos.x, vertex.pos.y, vertex.uv.u, vertex.uv.v });
    }
    return result;
}

/// @brief 判断两组字形几何是否完全一致。
/// @param lhs 第一组几何。
/// @param rhs 第二组几何。
/// @return 位置与 UV 均一致时返回 true。
bool glyphGeometryEqual(const std::vector<GlyphVertex>& lhs,
                        const std::vector<GlyphVertex>& rhs)
{
    if ( lhs.size() != rhs.size() || lhs.empty() ) return false;
    for ( std::size_t index = 0; index < lhs.size(); ++index ) {
        if ( !near(lhs[index].x, rhs[index].x) ||
             !near(lhs[index].y, rhs[index].y) ||
             !near(lhs[index].u, rhs[index].u) ||
             !near(lhs[index].v, rhs[index].v) ) {
            return false;
        }
    }
    return true;
}

/// @brief 为一个绑定音效的玩家 Tap 生成画布快照。
/// @param snapshot 输出快照。
/// @param enabled 是否启用绑定音效标签。
/// @param cameraId 目标画布 ID。
/// @param snapshotSysTime 标签滚动使用的单调时钟秒数。
/// @param noteScaleX 物件横向缩放。
void renderBoundTap(MMM::Logic::RenderSnapshot& snapshot, bool enabled,
                    std::string_view cameraId, double snapshotSysTime,
                    float noteScaleX = 1.2F)
{
    entt::registry noteRegistry;
    entt::registry sampleRegistry;
    entt::registry timelineRegistry;

    const auto bpmEntity = timelineRegistry.create();
    timelineRegistry.emplace<MMM::Logic::TimelineComponent>(
        bpmEntity,
        MMM::Logic::TimelineComponent{
            .m_timestamp = 0.0,
            .m_effect    = MMM::TimingEffect::BPM,
            .m_value     = 120.0,
        });

    MMM::Config::EditorConfig config;
    config.visual.trackLayout.left      = 0.1F;
    config.visual.trackLayout.right     = 0.5F;
    config.visual.noteScaleX            = noteScaleX;
    config.visual.noteScaleY            = 1.0F;
    config.visual.showBoundSampleLabels = enabled;
    config.visual.beatLineDisplayMode =
        MMM::Config::BeatLineDisplayMode::Hidden;
    config.visual.previewConfig.drawBeatLines   = false;
    config.visual.previewConfig.drawTimingLines = false;

    auto& cache =
        timelineRegistry.ctx().emplace<MMM::Logic::System::ScrollCache>();
    cache.rebuild(timelineRegistry, config, nullptr);

    const auto                noteEntity = noteRegistry.create();
    MMM::Logic::NoteComponent note;
    note.m_timestamp     = 0.0;
    note.m_type          = MMM::NoteType::NOTE;
    note.m_trackIndex    = 0;
    note.m_sampleBinding = MMM::AudioSampleBinding{
        .m_audioResourceId = "very_long_bound_effect_resource_name.wav",
        .m_volume          = 0.75F,
    };
    noteRegistry.emplace<MMM::Logic::NoteComponent>(noteEntity,
                                                    std::move(note));
    noteRegistry.emplace<MMM::Logic::TransformComponent>(noteEntity);
    const std::vector<entt::entity> sortedNotes{ noteEntity };
    noteRegistry.ctx().emplace<const std::vector<entt::entity>*>(&sortedNotes);

    snapshot.hasBeatmap      = true;
    snapshot.snapshotSysTime = snapshotSysTime;
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::None),
        glm::vec4{ 0.0F, 0.0F, 0.01F, 0.01F });
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::Note),
        glm::vec4{ 0.25F, 0.35F, 0.2F, 0.1F });
    configureAsciiFont(snapshot);

    MMM::Logic::System::NoteRenderSystem::generateSnapshot(
        noteRegistry,
        sampleRegistry,
        {},
        {},
        timelineRegistry,
        {},
        &snapshot,
        std::string(cameraId),
        0.0,
        VIEWPORT_WIDTH,
        VIEWPORT_HEIGHT,
        300.0F,
        4,
        0,
        config,
        VIEWPORT_HEIGHT);
}

/// @brief 验证开关仅在主画布为绑定玩家物件生成标签。
/// @return 关闭和预览区均无标签，主画布开启时存在标签。
bool testBoundLabelToggleAndMainCanvasScope()
{
    MMM::Logic::RenderSnapshot disabled;
    renderBoundTap(disabled, false, "Basic2DCanvas", 0.0);
    if ( !collectFirstLaneGlyphs(disabled).empty() ) {
        XERROR("Disabled bound sample label still rendered player glyphs");
        return false;
    }

    MMM::Logic::RenderSnapshot enabled;
    renderBoundTap(enabled, true, "Basic2DCanvas", 0.0);
    if ( collectFirstLaneGlyphs(enabled).empty() ) {
        XERROR("Enabled bound sample label did not render player glyphs");
        return false;
    }

    MMM::Logic::RenderSnapshot preview;
    renderBoundTap(preview, true, "Preview", 0.0);
    for ( const auto& vertex : preview.vertices ) {
        if ( vertex.uv.u >= 0.6F && vertex.uv.v >= 0.69F ) {
            XERROR("Preview rendered a main-canvas bound sample label");
            return false;
        }
    }
    return true;
}

/// @brief 验证玩家物件标签复用采样标签的滚动与固定轨道裁剪语义。
/// @return 单调时钟推动文本且横向物件缩放不改变标签范围时返回 true。
bool testBoundLabelMarqueeAndFixedLaneWidth()
{
    MMM::Logic::RenderSnapshot start;
    MMM::Logic::RenderSnapshot later;
    renderBoundTap(start, true, "Basic2DCanvas", 0.0, 1.2F);
    renderBoundTap(later, true, "Basic2DCanvas", 2.25, 1.2F);
    const auto startGlyphs = collectFirstLaneGlyphs(start);
    const auto laterGlyphs = collectFirstLaneGlyphs(later);
    if ( startGlyphs.empty() || laterGlyphs.empty() ||
         glyphGeometryEqual(startGlyphs, laterGlyphs) ) {
        XERROR("Bound sample label did not scroll with the shared marquee");
        return false;
    }

    MMM::Logic::RenderSnapshot narrow;
    MMM::Logic::RenderSnapshot wide;
    renderBoundTap(narrow, true, "Basic2DCanvas", 2.25, 0.5F);
    renderBoundTap(wide, true, "Basic2DCanvas", 2.25, 3.0F);
    if ( !glyphGeometryEqual(collectFirstLaneGlyphs(narrow),
                             collectFirstLaneGlyphs(wide)) ) {
        XERROR("Bound sample label width followed horizontal object scale");
        return false;
    }

    for ( const auto* glyphs : { &startGlyphs, &laterGlyphs } ) {
        for ( const auto& vertex : *glyphs ) {
            if ( vertex.x < LABEL_LEFT - 1e-4F ||
                 vertex.x > LABEL_RIGHT + 1e-4F ) {
                XERROR("Bound sample label escaped its player lane");
                return false;
            }
        }
    }
    return true;
}

}  // namespace

/// @brief 运行玩家物件绑定音效标签渲染回归测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testBoundLabelToggleAndMainCanvasScope() &&
                   testBoundLabelMarqueeAndFixedLaneWidth()
               ? 0
               : 1;
}
