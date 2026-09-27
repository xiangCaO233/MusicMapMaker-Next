#include "logic/ecs/system/BackgroundSpectrumRenderSystem.h"

#include "common/CanvasComponentLayout.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/system/render/Batcher.h"

#include <algorithm>

namespace MMM::Logic::System
{

/// @brief 将已计算的双声道频带值转换为背景覆盖层柱形图元。
/// @param batcher 接收图元，并在存在快照时登记组件交互边界。
/// @param viewportWidth 组件布局所在画布的宽度。
/// @param viewportHeight 组件布局所在画布的高度。
/// @param config 频带数量、尺寸比例、颜色和透明度配置。
/// @param placement 组件锚点、偏移、缩放及可见性。
/// @param levels 音频侧生成的归一化频带数据，本函数不执行频谱分析。
/// @warning 快照生成热路径：只处理有限频带和几何，不得加入资源读取或阻塞等待。
/// @pre levels.bandCount 不超过左右声道存储的容量，由频谱生产者保证。
/// @pre 视口尺寸和频带幅度为有限值；限幅不用于修复 NaN 输入。
void BackgroundSpectrumRenderSystem::render(
    Batcher& batcher, float viewportWidth, float viewportHeight,
    const Config::BackgroundSpectrumConfig& config,
    const Config::CanvasComponentPlacement& placement,
    const Audio::BackgroundSpectrumLevels&  levels)
{
    // 隐藏组件不登记交互区域；无有效视口时也不参与布局计算。
    if ( !placement.visible || viewportWidth <= 0.0F ||
         viewportHeight <= 0.0F ) {
        return;
    }

    // 先规范布局配置，后续尺寸和锚点使用同一份局部值，不改写用户配置。
    const auto sanitizedPlacement = sanitizeCanvasComponentPlacement(placement);
    // 复用组件通用的字号比例作为整体缩放，以默认频谱比例为单位尺度。
    // 频谱没有文字，比例在这里作用于图形尺寸而非字体。
    // 两个方向共享缩放系数，但各自受视口限制，不保证最终等比缩放。
    const float scale =
        sanitizedPlacement.fontSizeRatio /
        Config::DEFAULT_BACKGROUND_SPECTRUM_PLACEMENT.fontSizeRatio;
    // 宽高比例分别限幅，再应用组件缩放；最终尺寸限制在视口内。
    // 最小尺寸用于避免正常画布中的组件缩放到零而无法交互。
    const float contentWidth = std::clamp(
        viewportWidth * std::clamp(config.widthRatio, 0.10F, 1.0F) * scale,
        1.0F,
        viewportWidth);
    const float contentHeight = std::clamp(
        viewportHeight * std::clamp(config.heightRatio, 0.05F, 1.0F) * scale,
        1.0F,
        viewportHeight);
    // 布局区域始终为整个画布，组件边界则由锚点和内容尺寸共同决定。
    const CanvasComponentBounds layoutRegion{
        0.0F, 0.0F, viewportWidth, viewportHeight
    };
    const auto bounds = canvasComponentBoundsInRegion(
        sanitizedPlacement, layoutRegion, contentWidth, contentHeight);
    // 即使暂时没有声音或透明度为零，布局编辑仍需要组件的可选择区域。
    // 因而登记放在频谱数据和透明度检查之前，而不是依赖实际柱形范围。
    // 没有快照时只省略交互信息登记，后续图元提交仍然可以执行。
    if ( batcher.snapshot ) {
        // 同时保存组件边界和布局参考区域，供统一组件交互使用。
        batcher.snapshot->canvasComponentInstances.push_back(
            { Config::CanvasComponentType::BackgroundSpectrum,
              0,
              bounds.left,
              bounds.top,
              bounds.right,
              bounds.bottom,
              layoutRegion.left,
              layoutRegion.top,
              layoutRegion.right,
              layoutRegion.bottom });
    }

    // 不可见图元不必继续构造，但上面的布局信息已经保留。
    if ( config.opacity <= 0.0F || levels.bandCount == 0U ) return;

    // 实际绘制数量取生产者数据与配置上限的较小值，避免读取尚未提供的频带。
    // 这里只截取已有频带，不重新采样；频带计算属于音频侧职责。
    const std::size_t bandCount =
        std::min(levels.bandCount,
                 static_cast<std::size_t>(
                     std::clamp(config.bandCount,
                                Config::BACKGROUND_SPECTRUM_MIN_BANDS,
                                Config::BACKGROUND_SPECTRUM_MAX_BANDS)));
    if ( bandCount == 0U ) return;

    // 左右声道各占组件的一半，同一频带分别向中心两侧镜像排列。
    // 每侧独立使用完整 bandCount，不能将总频带数再次除以二。
    const float halfWidth = bounds.width() * 0.5F;
    const float centerX   = (bounds.left + bounds.right) * 0.5F;
    const float slotWidth = halfWidth / static_cast<float>(bandCount);
    // 柱宽保留槽间空隙，同时至少占一个坐标单位；极窄槽可能出现柱形重叠。
    const float barWidth = std::max(1.0F, slotWidth * 0.72F);
    // 内缩可以为负：最小柱宽超过槽宽时，仍使柱形中心与槽中心重合。
    const float barInset = (slotWidth - barWidth) * 0.5F;
    // 所有柱形从组件底边向上生长，幅度只改变高度，不改变基线。
    const float baseline  = bounds.bottom;
    const float maxHeight = bounds.height();
    const float opacity   = std::clamp(config.opacity, 0.0F, 1.0F);
    /// @brief 将单声道颜色限幅，并把组件透明度乘入其自身透明度。
    // 在频带循环外准备颜色，避免每根柱形重复处理相同配置。
    const auto makeBarColor = [opacity](const std::array<float, 4>& stored) {
        // RGB 不乘组件透明度，交由批处理器的颜色混合规则处理。
        return glm::vec4{ std::clamp(stored[0], 0.0F, 1.0F),
                          std::clamp(stored[1], 0.0F, 1.0F),
                          std::clamp(stored[2], 0.0F, 1.0F),
                          std::clamp(stored[3], 0.0F, 1.0F) * opacity };
    };
    const glm::vec4 leftColor  = makeBarColor(config.leftBarColor);
    const glm::vec4 rightColor = makeBarColor(config.rightBarColor);

    // 柱形使用纯色图元，不继承先前背景图的纹理状态。
    batcher.setTexture(TextureID::None);
    // 不保存上一帧幅度；平滑由频谱数据提供方负责，绘制仅反映当前值。
    for ( std::size_t band = 0U; band < bandCount; ++band ) {
        // 幅度限制在组件高度内；接近零的柱形直接跳过以减少无效图元。
        const float leftLevel = std::clamp(levels.left[band], 0.0F, 1.0F);
        if ( leftLevel > 0.001F ) {
            // 左侧从中心前一个槽开始，频带索引越大越向左延伸。
            const float x =
                centerX - static_cast<float>(band + 1U) * slotWidth + barInset;
            batcher.pushQuad(
                x, baseline, barWidth, leftLevel * maxHeight, leftColor);
        }

        // 双声道分别判断，某侧静音不会抑制另一侧的柱形。
        const float rightLevel = std::clamp(levels.right[band], 0.0F, 1.0F);
        if ( rightLevel > 0.001F ) {
            // 右侧首槽始于中心；相同内缩保持两侧柱形与槽位对称。
            const float x =
                centerX + static_cast<float>(band) * slotWidth + barInset;
            batcher.pushQuad(
                x, baseline, barWidth, rightLevel * maxHeight, rightColor);
        }
    }
}

}  // namespace MMM::Logic::System
