#include "audio/AudioManager.h"
#include "audio/SoundEffectPool.h"
#include "config/AppConfig.h"
#include "event/audio/AudioPlaybackEvent.h"
#include "event/core/EventBus.h"
#include "log/colorful-log.h"
#include "mmm/project/AudioResource.h"

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
#include <ice/out/play/sdl/SDLPlayer.hpp>
#include <ice/thread/ThreadPool.hpp>

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
    auto& settings    = Config::AppConfig::instance().getEditorSettings();
    m_globalVolume    = settings.globalVolume;
    m_globalMuted     = settings.globalMuted;
    m_bgmGain         = settings.bgmGain;
    m_bgmGainMuted    = settings.bgmGainMuted;
    m_sfxGain         = settings.sfxGain;
    m_sfxGainMuted    = settings.sfxGainMuted;
    m_mainTrackVolume = 0.5f;  // 默认主音轨音量

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

/// @brief 关闭播放器并释放所有音频引擎资源。
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

}  // namespace MMM::Audio
