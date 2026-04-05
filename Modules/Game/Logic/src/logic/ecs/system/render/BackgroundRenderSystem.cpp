#include "logic/ecs/system/BackgroundRenderSystem.h"
#include "Batcher.h"

namespace MMM::Logic::System
{

void BackgroundRenderSystem::render(Batcher& batcher, float viewportWidth,
                                    float                 viewportHeight,
                                    const EditorConfig&   config,
                                    const RenderSnapshot* snapshot)
{
    if ( snapshot->backgroundPath.empty() ) return;

    batcher.setTexture(TextureID::Background);

    glm::vec2 bgSize = snapshot->bgSize;

    // 背景暗化与透明度
    float     d = config.background.darken_ratio;
    glm::vec4 color(
        1.0f - d, 1.0f - d, 1.0f - d, config.background.opaque_ratio);

    // 调用 Batcher 的统一填充管线
    // y 使用 viewportHeight 因为 Batcher convention 是底边坐标向上画
    batcher.pushFilledQuad(0,
                           viewportHeight,
                           viewportWidth,
                           viewportHeight,
                           bgSize,
                           config.background.fillMode,
                           color);
}

}  // namespace MMM::Logic::System
