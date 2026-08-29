#include "logic/MczAudioOriginAlignment.h"

#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace MMM::Logic
{
namespace
{

/// @brief 时间计算使用的毫秒容差。
constexpr double ALIGNMENT_TIME_EPSILON_MS = 1.0e-6;

/// @brief 对一个持久化时间戳应用统一相位平移。
/// @param timestamp 待修改的毫秒时间戳。
/// @param phaseMilliseconds 需要从时间戳中减去的相位。
void shiftTimestamp(double& timestamp, double phaseMilliseconds) noexcept
{
    timestamp -= phaseMilliseconds;
    if ( std::abs(timestamp) <= ALIGNMENT_TIME_EPSILON_MS ) timestamp = 0.0;
}

}  // namespace

MczAudioOriginAlignmentTiming calculateMczAudioOriginAlignmentTiming(
    const BeatMap& beatMap)
{
    MczAudioOriginAlignmentTiming result;
    const auto                    firstBpm =
        std::min_element(beatMap.m_timings.begin(),
                         beatMap.m_timings.end(),
                         [](const Timing& lhs, const Timing& rhs) {
                             if ( lhs.m_timingEffect != TimingEffect::BPM )
                                 return false;
                             if ( rhs.m_timingEffect != TimingEffect::BPM )
                                 return true;
                             return lhs.m_timestamp < rhs.m_timestamp;
                         });
    if ( firstBpm == beatMap.m_timings.end() ||
         firstBpm->m_timingEffect != TimingEffect::BPM ) {
        result.errorMessage = "谱面没有可用于音频原点对齐的 BPM 红线";
        return result;
    }
    if ( !std::isfinite(firstBpm->m_timestamp) ||
         !std::isfinite(firstBpm->m_bpm) || firstBpm->m_bpm <= 0.0 ) {
        result.errorMessage = "首个 BPM 红线的时间或 BPM 无效";
        return result;
    }

    const double beatLengthMilliseconds = 60000.0 / firstBpm->m_bpm;
    if ( !std::isfinite(beatLengthMilliseconds) ||
         beatLengthMilliseconds <= 0.0 ) {
        result.errorMessage = "首个 BPM 红线无法计算有效拍长";
        return result;
    }

    result.phaseMilliseconds =
        std::fmod(firstBpm->m_timestamp, beatLengthMilliseconds);
    if ( std::abs(result.phaseMilliseconds) <= ALIGNMENT_TIME_EPSILON_MS ||
         std::abs(std::abs(result.phaseMilliseconds) -
                  beatLengthMilliseconds) <= ALIGNMENT_TIME_EPSILON_MS ) {
        result.phaseMilliseconds = 0.0;
    }
    result.success = true;
    return result;
}

bool applyMczAudioOriginAlignment(
    BeatMap&                               beatMap,
    const std::unordered_set<std::string>& mainAudioReferences,
    double phaseMilliseconds, std::string& errorMessage)
{
    errorMessage.clear();
    if ( !std::isfinite(phaseMilliseconds) ) {
        errorMessage = "主音频原点对齐相位无效";
        return false;
    }
    if ( mainAudioReferences.empty() ) {
        errorMessage = "谱面没有可识别的非 OGG Main 自动采样";
        return false;
    }

    auto firstBpm =
        std::min_element(beatMap.m_timings.begin(),
                         beatMap.m_timings.end(),
                         [](const Timing& lhs, const Timing& rhs) {
                             if ( lhs.m_timingEffect != TimingEffect::BPM )
                                 return false;
                             if ( rhs.m_timingEffect != TimingEffect::BPM )
                                 return true;
                             return lhs.m_timestamp < rhs.m_timestamp;
                         });
    if ( firstBpm == beatMap.m_timings.end() ||
         firstBpm->m_timingEffect != TimingEffect::BPM ) {
        errorMessage = "谱面没有可用于音频原点对齐的 BPM 红线";
        return false;
    }

    AudioSampleEvent* pairedMainSample = nullptr;
    double            nearestMainTime = std::numeric_limits<double>::infinity();
    for ( auto& sample : beatMap.m_audioSamples ) {
        if ( !mainAudioReferences.contains(sample.m_audioResourceId) ) continue;
        const double absoluteTime = std::abs(sample.effectiveTimestamp());
        if ( absoluteTime < nearestMainTime ) {
            nearestMainTime  = absoluteTime;
            pairedMainSample = &sample;
        }
    }
    if ( pairedMainSample == nullptr ) {
        errorMessage = "谱面没有引用目标非 OGG Main 音频的自动采样";
        return false;
    }
    if ( nearestMainTime > ALIGNMENT_TIME_EPSILON_MS ) {
        errorMessage =
            "目标非 OGG Main 自动采样不在时间原点，无法安全裁切或补静音";
        return false;
    }
    for ( const auto& sample : beatMap.m_audioSamples ) {
        if ( &sample == pairedMainSample ||
             !mainAudioReferences.contains(sample.m_audioResourceId) ) {
            continue;
        }
        errorMessage =
            "谱面包含多个目标非 OGG Main 自动采样，无法安全生成单一对齐音频";
        return false;
    }

    // 只移动首 BPM 红线的整数拍部分。相同 BPM 下整数拍移动不改变网格相位，
    // 同时避免为了很长的前导时间裁掉或补入数拍音频。旧 Malody delay
    // 不得覆盖新的零相位，否则重新导出会再次引入歌曲偏移。
    firstBpm->m_timestamp = phaseMilliseconds;
    if ( auto malodyProperties = firstBpm->m_metadata.timing_properties.find(
             TimingMetadataType::MALODY);
         malodyProperties != firstBpm->m_metadata.timing_properties.end() ) {
        malodyProperties->second.erase("delay");
    }

    for ( auto& timing : beatMap.m_timings ) {
        shiftTimestamp(timing.m_timestamp, phaseMilliseconds);
    }
    for ( auto& note : beatMap.m_noteData.notes ) {
        shiftTimestamp(note.m_timestamp, phaseMilliseconds);
    }
    for ( auto& hold : beatMap.m_noteData.holds ) {
        shiftTimestamp(hold.m_timestamp, phaseMilliseconds);
    }
    for ( auto& flick : beatMap.m_noteData.flicks ) {
        shiftTimestamp(flick.m_timestamp, phaseMilliseconds);
    }
    for ( auto& polyline : beatMap.m_noteData.polylines ) {
        shiftTimestamp(polyline.m_timestamp, phaseMilliseconds);
    }
    for ( auto& sample : beatMap.m_audioSamples ) {
        if ( &sample == pairedMainSample ) continue;
        shiftTimestamp(sample.m_timestamp, phaseMilliseconds);
    }
    for ( auto& annotation : beatMap.m_annotations ) {
        shiftTimestamp(annotation.m_timestamp, phaseMilliseconds);
    }

    // 变换后的音频文件本身已在零点完成裁头或补静音，主 SOUND 不得再次携带
    // 原来的局部时间，否则 Malody 会重复应用相位。
    pairedMainSample->m_timestamp = 0.0;
    pairedMainSample->m_offsetMs  = 0;
    beatMap.sync();
    return true;
}

}  // namespace MMM::Logic
