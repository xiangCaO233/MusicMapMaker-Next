#include "audio/AudioTimelineResourceProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <nlohmann/json.hpp>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ice/config/config.hpp>
#include <ice/core/effect/filter/BiquadFilter.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <rubberband/RubberBandStretcher.h>

namespace MMM::Audio
{
namespace
{

constexpr double      MIN_RESOURCE_SPEED           = 0.25;
constexpr double      MAX_RESOURCE_SPEED           = 2.0;
constexpr double      MIN_RESOURCE_PITCH           = -24.0;
constexpr double      MAX_RESOURCE_PITCH           = 24.0;
constexpr std::size_t OFFLINE_PROCESS_BLOCK_FRAMES = 32768U;

/// @brief 规范化资源播放倍率。
[[nodiscard]] double normalizedResourceSpeed(float speed) noexcept
{
    return std::isfinite(speed) ? std::clamp(static_cast<double>(speed),
                                             MIN_RESOURCE_SPEED,
                                             MAX_RESOURCE_SPEED)
                                : 1.0;
}

/// @brief 规范化资源音高。
[[nodiscard]] double normalizedResourcePitch(float pitch) noexcept
{
    return std::isfinite(pitch) ? std::clamp(static_cast<double>(pitch),
                                             MIN_RESOURCE_PITCH,
                                             MAX_RESOURCE_PITCH)
                                : 0.0;
}

/// @brief 获取项目 EQ 预设对应的中心频率。
[[nodiscard]] std::span<const double> equalizerFrequencies(int preset) noexcept
{
    static constexpr double TEN_BAND_FREQUENCIES[] = {
        31.25,  62.5,   125.0,  250.0,  500.0,
        1000.0, 2000.0, 4000.0, 8000.0, 16000.0,
    };
    static constexpr double FIFTEEN_BAND_FREQUENCIES[] = {
        25.0,   40.0,   63.0,   100.0,  160.0,  250.0,   400.0,   630.0,
        1000.0, 1600.0, 2500.0, 4000.0, 6300.0, 10000.0, 16000.0,
    };

    if ( preset == 1 ) return TEN_BAND_FREQUENCIES;
    if ( preset == 2 ) return FIFTEEN_BAND_FREQUENCIES;
    return {};
}

/// @brief 将资源 EQ 离线应用到自有 PCM。
void applyResourceEqualizer(std::vector<std::vector<float>>& channels,
                            const AudioTrackConfig&          config)
{
    if ( !config.eqEnabled ) return;
    const auto frequencies = equalizerFrequencies(config.eqPreset);
    if ( frequencies.empty() ) return;

    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( sampleRate <= 0.0 ) return;

    for ( auto& channel : channels ) {
        std::vector<ice::BiquadFilter> filters(frequencies.size());
        for ( std::size_t band = 0U; band < frequencies.size(); ++band ) {
            const float configuredGain = band < config.eqBandGains.size()
                                             ? config.eqBandGains[band]
                                             : 0.0F;
            const float configuredQ =
                band < config.eqBandQs.size()
                    ? config.eqBandQs[band]
                    : static_cast<float>(std::numbers::sqrt2);
            const double gain =
                std::isfinite(configuredGain)
                    ? std::clamp(
                          static_cast<double>(configuredGain), -24.0, 24.0)
                    : 0.0;
            const double q =
                std::isfinite(configuredQ) && configuredQ > 0.0F
                    ? std::clamp(static_cast<double>(configuredQ), 0.05, 50.0)
                    : std::numbers::sqrt2;
            filters[band].set_peaking(sampleRate, frequencies[band], q, gain);
        }
        for ( auto& filter : filters ) {
            filter.process(channel.data(), channel.size());
        }
    }
}

/// @brief 从 Rubber Band 取出当前全部可用帧并追加到结果。
void drainRubberBand(RubberBand::RubberBandStretcher& stretcher,
                     std::vector<std::vector<float>>& output,
                     std::vector<std::vector<float>>& retrieveChannels,
                     std::vector<float*>&             retrievePointers)
{
    for ( int available = stretcher.available(); available > 0;
          available     = stretcher.available() ) {
        const auto frames = static_cast<std::size_t>(available);
        for ( std::size_t channel = 0U; channel < retrieveChannels.size();
              ++channel ) {
            retrieveChannels[channel].resize(frames);
            retrievePointers[channel] = retrieveChannels[channel].data();
        }
        const std::size_t retrieved =
            stretcher.retrieve(retrievePointers.data(), frames);
        for ( std::size_t channel = 0U; channel < output.size(); ++channel ) {
            output[channel].insert(output[channel].end(),
                                   retrieveChannels[channel].begin(),
                                   retrieveChannels[channel].begin() +
                                       static_cast<std::ptrdiff_t>(retrieved));
        }
    }
}

/// @brief 通过 Rubber Band 两遍离线模式应用独立变速和变调。
[[nodiscard]] std::vector<std::vector<float>> stretchResourceOffline(
    const std::vector<std::vector<float>>& source, double playbackSpeed,
    double pitchSemitones)
{
    if ( source.empty() || source.front().empty() ) return {};
    const std::size_t channelCount = source.size();
    const std::size_t sourceFrames = source.front().size();
    const double      timeRatio    = 1.0 / playbackSpeed;
    const double      pitchScale   = std::pow(2.0, pitchSemitones / 12.0);
    const auto        options =
        RubberBand::RubberBandStretcher::OptionProcessOffline |
        RubberBand::RubberBandStretcher::OptionChannelsTogether |
        RubberBand::RubberBandStretcher::OptionPitchHighQuality |
        RubberBand::RubberBandStretcher::OptionThreadingNever |
        RubberBand::RubberBandStretcher::OptionEngineFiner;

    RubberBand::RubberBandStretcher stretcher(
        ice::ICEConfig::internal_format.samplerate,
        channelCount,
        options,
        timeRatio,
        pitchScale);
    stretcher.setExpectedInputDuration(sourceFrames);
    stretcher.setMaxProcessSize(OFFLINE_PROCESS_BLOCK_FRAMES);

    std::vector<const float*> inputPointers(channelCount);
    for ( std::size_t offset = 0U; offset < sourceFrames; ) {
        const std::size_t frameCount =
            std::min(OFFLINE_PROCESS_BLOCK_FRAMES, sourceFrames - offset);
        for ( std::size_t channel = 0U; channel < channelCount; ++channel ) {
            inputPointers[channel] = source[channel].data() + offset;
        }
        const bool finalBlock = offset + frameCount == sourceFrames;
        stretcher.study(inputPointers.data(), frameCount, finalBlock);
        offset += frameCount;
    }

    const auto expectedFramesLongDouble =
        static_cast<long double>(sourceFrames) *
        static_cast<long double>(timeRatio);
    const auto maximumSize =
        static_cast<long double>(std::numeric_limits<std::size_t>::max());
    const std::size_t expectedFrames =
        expectedFramesLongDouble >= maximumSize
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(
                  std::ceil(std::max(expectedFramesLongDouble, 0.0L)));
    std::vector<std::vector<float>> output(channelCount);
    for ( auto& channel : output ) {
        channel.reserve(expectedFrames);
    }
    std::vector<std::vector<float>> retrieveChannels(channelCount);
    std::vector<float*>             retrievePointers(channelCount);

    for ( std::size_t offset = 0U; offset < sourceFrames; ) {
        const std::size_t frameCount =
            std::min(OFFLINE_PROCESS_BLOCK_FRAMES, sourceFrames - offset);
        for ( std::size_t channel = 0U; channel < channelCount; ++channel ) {
            inputPointers[channel] = source[channel].data() + offset;
        }
        const bool finalBlock = offset + frameCount == sourceFrames;
        stretcher.process(inputPointers.data(), frameCount, finalBlock);
        drainRubberBand(stretcher, output, retrieveChannels, retrievePointers);
        offset += frameCount;
    }
    drainRubberBand(stretcher, output, retrieveChannels, retrievePointers);
    return output;
}

/// @brief 将只读准备数据复制为离线 DSP 的自有声道。
[[nodiscard]] std::vector<std::vector<float>> copyPreparedChannels(
    const PreparedTimelineAudio& source)
{
    std::vector<std::vector<float>> channels;
    channels.reserve(source.numChannels());
    for ( std::size_t channel = 0U; channel < source.numChannels();
          ++channel ) {
        const auto view = source.channel(channel);
        channels.emplace_back(view.begin(), view.end());
    }
    return channels;
}

}  // namespace

std::string makeAudioResourceProcessingCacheKey(std::string_view filePath,
                                                const AudioTrackConfig& config)
{
    const nlohmann::json processingConfig{
        { "playbackSpeed", config.playbackSpeed },
        { "playbackPitch", config.playbackPitch },
        { "eqEnabled", config.eqEnabled },
        { "eqPreset", config.eqPreset },
        { "eqBandGains", config.eqBandGains },
        { "eqBandQs", config.eqBandQs },
    };
    std::string key(filePath);
    key.push_back('\0');
    key.append(processingConfig.dump());
    return key;
}

std::shared_ptr<const PreparedTimelineAudio> prepareAudioTimelineResource(
    const std::shared_ptr<ice::AudioTrack>& track,
    const AudioTrackConfig&                 config)
{
    const auto source = PreparedTimelineAudio::fromTrack(track);
    if ( !source ) return {};

    const double speed = normalizedResourceSpeed(config.playbackSpeed);
    const double pitch = normalizedResourcePitch(config.playbackPitch);
    const bool   needsEqualizer =
        config.eqEnabled && !equalizerFrequencies(config.eqPreset).empty();
    const bool needsStretch =
        std::abs(speed - 1.0) > 1.0e-6 || std::abs(pitch) > 1.0e-6;
    if ( !needsEqualizer && !needsStretch ) return source;

    auto channels = copyPreparedChannels(*source);
    applyResourceEqualizer(channels, config);
    if ( needsStretch ) {
        channels = stretchResourceOffline(channels, speed, pitch);
    }
    return PreparedTimelineAudio::fromOwnedChannels(std::move(channels), track);
}

}  // namespace MMM::Audio
