#include "audio/AudioTimelineResourceProcessor.h"
#include "audio/AudioTimelineMixerNode.h"
#include "mmm/project/AudioResource.h"

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

/**
 * @file AudioTimelineResourceProcessor.cpp
 * @brief 在资源加载阶段把持久化变速、变调和 EQ 烘焙为不可变 PCM。
 *
 * 本文件只运行在低频资源准备路径。实时混音只读取 PreparedTimelineAudio，
 * 不执行 Rubber Band、滤波器构造、JSON 序列化或动态容器扩容。
 *
 * 资源级配置分为两类：
 *
 * - playbackSpeed、playbackPitch 与 EQ 改变资源 PCM，需要进入缓存键；
 * - volume 与 muted 属于时间线片段增益，不写入 PCM，也不进入处理缓存键。
 *
 * Rubber Band 离线模式使用 study/process 两遍流程。第一遍分析完整输入，
 * 第二遍产生输出，并在每个分块后排空可用帧。声道指针只在分块期间借用
 * 自有 PCM，最终结果重新封装成只读 PreparedTimelineAudio。
 * 处理结果不写回 AudioTrack；原始解码缓存仍可被中性配置或其它处理组合复用。
 * 缓存键同时包含资源路径和规范前的持久化配置，避免不同项目参数错误共享。
 * 实际 DSP 入口仍会再次规范化数值，保证旧缓存键不会绕过安全边界。
 * 这些操作均可能线性遍历完整资源，严禁从音频回调或逐帧更新路径调用。
 */

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
/// @param speed 持久化配置值，可能来自旧项目或手工编辑 JSON。
/// @return 有限值限制在 [0.25, 2.0]；非有限值回退为 1.0。
[[nodiscard]] double normalizedResourceSpeed(float speed) noexcept
{
    // 先排除 NaN/Inf，避免 std::clamp 的比较语义传播异常值。
    return std::isfinite(speed) ? std::clamp(static_cast<double>(speed),
                                             MIN_RESOURCE_SPEED,
                                             MAX_RESOURCE_SPEED)
                                : 1.0;
}

/// @brief 规范化资源音高。
/// @param pitch 以半音为单位的配置值。
/// @return 有限值限制在 [-24, 24]；非有限值回退为零半音。
[[nodiscard]] double normalizedResourcePitch(float pitch) noexcept
{
    // 两个八度边界限制离线处理成本，并防止无效配置进入 Rubber Band。
    return std::isfinite(pitch) ? std::clamp(static_cast<double>(pitch),
                                             MIN_RESOURCE_PITCH,
                                             MAX_RESOURCE_PITCH)
                                : 0.0;
}

/// @brief 获取项目 EQ 预设对应的中心频率。
/// @param preset 0 表示禁用，1 为十段，2 为十五段。
/// @return 静态频率表的只读视图；未知预设返回空视图。
[[nodiscard]] std::span<const double> equalizerFrequencies(int preset) noexcept
{
    // 频率表拥有静态生命周期，返回 span 不需要分配或复制。
    static constexpr double TEN_BAND_FREQUENCIES[] = {
        31.25,  62.5,   125.0,  250.0,  500.0,
        1000.0, 2000.0, 4000.0, 8000.0, 16000.0,
    };
    static constexpr double FIFTEEN_BAND_FREQUENCIES[] = {
        25.0,   40.0,   63.0,   100.0,  160.0,  250.0,   400.0,   630.0,
        1000.0, 1600.0, 2500.0, 4000.0, 6300.0, 10000.0, 16000.0,
    };

    // 未识别预设按无 EQ 处理，避免猜测用户期望的段数。
    if ( preset == 1 ) return TEN_BAND_FREQUENCIES;
    if ( preset == 2 ) return FIFTEEN_BAND_FREQUENCIES;
    return {};
}

/**
 * @brief 将资源 EQ 离线应用到自有 PCM。
 *
 * 每个声道建立独立滤波器状态，所有频段按固定中心频率串联处理。缺失的 gain
 * 使用 0dB，缺失的 Q 使用 sqrt(2)；异常输入先恢复安全默认值再限制范围。
 *
 * @param channels 可变的平面声道 PCM，处理后仍保持原帧数和声道数。
 * @param config 资源级 EQ 开关、预设、增益与 Q 配置。
 */
void applyResourceEqualizer(std::vector<std::vector<float>>& channels,
                            const AudioTrackConfig&          config)
{
    // 开关关闭或预设未知时保持 PCM 完全不变。
    if ( !config.eqEnabled ) return;
    const auto frequencies = equalizerFrequencies(config.eqPreset);
    if ( frequencies.empty() ) return;

    // 滤波器系数依赖引擎内部采样率，无效格式下不能安全初始化。
    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( sampleRate <= 0.0 ) return;

    // 各声道不能共享有状态滤波器，否则左右声道历史会相互串扰。
    for ( auto& channel : channels ) {
        std::vector<ice::BiquadFilter> filters(frequencies.size());
        for ( std::size_t band = 0U; band < frequencies.size(); ++band ) {
            // 配置数组可短于预设段数，未提供频段保持中性增益。
            const float configuredGain = band < config.eqBandGains.size()
                                             ? config.eqBandGains[band]
                                             : 0.0F;
            // Q 数组同样允许省略，默认值提供稳定且不过窄的峰值响应。
            const float configuredQ =
                band < config.eqBandQs.size()
                    ? config.eqBandQs[band]
                    : static_cast<float>(std::numbers::sqrt2);
            // 增益限制到可控范围，NaN/Inf 回退为 0dB。
            const double gain =
                std::isfinite(configuredGain)
                    ? std::clamp(
                          static_cast<double>(configuredGain), -24.0, 24.0)
                    : 0.0;
            // Q 必须为正；极端有限值再限制到滤波器可用区间。
            const double q =
                std::isfinite(configuredQ) && configuredQ > 0.0F
                    ? std::clamp(static_cast<double>(configuredQ), 0.05, 50.0)
                    : std::numbers::sqrt2;
            filters[band].set_peaking(sampleRate, frequencies[band], q, gain);
        }
        // 串联应用所有频段，每个滤波器原地处理完整声道。
        for ( auto& filter : filters ) {
            filter.process(channel.data(), channel.size());
        }
    }
}

/**
 * @brief 从 Rubber Band 取出当前全部可用帧并追加到结果。
 * @param stretcher 已完成部分 process 的离线变换器。
 * @param output 按声道累计的最终 PCM。
 * @param retrieveChannels 复用的临时声道缓冲区。
 * @param retrievePointers 指向临时声道缓冲区的 C API 指针数组。
 *
 * available 可能在一次 retrieve 后仍为正，因此循环直到内部输出排空。
 */
void drainRubberBand(RubberBand::RubberBandStretcher& stretcher,
                     std::vector<std::vector<float>>& output,
                     std::vector<std::vector<float>>& retrieveChannels,
                     std::vector<float*>&             retrievePointers)
{
    // 每轮重新读取 available，因为 retrieve 数量可能小于先前报告值。
    for ( int available = stretcher.available(); available > 0;
          available     = stretcher.available() ) {
        const auto frames = static_cast<std::size_t>(available);
        // 所有声道使用相同帧容量，保持平面 PCM 长度一致。
        for ( std::size_t channel = 0U; channel < retrieveChannels.size();
              ++channel ) {
            retrieveChannels[channel].resize(frames);
            retrievePointers[channel] = retrieveChannels[channel].data();
        }
        // retrievePointers 仅在本轮 resize 完成后获取，避免扩容使地址失效。
        const std::size_t retrieved =
            stretcher.retrieve(retrievePointers.data(), frames);
        // 只追加实际取回的前缀，不假设 available 全部一次返回。
        for ( std::size_t channel = 0U; channel < output.size(); ++channel ) {
            output[channel].insert(output[channel].end(),
                                   retrieveChannels[channel].begin(),
                                   retrieveChannels[channel].begin() +
                                       static_cast<std::ptrdiff_t>(retrieved));
        }
    }
}

/**
 * @brief 通过 Rubber Band 两遍离线模式应用独立变速和变调。
 * @param source 等长的平面只读声道 PCM。
 * @param playbackSpeed 播放倍率，已在调用侧规范化。
 * @param pitchSemitones 音高偏移半音数，已在调用侧规范化。
 * @return 处理后的自有平面 PCM；空输入返回空结果。
 *
 * timeRatio 是输出时长相对输入时长，因此等于 speed 的倒数；pitchScale
 * 使用十二平均律指数换算。处理固定禁用内部线程，避免与项目线程池争用。
 */
[[nodiscard]] std::vector<std::vector<float>> stretchResourceOffline(
    const std::vector<std::vector<float>>& source, double playbackSpeed,
    double pitchSemitones)
{
    // 第一声道为空代表资源没有可处理帧；所有声道由准备层保证等长。
    if ( source.empty() || source.front().empty() ) return {};
    const std::size_t channelCount = source.size();
    const std::size_t sourceFrames = source.front().size();
    // 2 倍播放速度产生约一半帧数；升降调独立使用 pitchScale。
    const double timeRatio  = 1.0 / playbackSpeed;
    const double pitchScale = std::pow(2.0, pitchSemitones / 12.0);
    // 离线高质量模式允许两遍分析，ChannelsTogether 保持多声道相位关系。
    const auto options =
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
    // 提前声明总帧数和最大块大小，帮助 Rubber Band 规划内部缓冲。
    stretcher.setExpectedInputDuration(sourceFrames);
    stretcher.setMaxProcessSize(OFFLINE_PROCESS_BLOCK_FRAMES);

    // study 第一遍只分析输入，不应尝试读取输出。
    std::vector<const float*> inputPointers(channelCount);
    for ( std::size_t offset = 0U; offset < sourceFrames; ) {
        const std::size_t frameCount =
            std::min(OFFLINE_PROCESS_BLOCK_FRAMES, sourceFrames - offset);
        for ( std::size_t channel = 0U; channel < channelCount; ++channel ) {
            inputPointers[channel] = source[channel].data() + offset;
        }
        // finalBlock 只在最后一块为 true，使分析器完成全局时域决策。
        const bool finalBlock = offset + frameCount == sourceFrames;
        stretcher.study(inputPointers.data(), frameCount, finalBlock);
        offset += frameCount;
    }

    // long double 降低巨大资源乘除时的中间精度损失，并在窄化前检查上界。
    const auto expectedFramesLongDouble =
        static_cast<long double>(sourceFrames) *
        static_cast<long double>(timeRatio);
    const auto maximumSize =
        static_cast<long double>(std::numeric_limits<std::size_t>::max());
    // reserve 仅是容量提示；真实输出长度仍以 Rubber Band retrieve 为准。
    const std::size_t expectedFrames =
        expectedFramesLongDouble >= maximumSize
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(
                  std::ceil(std::max(expectedFramesLongDouble, 0.0L)));
    std::vector<std::vector<float>> output(channelCount);
    for ( auto& channel : output ) {
        channel.reserve(expectedFrames);
    }
    // 临时取回缓冲跨所有 process 块复用，避免每次重新建立声道容器。
    std::vector<std::vector<float>> retrieveChannels(channelCount);
    std::vector<float*>             retrievePointers(channelCount);

    // process 第二遍产生输出，每块后立即排空以限制内部积压。
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
    // finalBlock 后仍可能延迟生成尾部帧，循环外再排空一次。
    drainRubberBand(stretcher, output, retrieveChannels, retrievePointers);
    return output;
}

/// @brief 将只读准备数据复制为离线 DSP 的自有声道。
/// @param source 已准备的只读完整缓存 PCM。
/// @return 可由 EQ 和 Rubber Band 原地修改的平面声道副本。
[[nodiscard]] std::vector<std::vector<float>> copyPreparedChannels(
    const PreparedTimelineAudio& source)
{
    // 按声道复制保持平面布局，不让离线 DSP 修改共享解码缓存。
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
    // 仅包含会改变 PCM 内容的字段；volume/muted 由时间线片段实时应用。
    const nlohmann::json processingConfig{
        { "playbackSpeed", config.playbackSpeed },
        { "playbackPitch", config.playbackPitch },
        { "eqEnabled", config.eqEnabled },
        { "eqPreset", config.eqPreset },
        { "eqBandGains", config.eqBandGains },
        { "eqBandQs", config.eqBandQs },
    };
    // NUL 分隔路径和 JSON，避免路径后缀与配置前缀连接产生歧义。
    std::string key(filePath);
    key.push_back('\0');
    key.append(processingConfig.dump());
    return key;
}

std::shared_ptr<const PreparedTimelineAudio> prepareAudioTimelineResource(
    const std::shared_ptr<ice::AudioTrack>& track,
    const AudioTrackConfig&                 config)
{
    // 首先把 AudioTrack 封装成不可变来源；解码或缓存不可用时显式失败。
    const auto source = PreparedTimelineAudio::fromTrack(track);
    if ( !source ) return {};

    // 所有外部配置先规范化，后续 DSP 不再处理 NaN、Inf 或越界值。
    const double speed = normalizedResourceSpeed(config.playbackSpeed);
    const double pitch = normalizedResourcePitch(config.playbackPitch);
    // 开启 EQ 但预设未知等同于无需处理，不复制完整 PCM。
    const bool needsEqualizer =
        config.eqEnabled && !equalizerFrequencies(config.eqPreset).empty();
    const bool needsStretch =
        std::abs(speed - 1.0) > 1.0e-6 || std::abs(pitch) > 1.0e-6;
    // 中性配置直接共享原缓存，避免一次无意义的全资源复制。
    if ( !needsEqualizer && !needsStretch ) return source;

    // 离线处理需要整个不可变 PCM；流式页不能作为稳定声道视图使用。
    // AudioManager 在加载阶段已为这些资源选择完整缓存，外部误用则显式失败。
    if ( source->channel(0).empty() ) return {};
    // 先 EQ 再变速变调，保持资源处理链的固定顺序和缓存可复现性。
    auto channels = copyPreparedChannels(*source);
    applyResourceEqualizer(channels, config);
    if ( needsStretch ) {
        channels = stretchResourceOffline(channels, speed, pitch);
    }
    // 保留原 AudioTrack 所有权，使资源身份与处理后的 PCM 生命周期一致。
    return PreparedTimelineAudio::fromOwnedChannels(std::move(channels), track);
}

}  // namespace MMM::Audio
