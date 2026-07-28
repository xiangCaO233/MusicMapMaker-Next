#include "audio/AudioTimelineMixerNode.h"

#include "log/colorful-log.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <memory>
#include <string>
#include <vector>

namespace
{

constexpr float SAMPLE_EPSILON = 1.0e-5F;

/// @brief 比较输出缓冲区与两个源区间的加权和。
bool verifyMixedRange(const ice::AudioBuffer& output,
                      const ice::AudioBuffer& firstSource,
                      const ice::AudioBuffer* secondSource,
                      std::size_t secondOutputStart, float firstVolume,
                      float secondVolume)
{
    for ( std::size_t channel = 0U; channel < output.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < output.num_frames(); ++frame ) {
            float expected =
                firstSource.raw_ptrs()[channel][frame] * firstVolume;
            if ( secondSource && frame >= secondOutputStart ) {
                expected +=
                    secondSource
                        ->raw_ptrs()[channel][frame - secondOutputStart] *
                    secondVolume;
            }
            if ( std::abs(output.raw_ptrs()[channel][frame] - expected) >
                 SAMPLE_EPSILON ) {
                XERROR(
                    "Timeline mix mismatch: channel={}, frame={}, actual={}, "
                    "expected={}",
                    channel,
                    frame,
                    output.raw_ptrs()[channel][frame],
                    expected);
                return false;
            }
        }
    }
    return true;
}

/// @brief 验证负起点裁切、同文件多声部和 Seek 源帧恢复。
bool testOverlapNegativeStartAndSeek(
    const std::shared_ptr<ice::AudioTrack>& track)
{
    constexpr std::size_t              BLOCK_FRAMES = 32U;
    MMM::Audio::AudioTimelineMixerNode node(
        {
            {
                .eventId    = 20U,
                .sourceKey  = "same",
                .startFrame = 8,
                .volume     = 0.25F,
                .track      = track,
            },
            {
                .eventId    = 10U,
                .sourceKey  = "same",
                .startFrame = -16,
                .volume     = 0.5F,
                .track      = track,
            },
        },
        96,
        16U);

    if ( node.clipCount() != 2U ||
         node.timelineEndFrame() <
             static_cast<MMM::Audio::AudioTimelineFrame>(track->num_frames()) +
                 8 ) {
        XERROR("Timeline mixer did not preserve both overlapping clips");
        return false;
    }

    ice::AudioBuffer output(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    ice::AudioBuffer firstReference(ice::ICEConfig::internal_format,
                                    BLOCK_FRAMES);
    ice::AudioBuffer secondReference(ice::ICEConfig::internal_format,
                                     BLOCK_FRAMES - 8U);
    firstReference.clear();
    secondReference.clear();
    track->read(firstReference, 16U, BLOCK_FRAMES);
    track->read(secondReference, 0U, BLOCK_FRAMES - 8U);

    node.play();
    node.process(output);
    if ( node.positionFrame() !=
             static_cast<MMM::Audio::AudioTimelineFrame>(BLOCK_FRAMES) ||
         !verifyMixedRange(
             output, firstReference, &secondReference, 8U, 0.5F, 0.25F) ) {
        return false;
    }

    constexpr std::size_t SEEK_BLOCK_FRAMES = 8U;
    node.seek(20);
    ice::AudioBuffer seekOutput(ice::ICEConfig::internal_format,
                                SEEK_BLOCK_FRAMES);
    ice::AudioBuffer seekFirstReference(ice::ICEConfig::internal_format,
                                        SEEK_BLOCK_FRAMES);
    ice::AudioBuffer seekSecondReference(ice::ICEConfig::internal_format,
                                         SEEK_BLOCK_FRAMES);
    seekFirstReference.clear();
    seekSecondReference.clear();
    track->read(seekFirstReference, 36U, SEEK_BLOCK_FRAMES);
    track->read(seekSecondReference, 12U, SEEK_BLOCK_FRAMES);
    node.process(seekOutput);
    if ( !verifyMixedRange(seekOutput,
                           seekFirstReference,
                           &seekSecondReference,
                           0U,
                           0.5F,
                           0.25F) ) {
        XERROR("Timeline seek did not resume every overlapping source frame");
        return false;
    }

    node.pause();
    ice::AudioBuffer pausedOutput(ice::ICEConfig::internal_format,
                                  SEEK_BLOCK_FRAMES);
    node.process(pausedOutput);
    for ( std::size_t channel = 0U; channel < pausedOutput.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < pausedOutput.num_frames();
              ++frame ) {
            if ( pausedOutput.raw_ptrs()[channel][frame] != 0.0F ) {
                XERROR("Paused timeline emitted non-silent audio");
                return false;
            }
        }
    }
    return node.positionFrame() == 28 &&
           node.state() == MMM::Audio::AudioTimelinePlaybackState::Paused;
}

/// @brief 验证半开循环在 R 处截断并于每轮 L 重建交叠 voice。
bool testHalfOpenLoop(const std::shared_ptr<ice::AudioTrack>& track)
{
    MMM::Audio::AudioTimelineMixerNode node(
        {
            {
                .eventId    = 1U,
                .sourceKey  = "loop",
                .startFrame = 4,
                .volume     = 1.0F,
                .track      = track,
            },
        },
        32,
        32U);
    if ( !node.setLoop({ 4, 12 }) ) {
        XERROR("Timeline mixer rejected a valid half-open loop");
        return false;
    }
    node.seek(4);
    node.play();

    ice::AudioBuffer output(ice::ICEConfig::internal_format, 16U);
    ice::AudioBuffer reference(ice::ICEConfig::internal_format, 8U);
    reference.clear();
    track->read(reference, 0U, 8U);
    node.process(output);

    for ( std::size_t channel = 0U; channel < output.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < output.num_frames(); ++frame ) {
            const float expected = reference.raw_ptrs()[channel][frame % 8U];
            if ( std::abs(output.raw_ptrs()[channel][frame] - expected) >
                 SAMPLE_EPSILON ) {
                XERROR("Half-open timeline loop mismatch at frame {}", frame);
                return false;
            }
        }
    }

    return node.positionFrame() == 4 && node.epoch() >= 3U && !node.finished();
}

/// @brief 验证空资源安全过滤和自然结束状态。
bool testMissingResourceAndFinish()
{
    MMM::Audio::AudioTimelineMixerNode node(
        {
            {
                .eventId    = 1U,
                .sourceKey  = "missing",
                .startFrame = 0,
                .volume     = 1.0F,
                .track      = {},
            },
        },
        6,
        4U);
    if ( node.clipCount() != 0U ) {
        XERROR("Timeline mixer retained a missing audio resource");
        return false;
    }

    node.play();
    ice::AudioBuffer output(ice::ICEConfig::internal_format, 10U);
    node.process(output);
    if ( node.positionFrame() != 6 || !node.finished() ||
         node.state() != MMM::Audio::AudioTimelinePlaybackState::Paused ) {
        XERROR("Timeline mixer did not stop at the composite end frame");
        return false;
    }
    for ( std::size_t channel = 0U; channel < output.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < output.num_frames(); ++frame ) {
            if ( output.raw_ptrs()[channel][frame] != 0.0F ) return false;
        }
    }
    return true;
}

}  // namespace

/// @brief 运行实时多采样时间线混音测试。
int main(int argc, char** argv)
{
    if ( argc < 2 ) {
        XERROR("Usage: AudioTimelineMixerNodeTest <sample_path>");
        return 1;
    }

    const std::filesystem::path samplePath(argv[1]);
    ice::ThreadPool             threadPool(2);
    ice::AudioPool              audioPool;
    auto track = audioPool.get_or_load(threadPool, samplePath.string()).lock();
    if ( !track || track->num_frames() < 64U ) {
        XERROR("Failed to prepare timeline mixer test sample");
        return 1;
    }

    const bool passed = testOverlapNegativeStartAndSeek(track) &&
                        testHalfOpenLoop(track) &&
                        testMissingResourceAndFinish();
    return passed ? 0 : 1;
}
