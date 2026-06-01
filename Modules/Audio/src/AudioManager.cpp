#include "audio/AudioManager.h"
#include "audio/SoundEffectPool.h"
#include "config/AppConfig.h"
#include "event/audio/AudioPlaybackEvent.h"
#include "event/core/EventBus.h"
#include "log/colorful-log.h"
#include "mmm/project/AudioResource.h"
#include "runtime/AppThreadPool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

#include <ice/core/MixBus.hpp>
#include <ice/core/PlayCallBack.hpp>
#include <ice/core/SourceNode.hpp>
#include <ice/core/effect/GraphicEqualizer.hpp>
#include <ice/core/effect/TimeStretcher.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/out/IReceiver.hpp>
#include <ice/out/play/openal/ALPlayer.hpp>
#include <ice/out/play/sdl/SDLPlayer.hpp>

namespace MMM::Audio
{
/// @brief 获取音频管理器全局实例。
/// @return 音频管理器全局实例引用。
AudioManager& AudioManager::instance()
{
    static AudioManager inst;
    return inst;
}

/// @brief 构造音频管理器并从编辑器配置初始化音量状态。
AudioManager::AudioManager()
{
    // 从配置初始化音量
    auto& settings        = Config::AppConfig::instance().getEditorSettings();
    m_globalVolume        = settings.globalVolume;
    m_globalMuted         = settings.globalMuted;
    m_bgmGain             = settings.bgmGain;
    m_bgmGainMuted        = settings.bgmGainMuted;
    m_sfxGain             = settings.sfxGain;
    m_sfxGainMuted        = settings.sfxGainMuted;
    m_mainTrackVolume     = 0.5f;  // 默认主音轨音量
    m_playbackBackend     = settings.audioPlaybackBackend;
    m_openALSpatialConfig = settings.openALSpatialConfig;

    // 初始化常驻音效静音状态
    for ( const auto& [key, muted] : settings.sfxConfig.permanentSfxMutes ) {
        m_sfxMutes[key] = muted;
    }
}

/// @brief 销毁音频管理器。
AudioManager::~AudioManager() = default;

/// @brief 初始化音频后端、线程池、音频池、播放器和主混音图。
void AudioManager::init()
{
    XINFO("Initializing AudioManager...");

    m_threadPool = MMM::Runtime::AppThreadPool::instance().get();
    if ( !m_threadPool ) {
        XERROR("AppThreadPool is not initialized before AudioManager::init.");
    }
    m_audioPool = std::make_unique<ice::AudioPool>();

    m_mainMixer         = std::make_shared<ice::MixBus>();
    m_preStretcherMixer = std::make_shared<ice::MixBus>();

    if ( !createPlaybackBackend(m_playbackBackend) ) {
        XERROR("Failed to open configured audio backend, falling back to SDL.");
        m_playbackBackend = Config::AudioPlaybackBackend::SDL;
        createPlaybackBackend(m_playbackBackend);
    }
    XINFO("AudioManager initialized.");
}

/// @brief 关闭播放器并释放所有音频引擎资源。
void AudioManager::shutdown()
{
    XINFO("Shutting down AudioManager...");
    destroyPlaybackBackend();

    m_bgmTrack.reset();
    m_bgmPath.clear();
    m_bgmSource.reset();
    m_stretcher.reset();
    m_mainMixer.reset();
    m_preStretcherMixer.reset();
    m_player.reset();
    m_audioPool.reset();
    m_threadPool = nullptr;
    XINFO("AudioManager shutdown.");
}

/// @brief 切换音频播放后端。
/// @param backend 目标播放后端。
/// @return 切换成功时返回 true。
bool AudioManager::setPlaybackBackend(Config::AudioPlaybackBackend backend)
{
    if ( backend == m_playbackBackend && m_player ) {
        return true;
    }

    const auto previousBackend = m_playbackBackend;
    destroyPlaybackBackend();

    if ( !createPlaybackBackend(backend) ) {
        XERROR(
            "Failed to switch audio backend, trying to restore previous "
            "backend.");
        if ( !createPlaybackBackend(previousBackend) ) {
            XERROR("Failed to restore previous audio backend.");
        }
        return false;
    }

    auto& settings = Config::AppConfig::instance().getEditorSettings();
    settings.audioPlaybackBackend = backend;
    Config::AppConfig::instance().save();
    return true;
}

/// @brief 获取当前正在使用的音频播放后端。
/// @return 当前播放后端。
Config::AudioPlaybackBackend AudioManager::getPlaybackBackend() const
{
    return m_playbackBackend;
}

/// @brief 设置 OpenAL 后端空间化输出参数。
/// @param config OpenAL 空间化配置。
/// @return 当前后端为 OpenAL 并成功应用时返回 true。
bool AudioManager::setOpenALSpatialConfig(
    const Config::OpenALSpatialConfig& config)
{
    m_openALSpatialConfig = config;

    auto& settings = Config::AppConfig::instance().getEditorSettings();
    settings.openALSpatialConfig = config;
    Config::AppConfig::instance().save();

    return applyOpenALSpatialConfig();
}

/// @brief 获取当前 OpenAL 空间化输出配置。
/// @return OpenAL 空间化配置。
const Config::OpenALSpatialConfig& AudioManager::getOpenALSpatialConfig() const
{
    return m_openALSpatialConfig;
}

/// @brief 创建并启动指定播放后端。
/// @param backend 目标播放后端。
/// @return 成功创建并启动时返回 true。
bool AudioManager::createPlaybackBackend(Config::AudioPlaybackBackend backend)
{
    if ( !m_mainMixer ) {
        return false;
    }

    std::unique_ptr<ice::IReceiver> nextPlayer;
    m_openALPlayer = nullptr;

    switch ( backend ) {
    case Config::AudioPlaybackBackend::SDL:
        ice::SDLPlayer::init_backend();
        nextPlayer = std::make_unique<ice::SDLPlayer>();
        break;
    case Config::AudioPlaybackBackend::OpenAL:
        ice::ALPlayer::init_backend();
        {
            auto alPlayer  = std::make_unique<ice::ALPlayer>();
            m_openALPlayer = alPlayer.get();
            nextPlayer     = std::move(alPlayer);
        }
        break;
    default: return false;
    }

    nextPlayer->set_source(m_mainMixer);
    if ( !nextPlayer->open() ) {
        nextPlayer->close();
        if ( backend == Config::AudioPlaybackBackend::SDL ) {
            ice::SDLPlayer::quit_backend();
        } else {
            ice::ALPlayer::quit_backend();
            m_openALPlayer = nullptr;
        }
        return false;
    }

    if ( backend == Config::AudioPlaybackBackend::OpenAL ) {
        applyOpenALSpatialConfig();
    }

    if ( !nextPlayer->start() ) {
        nextPlayer->close();
        if ( backend == Config::AudioPlaybackBackend::SDL ) {
            ice::SDLPlayer::quit_backend();
        } else {
            ice::ALPlayer::quit_backend();
            m_openALPlayer = nullptr;
        }
        return false;
    }

    m_player          = std::move(nextPlayer);
    m_playbackBackend = backend;
    XINFO("Audio playback backend switched to {}.",
          backend == Config::AudioPlaybackBackend::OpenAL ? "OpenAL" : "SDL");
    return true;
}

/// @brief 停止并释放当前播放后端。
void AudioManager::destroyPlaybackBackend()
{
    if ( m_player ) {
        m_player->stop();
        m_player->close();
        m_player.reset();
    }

    if ( m_playbackBackend == Config::AudioPlaybackBackend::OpenAL ) {
        ice::ALPlayer::quit_backend();
    } else {
        ice::SDLPlayer::quit_backend();
    }
    m_openALPlayer = nullptr;
}

/// @brief 将缓存的 OpenAL 空间化参数应用到当前后端。
/// @return 当前后端为 OpenAL 并成功应用时返回 true。
bool AudioManager::applyOpenALSpatialConfig()
{
    if ( !m_openALPlayer ) {
        return false;
    }

    m_openALPlayer->set_spatial_output_enabled(m_openALSpatialConfig.enabled);
    m_openALPlayer->set_spatial_parameters(
        m_openALSpatialConfig.directionX,
        m_openALSpatialConfig.directionY,
        m_openALSpatialConfig.directionZ,
        m_openALSpatialConfig.distance,
        m_openALSpatialConfig.referenceDistance,
        m_openALSpatialConfig.maxDistance,
        m_openALSpatialConfig.rolloffFactor);
    return true;
}

}  // namespace MMM::Audio
