#include "audio/AudioTimelineResourceProcessor.h"

#include "audio/AudioTimelineMixerNode.h"
#include "log/colorful-log.h"
#include "mmm/project/AudioResource.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <ice/manage/AudioPool.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/thread/ThreadPool.hpp>

/**
 * @file AudioTimelineResourceProcessorTest.cpp
 * @brief 验证资源级离线 DSP 与时间线片段增益的职责边界。
 *
 * 测试加载一段真实音频为完整 PCM，并分别应用音量/静音、播放倍率、音高和
 * 图形均衡器配置。资源处理只允许烘焙会改变 PCM 形态的 speed、pitch、EQ；
 * volume 与 muted 必须留给时间线片段混音，保证同一处理缓存可被不同事件
 * 以不同增益复用。
 *
 * Rubber Band 输出帧数允许 1% 或八帧的离线算法容差；音高和 EQ 则通过
 * 公共区间最大样本差证明内容确实改变。所有 EQ 结果还需保持有限值。
 */

namespace
{

/// @brief 判断两个帧数是否在离线变换允许的相对误差范围内。
/// @param actual Rubber Band 实际输出帧数。
/// @param expected 按输入长度与 time ratio 计算的理论帧数。
/// @return 差值不超过八帧或理论值 1% 中较大者时返回 true。
bool frameCountNear(std::size_t actual, long double expected)
{
    // 短音频使用固定八帧容差，长音频使用比例容差吸收算法尾部差异。
    const auto tolerance = std::max<long double>(8.0L, expected * 0.01L);
    return std::abs(static_cast<long double>(actual) - expected) <= tolerance;
}

/// @brief 计算两个首声道公共区间的最大绝对样本差。
/// @param lhs 第一份准备后的 PCM。
/// @param rhs 作为比较基准的 PCM。
/// @return 两者公共帧范围内的最大绝对差值。
float maximumCommonDifference(const MMM::Audio::PreparedTimelineAudio& lhs,
                              const MMM::Audio::PreparedTimelineAudio& rhs)
{
    // 仅比较公共前缀，让变速导致的帧数变化不会越界访问。
    const auto lhsChannel = lhs.channel(0U);
    const auto rhsChannel = rhs.channel(0U);
    const auto frameCount = std::min(lhsChannel.size(), rhsChannel.size());
    float      difference = 0.0F;
    // 最大差用于证明处理发生，不承担音质或频谱正确性的完整评价。
    for ( std::size_t frame = 0U; frame < frameCount; ++frame ) {
        difference = std::max(difference,
                              std::abs(lhsChannel[frame] - rhsChannel[frame]));
    }
    return difference;
}

/**
 * @brief 验证音量和静音保持为片段增益，不被烘焙进资源 PCM。
 * @param track 已加载的真实音频轨。
 * @return 准备结果与原始 PCM 帧数、样本完全相同时返回 true。
 *
 * 同时设置 volume=0 与 muted=true，若资源处理错误消费任一字段，输出 PCM
 * 会变为静音。正确实现应直接复用或无损保留原资源数据。
 */
bool testVolumeAndMuteRemainSeparate(
    const std::shared_ptr<ice::AudioTrack>& track)
{
    // 选择最极端的静音配置，使错误烘焙能够通过样本差立即暴露。
    MMM::AudioTrackConfig config;
    config.volume = 0.0F;
    config.muted  = true;
    // prepared 走生产资源处理入口，reference 直接封装同一原始轨道。
    const auto prepared =
        MMM::Audio::prepareAudioTimelineResource(track, config);
    const auto reference = MMM::Audio::PreparedTimelineAudio::fromTrack(track);
    // 帧数和最大样本差同时检查，避免空前缀比较产生假阳性。
    if ( !prepared || !reference ||
         prepared->numFrames() != reference->numFrames() ||
         maximumCommonDifference(*prepared, *reference) != 0.0F ) {
        XERROR("Resource volume or mute was baked into prepared PCM");
        return false;
    }
    return true;
}

/**
 * @brief 验证资源播放倍率按倒数关系改变该资源的 PCM 时长。
 * @param track 已加载的真实音频轨。
 * @return 2 倍速约为半长且 0.5 倍速约为双倍长度时返回 true。
 *
 * 两个方向共用同一 reference，证明配置只作用于各自准备结果，不会原地
 * 修改共享 AudioTrack 或污染下一次资源处理。
 */
bool testResourcePlaybackSpeed(const std::shared_ptr<ice::AudioTrack>& track)
{
    // 原始准备结果提供理论长度基准，失败时不继续构造比例断言。
    const auto reference = MMM::Audio::PreparedTimelineAudio::fromTrack(track);
    if ( !reference ) return false;

    // 快速和慢速分别创建独立配置，避免复用对象残留字段。
    MMM::AudioTrackConfig fastConfig;
    fastConfig.playbackSpeed = 2.0F;
    const auto fast =
        MMM::Audio::prepareAudioTimelineResource(track, fastConfig);

    MMM::AudioTrackConfig slowConfig;
    slowConfig.playbackSpeed = 0.5F;
    const auto slow =
        MMM::Audio::prepareAudioTimelineResource(track, slowConfig);

    // Rubber Band 允许少量尾帧差异，因此使用 frameCountNear 而非精确相等。
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

/**
 * @brief 验证资源音高改变内容但基本保持时长。
 * @param track 已加载的真实音频轨。
 * @return 升高十二半音后长度近似相同且样本确实变化时返回 true。
 *
 * 十二半音对应两倍频率比例。独立 pitch shift 不应同时应用 time stretch，
 * 因而输出帧数仍接近 reference；最大样本差只作为处理生效哨兵。
 */
bool testResourcePitch(const std::shared_ptr<ice::AudioTrack>& track)
{
    // reference 与 shifted 都保留到比较完成，确保共享源生命周期有效。
    const auto reference = MMM::Audio::PreparedTimelineAudio::fromTrack(track);
    MMM::AudioTrackConfig config;
    config.playbackPitch = 12.0F;
    const auto shifted =
        MMM::Audio::prepareAudioTimelineResource(track, config);
    // 同时要求近似等长和非零内容差，防止处理被完全跳过或错误改变速度。
    if ( !reference || !shifted ||
         !frameCountNear(shifted->numFrames(), reference->numFrames()) ||
         maximumCommonDifference(*shifted, *reference) < 1.0e-4F ) {
        XERROR("Per-resource pitch was not applied independently");
        return false;
    }
    return true;
}

/**
 * @brief 验证资源图形均衡器在离线阶段应用且结果保持有限。
 * @param track 已加载的真实音频轨。
 * @return 帧数不变、内容变化且所有声道样本有限时返回 true。
 *
 * 十段增益包含正负变化，Q 数量与频段一致。测试不判断具体频率响应曲线，
 * 只锁定 EQ 被应用、不会改变时长，也不会产生 NaN 或 Inf 的基础契约。
 */
bool testResourceEqualizer(const std::shared_ptr<ice::AudioTrack>& track)
{
    // 使用交替强增益配置，让处理后的样本差明显高于浮点噪声阈值。
    const auto reference = MMM::Audio::PreparedTimelineAudio::fromTrack(track);
    MMM::AudioTrackConfig config;
    config.eqEnabled   = true;
    config.eqPreset    = 1;
    config.eqBandGains = { 12.0F, 9.0F,  6.0F,   3.0F, -3.0F,
                           -6.0F, -9.0F, -12.0F, 6.0F, 9.0F };
    config.eqBandQs.assign(config.eqBandGains.size(), 1.2F);
    const auto equalized =
        MMM::Audio::prepareAudioTimelineResource(track, config);
    // EQ 是逐样本滤波，不允许改变资源帧数。
    if ( !reference || !equalized ||
         equalized->numFrames() != reference->numFrames() ||
         maximumCommonDifference(*equalized, *reference) < 1.0e-5F ) {
        XERROR("Per-resource equalizer was not applied independently");
        return false;
    }

    // 遍历全部声道与帧，确保任一滤波器状态异常都能被发现。
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
    // 真实音频路径由 CMake 测试注册传入，不在源码目录生成夹具。
    if ( argc < 2 ) {
        XERROR("Usage: AudioTimelineResourceProcessorTest <sample_path>");
        return 1;
    }

    // 独立线程池与 AudioPool 模拟生产加载路径，并在进程结束时统一释放。
    const std::filesystem::path samplePath(argv[1]);
    ice::ThreadPool             threadPool(2);
    ice::AudioPool              audioPool;
    auto track = audioPool.get_or_load(threadPool, samplePath.string()).lock();
    // 至少 64 帧确保时长比例和样本差比较具有有效公共区间。
    if ( !track || track->num_frames() < 64U ) {
        XERROR("Failed to prepare resource DSP test sample");
        return 1;
    }

    // 短路执行避免前一场景破坏前置条件后继续产生级联错误。
    const bool passed = testVolumeAndMuteRemainSeparate(track) &&
                        testResourcePlaybackSpeed(track) &&
                        testResourcePitch(track) &&
                        testResourceEqualizer(track);
    return passed ? 0 : 1;
}
