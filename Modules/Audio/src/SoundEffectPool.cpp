#include "audio/SoundEffectPool.h"

#include <algorithm>
#include <ice/core/MixBus.hpp>
#include <ice/core/PlayCallBack.hpp>
#include <ice/core/SourceNode.hpp>
#include <ice/core/effect/TimeStretcher.hpp>
#include <ice/manage/AudioTrack.hpp>

namespace MMM::Audio
{

class SoundEffectPool::SFXPlayCallback : public ice::PlayCallBack
{
public:
    SFXPlayCallback(std::weak_ptr<SoundEffectPool> pool,
                    std::weak_ptr<ice::SourceNode> node)
        : m_pool(std::move(pool)), m_node(std::move(node))
    {
    }

    void play_done(bool loop) const override
    {
        if ( !loop ) {
            if ( auto node = m_node.lock() ) {
                node->set_playpos(static_cast<size_t>(0));
                node->pause();  // 确保它不会被重复触发
                if ( auto pool = m_pool.lock() ) {
                    pool->releaseNode(node);
                }
            }
        }
    }

    void frameplaypos_updated(size_t frame_pos) override {}
    void timeplaypos_updated(std::chrono::nanoseconds time_pos) override {}

private:
    std::weak_ptr<SoundEffectPool> m_pool;
    std::weak_ptr<ice::SourceNode> m_node;
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
            m_localMixer->remove_source(instance->pitchStretcher);
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
    auto callback =
        std::make_shared<SFXPlayCallback>(shared_from_this(), instance->source);
    instance->source->add_playcallback(callback);
    instance->pitchStretcher->set_inputnode(instance->source);

    if ( m_localMixer ) {
        m_localMixer->add_source(instance->pitchStretcher);
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
        node->set_scheduled_start_frame(0);  // 确保没有残留的预定
        node->set_reference_pos_provider(std::function<size_t()>());
        node->set_playpos(static_cast<size_t>(0));
        node->setvolume(volume);
        node->play();
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_latestNode = node;
        }
    }
}

void SoundEffectPool::playScheduled(float volume, size_t targetFrame,
                                    std::function<size_t()> refProvider)
{
    auto instance = acquireInstance();
    auto node     = instance ? instance->source : nullptr;

    if ( node ) {
        if ( instance->pitchStretcher ) {
            instance->pitchStretcher->set_pitch_semitones(0.0);
        }
        node->set_scheduled_start_frame(targetFrame);
        node->set_reference_pos_provider(std::move(refProvider));
        node->setvolume(volume);
        node->play();
    }
}

/// @brief 停止所有正在播放或预定的音效，并重置状态
void SoundEffectPool::stopAll()
{
    std::lock_guard<std::mutex> lock(m_mtx);
    m_readyQueue.clear();

    for ( auto& instance : m_allInstances ) {
        instance->source->pause();
        instance->source->set_playpos(static_cast<size_t>(0));
        instance->source->set_scheduled_start_frame(0);
        instance->source->set_reference_pos_provider(std::function<size_t()>());
        if ( instance->pitchStretcher ) {
            instance->pitchStretcher->set_pitch_semitones(0.0);
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
