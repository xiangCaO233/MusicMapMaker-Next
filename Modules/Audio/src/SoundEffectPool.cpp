#include "audio/SoundEffectPool.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <ice/core/IAudioNode.hpp>
#include <ice/core/MixBus.hpp>
#include <ice/core/SourceNode.hpp>
#include <ice/core/effect/TimeStretcher.hpp>
#include <ice/manage/AudioTrack.hpp>

namespace MMM::Audio
{

/// @brief 为单个音效播放实例应用固定或线性变化的左右声道增益。
class StereoGainNode final : public ice::IAudioNode
{
public:
    /// @brief 构造与播放实例生命周期绑定的双声道增益节点。
    /// @param input 已完成实例内变调处理的稳定输入节点。
    /// @param source 提供原始音效播放进度的稳定源节点。
    /// @param pool 音效结束后接收实例回收通知的所属池。
    StereoGainNode(std::shared_ptr<ice::IAudioNode> input,
                   std::shared_ptr<ice::SourceNode> source,
                   std::weak_ptr<SoundEffectPool>   pool)
        : m_input(std::move(input))
        , m_source(std::move(source))
        , m_pool(std::move(pool))
    {
    }

    /// @brief 为下一次播放配置双声道包络和预计预定等待帧数。
    /// @param envelope 本次播放的左右声道增益包络。
    /// @param scheduledDelayFrames 当前参考位置到目标播放位置的预计帧数。
    void prepare(const StereoGainEnvelope& envelope,
                 std::size_t               scheduledDelayFrames)
    {
        m_startLeft.store(std::clamp(envelope.startLeft, 0.0F, 1.0F),
                          std::memory_order_relaxed);
        m_startRight.store(std::clamp(envelope.startRight, 0.0F, 1.0F),
                           std::memory_order_relaxed);
        m_endLeft.store(std::clamp(envelope.endLeft, 0.0F, 1.0F),
                        std::memory_order_relaxed);
        m_endRight.store(std::clamp(envelope.endRight, 0.0F, 1.0F),
                         std::memory_order_relaxed);
        m_remainingDelayFrames.store(scheduledDelayFrames,
                                     std::memory_order_relaxed);
        m_audioStarted.store(false, std::memory_order_relaxed);
        m_inUse.store(true, std::memory_order_release);
    }

    /// @brief 停止当前包络并使节点返回静音。
    void deactivate()
    {
        m_inUse.store(false, std::memory_order_release);
        m_remainingDelayFrames.store(0U, std::memory_order_relaxed);
        m_audioStarted.store(false, std::memory_order_relaxed);
    }

    /// @brief 拉取单个音效实例并按原始采样进度应用左右声道增益。
    /// @param buffer 上游请求的音频缓冲。
    /// @warning SDL 音频回调热路径：每个活跃 HitEffect
    /// 每个缓冲周期执行，只允许原子读取与固定帧遍历；音效结束分支才允许
    /// weak_ptr 锁定以安全回收跨线程实例。
    void process(ice::AudioBuffer& buffer) override
    {
        if ( !m_inUse.load(std::memory_order_acquire) || !m_input ||
             !m_source ) {
            buffer.clear();
            return;
        }

        const std::size_t playPositionBefore = m_source->get_playpos();
        m_input->process(buffer);
        const std::size_t playPositionAfter = m_source->get_playpos();
        const std::size_t gainedFrames =
            playPositionAfter >= playPositionBefore
                ? playPositionAfter - playPositionBefore
                : 0U;
        const std::size_t totalFrames = m_source->num_frames();

        const std::size_t bufferFrames = buffer.num_frames();
        const std::size_t delayBefore =
            m_remainingDelayFrames.load(std::memory_order_relaxed);
        m_remainingDelayFrames.store(
            delayBefore > bufferFrames ? delayBefore - bufferFrames : 0U,
            std::memory_order_relaxed);

        if ( gainedFrames > 0U ) {
            const bool audioStarted =
                m_audioStarted.load(std::memory_order_relaxed);
            std::size_t outputOffset = 0U;
            if ( !audioStarted && gainedFrames < bufferFrames ) {
                if ( playPositionAfter < totalFrames ) {
                    outputOffset = bufferFrames - gainedFrames;
                } else if ( delayBefore > 0U && delayBefore < bufferFrames ) {
                    outputOffset = delayBefore;
                }
            }
            applyEnvelope(buffer,
                          outputOffset,
                          std::min(gainedFrames, bufferFrames - outputOffset),
                          playPositionBefore);
            m_audioStarted.store(true, std::memory_order_relaxed);
        }

        if ( totalFrames > 0U && playPositionAfter >= totalFrames &&
             !m_source->isplaying() &&
             m_inUse.exchange(false, std::memory_order_acq_rel) ) {
            if ( auto pool = m_pool.lock() ) {
                pool->releaseNode(m_source);
            }
        }
    }

private:
    /// @brief 对缓冲中对应原始音效采样的区域应用线性双声道增益。
    /// @param buffer 待修改的输出缓冲。
    /// @param outputOffset 有效音效在输出缓冲中的起始帧。
    /// @param frameCount 本次实际读取的音效帧数。
    /// @param sourceFrame 音效区域首帧对应的原始音效帧位置。
    void applyEnvelope(ice::AudioBuffer& buffer, std::size_t outputOffset,
                       std::size_t frameCount, std::size_t sourceFrame) const
    {
        if ( frameCount == 0U || buffer.num_channels() < 2U ) return;

        const float startLeft  = m_startLeft.load(std::memory_order_relaxed);
        const float startRight = m_startRight.load(std::memory_order_relaxed);
        const float endLeft    = m_endLeft.load(std::memory_order_relaxed);
        const float endRight   = m_endRight.load(std::memory_order_relaxed);
        if ( std::abs(startLeft - 1.0F) < 1e-6F &&
             std::abs(startRight - 1.0F) < 1e-6F &&
             std::abs(endLeft - 1.0F) < 1e-6F &&
             std::abs(endRight - 1.0F) < 1e-6F ) {
            return;
        }

        const std::size_t totalFrames = m_source->num_frames();
        const float       progressDivisor =
            totalFrames > 1U ? static_cast<float>(totalFrames - 1U) : 1.0F;
        const float firstProgress = std::clamp(
            static_cast<float>(sourceFrame) / progressDivisor, 0.0F, 1.0F);
        const float progressStep = 1.0F / progressDivisor;
        float leftGain  = startLeft + (endLeft - startLeft) * firstProgress;
        float rightGain = startRight + (endRight - startRight) * firstProgress;
        const float leftStep  = (endLeft - startLeft) * progressStep;
        const float rightStep = (endRight - startRight) * progressStep;

        float** samples = buffer.raw_ptrs();
        if ( !samples ) return;
        for ( std::size_t frame = 0U; frame < frameCount; ++frame ) {
            samples[0][outputOffset + frame] *=
                std::clamp(leftGain, 0.0F, 1.0F);
            samples[1][outputOffset + frame] *=
                std::clamp(rightGain, 0.0F, 1.0F);
            leftGain += leftStep;
            rightGain += rightStep;
        }
    }

    /// @brief 实例内变调器的稳定非空输入。
    std::shared_ptr<ice::IAudioNode> m_input;
    /// @brief 提供原始采样进度并由所属播放实例稳定持有的源节点。
    std::shared_ptr<ice::SourceNode> m_source;
    /// @brief 音效完成时用于安全取得所属池生命周期。
    /// @warning 仅在一次音效结束时锁定，避免音频回调持有悬空观察指针。
    std::weak_ptr<SoundEffectPool> m_pool;
    /// @brief 本次包络的起始左声道增益。
    /// @warning 逻辑线程写、音频线程读；由 m_inUse 的 release/acquire
    /// 发布后仅需 relaxed 访问。
    std::atomic<float> m_startLeft{ 1.0F };
    /// @brief 本次包络的起始右声道增益。
    /// @warning 逻辑线程写、音频线程读；由 m_inUse 的 release/acquire
    /// 发布后仅需 relaxed 访问。
    std::atomic<float> m_startRight{ 1.0F };
    /// @brief 本次包络的结束左声道增益。
    /// @warning 逻辑线程写、音频线程读；由 m_inUse 的 release/acquire
    /// 发布后仅需 relaxed 访问。
    std::atomic<float> m_endLeft{ 1.0F };
    /// @brief 本次包络的结束右声道增益。
    /// @warning 逻辑线程写、音频线程读；由 m_inUse 的 release/acquire
    /// 发布后仅需 relaxed 访问。
    std::atomic<float> m_endRight{ 1.0F };
    /// @brief 预计仍需等待的预定播放帧数，用于定位首个非静音缓冲。
    /// @warning 逻辑线程初始化、音频线程递减；原子访问用于安全处理停止与复用。
    std::atomic<std::size_t> m_remainingDelayFrames{ 0U };
    /// @brief 当前实例是否已经产出过音效采样。
    /// @warning 逻辑线程重置、音频线程更新；原子访问用于安全处理停止与复用。
    std::atomic_bool m_audioStarted{ false };
    /// @brief 当前实例是否已被池分配给一次播放。
    /// @warning 逻辑线程发布或停止、音频线程读取并在完成时清除；release/acquire
    /// 用于发布整组包络参数。
    std::atomic_bool m_inUse{ false };
};

SoundEffectPool::SoundEffectPool(std::shared_ptr<ice::AudioTrack> track)
    : m_track(std::move(track))
{
    m_localMixer = std::make_shared<ice::MixBus>();
}

SoundEffectPool::~SoundEffectPool()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    for ( auto& instance : m_allInstances ) {
        if ( m_localMixer ) {
            m_localMixer->remove_source(instance->channelMixer);
        }
        if ( instance->channelMixer ) {
            instance->channelMixer->remove_source(instance->stereoGainNode);
        }
    }
}

std::shared_ptr<ice::MixBus> SoundEffectPool::getMixer() const
{
    return m_localMixer;
}

std::shared_ptr<SoundEffectPool::SFXPlayInstance>
SoundEffectPool::createInstance()
{
    auto instance            = std::make_shared<SFXPlayInstance>();
    instance->source         = std::make_shared<ice::SourceNode>(m_track);
    instance->pitchStretcher = std::make_shared<ice::TimeStretcher>();
    instance->channelMixer   = std::make_shared<ice::MixBus>();
    instance->stereoGainNode = std::make_shared<StereoGainNode>(
        instance->pitchStretcher, instance->source, weak_from_this());
    instance->pitchStretcher->set_inputnode(instance->source);
    instance->channelMixer->add_source(instance->stereoGainNode);

    if ( m_localMixer ) {
        m_localMixer->add_source(instance->channelMixer);
    }
    return instance;
}

std::shared_ptr<SoundEffectPool::SFXPlayInstance>
SoundEffectPool::acquireInstance()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if ( m_readyQueue.empty() ) {
        auto instance = createInstance();
        m_allInstances.push_back(instance);
        return instance;
    }

    auto instance = m_readyQueue.front();
    m_readyQueue.pop_front();
    return instance;
}

void SoundEffectPool::init(int count)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    for ( int i = 0; i < count; ++i ) {
        auto instance = createInstance();
        m_allInstances.push_back(instance);
        m_readyQueue.push_back(instance);
    }
}

void SoundEffectPool::play(float volume)
{
    play(volume, 0.0);
}

void SoundEffectPool::play(float volume, double pitchSemitones)
{
    auto instance = acquireInstance();
    auto node     = instance ? instance->source : nullptr;

    if ( node ) {
        if ( instance->pitchStretcher ) {
            instance->pitchStretcher->set_pitch_semitones(pitchSemitones);
        }
        if ( instance->channelMixer ) {
            instance->channelMixer->set_channel_mode(
                ice::MixBusChannelMode::Stereo);
        }
        node->set_scheduled_start_frame(0);  // 确保没有残留的预定
        node->set_reference_pos_provider(std::function<size_t()>());
        node->set_playpos(static_cast<size_t>(0));
        node->setvolume(volume);
        if ( instance->stereoGainNode ) {
            instance->stereoGainNode->prepare(StereoGainEnvelope{}, 0U);
        }
        node->play();
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_latestNode = node;
        }
    }
}

void SoundEffectPool::playScheduled(float volume, size_t targetFrame,
                                    std::function<size_t()>   refProvider,
                                    const StereoGainEnvelope& stereoEnvelope,
                                    std::size_t scheduledDelayFrames)
{
    auto instance = acquireInstance();
    auto node     = instance ? instance->source : nullptr;

    if ( node ) {
        if ( instance->pitchStretcher ) {
            instance->pitchStretcher->set_pitch_semitones(0.0);
        }
        if ( instance->channelMixer ) {
            instance->channelMixer->set_channel_mode(
                ice::MixBusChannelMode::Stereo);
        }
        node->set_playpos(static_cast<size_t>(0));
        node->set_scheduled_start_frame(targetFrame);
        node->set_reference_pos_provider(std::move(refProvider));
        node->setvolume(volume);
        if ( instance->stereoGainNode ) {
            instance->stereoGainNode->prepare(stereoEnvelope,
                                              scheduledDelayFrames);
        }
        node->play();
    }
}

/// @brief 停止所有正在播放或预定的音效，并重置状态
void SoundEffectPool::stopAll()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    m_readyQueue.clear();

    for ( auto& instance : m_allInstances ) {
        if ( instance->stereoGainNode ) {
            instance->stereoGainNode->deactivate();
        }
        instance->source->pause();
        instance->source->set_playpos(static_cast<size_t>(0));
        instance->source->set_scheduled_start_frame(0);
        instance->source->set_reference_pos_provider(std::function<size_t()>());
        if ( instance->pitchStretcher ) {
            instance->pitchStretcher->set_pitch_semitones(0.0);
        }
        if ( instance->channelMixer ) {
            instance->channelMixer->set_channel_mode(
                ice::MixBusChannelMode::Stereo);
        }
        m_readyQueue.push_back(instance);
    }
    m_latestNode.reset();
}

void SoundEffectPool::releaseNode(std::shared_ptr<ice::SourceNode> node)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    auto                        instanceIt =
        std::find_if(m_allInstances.begin(),
                     m_allInstances.end(),
                     [&](const std::shared_ptr<SFXPlayInstance>& instance) {
                         return instance && instance->source == node;
                     });
    if ( instanceIt == m_allInstances.end() ) {
        return;
    }

    if ( std::find(m_readyQueue.begin(), m_readyQueue.end(), *instanceIt) ==
         m_readyQueue.end() ) {
        m_readyQueue.push_back(*instanceIt);
    }
    if ( m_latestNode == node ) {
        m_latestNode.reset();
    }
}

void SoundEffectPool::setVolume(float volume)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    m_volume = volume;
    // 注意：此处不直接更新 node 音量，因为我们不知道当前的全局音量和静音状态。
    // 实际音量更新应通过 updateEffectiveVolume 或在 play 时计算。
}

void SoundEffectPool::updateEffectiveVolume(float globalVolume, bool muted)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    float effectiveVolume = muted ? 0.0f : m_volume * globalVolume;
    for ( auto& instance : m_allInstances ) {
        instance->source->setvolume(effectiveVolume);
    }
}

double SoundEffectPool::getDuration() const
{
    if ( !m_track ) return 0.0;
    const auto samplerate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( samplerate <= 0 ) return 0.0;
    return static_cast<double>(m_track->num_frames()) / samplerate;
}

double SoundEffectPool::getLatestPlaybackTime() const
{
    std::shared_ptr<ice::SourceNode> latest;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        latest = m_latestNode;
    }

    if ( !latest ) return 0.0;

    const auto samplerate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( samplerate <= 0 ) return 0.0;
    return static_cast<double>(latest->get_playpos()) / samplerate;
}

bool SoundEffectPool::isPlaying() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if ( !m_latestNode ) return false;
    return m_latestNode->isplaying();
}

bool SoundEffectPool::isPaused() const
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if ( !m_latestNode ) return false;
    return !m_latestNode->isplaying() && (m_latestNode->get_playpos() > 0);
}

void SoundEffectPool::pause()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if ( m_latestNode ) {
        m_latestNode->pause();
    }
}

void SoundEffectPool::resume()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    if ( m_latestNode ) {
        m_latestNode->play();
    }
}

}  // namespace MMM::Audio
