#include "BackgroundSpectrumAnalyzer.h"

#include "audio/AudioManager.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <cmath>
#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <mutex>
#include <numbers>
#include <utility>

namespace MMM::Audio
{
namespace
{
/// @brief FFTW 计划创建和销毁使用的进程内互斥量。
std::mutex& fftwPlanMutex()
{
    static std::mutex mutex;
    return mutex;
}
}  // namespace

BackgroundSpectrumCaptureNode::BackgroundSpectrumCaptureNode(
    std::shared_ptr<ice::IAudioNode> input)
    : m_input(std::move(input))
{
    for ( auto& sample : m_left ) {
        sample.store(0.0f, std::memory_order_relaxed);
    }
    for ( auto& sample : m_right ) {
        sample.store(0.0f, std::memory_order_relaxed);
    }
}

void BackgroundSpectrumCaptureNode::process(ice::AudioBuffer& buffer)
{
    buffer.clear();
    if ( m_input ) {
        m_input->process(buffer);
    }

    const float* const* samples      = buffer.raw_ptrs();
    const std::size_t   frameCount   = buffer.num_frames();
    const std::size_t   channelCount = buffer.num_channels();
    const std::uint64_t writeStart =
        m_writtenFrames.load(std::memory_order_relaxed);
    if ( !samples || channelCount == 0U || frameCount == 0U ) {
        return;
    }

    const std::size_t firstFrame =
        frameCount > FFT_SIZE ? frameCount - FFT_SIZE : 0U;
    for ( std::size_t frame = firstFrame; frame < frameCount; ++frame ) {
        const std::size_t ringIndex =
            static_cast<std::size_t>((writeStart + frame) % FFT_SIZE);
        m_left[ringIndex].store(samples[0][frame], std::memory_order_relaxed);
        const float rightSample =
            channelCount > 1U ? samples[1][frame] : samples[0][frame];
        m_right[ringIndex].store(rightSample, std::memory_order_relaxed);
    }
    m_writtenFrames.store(writeStart + frameCount, std::memory_order_release);
}

void BackgroundSpectrumCaptureNode::copyLatest(std::span<float> left,
                                               std::span<float> right) const
{
    if ( left.size() != FFT_SIZE || right.size() != FFT_SIZE ) return;

    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    const std::uint64_t written =
        m_writtenFrames.load(std::memory_order_acquire);
    const std::size_t available =
        static_cast<std::size_t>(std::min<std::uint64_t>(written, FFT_SIZE));
    const std::size_t   outputOffset = FFT_SIZE - available;
    const std::uint64_t firstFrame   = written - available;
    for ( std::size_t index = 0U; index < available; ++index ) {
        const std::size_t ringIndex =
            static_cast<std::size_t>((firstFrame + index) % FFT_SIZE);
        left[outputOffset + index] =
            m_left[ringIndex].load(std::memory_order_relaxed);
        right[outputOffset + index] =
            m_right[ringIndex].load(std::memory_order_relaxed);
    }
}

BackgroundSpectrumAnalyzer::BackgroundSpectrumAnalyzer()
{
    constexpr std::size_t fftSize = BackgroundSpectrumCaptureNode::FFT_SIZE;
    for ( std::size_t index = 0U; index < fftSize; ++index ) {
        m_window[index] =
            0.5 -
            0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(index) /
                           static_cast<double>(fftSize - 1U));
    }

    m_fftInputLeft =
        static_cast<double*>(fftw_malloc(sizeof(double) * fftSize));
    m_fftInputRight =
        static_cast<double*>(fftw_malloc(sizeof(double) * fftSize));
    m_fftOutputLeft = static_cast<fftw_complex*>(
        fftw_malloc(sizeof(fftw_complex) * (fftSize / 2U + 1U)));
    m_fftOutputRight = static_cast<fftw_complex*>(
        fftw_malloc(sizeof(fftw_complex) * (fftSize / 2U + 1U)));
    if ( !m_fftInputLeft || !m_fftInputRight || !m_fftOutputLeft ||
         !m_fftOutputRight ) {
        XERROR("Failed to allocate background spectrum FFT buffers.");
        return;
    }

    std::lock_guard<std::mutex> lock(fftwPlanMutex());
    m_fftPlanLeft  = fftw_plan_dft_r2c_1d(static_cast<int>(fftSize),
                                          m_fftInputLeft,
                                          m_fftOutputLeft,
                                          FFTW_ESTIMATE);
    m_fftPlanRight = fftw_plan_dft_r2c_1d(static_cast<int>(fftSize),
                                          m_fftInputRight,
                                          m_fftOutputRight,
                                          FFTW_ESTIMATE);
    if ( !m_fftPlanLeft || !m_fftPlanRight ) {
        XERROR("Failed to create background spectrum FFT plans.");
    }
}

BackgroundSpectrumAnalyzer::~BackgroundSpectrumAnalyzer()
{
    {
        std::lock_guard<std::mutex> lock(fftwPlanMutex());
        if ( m_fftPlanLeft ) fftw_destroy_plan(m_fftPlanLeft);
        if ( m_fftPlanRight ) fftw_destroy_plan(m_fftPlanRight);
    }
    if ( m_fftInputLeft ) fftw_free(m_fftInputLeft);
    if ( m_fftInputRight ) fftw_free(m_fftInputRight);
    if ( m_fftOutputLeft ) fftw_free(m_fftOutputLeft);
    if ( m_fftOutputRight ) fftw_free(m_fftOutputRight);
}

const BackgroundSpectrumLevels& BackgroundSpectrumAnalyzer::analyze(
    const BackgroundSpectrumCaptureNode* bgmCapture,
    const BackgroundSpectrumCaptureNode* hitEffectCapture,
    std::size_t                          requestedBandCount)
{
    const std::size_t bandCount = std::clamp(
        requestedBandCount,
        static_cast<std::size_t>(Config::BACKGROUND_SPECTRUM_MIN_BANDS),
        static_cast<std::size_t>(Config::BACKGROUND_SPECTRUM_MAX_BANDS));
    m_levels.bandCount = bandCount;
    if ( bandCount != m_previousBandCount ) {
        m_smoothedLeft.fill(0.0f);
        m_smoothedRight.fill(0.0f);
        m_adaptivePeakReference = 0.0f;
        m_previousBandCount     = bandCount;
    }

    m_captureLeft.fill(0.0f);
    m_captureRight.fill(0.0f);
    if ( bgmCapture ) {
        bgmCapture->copyLatest(m_captureLeft, m_captureRight);
    }
    if ( hitEffectCapture ) {
        hitEffectCapture->copyLatest(m_hitCaptureLeft, m_hitCaptureRight);
        for ( std::size_t index = 0U;
              index < BackgroundSpectrumCaptureNode::FFT_SIZE;
              ++index ) {
            m_captureLeft[index] += m_hitCaptureLeft[index];
            m_captureRight[index] += m_hitCaptureRight[index];
        }
    }

    if ( !m_fftPlanLeft || !m_fftPlanRight || !m_fftInputLeft ||
         !m_fftInputRight || !m_fftOutputLeft || !m_fftOutputRight ) {
        m_levels.left.fill(0.0f);
        m_levels.right.fill(0.0f);
        m_adaptivePeakReference = 0.0f;
        return m_levels;
    }

    for ( std::size_t index = 0U;
          index < BackgroundSpectrumCaptureNode::FFT_SIZE;
          ++index ) {
        m_fftInputLeft[index] =
            static_cast<double>(m_captureLeft[index]) * m_window[index];
        m_fftInputRight[index] =
            static_cast<double>(m_captureRight[index]) * m_window[index];
    }
    fftw_execute(m_fftPlanLeft);
    fftw_execute(m_fftPlanRight);
    const float leftSignalPeak = updateChannel(
        m_fftOutputLeft, m_smoothedLeft, m_levels.left, bandCount);
    const float rightSignalPeak = updateChannel(
        m_fftOutputRight, m_smoothedRight, m_levels.right, bandCount);
    normalizeLevels(std::max(leftSignalPeak, rightSignalPeak), bandCount);
    return m_levels;
}

float BackgroundSpectrumAnalyzer::updateChannel(
    const fftw_complex*                                       spectrum,
    std::array<float, Config::BACKGROUND_SPECTRUM_MAX_BANDS>& smoothed,
    std::array<float, Config::BACKGROUND_SPECTRUM_MAX_BANDS>& output,
    std::size_t                                               bandCount)
{
    constexpr std::size_t fftSize = BackgroundSpectrumCaptureNode::FFT_SIZE;
    const double          sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( !spectrum || sampleRate <= 0.0 ) {
        output.fill(0.0f);
        return 0.0f;
    }

    constexpr double minFrequency = 40.0;
    const double     maxFrequency = std::min(16000.0, sampleRate * 0.5);
    if ( maxFrequency <= minFrequency ) {
        output.fill(0.0f);
        return 0.0f;
    }
    const double     logMin        = std::log(minFrequency);
    const double     logRange      = std::log(maxFrequency) - logMin;
    const double     binFrequency  = sampleRate / static_cast<double>(fftSize);
    constexpr double responseScale = 80.0;
    const double     responseDenominator = std::log1p(responseScale);
    float            signalPeak          = 0.0f;

    for ( std::size_t band = 0U; band < bandCount; ++band ) {
        const double lowerRatio =
            static_cast<double>(band) / static_cast<double>(bandCount);
        const double upperRatio =
            static_cast<double>(band + 1U) / static_cast<double>(bandCount);
        const double lowerFrequency = std::exp(logMin + logRange * lowerRatio);
        const double upperFrequency = std::exp(logMin + logRange * upperRatio);
        std::size_t  firstBin =
            static_cast<std::size_t>(std::floor(lowerFrequency / binFrequency));
        std::size_t finalBin =
            static_cast<std::size_t>(std::ceil(upperFrequency / binFrequency));
        firstBin = std::clamp(firstBin, std::size_t{ 1U }, fftSize / 2U);
        finalBin = std::clamp(std::max(finalBin, firstBin + 1U),
                              std::size_t{ 1U },
                              fftSize / 2U + 1U);

        double peakMagnitude = 0.0;
        for ( std::size_t bin = firstBin; bin < finalBin; ++bin ) {
            const double real = spectrum[bin][0];
            const double imag = spectrum[bin][1];
            peakMagnitude =
                std::max(peakMagnitude, std::sqrt(real * real + imag * imag));
        }
        const double normalizedMagnitude =
            peakMagnitude * 2.0 / static_cast<double>(fftSize);
        const float target =
            std::clamp(static_cast<float>(std::sqrt(
                           std::log1p(normalizedMagnitude * responseScale) /
                           responseDenominator)),
                       0.0f,
                       1.0f);
        signalPeak            = std::max(signalPeak, target);
        const float smoothing = target > smoothed[band] ? 0.58f : 0.14f;
        smoothed[band] += (target - smoothed[band]) * smoothing;
        output[band] = smoothed[band];
    }
    std::fill(output.begin() + bandCount, output.end(), 0.0f);
    return signalPeak;
}

void BackgroundSpectrumAnalyzer::normalizeLevels(float       signalPeak,
                                                 std::size_t bandCount)
{
    constexpr float signalFloor            = 0.0125f;
    constexpr float targetFill             = 0.94f;
    constexpr float referenceAttack        = 0.65f;
    constexpr float referenceRelease       = 0.18f;
    constexpr float silentReferenceRelease = 0.94f;
    constexpr float maximumHeadroom        = 1.25f;

    float displayPeak = 0.0f;
    for ( std::size_t band = 0U; band < bandCount; ++band ) {
        displayPeak = std::max(
            displayPeak, std::max(m_levels.left[band], m_levels.right[band]));
    }

    if ( std::isfinite(signalPeak) && signalPeak > signalFloor &&
         displayPeak > 0.0f ) {
        if ( m_adaptivePeakReference <= signalFloor ) {
            m_adaptivePeakReference = displayPeak;
        } else {
            const float response = displayPeak > m_adaptivePeakReference
                                       ? referenceAttack
                                       : referenceRelease;
            m_adaptivePeakReference +=
                (displayPeak - m_adaptivePeakReference) * response;
            // 音量降低时限制保留余量，避免电平柱长时间只占据底部。
            m_adaptivePeakReference = std::min(m_adaptivePeakReference,
                                               displayPeak * maximumHeadroom);
        }
        m_adaptivePeakReference =
            std::max(m_adaptivePeakReference, signalFloor);
    } else {
        // 静音时参考峰值比频段平滑值回落得更慢，使残余柱自然收缩而非放大底噪。
        m_adaptivePeakReference = std::max(
            signalFloor, m_adaptivePeakReference * silentReferenceRelease);
    }

    const float scale =
        targetFill / std::max(m_adaptivePeakReference, signalFloor);
    for ( std::size_t band = 0U; band < bandCount; ++band ) {
        m_levels.left[band] =
            std::clamp(m_levels.left[band] * scale, 0.0f, 1.0f);
        m_levels.right[band] =
            std::clamp(m_levels.right[band] * scale, 0.0f, 1.0f);
    }
}

const BackgroundSpectrumLevels& AudioManager::updateBackgroundSpectrum(
    std::size_t bandCount, bool includeHitEffects)
{
    static const BackgroundSpectrumLevels empty;
    if ( !m_backgroundSpectrumAnalyzer ) return empty;
    return m_backgroundSpectrumAnalyzer->analyze(
        m_bgmSpectrumCapture.get(),
        includeHitEffects ? m_hitEffectSpectrumCapture.get() : nullptr,
        bandCount);
}

}  // namespace MMM::Audio
