#include "BackgroundSpectrumAnalyzer.h"
#include "audio/AudioManager.h"
#include "audio/AudioTimelineMixerNode.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include <ice/config/config.hpp>
#include <ice/core/MixBus.hpp>
#include <ice/core/effect/GraphicEqualizer.hpp>

namespace MMM::Audio
{
// 主轨 EQ 是时间线预览图中的可选处理节点，管理时遵循以下约束：
//
// - 频段表和滤波状态在控制线程创建并 prepare，音频回调只处理固定缓冲；
// - 热插拔通过 MixBus 原子替换来源，不能先断开旧路由再连接新节点；
// - 频谱采集若存在则位于 EQ 前，关闭 EQ 后仍恢复到同一上游节点；
// - m_mainEQ 只在路由替换成功后提交，失败时保留旧节点与预设；
// - UI 的频段参数读写直接转交 GraphicEqualizer 的线程安全控制接口。
// - 未创建 EQ 时 getter 返回中性默认值，setter 安全忽略；
// - 频响查询把接近零的线性幅值限制为可绘制的 -120 dB 下限；
// - 创建与销毁日志只在控制路径输出，不进入实时音频回调。
// - 预设枚举与中心频率表一一对应，不允许发布空频段的未知非 None 预设；
// - 所有公开查询在节点缺失时保持无副作用并返回可直接绘制的中性结果。

/// @brief 为复合时间线创建或替换全局预览图形均衡器。
/// @param preset 目标 EQ 预设。
///
/// None 复用销毁路径；其余预设先构造并预备完整新节点，再原子替换当前路由。
/// 十段与十五段中心频率采用固定音乐均衡器标准分布，不在运行时自动重排。
/// @warning 低频控制路径；会分配滤波状态并调整音频图，不在音频回调调用。
void AudioManager::createMainTrackEQ(EQPreset preset)
{
    if ( preset == EQPreset::None ) {
        destroyMainTrackEQ();
        return;
    }

    // 中心频率由预设完整给出，顺序即 UI 频段索引顺序。
    std::vector<double> freqs;
    if ( preset == EQPreset::TenBand ) {
        freqs = { 31.25,  62.5,   125.0,  250.0,  500.0,
                  1000.0, 2000.0, 4000.0, 8000.0, 16000.0 };
    } else if ( preset == EQPreset::FifteenBand ) {
        freqs = { 25.0,   40.0,   63.0,   100.0,   160.0,
                  250.0,  400.0,  630.0,  1000.0,  1600.0,
                  2500.0, 4000.0, 6300.0, 10000.0, 16000.0 };
    }

    // 在接入运行图之前完成 prepare，避免回调观察到未分配 scratch 的节点。
    auto newEQ = std::make_shared<ice::GraphicEqualizer>(freqs);
    newEQ->prepare(
        ice::ICEConfig::internal_format,
        std::max<std::size_t>(ice::ICEConfig::default_buffer_size, 1U));

    // 如果当前已加载复合时间线，需要热插拔全局预览 EQ。
    if ( m_audioTimelineNode && m_preStretcherMixer ) {
        std::shared_ptr<ice::IAudioNode> input = m_bgmSpectrumCapture;
        if ( !input ) input = m_audioTimelineNode;
        newEQ->set_inputnode(input);

        // replace_source 需要当前实际路由身份；没有旧 EQ 时当前路由就是 input。
        std::shared_ptr<ice::IAudioNode> currentRoute = m_mainEQ;
        if ( !currentRoute ) currentRoute = input;
        if ( !m_preStretcherMixer->replace_source(currentRoute, newEQ) ) {
            XERROR("Failed to preserve main timeline route while creating EQ.");
            return;
        }
    }

    // 路由切换完成后再发布成员，保证成员状态与音频图一致。
    m_mainEQ       = std::move(newEQ);
    m_mainEQPreset = preset;
    XINFO("Main track EQ created with {} bands.", freqs.size());
}

/// @brief 销毁复合时间线全局预览均衡器并恢复原始路由。
///
/// 先把 MixBus 来源从 EQ 换回频谱采集或时间线，再释放 EQ 所有权；替换失败时
/// 保留原路由，避免正在播放的预览链出现断音。
void AudioManager::destroyMainTrackEQ()
{
    if ( !m_mainEQ ) return;

    if ( m_audioTimelineNode && m_preStretcherMixer ) {
        std::shared_ptr<ice::IAudioNode> restoredRoute = m_bgmSpectrumCapture;
        if ( !restoredRoute ) restoredRoute = m_audioTimelineNode;
        if ( !m_preStretcherMixer->replace_source(m_mainEQ, restoredRoute) ) {
            XERROR(
                "Failed to preserve main timeline route while destroying EQ.");
            return;
        }
    }

    // 只有路由不再引用 EQ 后才清空管理器成员与预设状态。
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
        // 线性幅度先设下限，避免 log10(0) 产生负无穷污染绘图。
        double mag = m_mainEQ->get_total_magnitude_response(
            static_cast<double>(frequency));
        if ( mag <= 1e-6 ) return -120.0f;  // 避免 log10(0)
        return static_cast<float>(20.0 * std::log10(mag));
    }
    return 0.0f;
}

}  // namespace MMM::Audio
