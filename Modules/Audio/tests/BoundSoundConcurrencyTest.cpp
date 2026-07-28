#include "audio/SoundEffectPool.h"
#include "log/colorful-log.h"

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <ice/config/config.hpp>
#include <ice/core/MixBus.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <memory>
#include <set>
#include <vector>

namespace
{
/// @brief 从测试上下文读取当前参考帧。
std::size_t readReferenceFrame(const void* context) noexcept
{
    return context ? *static_cast<const std::size_t*>(context) : 0U;
}

/// @brief 验证数十个采样播放池可保持互相独立的播放进度。
/// @param samplePath 短音效测试资源路径。
/// @return 至少 40 个播放池同时发声且具有多个独立进度时返回 true。
bool testDozensOfIndependentSampleVoices(
    const std::filesystem::path& samplePath)
{
    ice::ThreadPool threadPool(4);
    ice::AudioPool  audioPool;
    auto track = audioPool.get_or_load(threadPool, samplePath.string()).lock();
    if ( !track || track->num_frames() == 0U ) {
        XERROR("Failed to decode bound sound concurrency test sample");
        return false;
    }

    auto mainMixer = std::make_shared<ice::MixBus>();
    std::vector<std::shared_ptr<MMM::Audio::SoundEffectPool>> voices;
    constexpr std::size_t                                     VOICE_COUNT = 48U;
    constexpr std::size_t START_FRAME_SPACING                             = 64U;
    voices.reserve(VOICE_COUNT);

    std::size_t referenceFrame = 0U;
    for ( std::size_t index = 0U; index < VOICE_COUNT; ++index ) {
        auto voice = std::make_shared<MMM::Audio::SoundEffectPool>(track);
        voice->init(1);
        mainMixer->add_source(voice->getMixer());

        const std::size_t targetFrame = index * START_FRAME_SPACING;
        voice->playScheduled(1.0F,
                             targetFrame,
                             &referenceFrame,
                             &readReferenceFrame,
                             {},
                             targetFrame);
        voices.push_back(std::move(voice));
    }

    constexpr std::size_t BUFFER_FRAMES = 256U;
    ice::AudioBuffer buffer(ice::ICEConfig::internal_format, BUFFER_FRAMES);
    while ( referenceFrame < 4096U ) {
        mainMixer->process(buffer);
        referenceFrame += BUFFER_FRAMES;
    }

    std::size_t       activeVoiceCount = 0U;
    std::set<int64_t> progressBuckets;
    for ( const auto& voice : voices ) {
        const double playbackTime = voice->getLatestPlaybackTime();
        if ( playbackTime <= 0.0 ) continue;
        ++activeVoiceCount;
        progressBuckets.insert(
            static_cast<int64_t>(std::llround(playbackTime * 10000.0)));
    }

    if ( activeVoiceCount < 40U || progressBuckets.size() < 8U ) {
        XERROR(
            "Bound sound concurrency lost independent voices: active={}, "
            "progressBuckets={}",
            activeVoiceCount,
            progressBuckets.size());
        return false;
    }
    return true;
}

/// @brief 验证 0.5x 与 2x 下同步和非同步路由使用正确帧域。
/// @return 四种调度计划均符合预期时返回 true。
bool testPreviewSpeedScheduleRouting()
{
    constexpr std::size_t CURRENT_FRAME = 1200U;
    constexpr std::size_t TARGET_FRAME  = 1680U;
    constexpr std::size_t FRAME_DELTA   = TARGET_FRAME - CURRENT_FRAME;

    const auto syncedHalf = MMM::Audio::planSoundEffectSchedule(
        TARGET_FRAME, CURRENT_FRAME, 0.5, true);
    const auto syncedDouble = MMM::Audio::planSoundEffectSchedule(
        TARGET_FRAME, CURRENT_FRAME, 2.0, true);
    const auto independentHalf = MMM::Audio::planSoundEffectSchedule(
        TARGET_FRAME, CURRENT_FRAME, 0.5, false);
    const auto independentDouble = MMM::Audio::planSoundEffectSchedule(
        TARGET_FRAME, CURRENT_FRAME, 2.0, false);

    const bool valid =
        syncedHalf.mode ==
            MMM::Audio::SoundEffectScheduleMode::AbsoluteTimelineFrame &&
        syncedDouble.mode ==
            MMM::Audio::SoundEffectScheduleMode::AbsoluteTimelineFrame &&
        syncedHalf.frame == TARGET_FRAME &&
        syncedDouble.frame == TARGET_FRAME &&
        independentHalf.mode ==
            MMM::Audio::SoundEffectScheduleMode::RelativeOutputDelay &&
        independentDouble.mode ==
            MMM::Audio::SoundEffectScheduleMode::RelativeOutputDelay &&
        independentHalf.frame == FRAME_DELTA * 2U &&
        independentDouble.frame == FRAME_DELTA / 2U;
    if ( !valid ) {
        XERROR(
            "Preview speed SFX schedule routing mismatch: syncHalf={}, "
            "syncDouble={}, independentHalf={}, independentDouble={}",
            syncedHalf.frame,
            syncedDouble.frame,
            independentHalf.frame,
            independentDouble.frame);
    }
    return valid;
}

/// @brief 验证空帧音轨不会创建或永久占用音效 voice。
/// @param samplePath 可由流式解码器探测的测试资源。
/// @return 重复播放后池中仍无实例时返回 true。
bool testZeroFrameTrackDoesNotOccupyVoice(
    const std::filesystem::path& samplePath)
{
    ice::ThreadPool threadPool(2);
    ice::AudioPool  streamingPool;
    auto zeroFrameTrack = streamingPool
                              .get_or_load(threadPool,
                                           samplePath.string(),
                                           ice::CachingStrategy::STREAMING)
                              .lock();
    if ( !zeroFrameTrack || zeroFrameTrack->num_frames() != 0U ) {
        XERROR("Failed to create zero-frame streaming track");
        return false;
    }

    MMM::Audio::SoundEffectPool pool(zeroFrameTrack);
    pool.init(2);
    for ( std::size_t iteration = 0U; iteration < 8U; ++iteration ) {
        pool.play(1.0F);
        pool.playScheduledRelative(1.0F, 32U);
    }

    ice::AudioBuffer buffer(ice::ICEConfig::internal_format, 64U);
    pool.getMixer()->process(buffer);
    const bool valid =
        pool.getMixer()->sourceCount() == 0U && !pool.isPlaying();
    if ( !valid ) {
        XERROR("Zero-frame SFX allocated persistent voices: {}",
               pool.getMixer()->sourceCount());
    }
    return valid;
}

/// @brief 验证停止中的实例不会被立即复用，新播放也不会被旧停止覆盖。
/// @param track 已完整缓存的短音效。
/// @return stop 后立即播放可正常推进且使用独立实例时返回 true。
bool testStopThenImmediatePlayKeepsNewVoice(
    const std::shared_ptr<ice::AudioTrack>& track)
{
    MMM::Audio::SoundEffectPool pool(track);
    pool.init(1);
    pool.playScheduledRelative(1.0F, 512U);
    pool.stopAll();
    pool.play(1.0F);

    const std::size_t voicesBeforeProcess = pool.getMixer()->sourceCount();
    ice::AudioBuffer  buffer(ice::ICEConfig::internal_format, 128U);
    pool.getMixer()->process(buffer);
    const bool valid =
        voicesBeforeProcess == 2U && pool.getLatestPlaybackTime() > 0.0;
    if ( !valid ) {
        XERROR(
            "stop->play reused stopping voice or cancelled new playback: "
            "voices={}, playback={}",
            voicesBeforeProcess,
            pool.getLatestPlaybackTime());
    }
    return valid;
}

/// @brief 验证带变调的音效在 Source final 后完整 drain 并可复用。
/// @param track 已完整缓存的短音效。
/// @return drain 后再次播放未扩容时返回 true。
bool testFinalDrainReturnsVoiceToPool(
    const std::shared_ptr<ice::AudioTrack>& track)
{
    constexpr std::size_t       BLOCK_FRAMES = 128U;
    MMM::Audio::SoundEffectPool pool(track);
    pool.init(1);
    pool.play(1.0F, 3.0);

    ice::AudioBuffer  buffer(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    const std::size_t sourceBlocks =
        (track->num_frames() + BLOCK_FRAMES - 1U) / BLOCK_FRAMES;
    for ( std::size_t block = 0U; block < sourceBlocks + 256U; ++block ) {
        pool.getMixer()->process(buffer);
    }

    pool.play(1.0F, 3.0);
    const bool valid = pool.getMixer()->sourceCount() == 1U;
    if ( !valid ) {
        XERROR("Final-drained SFX voice was not reused: {}",
               pool.getMixer()->sourceCount());
    }
    return valid;
}
}  // namespace

/// @brief 运行绑定采样多声部并发测试。
/// @param argc 参数数量。
/// @param argv 第一个参数为短音效资源路径。
/// @return 测试通过时返回 0。
int main(int argc, char** argv)
{
    if ( argc < 2 ) {
        XERROR("Usage: BoundSoundConcurrencyTest <sample_path>");
        return 1;
    }
    const std::filesystem::path samplePath(argv[1]);
    ice::ThreadPool             threadPool(2);
    ice::AudioPool              audioPool;
    auto track = audioPool.get_or_load(threadPool, samplePath.string()).lock();
    if ( !track || track->num_frames() == 0U ) {
        XERROR("Failed to decode SFX lifecycle test sample");
        return 1;
    }

    const bool passed = testDozensOfIndependentSampleVoices(samplePath) &&
                        testPreviewSpeedScheduleRouting() &&
                        testZeroFrameTrackDoesNotOccupyVoice(samplePath) &&
                        testStopThenImmediatePlayKeepsNewVoice(track) &&
                        testFinalDrainReturnsVoiceToPool(track);
    return passed ? 0 : 1;
}
