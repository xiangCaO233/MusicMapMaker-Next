#include "logic/ecs/system/BackgroundSpectrumRenderSystem.h"

#include "logic/ecs/system/render/Batcher.h"

#include <algorithm>

namespace MMM::Logic::System
{

void BackgroundSpectrumRenderSystem::render(
    Batcher& batcher, float viewportWidth, float viewportHeight,
    const Config::BackgroundSpectrumConfig& config,
    const Audio::BackgroundSpectrumLevels&  levels)
{
    if ( !config.enabled || viewportWidth <= 0.0F || viewportHeight <= 0.0F ||
         config.opacity <= 0.0F || levels.bandCount == 0U ) {
        return;
    }

    const std::size_t bandCount =
        std::min(levels.bandCount,
                 static_cast<std::size_t>(
                     std::clamp(config.bandCount,
                                Config::BACKGROUND_SPECTRUM_MIN_BANDS,
                                Config::BACKGROUND_SPECTRUM_MAX_BANDS)));
    if ( bandCount == 0U ) return;

    const float width =
        viewportWidth * std::clamp(config.widthRatio, 0.10F, 1.0F);
    const float halfWidth = width * 0.5F;
    const float centerX   = viewportWidth * 0.5F;
    const float slotWidth = halfWidth / static_cast<float>(bandCount);
    const float barWidth  = std::max(1.0F, slotWidth * 0.72F);
    const float barInset  = (slotWidth - barWidth) * 0.5F;
    const float baseline =
        viewportHeight * std::clamp(config.baselineRatio, 0.05F, 1.0F);
    const float maxHeight = std::min(
        viewportHeight * std::clamp(config.heightRatio, 0.05F, 1.0F), baseline);
    const float     opacity = std::clamp(config.opacity, 0.0F, 1.0F);
    const glm::vec4 leftColor{ 0.18F, 0.72F, 1.0F, opacity };
    const glm::vec4 rightColor{ 1.0F, 0.30F, 0.72F, opacity };

    batcher.setTexture(TextureID::None);
    for ( std::size_t band = 0U; band < bandCount; ++band ) {
        const float leftLevel = std::clamp(levels.left[band], 0.0F, 1.0F);
        if ( leftLevel > 0.001F ) {
            const float x =
                centerX - static_cast<float>(band + 1U) * slotWidth + barInset;
            batcher.pushQuad(
                x, baseline, barWidth, leftLevel * maxHeight, leftColor);
        }

        const float rightLevel = std::clamp(levels.right[band], 0.0F, 1.0F);
        if ( rightLevel > 0.001F ) {
            const float x =
                centerX + static_cast<float>(band) * slotWidth + barInset;
            batcher.pushQuad(
                x, baseline, barWidth, rightLevel * maxHeight, rightColor);
        }
    }
}

}  // namespace MMM::Logic::System
