#include "logic/ecs/system/BackgroundRenderSystem.h"
#include "logic/ecs/system/render/Batcher.h"

namespace MMM::Logic::System
{

void BackgroundRenderSystem::render(Batcher& batcher, float viewportWidth,
                                    float                       viewportHeight,
                                    const Config::EditorConfig& config,
                                    const RenderSnapshot*       snapshot)
{
    if ( !snapshot->hasBeatmap ) return;

    const float darkenRatio = config.visual.background.darken_ratio;
    glm::vec4   color(1.0f - darkenRatio,
                      1.0f - darkenRatio,
                      1.0f - darkenRatio,
                      config.visual.background.opaque_ratio);

    if ( snapshot->backgroundPath.empty() ) {
        // 无背景资源时使用白色图集纹理生成固定覆盖层，确保暗化与透明度仍参与混合。
        batcher.setTexture(TextureID::None);
        batcher.pushQuad(
            0, viewportHeight, viewportWidth, viewportHeight, color);
        return;
    }

    batcher.setTexture(TextureID::Background);

    batcher.pushFilledQuad(0,
                           viewportHeight,
                           viewportWidth,
                           viewportHeight,
                           snapshot->bgSize,
                           config.visual.background.fillMode,
                           color);
}

}  // namespace MMM::Logic::System
