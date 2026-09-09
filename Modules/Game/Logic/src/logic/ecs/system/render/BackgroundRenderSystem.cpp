#include "logic/ecs/system/BackgroundRenderSystem.h"

#include "audio/AudioManager.h"
#include "logic/ecs/system/BackgroundSpectrumRenderSystem.h"
#include "logic/ecs/system/render/Batcher.h"

namespace MMM::Logic::System
{

/// @brief 在画布底层绘制背景，再叠加启用的频谱组件。
/// @param batcher 接收背景与频谱图元的当前批次。
/// @param viewportWidth 当前画布宽度，不是背景图的原始宽度。
/// @param viewportHeight 当前画布高度，同时作为底部坐标。
/// @param config 提供暗化、透明度、填充方式与频谱布局。
/// @param snapshot 调用方保证非空且在本次绘制期间有效的只读快照。
/// @warning 每帧调用；不得在这里加载背景文件或等待音频解码。
void BackgroundRenderSystem::render(Batcher& batcher, float viewportWidth,
                                    float                       viewportHeight,
                                    const Config::EditorConfig& config,
                                    const RenderSnapshot*       snapshot)
{
    // 空会话既不铺背景，也不请求频谱；占位内容由其他绘制流程负责。
    if ( !snapshot->hasBeatmap ) return;

    // 暗化作用于 RGB 乘色，透明度单独交给混合阶段，避免两者互相替代。
    // 此处使用配置值本身，不额外改变配置层约定的取值范围。
    const float darkenRatio = config.visual.background.darken_ratio;
    glm::vec4   color(1.0f - darkenRatio,
                    1.0f - darkenRatio,
                    1.0f - darkenRatio,
                    config.visual.background.opaque_ratio);

    if ( snapshot->backgroundPath.empty() ) {
        // 无背景资源时使用白色图集纹理生成固定覆盖层，确保暗化与透明度仍参与混合。
        // 显式切换纹理，防止继承上一批图元所绑定的背景资源。
        batcher.setTexture(TextureID::None);
        // 覆盖层使用整个视口尺寸，不依赖缺失资源的 bgSize。
        batcher.pushQuad(
            0, viewportHeight, viewportWidth, viewportHeight, color);
    } else {
        // 路径在此只区分是否配置资源；实际纹理由资源管理侧准备。
        batcher.setTexture(TextureID::Background);

        // 快照中的原图尺寸与画布尺寸一起参与填充计算。
        // 将裁剪或留白交给统一填充逻辑，不在此处直接拉伸原始图像。
        batcher.pushFilledQuad(0,
                               viewportHeight,
                               viewportWidth,
                               viewportHeight,
                               snapshot->bgSize,
                               config.visual.background.fillMode,
                               color);
    }

    // 频谱外观与画布组件布局分开读取，组件可见性才是绘制入口开关。
    const auto& spectrumConfig = config.visual.background.spectrum;
    const auto& spectrumPlacement =
        config.visual.canvasComponents.backgroundSpectrum;
    if ( spectrumPlacement.visible ) {
        // 仅在组件可见时更新频谱；频带数及是否混入打击音取自同一帧配置。
        // 借用音频管理器返回的数据，在本次调用内立即消费，不复制所有权。
        const auto& levels =
            Audio::AudioManager::instance().updateBackgroundSpectrum(
                static_cast<std::size_t>(spectrumConfig.bandCount),
                spectrumConfig.includeHitEffects);
        // 频谱在背景图元之后提交，确保作为覆盖层显示。
        // 独立系统负责频谱几何，背景系统只负责绘制顺序和数据接入。
        BackgroundSpectrumRenderSystem::render(batcher,
                                               viewportWidth,
                                               viewportHeight,
                                               spectrumConfig,
                                               spectrumPlacement,
                                               levels);
    }
}

}  // namespace MMM::Logic::System
