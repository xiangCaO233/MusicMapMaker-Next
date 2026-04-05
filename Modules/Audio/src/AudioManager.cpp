#include "audio/AudioManager.h"
#include "log/colorful-log.h"

#include <ice/core/MixBus.hpp>
#include <ice/core/PlayCallBack.hpp>
#include <ice/core/SourceNode.hpp>
#include <ice/core/effect/TimeStretcher.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/out/play/sdl/SDLPlayer.hpp>
#include <ice/thread/ThreadPool.hpp>

#include <mutex>
#include <queue>
#include <vector>

namespace MMM::Audio
{

struct AudioManager::SoundEffectPool {
    class SFXPlayCallback : public ice::PlayCallBack
    {
    public:
        SFXPlayCallback(SoundEffectPool*               pool,
                        std::weak_ptr<ice::SourceNode> node)
            : m_pool(pool), m_node(std::move(node))
        {
        }

        void play_done(bool loop) const override
        {
            if ( !loop ) {
                if ( auto node = m_node.lock() ) {
                    node->set_playpos(static_cast<size_t>(0));
                    node->pause();  // 确保它不会被重复触发
                    if ( m_pool ) {
                        m_pool->release_node(node);
                    }
                }
            }
        }

        void frameplaypos_updated(size_t frame_pos) override {}
        void timeplaypos_updated(std::chrono::nanoseconds time_pos) override {}

    private:
        SoundEffectPool*               m_pool;
        std::weak_ptr<ice::SourceNode> m_node;
    };

    std::shared_ptr<ice::AudioTrack>              track;
    std::queue<std::shared_ptr<ice::SourceNode>>  readyQueue;
    std::vector<std::shared_ptr<ice::SourceNode>> allNodes;
    std::mutex                                    mtx;

    std::shared_ptr<ice::SourceNode> get_node()
    {
        std::lock_guard<std::mutex> lock(mtx);
        if ( readyQueue.empty() ) {
            return nullptr;
        }
        auto node = readyQueue.front();
        readyQueue.pop();
        return node;
    }

    void release_node(std::shared_ptr<ice::SourceNode> node)
    {
        std::lock_guard<std::mutex> lock(mtx);
        readyQueue.push(node);
    }
};

AudioManager& AudioManager::instance()
{
    static AudioManager inst;
    return inst;
}

AudioManager::~AudioManager() = default;

void AudioManager::init()
{
    XINFO("Initializing AudioManager...");
    ice::SDLPlayer::init_backend();

    m_threadPool = std::make_unique<ice::ThreadPool>(4);
    m_audioPool  = std::make_unique<ice::AudioPool>();
    m_player     = std::make_unique<ice::SDLPlayer>();

    m_mixer     = std::make_shared<ice::MixBus>();
    m_stretcher = std::make_shared<ice::TimeStretcher>();

    m_stretcher->set_inputnode(m_mixer);
    m_player->set_source(m_stretcher);

    if ( !m_player->open() ) {
        XERROR("Failed to open SDL audio device.");
    }
    m_player->start();
    XINFO("AudioManager initialized.");
}

void AudioManager::shutdown()
{
    XINFO("Shutting down AudioManager...");
    if ( m_player ) {
        m_player->stop();
        m_player->close();
    }
    ice::SDLPlayer::quit_backend();

    m_bgmSource.reset();
    m_stretcher.reset();
    m_mixer.reset();
    m_player.reset();
    m_audioPool.reset();
    m_threadPool.reset();
    XINFO("AudioManager shutdown.");
}

bool AudioManager::loadBGM(const std::string& filePath)
{
    if ( !m_audioPool || !m_threadPool ) return false;

    XINFO("Loading BGM: {}", filePath);
    auto trackWeak = m_audioPool->get_or_load(*m_threadPool, filePath);
    auto track     = trackWeak.lock();

    if ( !track ) {
        XERROR("Failed to load audio track: {}", filePath);
        return false;
    }

    stop();

    if ( m_bgmSource ) {
        m_mixer->remove_source(m_bgmSource);
    }

    m_bgmSource = std::make_shared<ice::SourceNode>(track);
    m_bgmSource->setvolume(m_volume);

    m_mixer->add_source(m_bgmSource);

    XINFO("BGM loaded successfully.");
    return true;
}

void AudioManager::play()
{
    if ( m_bgmSource ) {
        m_bgmSource->play();
        m_status = PlaybackStatus::Playing;
    }
}

void AudioManager::pause()
{
    if ( m_bgmSource ) {
        m_bgmSource->pause();
        m_status = PlaybackStatus::Paused;
    }
}

void AudioManager::stop()
{
    if ( m_bgmSource ) {
        m_bgmSource->pause();
        m_bgmSource->set_playpos(static_cast<size_t>(0));
        m_status = PlaybackStatus::Stopped;
    }
}

void AudioManager::seek(double seconds)
{
    if ( m_bgmSource ) {
        m_bgmSource->set_playpos(std::chrono::duration<double>(seconds));
    }
}

PlaybackStatus AudioManager::getStatus() const
{
    return m_status;
}

double AudioManager::getCurrentTime() const
{
    if ( !m_bgmSource ) return 0.0;

    auto   pos = m_bgmSource->get_playpos();
    double samplerate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( samplerate <= 0 ) return 0.0;

    return static_cast<double>(pos) / samplerate;
}

double AudioManager::getTotalTime() const
{
    if ( !m_bgmSource ) return 0.0;
    return std::chrono::duration_cast<std::chrono::duration<double>>(
               m_bgmSource->total_time())
        .count();
}

void AudioManager::setVolume(float volume)
{
    m_volume = std::clamp(volume, 0.0f, 1.0f);
    if ( m_bgmSource ) {
        m_bgmSource->setvolume(m_volume);
    }
}

float AudioManager::getVolume() const
{
    return m_volume;
}

void AudioManager::setPlaybackSpeed(double speed)
{
    m_speed = std::clamp(speed, 0.1, 4.0);
    if ( m_stretcher ) {
        m_stretcher->set_playback_ratio(m_speed);
    }
}

double AudioManager::getPlaybackSpeed() const
{
    return m_speed;
}

bool AudioManager::preloadSoundEffect(const std::string& key,
                                      const std::string& filePath)
{
    if ( !m_audioPool || !m_threadPool ) return false;

    XINFO("Preloading SFX: {} from {}", key, filePath);
    auto trackWeak = m_audioPool->get_or_load(*m_threadPool, filePath);
    auto track     = trackWeak.lock();

    if ( !track ) {
        XERROR("Failed to load SFX track: {}", filePath);
        return false;
    }

    auto pool   = std::make_unique<SoundEffectPool>();
    pool->track = track;

    // 预分配 8 个并发节点
    const int PREALLOC_COUNT = 8;
    for ( int i = 0; i < PREALLOC_COUNT; ++i ) {
        auto node     = std::make_shared<ice::SourceNode>(track);
        auto callback = std::make_shared<SoundEffectPool::SFXPlayCallback>(
            pool.get(), node);
        node->add_playcallback(callback);

        pool->allNodes.push_back(node);
        pool->readyQueue.push(node);

        // 提前加入混合器
        m_mixer->add_source(node);
    }

    m_sfxPools[key] = std::move(pool);
    return true;
}

void AudioManager::playSoundEffect(const std::string& key, float volumeFactor)
{
    if ( m_sfxPools.find(key) == m_sfxPools.end() ) return;

    auto& pool = m_sfxPools[key];
    if ( auto node = pool->get_node() ) {
        node->setvolume(m_volume * volumeFactor);  // 根据需要使用额外音量倍率
        node->play();
    }
}

}  // namespace MMM::Audio
