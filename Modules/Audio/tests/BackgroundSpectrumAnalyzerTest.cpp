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

/// @brief 生成可实时调整音量的固定频率立体声测试信号。
class StereoSineNode final : public ice::IAudioNode
{
public:
    /// @brief 设置左右声道峰值音量。
    /// @param leftAmplitude 左声道峰值。
    /// @param rightAmplitude 右声道峰值。
    void setAmplitudes(float leftAmplitude, float rightAmplitude)
    {
        m_leftAmplitude  = leftAmplitude;
        m_rightAmplitude = rightAmplitude;
    }

    /// @brief 向音频缓冲写入连续的立体声正弦信号。
    /// @param buffer 待填充的音频缓冲。
    void process(ice::AudioBuffer& buffer) override
    {
        float** samples = buffer.raw_ptrs();
        if ( !samples || buffer.num_channels() == 0U ) return;

        const double sampleRate =
            static_cast<double>(ice::ICEConfig::internal_format.samplerate);
        const double phaseStep =
            2.0 * std::numbers::pi * 440.0 / std::max(sampleRate, 1.0);
        for ( std::size_t frame = 0U; frame < buffer.num_frames(); ++frame ) {
            const float wave  = static_cast<float>(std::sin(m_phase));
            samples[0][frame] = wave * m_leftAmplitude;
            if ( buffer.num_channels() > 1U ) {
                samples[1][frame] = wave * m_rightAmplitude;
            }
            m_phase += phaseStep;
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
    float       peak    = 0.0f;
    const auto& channel = useLeft ? levels.left : levels.right;
    for ( std::size_t band = 0U; band < levels.bandCount; ++band ) {
        peak = std::max(peak, channel[band]);
    }
    return peak;
}

/// @brief 验证不同播放音量下电平柱都会动态使用大部分可用高度。
/// @return 峰值适配且静音可自然回落时返回 true。
bool testAdaptivePeakNormalization()
{
    auto source = std::make_shared<StereoSineNode>();
    MMM::Audio::BackgroundSpectrumCaptureNode capture(source);
    MMM::Audio::BackgroundSpectrumAnalyzer    analyzer;
    ice::AudioBuffer                          buffer;
    buffer.resize(ice::ICEConfig::internal_format,
                  MMM::Audio::BackgroundSpectrumCaptureNode::FFT_SIZE);

    const auto analyzeFrame = [&]() -> const auto& {
        capture.process(buffer);
        return analyzer.analyze(&capture, nullptr, 24U);
    };

    source->setAmplitudes(0.20f, 0.08f);
    const MMM::Audio::BackgroundSpectrumLevels* levels = nullptr;
    for ( int frame = 0; frame < 8; ++frame ) {
        levels = &analyzeFrame();
    }
    if ( !levels || maximumLevel(*levels) < 0.75f ||
         maximumChannelLevel(*levels, true) <=
             maximumChannelLevel(*levels, false) ) {
        XERROR(
            "Background level normalization did not fill the available "
            "height or preserve stereo balance");
        return false;
    }

    source->setAmplitudes(0.01f, 0.004f);
    for ( int frame = 0; frame < 12; ++frame ) {
        levels = &analyzeFrame();
    }
    if ( maximumLevel(*levels) < 0.70f ) {
        XERROR(
            "Background level normalization did not adapt after lowering "
            "the source volume");
        return false;
    }

    source->setAmplitudes(0.0f, 0.0f);
    for ( int frame = 0; frame < 80; ++frame ) {
        levels = &analyzeFrame();
    }
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
    return testAdaptivePeakNormalization() ? 0 : 1;
}
