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
        voice->playScheduled(
            1.0F,
            targetFrame,
            [&referenceFrame]() { return referenceFrame; },
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
    return testDozensOfIndependentSampleVoices(argv[1]) ? 0 : 1;
}
