#include "config/EditorConfig.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/ScrollCache.h"

#include <entt/entt.hpp>

namespace
{

/// @brief 验证无谱面占位 Logo 使用完整画布裁剪区域。
/// @return 找到 Logo 绘制命令且其 scissor 覆盖完整视口时返回 true。
bool testLogoUsesFullViewportScissor()
{
    constexpr float            VIEWPORT_WIDTH  = 1000.0f;
    constexpr float            VIEWPORT_HEIGHT = 700.0f;
    entt::registry             noteRegistry;
    entt::registry             sampleRegistry;
    entt::registry             timelineRegistry;
    MMM::Logic::RenderSnapshot snapshot;
    MMM::Config::EditorConfig  config;

    timelineRegistry.ctx().emplace<MMM::Logic::System::ScrollCache>();
    snapshot.hasBeatmap = false;
    config.visual.beatLineDisplayMode =
        MMM::Config::BeatLineDisplayMode::Hidden;
    config.visual.trackLayout.left  = 0.10f;
    config.visual.trackLayout.right = 0.60f;

    MMM::Logic::System::NoteRenderSystem::generateSnapshot(
        noteRegistry,
        sampleRegistry,
        {},
        {},
        timelineRegistry,
        {},
        &snapshot,
        "Basic2DCanvas",
        0.0,
        VIEWPORT_WIDTH,
        VIEWPORT_HEIGHT,
        VIEWPORT_HEIGHT * 0.8f,
        4,
        0,
        config);

    for ( const auto& command : snapshot.cmds ) {
        if ( command.customTextureId !=
             static_cast<std::uint32_t>(MMM::Logic::TextureID::Logo) ) {
            continue;
        }
        const auto& scissor = command.scissor;
        return scissor.x == 0 && scissor.y == 0 &&
               scissor.width == static_cast<std::uint32_t>(VIEWPORT_WIDTH) &&
               scissor.height == static_cast<std::uint32_t>(VIEWPORT_HEIGHT);
    }

    XERROR("LogoPlaceholderScissorTest: missing Logo draw command");
    return false;
}

}  // namespace

/// @brief 无谱面 Logo 裁剪回归测试入口。
/// @return 全部断言通过时返回 0。
int main()
{
    return testLogoUsesFullViewportScissor() ? 0 : 1;
}
