#include "logic/ecs/system/BackgroundRenderSystem.h"

#include "audio/BackgroundSpectrum.h"
#include "log/colorful-log.h"
#include "logic/ecs/system/BackgroundSpectrumRenderSystem.h"
#include "logic/ecs/system/render/Batcher.h"

#include <cmath>

namespace
{

/// @brief 使用小容差比较背景顶点数据。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-6f;
}

/// @brief 验证无背景资源时仍生成覆盖整个视口的暗化混合层。
/// @return 行为符合预期时返回 true。
bool testMissingBackgroundUsesFixedOverlay()
{
    MMM::Logic::RenderSnapshot snapshot;
    snapshot.hasBeatmap = true;
    snapshot.bgSize     = { 32.0f, 32.0f };
    snapshot.uvMap.emplace(static_cast<uint32_t>(MMM::Logic::TextureID::None),
                           glm::vec4{ 0.1f, 0.2f, 0.3f, 0.4f });

    MMM::Config::EditorConfig config;
    config.visual.background.fillMode = MMM::Config::BackgroundFillMode::Center;
    config.visual.background.darken_ratio = 0.25f;
    config.visual.background.opaque_ratio = 0.4f;

    MMM::Logic::System::Batcher batcher(&snapshot);
    MMM::Logic::System::BackgroundRenderSystem::render(
        batcher, 320.0f, 180.0f, config, &snapshot);
    batcher.flush();

    if ( snapshot.vertices.size() != 4 || snapshot.indices.size() != 6 ||
         snapshot.cmds.size() != 1 ||
         snapshot.cmds.front().customTextureId !=
             static_cast<uint32_t>(MMM::Logic::TextureID::None) ) {
        XERROR("Missing background did not generate a solid overlay draw call");
        return false;
    }

    const auto& topLeft     = snapshot.vertices[0];
    const auto& bottomRight = snapshot.vertices[2];
    if ( !near(topLeft.pos.x, 0.0f) || !near(topLeft.pos.y, 180.0f) ||
         !near(bottomRight.pos.x, 320.0f) || !near(bottomRight.pos.y, 0.0f) ) {
        XERROR("Missing background overlay did not cover the viewport");
        return false;
    }

    for ( const auto& vertex : snapshot.vertices ) {
        if ( !near(vertex.color.r, 0.75f) || !near(vertex.color.g, 0.75f) ||
             !near(vertex.color.b, 0.75f) || !near(vertex.color.a, 0.4f) ) {
            XERROR("Missing background overlay ignored darken or opacity");
            return false;
        }
    }

    return true;
}

/// @brief 验证存在背景资源时仍使用专用背景纹理。
/// @return 行为符合预期时返回 true。
bool testConfiguredBackgroundKeepsTexture()
{
    MMM::Logic::RenderSnapshot snapshot;
    snapshot.hasBeatmap     = true;
    snapshot.backgroundPath = "/project/background.png";
    snapshot.bgSize         = { 640.0f, 360.0f };

    MMM::Config::EditorConfig config;
    config.visual.background.fillMode =
        MMM::Config::BackgroundFillMode::Stretch;

    MMM::Logic::System::Batcher batcher(&snapshot);
    MMM::Logic::System::BackgroundRenderSystem::render(
        batcher, 320.0f, 180.0f, config, &snapshot);
    batcher.flush();

    if ( snapshot.cmds.size() != 1 ||
         snapshot.cmds.front().customTextureId !=
             static_cast<uint32_t>(MMM::Logic::TextureID::Background) ) {
        XERROR("Configured background stopped using its dedicated texture");
        return false;
    }
    return true;
}

/// @brief 验证未打开谱面时不生成背景或固定覆盖层。
/// @return 行为符合预期时返回 true。
bool testNoBeatmapSkipsBackgroundLayer()
{
    MMM::Logic::RenderSnapshot snapshot;
    MMM::Config::EditorConfig  config;

    MMM::Logic::System::Batcher batcher(&snapshot);
    MMM::Logic::System::BackgroundRenderSystem::render(
        batcher, 320.0f, 180.0f, config, &snapshot);
    batcher.flush();

    if ( !snapshot.vertices.empty() || !snapshot.indices.empty() ||
         !snapshot.cmds.empty() ) {
        XERROR("Background layer was generated without an open beatmap");
        return false;
    }
    return true;
}

/// @brief 验证左右声道由画布中心分别向两侧绘制且位于背景命令之后。
/// @return 立体声几何、透明度和图层顺序符合预期时返回 true。
bool testStereoSpectrumRendersAboveBackground()
{
    MMM::Logic::RenderSnapshot snapshot;
    snapshot.bgSize = { 320.0F, 180.0F };

    MMM::Config::BackgroundSpectrumConfig config;
    config.bandCount     = 10;
    config.widthRatio    = 1.0F;
    config.heightRatio   = 0.5F;
    config.baselineRatio = 0.8F;
    config.opacity       = 0.25F;

    MMM::Audio::BackgroundSpectrumLevels levels;
    levels.bandCount = 10U;
    levels.left[0]   = 0.5F;
    levels.right[0]  = 0.75F;

    MMM::Config::CanvasComponentPlacement placement =
        MMM::Config::DEFAULT_BACKGROUND_SPECTRUM_PLACEMENT;
    placement.visible = true;
    placement.anchorY = 0.55F;

    MMM::Logic::System::Batcher batcher(&snapshot);
    batcher.setTexture(MMM::Logic::TextureID::Background);
    batcher.pushFilledQuad(0.0F,
                           180.0F,
                           320.0F,
                           180.0F,
                           snapshot.bgSize,
                           MMM::Config::BackgroundFillMode::Stretch,
                           glm::vec4{ 1.0F });
    MMM::Logic::System::BackgroundSpectrumRenderSystem::render(
        batcher, 320.0F, 180.0F, config, placement, levels);
    batcher.flush();

    if ( snapshot.vertices.size() != 12U || snapshot.indices.size() != 18U ||
         snapshot.cmds.size() != 2U ||
         snapshot.canvasComponentInstances.size() != 1U ||
         snapshot.canvasComponentInstances.front().type !=
             MMM::Config::CanvasComponentType::BackgroundSpectrum ||
         snapshot.cmds[0].customTextureId !=
             static_cast<uint32_t>(MMM::Logic::TextureID::Background) ||
         snapshot.cmds[1].customTextureId !=
             static_cast<uint32_t>(MMM::Logic::TextureID::None) ) {
        XERROR("Stereo spectrum was not layered directly above background");
        return false;
    }

    const auto& componentBounds = snapshot.canvasComponentInstances.front();
    if ( !near(componentBounds.left, 0.0F) ||
         !near(componentBounds.top, 54.0F) ||
         !near(componentBounds.right, 320.0F) ||
         !near(componentBounds.bottom, 144.0F) ) {
        XERROR("Background spectrum did not publish editable canvas bounds");
        return false;
    }

    const float centerX         = 160.0F;
    const auto& leftBottomLeft  = snapshot.vertices[4];
    const auto& leftTopRight    = snapshot.vertices[6];
    const auto& rightBottomLeft = snapshot.vertices[8];
    const auto& rightTopRight   = snapshot.vertices[10];
    if ( leftTopRight.pos.x > centerX || rightBottomLeft.pos.x < centerX ||
         !near(leftBottomLeft.pos.y, 144.0F) ||
         !near(rightBottomLeft.pos.y, 144.0F) ||
         leftTopRight.pos.y >= leftBottomLeft.pos.y ||
         rightTopRight.pos.y >= rightBottomLeft.pos.y ||
         !near(leftBottomLeft.color.a, 0.25F) ||
         !near(rightBottomLeft.color.a, 0.25F) ) {
        XERROR("Stereo spectrum geometry ignored channel split or ratios");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行背景渲染系统回归测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testNoBeatmapSkipsBackgroundLayer() &&
                   testMissingBackgroundUsesFixedOverlay() &&
                   testConfiguredBackgroundKeepsTexture() &&
                   testStereoSpectrumRendersAboveBackground()
               ? 0
               : 1;
}
