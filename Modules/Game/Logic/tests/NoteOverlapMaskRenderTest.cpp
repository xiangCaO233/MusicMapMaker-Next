#include "config/EditorConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"

#include <entt/entt.hpp>
#include <glm/vec4.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace
{

constexpr float VIEWPORT_WIDTH  = 800.0F;
constexpr float VIEWPORT_HEIGHT = 600.0F;

/// @brief 为同轨同时间的两个 Tap 生成重叠遮罩快照。
/// @param snapshot 输出快照。
/// @param isPlaying 是否模拟播放中的渲染状态。
void renderOverlappingTaps(MMM::Logic::RenderSnapshot& snapshot, bool isPlaying)
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
    config.visual.trackLayout.left  = 0.1F;
    config.visual.trackLayout.right = 0.5F;
    config.visual.beatLineDisplayMode =
        MMM::Config::BeatLineDisplayMode::Hidden;

    auto& cache =
        timelineRegistry.ctx().emplace<MMM::Logic::System::ScrollCache>();
    cache.rebuild(timelineRegistry, config, nullptr);

    std::vector<entt::entity> sortedNotes;
    sortedNotes.reserve(2U);
    for ( std::size_t index = 0; index < 2U; ++index ) {
        const auto                entity = noteRegistry.create();
        MMM::Logic::NoteComponent note;
        note.m_timestamp  = 1.0;
        note.m_type       = MMM::NoteType::NOTE;
        note.m_trackIndex = 1;
        noteRegistry.emplace<MMM::Logic::NoteComponent>(entity,
                                                        std::move(note));
        noteRegistry.emplace<MMM::Logic::TransformComponent>(entity);
        sortedNotes.push_back(entity);
    }
    noteRegistry.ctx().emplace<const std::vector<entt::entity>*>(&sortedNotes);

    snapshot.hasBeatmap    = true;
    snapshot.isPlaying     = isPlaying;
    snapshot.playbackSpeed = 1.0;
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::None),
        glm::vec4{ 0.0F, 0.0F, 0.01F, 0.01F });
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::Note),
        glm::vec4{ 0.25F, 0.35F, 0.2F, 0.1F });

    MMM::Logic::System::NoteRenderSystem::generateSnapshot(noteRegistry,
                                                           sampleRegistry,
                                                           {},
                                                           {},
                                                           timelineRegistry,
                                                           {},
                                                           &snapshot,
                                                           "Basic2DCanvas",
                                                           1.0,
                                                           VIEWPORT_WIDTH,
                                                           VIEWPORT_HEIGHT,
                                                           500.0F,
                                                           4,
                                                           0,
                                                           4,
                                                           config,
                                                           VIEWPORT_HEIGHT);
}

/// @brief 为玩家区和草稿区各生成一个带相同自定义颜色的 Tap。
/// @param snapshot 输出快照。
void renderPlayerAndDraftTaps(MMM::Logic::RenderSnapshot& snapshot)
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
    config.visual.trackLayout.left  = 0.5F;
    config.visual.trackLayout.right = 0.9F;
    config.visual.beatLineDisplayMode =
        MMM::Config::BeatLineDisplayMode::Hidden;
    config.settings.enableDraftLanes = true;

    auto& cache =
        timelineRegistry.ctx().emplace<MMM::Logic::System::ScrollCache>();
    cache.rebuild(timelineRegistry, config, nullptr);

    constexpr glm::vec4       SHARED_CUSTOM_COLOR{ 0.12F, 0.34F, 0.56F, 1.0F };
    std::vector<entt::entity> sortedNotes;
    for ( const auto [track, isDraft] :
          { std::pair{ 0, false }, std::pair{ -4, true } } ) {
        const auto                entity = noteRegistry.create();
        MMM::Logic::NoteComponent note;
        note.m_timestamp        = 1.0;
        note.m_type             = MMM::NoteType::NOTE;
        note.m_trackIndex       = track;
        note.m_isDraft          = isDraft;
        note.m_customColors.tap = SHARED_CUSTOM_COLOR;
        noteRegistry.emplace<MMM::Logic::NoteComponent>(entity,
                                                        std::move(note));
        noteRegistry.emplace<MMM::Logic::TransformComponent>(entity);
        sortedNotes.push_back(entity);
    }
    noteRegistry.ctx().emplace<const std::vector<entt::entity>*>(&sortedNotes);

    snapshot.hasBeatmap = true;
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::None),
        glm::vec4{ 0.0F, 0.0F, 0.01F, 0.01F });
    snapshot.uvMap.emplace(
        static_cast<std::uint32_t>(MMM::Logic::TextureID::Note),
        glm::vec4{ 0.25F, 0.35F, 0.2F, 0.1F });

    MMM::Logic::System::NoteRenderSystem::generateSnapshot(noteRegistry,
                                                           sampleRegistry,
                                                           {},
                                                           {},
                                                           timelineRegistry,
                                                           {},
                                                           &snapshot,
                                                           "Basic2DCanvas",
                                                           1.0,
                                                           VIEWPORT_WIDTH,
                                                           VIEWPORT_HEIGHT,
                                                           500.0F,
                                                           4,
                                                           0,
                                                           4,
                                                           config,
                                                           VIEWPORT_HEIGHT);
}

/// @brief 判断颜色通道在小容差内相等。
/// @param color 画布顶点颜色。
/// @param expected 期望颜色。
/// @return 四个通道均匹配时返回 true。
bool sameColor(const MMM::Common::Render::CanvasColor& color,
               const MMM::Config::Color&               expected)
{
    constexpr float EPSILON = 1e-4F;
    return std::abs(color.r - expected.r) < EPSILON &&
           std::abs(color.g - expected.g) < EPSILON &&
           std::abs(color.b - expected.b) < EPSILON &&
           std::abs(color.a - expected.a) < EPSILON;
}

/// @brief 验证玩家自定义颜色不能覆盖草稿皮肤色。
/// @return 玩家与草稿 Tap 的实际顶点颜色分别匹配对应来源时返回 true。
bool testDraftTapUsesDedicatedSkinColor()
{
    MMM::Logic::RenderSnapshot snapshot;
    renderPlayerAndDraftTaps(snapshot);

    auto&      skin       = MMM::Config::SkinManager::instance();
    const auto draftColor = skin.getColor("draft_notes.note_tap");
    constexpr MMM::Config::Color PLAYER_CUSTOM_COLOR{
        0.12F, 0.34F, 0.56F, 1.0F
    };
    bool playerMatched = false;
    bool draftMatched  = false;
    for ( const auto& vertex : snapshot.vertices ) {
        if ( vertex.uv.u < 0.25F || vertex.uv.u > 0.45F ||
             vertex.uv.v < 0.35F || vertex.uv.v > 0.45F ) {
            continue;
        }
        if ( vertex.pos.x >= VIEWPORT_WIDTH * 0.5F ) {
            playerMatched |= sameColor(vertex.color, PLAYER_CUSTOM_COLOR);
        } else {
            draftMatched |= sameColor(vertex.color, draftColor);
        }
    }
    if ( !playerMatched || !draftMatched ) {
        XERROR("Draft and player Tap vertices did not use separated colors");
        return false;
    }
    return true;
}

/// @brief 验证播放状态不会抑制重叠键的顶层遮罩。
/// @return 静止与播放快照均生成等价遮罩及覆盖层命令时返回 true。
bool testOverlapMaskRemainsVisibleDuringPlayback()
{
    MMM::Logic::RenderSnapshot stopped;
    MMM::Logic::RenderSnapshot playing;
    renderOverlappingTaps(stopped, false);
    renderOverlappingTaps(playing, true);

    if ( stopped.overlapMasks.empty() || stopped.overlayCmds.empty() ) {
        XERROR("Stopped overlap fixture did not generate an overlay mask");
        return false;
    }
    if ( playing.overlapMasks.empty() || playing.overlayCmds.empty() ) {
        XERROR("Playback suppressed the overlap overlay mask");
        return false;
    }

    const auto&     stoppedMask = stopped.overlapMasks.front();
    const auto&     playingMask = playing.overlapMasks.front();
    constexpr float EPSILON     = 1e-4F;
    return stoppedMask.objectCount == 2 && playingMask.objectCount == 2 &&
           std::abs(stoppedMask.x - playingMask.x) < EPSILON &&
           std::abs(stoppedMask.y - playingMask.y) < EPSILON &&
           std::abs(stoppedMask.w - playingMask.w) < EPSILON &&
           std::abs(stoppedMask.h - playingMask.h) < EPSILON;
}

}  // namespace

/// @brief 运行草稿分色与重叠键遮罩渲染回归测试。
/// @return 全部测试通过时返回 0。
int main(int argc, char* argv[])
{
    if ( argc < 3 || !argv[1] || !argv[2] ) {
        XERROR("NoteOverlapMaskRenderTest requires skin and translation paths");
        return 1;
    }
    auto& skin = MMM::Config::SkinManager::instance();
    if ( !skin.loadSkin(argv[1], MMM::Config::utf8ToPath(argv[2])) ) {
        XERROR("NoteOverlapMaskRenderTest failed to load bundled skin");
        return 1;
    }
    return testDraftTapUsesDedicatedSkinColor() &&
                   testOverlapMaskRemainsVisibleDuringPlayback()
               ? 0
               : 1;
}
