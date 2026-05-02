#include "audio/AudioManager.h"
#include "audio/SoundEffectPool.h"
#include "config/AppConfig.h"
#include "event/audio/AudioPlaybackEvent.h"
#include "event/core/EventBus.h"
#include "log/colorful-log.h"
#include "mmm/project/AudioResource.h"

#include <ice/core/MixBus.hpp>
#include <ice/core/PlayCallBack.hpp>
#include <ice/core/SourceNode.hpp>
#include <ice/core/effect/GraphicEqualizer.hpp>
#include <ice/core/effect/TimeStretcher.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/out/play/sdl/SDLPlayer.hpp>
#include <ice/thread/ThreadPool.hpp>

namespace MMM::Audio
{

class AudioPlayCallBack : public ice::PlayCallBack
{
public:
    void play_done(bool loop) const override
    {
        if ( !loop ) {
            Event::AudioFinishedEvent e;
            e.isLooping = loop;
            Event::EventBus::instance().publish(e);
        }
    }

    void frameplaypos_updated(size_t frame_pos) override {}

    void timeplaypos_updated(std::chrono::nanoseconds time_pos) override
    {
        Event::AudioPositionEvent e;
        e.positionSeconds = std::chrono::duration<double>(time_pos).count();
        e.systemTimeSeconds =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        Event::EventBus::instance().publish(e);
    }
};

static std::shared_ptr<AudioPlayCallBack> g_callback =
    std::make_shared<AudioPlayCallBack>();

AudioManager& AudioManager::instance()
{
    static AudioManager inst;
    return inst;
}

AudioManager::AudioManager()
{
    // 从配置初始化音量
    auto& settings    = Config::AppConfig::instance().getEditorSettings();
    m_globalVolume    = settings.globalVolume;
    m_mainTrackVolume = 0.5f;  // 默认主音轨音量

    // 初始化常驻音效静音状态
    for ( const auto& [key, muted] : settings.sfxConfig.permanentSfxMutes ) {
        m_sfxMutes[key] = muted;
    }
}

AudioManager::~AudioManager() = default;

void AudioManager::init()
{
    XINFO("Initializing AudioManager...");
    ice::SDLPlayer::init_backend();

    m_threadPool = std::make_unique<ice::ThreadPool>(4);
    m_audioPool  = std::make_unique<ice::AudioPool>();
    m_player     = std::make_unique<ice::SDLPlayer>();

    m_mainMixer         = std::make_shared<ice::MixBus>();
    m_preStretcherMixer = std::make_shared<ice::MixBus>();

    m_player->set_source(m_mainMixer);

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

    m_bgmTrack.reset();
    m_bgmSource.reset();
    m_stretcher.reset();
    m_mainMixer.reset();
    m_preStretcherMixer.reset();
    m_player.reset();
    m_audioPool.reset();
    m_threadPool.reset();
    XINFO("AudioManager shutdown.");
}

bool AudioManager::loadBGM(const std::string&      filePath,
                           const AudioTrackConfig& config)
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

    m_bgmTrack.reset();
    if ( m_stretcher ) {
        m_mainMixer->remove_source(m_stretcher);
    } else if ( m_bgmSource ) {
        m_mainMixer->remove_source(m_bgmSource);
    }
    if ( m_mainEQ ) {
        m_preStretcherMixer->remove_source(m_mainEQ);
    } else if ( m_bgmSource ) {
        m_preStretcherMixer->remove_source(m_bgmSource);
    }

    m_mainTrackVolume = config.volume;
    m_mainTrackMuted  = config.muted;

    m_bgmTrack  = track;
    m_bgmSource = std::make_shared<ice::SourceNode>(track);
    m_bgmSource->setvolume(
        m_mainTrackMuted ? 0.0f : m_mainTrackVolume * m_globalVolume);
    m_bgmSource->add_playcallback(g_callback);

    m_stretcher = std::make_shared<ice::TimeStretcher>();
    m_stretcher->set_inputnode(m_preStretcherMixer);

    if ( m_mainEQ ) {
        m_mainEQ->set_inputnode(m_bgmSource);
        m_preStretcherMixer->add_source(m_mainEQ);
    } else {
        m_preStretcherMixer->add_source(m_bgmSource);
    }
    m_mainMixer->add_source(m_stretcher);

    // 应用播放速度与音高
    setPlaybackSpeed(config.playbackSpeed);
    setPlaybackPitch(config.playbackPitch);

    // 应用图形均衡器设置
    if ( config.eqEnabled &&
         config.eqPreset != static_cast<int>(EQPreset::None) ) {
        createMainTrackEQ(static_cast<EQPreset>(config.eqPreset));
        const size_t bandCount = getMainTrackEQBandCount();
        for ( size_t i = 0; i < bandCount; ++i ) {
            if ( i < config.eqBandGains.size() ) {
                setMainTrackEQBandGain(i, config.eqBandGains[i]);
            }
            if ( i < config.eqBandQs.size() ) {
                setMainTrackEQBandQ(i, config.eqBandQs[i]);
            }
        }
    } else {
        destroyMainTrackEQ();
    }

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

void AudioManager::setMainTrackVolume(float volume)
{
    m_mainTrackVolume = std::clamp(volume, 0.0f, 1.0f);
    if ( m_bgmSource ) {
        m_bgmSource->setvolume(
            m_mainTrackMuted ? 0.0f : m_mainTrackVolume * m_globalVolume);
    }
}

float AudioManager::getMainTrackVolume() const
{
    return m_mainTrackVolume;
}

void AudioManager::setMainTrackMute(bool muted)
{
    m_mainTrackMuted = muted;
    if ( m_bgmSource ) {
        m_bgmSource->setvolume(
            m_mainTrackMuted ? 0.0f : m_mainTrackVolume * m_globalVolume);
    }
}

bool AudioManager::isMainTrackMuted() const
{
    return m_mainTrackMuted;
}

void AudioManager::setGlobalVolume(float volume)
{
    m_globalVolume = std::clamp(volume, 0.0f, 1.0f);

    // 同步到配置并保存
    auto& settings        = Config::AppConfig::instance().getEditorSettings();
    settings.globalVolume = m_globalVolume;
    Config::AppConfig::instance().save();

    // 重新应用全局音量到主音轨
    if ( m_bgmSource ) {
        m_bgmSource->setvolume(
            m_mainTrackMuted ? 0.0f : m_mainTrackVolume * m_globalVolume);
    }

    // 重新应用全局音量到所有音效池
    for ( auto& [key, pool] : m_sfxPools ) {
        pool->updateEffectiveVolume(m_globalVolume, getSFXPoolMute(key));
    }
}

float AudioManager::getGlobalVolume() const
{
    return m_globalVolume;
}

void AudioManager::setMainMixerLeftMute(bool muted)
{
    if ( m_mainMixer ) {
        m_mainMixer->set_mute_left(muted);
    }
}

bool AudioManager::isMainMixerLeftMuted() const
{
    if ( m_mainMixer ) {
        return m_mainMixer->is_mute_left();
    }
    return false;
}

void AudioManager::setMainMixerRightMute(bool muted)
{
    if ( m_mainMixer ) {
        m_mainMixer->set_mute_right(muted);
    }
}

bool AudioManager::isMainMixerRightMuted() const
{
    if ( m_mainMixer ) {
        return m_mainMixer->is_mute_right();
    }
    return false;
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

double AudioManager::getActualPlaybackSpeed() const
{
    if ( m_stretcher ) {
        return m_stretcher->get_actual_playback_ratio();
    }
    return m_speed;
}

void AudioManager::setPlaybackPitch(double semitones)
{
    // range check -24.0 to 24.0 is inside TimeStretcher
    if ( m_stretcher ) {
        m_stretcher->set_pitch_semitones(semitones);
    }
}

double AudioManager::getPlaybackPitch() const
{
    if ( m_stretcher ) {
        return m_stretcher->get_pitch_semitones();
    }
    return 0.0;
}

void AudioManager::setPlaybackQuality(StretchQuality quality)
{
    if ( m_stretcher ) {
        ice::TimeStretchQuality iceQuality;
        switch ( quality ) {
        case StretchQuality::Fast:
            iceQuality = ice::TimeStretchQuality::Fast;
            break;
        case StretchQuality::Balanced:
            iceQuality = ice::TimeStretchQuality::Balanced;
            break;
        case StretchQuality::Finer:
            iceQuality = ice::TimeStretchQuality::Finer;
            break;
        case StretchQuality::Best:
            iceQuality = ice::TimeStretchQuality::Best;
            break;
        default: iceQuality = ice::TimeStretchQuality::Finer; break;
        }
        m_stretcher->set_quality(iceQuality);
    }
}

AudioManager::StretchQuality AudioManager::getPlaybackQuality() const
{
    if ( m_stretcher ) {
        auto iceQuality = m_stretcher->get_quality();
        switch ( iceQuality ) {
        case ice::TimeStretchQuality::Fast: return StretchQuality::Fast;
        case ice::TimeStretchQuality::Balanced: return StretchQuality::Balanced;
        case ice::TimeStretchQuality::Finer: return StretchQuality::Finer;
        case ice::TimeStretchQuality::Best: return StretchQuality::Best;
        }
    }
    return StretchQuality::Finer;
}

void AudioManager::setSFXPoolVolume(const std::string& key, float volume,
                                    bool isPermanent)
{
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        it->second->setVolume(volume);

        if ( isPermanent ) {
            // 保存到编辑器配置
            auto& sfxCfg =
                Config::AppConfig::instance().getEditorSettings().sfxConfig;
            sfxCfg.permanentSfxVolumes[key] = volume;
            Config::AppConfig::instance().save();
        }

        // 刷新实际音量 (考虑静音)
        it->second->updateEffectiveVolume(m_globalVolume, getSFXPoolMute(key));
    }
}

void AudioManager::setSFXPoolMute(const std::string& key, bool muted,
                                  bool isPermanent)
{
    m_sfxMutes[key] = muted;

    if ( isPermanent ) {
        auto& sfxCfg =
            Config::AppConfig::instance().getEditorSettings().sfxConfig;
        sfxCfg.permanentSfxMutes[key] = muted;
        Config::AppConfig::instance().save();
    }

    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        it->second->updateEffectiveVolume(m_globalVolume, muted);
    }
}

void AudioManager::updateSFXSyncSpeedRouting(bool syncSpeed)
{
    if ( !m_mainMixer || !m_preStretcherMixer ) return;

    for ( auto& [key, pool] : m_sfxPools ) {
        auto mixer = pool->getMixer();
        if ( !mixer ) continue;

        if ( syncSpeed ) {
            m_mainMixer->remove_source(mixer);
            m_preStretcherMixer->add_source(mixer);
        } else {
            m_preStretcherMixer->remove_source(mixer);
            m_mainMixer->add_source(mixer);
        }
    }
}

float AudioManager::getSFXPoolVolume(const std::string& key) const
{
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        return it->second->getVolume();
    }
    return 1.0f;
}

bool AudioManager::getSFXPoolMute(const std::string& key) const
{
    auto it = m_sfxMutes.find(key);
    if ( it != m_sfxMutes.end() ) {
        return it->second;
    }
    return false;
}

double AudioManager::getSFXDuration(const std::string& key) const
{
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        return it->second->getDuration();
    }
    return 0.0;
}

double AudioManager::getSFXPlaybackTime(const std::string& key) const
{
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        return it->second->getLatestPlaybackTime();
    }
    return 0.0;
}

bool AudioManager::preloadSoundEffect(const std::string& key,
                                      const std::string& filePath,
                                      float              defaultVolume)
{
    if ( !m_audioPool || !m_threadPool || !m_mainMixer ) return false;

    // 检查是否已经有配置好的音量 (来自 EditorSettings 或之前的加载)
    float activeVolume = defaultVolume;
    auto& sfxCfg = Config::AppConfig::instance().getEditorSettings().sfxConfig;
    if ( sfxCfg.permanentSfxVolumes.count(key) ) {
        activeVolume = sfxCfg.permanentSfxVolumes.at(key);
    }

    XINFO(
        "Preloading SFX: {} from {} (Volume: {})", key, filePath, activeVolume);
    auto trackWeak = m_audioPool->get_or_load(*m_threadPool, filePath);
    auto track     = trackWeak.lock();

    if ( !track ) {
        XERROR("Failed to load SFX track: {}", filePath);
        return false;
    }

    auto pool = std::make_shared<SoundEffectPool>(track);
    pool->init(8);  // 预分配 8 个并发节点
    pool->setVolume(activeVolume);
    pool->updateEffectiveVolume(m_globalVolume, getSFXPoolMute(key));

    // 根据配置决定连接到哪个 Mixer
    if ( sfxCfg.hitSfxSyncSpeed ) {
        m_preStretcherMixer->add_source(pool->getMixer());
    } else {
        m_mainMixer->add_source(pool->getMixer());
    }

    m_sfxPools[key] = std::move(pool);
    return true;
}

void AudioManager::playSoundEffect(const std::string& key, float volumeFactor)
{
    if ( getSFXPoolMute(key) ) return;

    auto it = m_sfxPools.find(key);
    if ( it == m_sfxPools.end() ) return;

    it->second->play(m_globalVolume * it->second->getVolume() * volumeFactor);
}

void AudioManager::playSoundEffectScheduled(const std::string& key,
                                            double             targetTime,
                                            float              volumeFactor)
{
    if ( getSFXPoolMute(key) ) return;

    auto it = m_sfxPools.find(key);
    if ( it == m_sfxPools.end() ) return;

    if ( !m_bgmSource ) return;

    double samplerate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    size_t targetFrame = static_cast<size_t>(targetTime * samplerate);

    // 获取 BGM 播放位置的闭包，用于 SourceNode 内部参考
    auto bgmRef = [this]() -> size_t {
        if ( m_bgmSource ) return m_bgmSource->get_playpos();
        return 0;
    };

    it->second->playScheduled(
        m_globalVolume * it->second->getVolume() * volumeFactor,
        targetFrame,
        bgmRef);
}

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

void AudioManager::setMainTrackEQBandGain(size_t bandIndex, float gainDb)
{
    if ( m_mainEQ ) {
        m_mainEQ->set_band_gain_db(bandIndex, gainDb);
    }
}

float AudioManager::getMainTrackEQBandGain(size_t bandIndex) const
{
    if ( m_mainEQ ) {
        return static_cast<float>(m_mainEQ->get_band_gain_db(bandIndex));
    }
    return 0.0f;
}

void AudioManager::setMainTrackEQBandQ(size_t bandIndex, float q)
{
    if ( m_mainEQ ) {
        m_mainEQ->set_band_q_factor(bandIndex, q);
    }
}

float AudioManager::getMainTrackEQBandQ(size_t bandIndex) const
{
    if ( m_mainEQ ) {
        return static_cast<float>(m_mainEQ->get_band_q_factor(bandIndex));
    }
    return 1.414f;  // 默认 Q 值 (sqrt(2))
}

size_t AudioManager::getMainTrackEQBandCount() const
{
    if ( m_mainEQ ) {
        return m_mainEQ->get_band_count();
    }
    return 0;
}

float AudioManager::getMainTrackEQBandFrequency(size_t bandIndex) const
{
    if ( m_mainEQ ) {
        return static_cast<float>(m_mainEQ->get_band_frequency(bandIndex));
    }
    return 0.0f;
}


bool AudioManager::isMainTrackEQEnabled() const
{
    return m_mainEQ != nullptr;
}

EQPreset AudioManager::getMainTrackEQPreset() const
{
    return m_mainEQPreset;
}

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

std::shared_ptr<ice::AudioTrack> AudioManager::getBGMTrack() const
{
    return m_bgmTrack;
}

}  // namespace MMM::Audio
