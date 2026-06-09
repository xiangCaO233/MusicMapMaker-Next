#include "audio/AudioManager.h"
#include "log/colorful-log.h"

#include <cmath>
#include <memory>
#include <vector>

#include <ice/core/MixBus.hpp>
#include <ice/core/SourceNode.hpp>
#include <ice/core/effect/GraphicEqualizer.hpp>

namespace MMM::Audio
{
/// @brief 为主音轨创建或替换图形均衡器。
/// @param preset 目标 EQ 预设。
void AudioManager::createMainTrackEQ(EQPreset preset)
{
    if ( preset == EQPreset::None ) {
        destroyMainTrackEQ();
        return;
    }

    std::vector<double> freqs;
    if ( preset == EQPreset::TenBand ) {
        freqs = { 31.25,  62.5,   125.0,  250.0,  500.0,
                  1000.0, 2000.0, 4000.0, 8000.0, 16000.0 };
    } else if ( preset == EQPreset::FifteenBand ) {
        freqs = { 25.0,   40.0,   63.0,   100.0,   160.0,
                  250.0,  400.0,  630.0,  1000.0,  1600.0,
                  2500.0, 4000.0, 6300.0, 10000.0, 16000.0 };
    }

    auto newEQ = std::make_shared<ice::GraphicEqualizer>(freqs);

    // 如果当前正在播放 BGM，需要热插拔
    if ( m_bgmSource && m_preStretcherMixer ) {
        m_preStretcherMixer->remove_source(
            m_mainEQ ? std::static_pointer_cast<ice::IAudioNode>(m_mainEQ)
                     : std::static_pointer_cast<ice::IAudioNode>(m_bgmSource));

        newEQ->set_inputnode(m_bgmSource);
        m_preStretcherMixer->add_source(newEQ);
    }

    m_mainEQ       = std::move(newEQ);
    m_mainEQPreset = preset;
    XINFO("Main track EQ created with {} bands.", freqs.size());
}

/// @brief 销毁主音轨图形均衡器并恢复原始路由。
void AudioManager::destroyMainTrackEQ()
{
    if ( !m_mainEQ ) return;

    if ( m_bgmSource && m_preStretcherMixer ) {
        m_preStretcherMixer->remove_source(m_mainEQ);
        m_preStretcherMixer->add_source(m_bgmSource);
    }

    m_mainEQ.reset();
    m_mainEQPreset = EQPreset::None;
    XINFO("Main track EQ destroyed.");
}

/// @brief 设置主音轨 EQ 指定频段增益。
/// @param bandIndex 频段索引。
/// @param gainDb 增益，单位 dB。
void AudioManager::setMainTrackEQBandGain(size_t bandIndex, float gainDb)
{
    if ( m_mainEQ ) {
        m_mainEQ->set_band_gain_db(bandIndex, gainDb);
    }
}

/// @brief 获取主音轨 EQ 指定频段增益。
/// @param bandIndex 频段索引。
/// @return 增益，单位 dB。
float AudioManager::getMainTrackEQBandGain(size_t bandIndex) const
{
    if ( m_mainEQ ) {
        return static_cast<float>(m_mainEQ->get_band_gain_db(bandIndex));
    }
    return 0.0f;
}

/// @brief 设置主音轨 EQ 指定频段 Q 值。
/// @param bandIndex 频段索引。
/// @param q Q 值。
void AudioManager::setMainTrackEQBandQ(size_t bandIndex, float q)
{
    if ( m_mainEQ ) {
        m_mainEQ->set_band_q_factor(bandIndex, q);
    }
}

/// @brief 获取主音轨 EQ 指定频段 Q 值。
/// @param bandIndex 频段索引。
/// @return Q 值。
float AudioManager::getMainTrackEQBandQ(size_t bandIndex) const
{
    if ( m_mainEQ ) {
        return static_cast<float>(m_mainEQ->get_band_q_factor(bandIndex));
    }
    return 1.414f;  // 默认 Q 值 (sqrt(2))
}

/// @brief 获取主音轨 EQ 频段数量。
/// @return 频段数量。
size_t AudioManager::getMainTrackEQBandCount() const
{
    if ( m_mainEQ ) {
        return m_mainEQ->get_band_count();
    }
    return 0;
}

/// @brief 获取主音轨 EQ 指定频段中心频率。
/// @param bandIndex 频段索引。
/// @return 中心频率，单位 Hz。
float AudioManager::getMainTrackEQBandFrequency(size_t bandIndex) const
{
    if ( m_mainEQ ) {
        return static_cast<float>(m_mainEQ->get_band_frequency(bandIndex));
    }
    return 0.0f;
}

/// @brief 获取主音轨 EQ 是否启用。
/// @return 启用时返回 true。
bool AudioManager::isMainTrackEQEnabled() const
{
    return m_mainEQ != nullptr;
}

/// @brief 获取主音轨 EQ 当前预设。
/// @return 当前 EQ 预设。
EQPreset AudioManager::getMainTrackEQPreset() const
{
    return m_mainEQPreset;
}

/// @brief 获取主音轨 EQ 在指定频率处的幅频响应。
/// @param frequency 频率，单位 Hz。
/// @return 响应增益，单位 dB。
float AudioManager::getMainTrackEQResponse(float frequency) const
{
    if ( m_mainEQ ) {
        double mag = m_mainEQ->get_total_magnitude_response(
            static_cast<double>(frequency));
        if ( mag <= 1e-6 ) return -120.0f;  // 避免 log10(0)
        return static_cast<float>(20.0 * std::log10(mag));
    }
    return 0.0f;
}

}  // namespace MMM::Audio
