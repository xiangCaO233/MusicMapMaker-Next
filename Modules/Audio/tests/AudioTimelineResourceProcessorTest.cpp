#include "audio/AudioTimelineResourceProcessor.h"

#include "log/colorful-log.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <ice/manage/AudioPool.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/thread/ThreadPool.hpp>

namespace
{

/// @brief 判断两个帧数是否在相对误差范围内。
bool frameCountNear(std::size_t actual, long double expected)
{
    const auto tolerance = std::max<long double>(8.0L, expected * 0.01L);
    return std::abs(static_cast<long double>(actual) - expected) <= tolerance;
}

/// @brief 计算两个首声道公共区间的最大样本差。
float maximumCommonDifference(const MMM::Audio::PreparedTimelineAudio& lhs,
                              const MMM::Audio::PreparedTimelineAudio& rhs)
{
    const auto lhsChannel = lhs.channel(0U);
    const auto rhsChannel = rhs.channel(0U);
    const auto frameCount = std::min(lhsChannel.size(), rhsChannel.size());
    float      difference = 0.0F;
    for ( std::size_t frame = 0U; frame < frameCount; ++frame ) {
        difference = std::max(difference,
                              std::abs(lhsChannel[frame] - rhsChannel[frame]));
    }
    return difference;
}

/// @brief 验证音量和静音保持为片段增益，不被烘焙进资源 PCM。
bool testVolumeAndMuteRemainSeparate(
    const std::shared_ptr<ice::AudioTrack>& track)
{
    MMM::AudioTrackConfig config;
    config.volume = 0.0F;
    config.muted  = true;
    const auto prepared =
        MMM::Audio::prepareAudioTimelineResource(track, config);
    const auto reference = MMM::Audio::PreparedTimelineAudio::fromTrack(track);
    if ( !prepared || !reference ||
         prepared->numFrames() != reference->numFrames() ||
         maximumCommonDifference(*prepared, *reference) != 0.0F ) {
        XERROR("Resource volume or mute was baked into prepared PCM");
        return false;
    }
    return true;
}

/// @brief 验证资源播放倍率只改变该资源的 PCM 时长。
bool testResourcePlaybackSpeed(const std::shared_ptr<ice::AudioTrack>& track)
{
    const auto reference = MMM::Audio::PreparedTimelineAudio::fromTrack(track);
    if ( !reference ) return false;

    MMM::AudioTrackConfig fastConfig;
    fastConfig.playbackSpeed = 2.0F;
    const auto fast =
        MMM::Audio::prepareAudioTimelineResource(track, fastConfig);

    MMM::AudioTrackConfig slowConfig;
    slowConfig.playbackSpeed = 0.5F;
    const auto slow =
        MMM::Audio::prepareAudioTimelineResource(track, slowConfig);

    if ( !fast || !slow ||
         !frameCountNear(
             fast->numFrames(),
             static_cast<long double>(reference->numFrames()) / 2.0L) ||
         !frameCountNear(
             slow->numFrames(),
             static_cast<long double>(reference->numFrames()) * 2.0L) ) {
        XERROR(
            "Per-resource playbackSpeed did not produce independent duration");
        return false;
    }
    return true;
}

/// @brief 验证资源音高不改变时长但确实产生独立 PCM。
bool testResourcePitch(const std::shared_ptr<ice::AudioTrack>& track)
{
    const auto reference = MMM::Audio::PreparedTimelineAudio::fromTrack(track);
    MMM::AudioTrackConfig config;
    config.playbackPitch = 12.0F;
    const auto shifted =
        MMM::Audio::prepareAudioTimelineResource(track, config);
    if ( !reference || !shifted ||
         !frameCountNear(shifted->numFrames(), reference->numFrames()) ||
         maximumCommonDifference(*shifted, *reference) < 1.0e-4F ) {
        XERROR("Per-resource pitch was not applied independently");
        return false;
    }
    return true;
}

/// @brief 验证资源图形均衡器在离线阶段应用。
bool testResourceEqualizer(const std::shared_ptr<ice::AudioTrack>& track)
{
    const auto reference = MMM::Audio::PreparedTimelineAudio::fromTrack(track);
    MMM::AudioTrackConfig config;
    config.eqEnabled   = true;
    config.eqPreset    = 1;
    config.eqBandGains = { 12.0F, 9.0F,  6.0F,   3.0F, -3.0F,
                           -6.0F, -9.0F, -12.0F, 6.0F, 9.0F };
    config.eqBandQs.assign(config.eqBandGains.size(), 1.2F);
    const auto equalized =
        MMM::Audio::prepareAudioTimelineResource(track, config);
    if ( !reference || !equalized ||
         equalized->numFrames() != reference->numFrames() ||
         maximumCommonDifference(*equalized, *reference) < 1.0e-5F ) {
        XERROR("Per-resource equalizer was not applied independently");
        return false;
    }

    for ( std::size_t channel = 0U; channel < equalized->numChannels();
          ++channel ) {
        for ( const float sample : equalized->channel(channel) ) {
            if ( !std::isfinite(sample) ) {
                XERROR("Per-resource equalizer produced non-finite PCM");
                return false;
            }
        }
    }
    return true;
}

}  // namespace

/// @brief 运行自动采样资源级离线 DSP 测试。
int main(int argc, char** argv)
{
    if ( argc < 2 ) {
        XERROR("Usage: AudioTimelineResourceProcessorTest <sample_path>");
        return 1;
    }

    const std::filesystem::path samplePath(argv[1]);
    ice::ThreadPool             threadPool(2);
    ice::AudioPool              audioPool;
    auto track = audioPool.get_or_load(threadPool, samplePath.string()).lock();
    if ( !track || track->num_frames() < 64U ) {
        XERROR("Failed to prepare resource DSP test sample");
        return 1;
    }

    const bool passed = testVolumeAndMuteRemainSeparate(track) &&
                        testResourcePlaybackSpeed(track) &&
                        testResourcePitch(track) &&
                        testResourceEqualizer(track);
    return passed ? 0 : 1;
}
