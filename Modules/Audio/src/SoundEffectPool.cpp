#include "audio/SoundEffectPool.h"

#include <ice/core/MixBus.hpp>
#include <ice/core/PlayCallBack.hpp>
#include <ice/core/SourceNode.hpp>
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
    for ( auto& node : m_allNodes ) {
        if ( m_localMixer ) {
            m_localMixer->remove_source(node);
        }
    }
}

std::shared_ptr<ice::MixBus> SoundEffectPool::getMixer() const
{
    return m_localMixer;
}

void SoundEffectPool::init(int count)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    for ( int i = 0; i < count; ++i ) {
        auto node = std::make_shared<ice::SourceNode>(m_track);
        auto callback =
            std::make_shared<SFXPlayCallback>(shared_from_this(), node);
        node->add_playcallback(callback);
        m_allNodes.push_back(node);
        m_readyQueue.push(node);
        if ( m_localMixer ) {
            m_localMixer->add_source(node);
        }
    }
}

void SoundEffectPool::play(float volume)
{
    std::shared_ptr<ice::SourceNode> node;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if ( m_readyQueue.empty() ) {
            // 如果池空了，动态创建一个 (或者复用最老的，这里简单处理为扩容)
            node = std::make_shared<ice::SourceNode>(m_track);
            auto callback =
                std::make_shared<SFXPlayCallback>(shared_from_this(), node);
            node->add_playcallback(callback);
            m_allNodes.push_back(node);
            if ( m_localMixer ) {
                m_localMixer->add_source(node);
            }
        } else {
            node = m_readyQueue.front();
            m_readyQueue.pop();
        }
    }

    if ( node ) {
        node->set_scheduled_start_frame(0);  // 确保没有残留的预定
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
    std::shared_ptr<ice::SourceNode> node;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if ( m_readyQueue.empty() ) {
            node = std::make_shared<ice::SourceNode>(m_track);
            auto callback =
                std::make_shared<SFXPlayCallback>(shared_from_this(), node);
            node->add_playcallback(callback);
            m_allNodes.push_back(node);
            if ( m_localMixer ) {
                m_localMixer->add_source(node);
            }
        } else {
            node = m_readyQueue.front();
            m_readyQueue.pop();
        }
    }

    if ( node ) {
        node->set_scheduled_start_frame(targetFrame);
        node->set_reference_pos_provider(std::move(refProvider));
        node->setvolume(volume);
        node->play();
    }
}

void SoundEffectPool::releaseNode(std::shared_ptr<ice::SourceNode> node)
{
    std::lock_guard<std::mutex> lock(m_mtx);
    m_readyQueue.push(std::move(node));
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
    for ( auto& node : m_allNodes ) {
        node->setvolume(effectiveVolume);
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

}  // namespace MMM::Audio
