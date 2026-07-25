#include "logic/ecs/system/BackgroundRenderSystem.h"

#include "audio/AudioManager.h"
#include "logic/ecs/system/BackgroundSpectrumRenderSystem.h"
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
    } else {
        batcher.setTexture(TextureID::Background);

        batcher.pushFilledQuad(0,
                               viewportHeight,
                               viewportWidth,
                               viewportHeight,
                               snapshot->bgSize,
                               config.visual.background.fillMode,
                               color);
    }

    const auto& spectrumConfig = config.visual.background.spectrum;
    const auto& spectrumPlacement =
        config.visual.canvasComponents.backgroundSpectrum;
    if ( spectrumPlacement.visible ) {
        const auto& levels =
            Audio::AudioManager::instance().updateBackgroundSpectrum(
                static_cast<std::size_t>(spectrumConfig.bandCount),
                spectrumConfig.includeHitEffects);
        BackgroundSpectrumRenderSystem::render(batcher,
                                               viewportWidth,
                                               viewportHeight,
                                               spectrumConfig,
                                               spectrumPlacement,
                                               levels);
    }
}

}  // namespace MMM::Logic::System
