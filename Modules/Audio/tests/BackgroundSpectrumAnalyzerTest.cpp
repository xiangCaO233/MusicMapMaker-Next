#include "BackgroundSpectrumAnalyzer.h"

#include "log/colorful-log.h"

#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

namespace
{

// 本测试用连续正弦波驱动真实采集节点与分析器，重点验证视觉归一化行为：
//
// - 左右声道共享参考峰值后仍保留原始强弱关系；
// - 中等音量能在数帧内利用大部分绘制高度；
// - 整体音量降低后参考峰值会释放，不让柱体永久缩在底部；
// - 输入静音后频段平滑值与参考值共同衰减，不会把底噪重新放大。
// - 完整调用链使用固定缓冲，不依赖实际音频设备或后台线程调度。
// - 阈值只约束可见趋势，不绑定内部平滑系数的逐帧精确轨迹。

/// @brief 生成可实时调整音量的固定频率立体声测试信号。
class StereoSineNode final : public ice::IAudioNode
{
public:
    /// @brief 设置左右声道峰值音量。
    /// @param leftAmplitude 左声道峰值。
    /// @param rightAmplitude 右声道峰值。
    void setAmplitudes(float leftAmplitude, float rightAmplitude)
    {
        // 两声道独立设置，以验证共享归一化不会抹平立体声比例。
        m_leftAmplitude  = leftAmplitude;
        m_rightAmplitude = rightAmplitude;
    }

    /// @brief 向音频缓冲写入连续的立体声正弦信号。
    /// @param buffer 待填充的音频缓冲。
    void process(ice::AudioBuffer& buffer) override
    {
        // 相位跨 block 延续，避免每次分析窗口都从人为零交叉点重新开始。
        float** samples = buffer.raw_ptrs();
        if ( !samples || buffer.num_channels() == 0U ) return;

        const double sampleRate =
            static_cast<double>(ice::ICEConfig::internal_format.samplerate);
        // 440 Hz 位于分析范围内且不接近直流或 Nyquist 边界。
        const double phaseStep =
            2.0 * std::numbers::pi * 440.0 / std::max(sampleRate, 1.0);
        for ( std::size_t frame = 0U; frame < buffer.num_frames(); ++frame ) {
            const float wave  = static_cast<float>(std::sin(m_phase));
            samples[0][frame] = wave * m_leftAmplitude;
            if ( buffer.num_channels() > 1U ) {
                samples[1][frame] = wave * m_rightAmplitude;
            }
            m_phase += phaseStep;
            // 限制相位量级，长期重复调用也不会因大数值降低 sin 精度。
            if ( m_phase >= 2.0 * std::numbers::pi ) {
                m_phase -= 2.0 * std::numbers::pi;
            }
        }
    }

private:
    /// @brief 左声道当前峰值音量。
    float m_leftAmplitude{ 0.0f };
    /// @brief 右声道当前峰值音量。
    float m_rightAmplitude{ 0.0f };
    /// @brief 跨缓冲连续保存的正弦相位。
    double m_phase{ 0.0 };
};

/// @brief 取得有效频段中的最大绘制电平。
/// @param levels 待检查的立体声频段。
/// @return 左右声道所有有效频段的最大值。
float maximumLevel(const MMM::Audio::BackgroundSpectrumLevels& levels)
{
    // 只扫描 bandCount 有效前缀，固定数组尾部不参与显示契约。
    float peak = 0.0f;
    for ( std::size_t band = 0U; band < levels.bandCount; ++band ) {
        peak = std::max(peak, std::max(levels.left[band], levels.right[band]));
    }
    return peak;
}

/// @brief 取得指定声道有效频段中的最大绘制电平。
/// @param levels 待检查的立体声频段。
/// @param useLeft 是否读取左声道。
/// @return 指定声道所有有效频段的最大值。
float maximumChannelLevel(const MMM::Audio::BackgroundSpectrumLevels& levels,
                          bool                                        useLeft)
{
    // 与总体峰值 helper 分开，直接验证左右声道相对关系。
    float       peak    = 0.0f;
    const auto& channel = useLeft ? levels.left : levels.right;
    for ( std::size_t band = 0U; band < levels.bandCount; ++band ) {
        peak = std::max(peak, channel[band]);
    }
    return peak;
}

/// @brief 验证不同播放音量下电平柱都会动态使用大部分可用高度。
/// @return 峰值适配且静音可自然回落时返回 true。
///
/// 每次 analyzeFrame 先让 CaptureNode 生成一个完整 FFT 窗口，再立即分析最新
/// 样本。重复帧数给快攻、慢放和自适应参考足够的收敛时间，而不约束单帧细节。
/// 请求 24 个频段位于配置合法范围内，测试只关心归一化趋势，不固定具体频谱
/// bin 映射。阈值为行为余量而不是精确常量，允许 FFT 舍入与平台数学库差异。
/// 左右振幅始终保持 2.5 倍比例，任一有效阶段都应观察到左峰值更高。
bool testAdaptivePeakNormalization()
{
    auto source = std::make_shared<StereoSineNode>();
    MMM::Audio::BackgroundSpectrumCaptureNode capture(source);
    MMM::Audio::BackgroundSpectrumAnalyzer    analyzer;
    ice::AudioBuffer                          buffer;
    buffer.resize(ice::ICEConfig::internal_format,
                  MMM::Audio::BackgroundSpectrumCaptureNode::FFT_SIZE);

    // lambda 保证每次分析前都有一份与当前音量对应的新采集窗口。
    const auto analyzeFrame = [&]() -> const auto& {
        capture.process(buffer);
        return analyzer.analyze(&capture, nullptr, 24U);
    };

    // 首阶段验证中等音量快速填充，且左声道峰值仍高于右声道。
    source->setAmplitudes(0.20f, 0.08f);
    const MMM::Audio::BackgroundSpectrumLevels* levels = nullptr;
    for ( int frame = 0; frame < 8; ++frame ) {
        levels = &analyzeFrame();
    }
    // 0.75 下限验证有效利用绘制高度，左右比较验证共享参考没有各自归一化。
    if ( !levels || maximumLevel(*levels) < 0.75f ||
         maximumChannelLevel(*levels, true) <=
             maximumChannelLevel(*levels, false) ) {
        XERROR(
            "Background level normalization did not fill the available "
            "height or preserve stereo balance");
        return false;
    }

    // 同比例降低二十倍后，参考峰值应释放并恢复可读高度。
    source->setAmplitudes(0.01f, 0.004f);
    for ( int frame = 0; frame < 12; ++frame ) {
        levels = &analyzeFrame();
    }
    // 较低音量稳定后仍应超过 0.70，证明 release 能跟随长期响度变化。
    if ( maximumLevel(*levels) < 0.70f ) {
        XERROR(
            "Background level normalization did not adapt after lowering "
            "the source volume");
        return false;
    }

    // 最后输入严格静音，长时间迭代后所有残余柱体必须接近零。
    source->setAmplitudes(0.0f, 0.0f);
    for ( int frame = 0; frame < 80; ++frame ) {
        levels = &analyzeFrame();
    }
    // 静音 80 帧后上限 0.10，允许平滑尾迹但拒绝持续底噪放大。
    if ( maximumLevel(*levels) > 0.10f ) {
        XERROR("Background level normalization amplified residual silence");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行背景电平图分析器回归测试。
/// @return 全部测试通过时返回零。
int main()
{
    // 单一场景已经覆盖采集、FFT、频段平滑和归一化的完整调用链。
    return testAdaptivePeakNormalization() ? 0 : 1;
}
