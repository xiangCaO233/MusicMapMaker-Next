#include "audio/OfflineAnalysisEQ.h"
#include "audio/AudioManager.h"

#include <ice/core/effect/GraphicEqualizer.hpp>
#include <ice/manage/AudioBuffer.hpp>

#include <cstddef>

namespace MMM::Audio
{
// 分析缓存根据重新计算请求刷新，播放参数变动不会即时失效旧缓存。
// 把参数捕获和构造过程分开，可在控制侧取得完整设置，在线程池内执行数字滤波。
// 频段决定串联顺序。声道使用相同的系数，各自持有不同的输入输出历史。
// 采样率由 PCM 格式提供，不能用原始媒体的采样率替代转换后的分析采样率。
//
/// @brief 按稳定的频段索引读取当前主轨设置，作为整次分析的参数快照。
OfflineAnalysisEQSettings captureOfflineAnalysisEQ(const AudioManager& audio)
{
    if ( !audio.isMainTrackEQEnabled() ) return {};

    // 行为与此前的各面板一致，快照读取的是主轨节点实际生效的参数。
    // 配置中的预设可能尚未提交到播放图，不能直接从编辑器配置构造滤波器。
    // 同一控制线程也负责修改主轨 EQ，快照期间不会切换预设或频段。
    const auto                count = audio.getMainTrackEQBandCount();
    OfflineAnalysisEQSettings settings;
    settings.reserve(count);
    for ( std::size_t i = 0; i < count; ++i ) {
        settings.push_back({ audio.getMainTrackEQBandFrequency(i),
                             audio.getMainTrackEQBandGain(i),
                             audio.getMainTrackEQBandQ(i) });
    }
    return settings;
}

/// @brief 计算一次系数，再复制到各声道的独立滤波器链。
/// @details 离线任务独占滤波链，不需要逐段向实时音频节点发布控制状态。
/// @pre sampleRate 为分析 PCM 实际的正采样率，channels 与待处理缓冲一致。
OfflineAnalysisEQ::OfflineAnalysisEQ(const OfflineAnalysisEQSettings& settings,
                                     double sampleRate, std::uint16_t channels)
{
    if ( settings.empty() ) return;

    std::vector<ice::BiquadFilter> chain(settings.size());
    for ( std::size_t i = 0; i < settings.size(); ++i ) {
        const auto& band = settings[i];
        // 保留引擎 setter 对非正 Q 的回退规则，即保持频段的初始 Q。
        const auto q = band.q <= 0.0F ? ice::EQBandOptions{}.q_factor : band.q;
        chain[i].set_peaking(sampleRate, band.frequency, q, band.gain);
    }

    // 计算量随频段数线性增长，两声道无需重复计算同一组三角函数。
    // 系数相同，复制尚未处理 PCM 的滤波器可得到零历史且相互独立的声道。
    m_channelChains.assign(channels, chain);
}

/// @brief 在一个工作块内按频段顺序处理各声道的有效采样。
/// @warning 每块执行，不访问实时图或可变的控制参数。
/// @note 连续调用必须按需保留相同历史，即使相邻工作块的长度不同。
void OfflineAnalysisEQ::process(ice::AudioBuffer& buffer)
{
    if ( m_channelChains.size() != buffer.num_channels() ||
         buffer.num_frames() == 0 ) {
        // 空频段快照或无有效 PCM 时不访问采样平面；格式仍由分析任务负责。
        return;
    }

    auto* const planes = buffer.raw_ptrs();
    for ( std::size_t ch = 0; ch < m_channelChains.size(); ++ch ) {
        for ( auto& filter : m_channelChains[ch] ) {
            // 相邻工作块共享声道历史，声道之间互不影响。
            filter.process(planes[ch], buffer.num_frames());
        }
    }
}
}  // namespace MMM::Audio
