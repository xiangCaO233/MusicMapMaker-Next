#include "audio/AudioManager.h"
#include "audio/SoundEffectPool.h"
#include "config/AppConfig.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <ice/core/MixBus.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/out/IReceiver.hpp>
#include <ice/out/play/openal/ALPlayer.hpp>
#include <ice/out/play/sdl/SDLPlayer.hpp>

namespace MMM::Audio
{
namespace
{
/// @brief 获取音频播放后端的日志名称。
/// @param backend 音频播放后端类型。
/// @return 用于日志输出的后端名称。
const char* getPlaybackBackendName(Config::AudioPlaybackBackend backend)
{
    switch ( backend ) {
    case Config::AudioPlaybackBackend::SDL: return "SDL";
    case Config::AudioPlaybackBackend::OpenAL: return "OpenAL";
    default: return "Unknown";
    }
}

/// @brief 获取日志中显示的输出设备名称。
/// @param deviceName 配置中的设备名称。
/// @return 用于日志输出的设备名称。
std::string outputDeviceNameForLog(const std::string& deviceName)
{
    return deviceName.empty() ? std::string("Default") : deviceName;
}

/// @brief 枚举指定播放后端的输出设备。
/// @param backend 目标播放后端。
/// @return 输出设备列表，第一项为默认设备。
std::vector<AudioOutputDevice> listOutputDevicesForBackend(
    Config::AudioPlaybackBackend backend)
{
    std::vector<AudioOutputDevice> devices;
    devices.push_back(AudioOutputDevice{ "", true });

    switch ( backend ) {
    case Config::AudioPlaybackBackend::SDL:
        for ( const auto& device : ice::SDLPlayer::list_devices() ) {
            if ( !device.name.empty() ) {
                devices.push_back(AudioOutputDevice{ device.name, false });
            }
        }
        break;
    case Config::AudioPlaybackBackend::OpenAL:
        for ( const auto& device : ice::ALPlayer::list_devices() ) {
            if ( !device.name.empty() ) {
                devices.push_back(AudioOutputDevice{ device.name, false });
            }
        }
        break;
    default: break;
    }

    return devices;
}

/// @brief 判断设备列表中是否包含指定设备名称。
/// @param devices 已枚举的设备列表。
/// @param deviceName 设备名称。
/// @return 找到时返回 true。
bool containsOutputDeviceName(const std::vector<AudioOutputDevice>& devices,
                              const std::string&                    deviceName)
{
    return std::any_of(devices.begin(), devices.end(), [&](const auto& device) {
        return device.name == deviceName;
    });
}

/// @brief Log OpenAL playback devices visible to the backend.
void logOpenALDeviceDiagnostics()
{
    const auto devices = ice::ALPlayer::list_devices();
    if ( devices.empty() ) {
        XERROR("OpenAL reported no playback devices.");
        return;
    }

    std::string deviceNames;
    for ( const auto& device : devices ) {
        if ( !deviceNames.empty() ) {
            deviceNames += " | ";
        }
        deviceNames += device.name;
    }
    XINFO("OpenAL reported playback devices: {}", deviceNames);
}
}  // namespace

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
    m_sdlOutputDeviceName = settings.sdlAudioOutputDeviceName;
    m_openALOutputDeviceName = settings.openALAudioOutputDeviceName;
    m_openALSpatialConfig    = settings.openALSpatialConfig;

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

    if ( createPlaybackBackend(m_playbackBackend) ) {
        XINFO("Audio playback backend opened with configured backend: {}.",
              getPlaybackBackendName(m_playbackBackend));
    } else {
        XERROR(
            "Failed to open configured audio backend: {}, falling back to "
            "SDL.",
            getPlaybackBackendName(m_playbackBackend));
        if ( createPlaybackBackend(Config::AudioPlaybackBackend::SDL) ) {
            XINFO("Audio playback backend opened with fallback backend: SDL.");
        } else {
            XERROR("Failed to open fallback audio backend: SDL.");
            m_playbackBackend = Config::AudioPlaybackBackend::SDL;
        }
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
            "Failed to switch audio backend from {} to {}, trying to restore "
            "previous backend.",
            getPlaybackBackendName(previousBackend),
            getPlaybackBackendName(backend));
        if ( !createPlaybackBackend(previousBackend) ) {
            XERROR("Failed to restore previous audio backend: {}.",
                   getPlaybackBackendName(previousBackend));
        } else {
            XINFO("Previous audio backend restored: {}.",
                  getPlaybackBackendName(previousBackend));
        }
        return false;
    }

    auto& settings = Config::AppConfig::instance().getEditorSettings();
    settings.audioPlaybackBackend = backend;
    Config::AppConfig::instance().save();
    XINFO("Audio playback backend switched from {} to {}.",
          getPlaybackBackendName(previousBackend),
          getPlaybackBackendName(backend));
    return true;
}

/// @brief 获取当前正在使用的音频播放后端。
/// @return 当前播放后端。
Config::AudioPlaybackBackend AudioManager::getPlaybackBackend() const
{
    return m_playbackBackend;
}

/// @brief 枚举当前播放后端可用的输出设备。
/// @return 输出设备列表，第一项始终为系统默认设备。
std::vector<AudioOutputDevice> AudioManager::listOutputDevices() const
{
    return listOutputDevicesForBackend(m_playbackBackend);
}

/// @brief 设置当前播放后端使用的输出设备。
/// @param deviceName 设备名称；空字符串表示系统默认设备。
/// @return 成功切换并持久化时返回 true。
bool AudioManager::setOutputDeviceName(const std::string& deviceName)
{
    const auto  backend            = m_playbackBackend;
    const auto& previousDeviceName = getConfiguredOutputDeviceName(backend);
    if ( deviceName == previousDeviceName && m_player ) {
        return true;
    }

    const auto devices = listOutputDevicesForBackend(backend);
    if ( !deviceName.empty() &&
         !containsOutputDeviceName(devices, deviceName) ) {
        XERROR("Configured audio output device is not available: {}.",
               deviceName);
        return false;
    }

    const std::string previousDeviceNameCopy = previousDeviceName;
    setConfiguredOutputDeviceName(backend, deviceName);
    destroyPlaybackBackend();

    if ( !createPlaybackBackend(backend, false) ) {
        XERROR(
            "Failed to switch audio output device from {} to {}, trying to "
            "restore previous device.",
            outputDeviceNameForLog(previousDeviceNameCopy),
            outputDeviceNameForLog(deviceName));
        setConfiguredOutputDeviceName(backend, previousDeviceNameCopy);
        if ( !createPlaybackBackend(backend) ) {
            XERROR("Failed to restore previous audio output device: {}.",
                   outputDeviceNameForLog(previousDeviceNameCopy));
        }
        return false;
    }

    auto& settings = Config::AppConfig::instance().getEditorSettings();
    settings.sdlAudioOutputDeviceName    = m_sdlOutputDeviceName;
    settings.openALAudioOutputDeviceName = m_openALOutputDeviceName;
    Config::AppConfig::instance().save();
    XINFO("Audio output device switched from {} to {}.",
          outputDeviceNameForLog(previousDeviceNameCopy),
          outputDeviceNameForLog(deviceName));
    return true;
}

/// @brief 获取当前播放后端配置的输出设备名称。
/// @return 设备名称；空字符串表示系统默认设备。
const std::string& AudioManager::getOutputDeviceName() const
{
    return getConfiguredOutputDeviceName(m_playbackBackend);
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
/// @param allowDefaultDeviceFallback 指定设备打开失败时是否回退到默认设备。
/// @return 成功创建并启动时返回 true。
/// @warning 低频后端切换路径；OpenAL 打开失败时会短暂 sleep 后重试，
/// 禁止在每帧或音频热路径中调用。
bool AudioManager::createPlaybackBackend(Config::AudioPlaybackBackend backend,
                                         bool allowDefaultDeviceFallback)
{
    if ( !m_mainMixer ) {
        return false;
    }

    std::unique_ptr<ice::IReceiver> nextPlayer;
    m_openALPlayer = nullptr;

    switch ( backend ) {
    case Config::AudioPlaybackBackend::SDL:
        if ( !ice::SDLPlayer::init_backend() ) {
            XERROR("Failed to initialize audio playback backend: {}.",
                   getPlaybackBackendName(backend));
            return false;
        }
        nextPlayer = std::make_unique<ice::SDLPlayer>();
        break;
    case Config::AudioPlaybackBackend::OpenAL:
        if ( !ice::ALPlayer::init_backend() ) {
            XERROR("Failed to initialize audio playback backend: {}.",
                   getPlaybackBackendName(backend));
            return false;
        }
        {
            auto alPlayer  = std::make_unique<ice::ALPlayer>();
            m_openALPlayer = alPlayer.get();
            nextPlayer     = std::move(alPlayer);
        }
        break;
    default:
        XERROR("Unsupported audio playback backend: {}.",
               getPlaybackBackendName(backend));
        return false;
    }

    nextPlayer->set_source(m_mainMixer);
    auto openSelectedOutputDevice = [&]() {
        const auto& deviceName = getConfiguredOutputDeviceName(backend);
        if ( backend == Config::AudioPlaybackBackend::SDL ) {
            auto* sdlPlayer = static_cast<ice::SDLPlayer*>(nextPlayer.get());
            if ( deviceName.empty() ) {
                return sdlPlayer->open();
            }

            const auto sdlDevices = ice::SDLPlayer::list_devices();
            auto       deviceIt   = std::find_if(
                sdlDevices.begin(), sdlDevices.end(), [&](const auto& device) {
                    return device.name == deviceName;
                });
            if ( deviceIt == sdlDevices.end() ) {
                XERROR("SDL audio output device not found: {}.", deviceName);
                return allowDefaultDeviceFallback ? sdlPlayer->open() : false;
            }

            if ( sdlPlayer->open(deviceIt->id) ) {
                return true;
            }

            XERROR("Failed to open SDL audio output device: {}.", deviceName);
            return allowDefaultDeviceFallback ? sdlPlayer->open() : false;
        }

        if ( backend == Config::AudioPlaybackBackend::OpenAL ) {
            if ( !m_openALPlayer ) {
                return false;
            }
            if ( deviceName.empty() ) {
                return m_openALPlayer->open();
            }

            if ( m_openALPlayer->open(deviceName) ) {
                return true;
            }

            const auto& detail = m_openALPlayer->getLastError();
            if ( !detail.empty() ) {
                XERROR("OpenAL backend detail: {}", detail);
            }
            XERROR("Failed to open OpenAL audio output device: {}.",
                   deviceName);
            return allowDefaultDeviceFallback ? m_openALPlayer->open() : false;
        }

        return false;
    };

    const int openAttemptCount =
        backend == Config::AudioPlaybackBackend::OpenAL ? 3 : 1;
    bool backendOpened = false;
    for ( int attempt = 1; attempt <= openAttemptCount; ++attempt ) {
        if ( openSelectedOutputDevice() ) {
            backendOpened = true;
            if ( attempt > 1 ) {
                XINFO(
                    "Audio playback backend opened after retry: {} "
                    "(attempt {}/{}).",
                    getPlaybackBackendName(backend),
                    attempt,
                    openAttemptCount);
            }
            break;
        }

        if ( attempt < openAttemptCount ) {
            const auto retryDelay = std::chrono::milliseconds(100 * attempt);
            XERROR(
                "Audio playback backend open attempt {}/{} failed: {}, "
                "retrying in {} ms.",
                attempt,
                openAttemptCount,
                getPlaybackBackendName(backend),
                retryDelay.count());
            nextPlayer->close();
            std::this_thread::sleep_for(retryDelay);
        }
    }

    if ( !backendOpened ) {
        XERROR("Failed to open audio playback backend: {}.",
               getPlaybackBackendName(backend));
        if ( backend == Config::AudioPlaybackBackend::OpenAL &&
             m_openALPlayer ) {
            const auto& detail = m_openALPlayer->getLastError();
            if ( !detail.empty() ) {
                XERROR("OpenAL backend detail: {}", detail);
            }
            logOpenALDeviceDiagnostics();
        }
        nextPlayer->close();
        if ( backend == Config::AudioPlaybackBackend::SDL ) {
            ice::SDLPlayer::quit_backend();
        } else {
            ice::ALPlayer::quit_backend();
            m_openALPlayer = nullptr;
        }
        return false;
    }

    if ( backend == Config::AudioPlaybackBackend::SDL ) {
        auto*      sdlPlayer = static_cast<ice::SDLPlayer*>(nextPlayer.get());
        const auto deviceId  = sdlPlayer->get_current_device();
        const auto devices   = ice::SDLPlayer::list_devices();
        auto       deviceIt  = std::find_if(
            devices.begin(), devices.end(), [&](const auto& device) {
                return device.id == deviceId;
            });
        if ( deviceIt != devices.end() ) {
            XINFO("SDL playback device opened: {}", deviceIt->name);
        }
    }

    if ( backend == Config::AudioPlaybackBackend::OpenAL ) {
        if ( m_openALPlayer ) {
            const auto& openedDevice = m_openALPlayer->getOpenedDeviceName();
            if ( !openedDevice.empty() ) {
                XINFO("OpenAL playback device opened: {}", openedDevice);
            }
        }
        applyOpenALSpatialConfig();
    }

    if ( !nextPlayer->start() ) {
        XERROR("Failed to start audio playback backend: {}.",
               getPlaybackBackendName(backend));
        if ( backend == Config::AudioPlaybackBackend::OpenAL &&
             m_openALPlayer ) {
            const auto& detail = m_openALPlayer->getLastError();
            if ( !detail.empty() ) {
                XERROR("OpenAL backend detail: {}", detail);
            }
        }
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
    return true;
}

/// @brief 获取指定后端配置的输出设备名称。
/// @param backend 目标播放后端。
/// @return 设备名称；空字符串表示系统默认设备。
const std::string& AudioManager::getConfiguredOutputDeviceName(
    Config::AudioPlaybackBackend backend) const
{
    if ( backend == Config::AudioPlaybackBackend::OpenAL ) {
        return m_openALOutputDeviceName;
    }
    return m_sdlOutputDeviceName;
}

/// @brief 更新指定后端配置的输出设备名称。
/// @param backend 目标播放后端。
/// @param deviceName 设备名称；空字符串表示系统默认设备。
void AudioManager::setConfiguredOutputDeviceName(
    Config::AudioPlaybackBackend backend, const std::string& deviceName)
{
    if ( backend == Config::AudioPlaybackBackend::OpenAL ) {
        m_openALOutputDeviceName = deviceName;
        return;
    }
    m_sdlOutputDeviceName = deviceName;
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
