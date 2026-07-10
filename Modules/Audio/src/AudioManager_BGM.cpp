#include "audio/AudioManager.h"
#include "audio/SoundEffectPool.h"
#include "config/Utf8Path.h"
#include "event/audio/AudioPlaybackEvent.h"
#include "event/core/EventBus.h"
#include "log/colorful-log.h"
#include "mmm/project/AudioResource.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <vector>

#include <ice/core/MixBus.hpp>
#include <ice/core/PlayCallBack.hpp>
#include <ice/core/SourceNode.hpp>
#include <ice/core/effect/GraphicEqualizer.hpp>
#include <ice/core/effect/TimeStretcher.hpp>
#include <ice/manage/AudioPool.hpp>

namespace MMM::Audio
{
namespace
{
/// @brief 将音频文件路径转换为稳定的规范化绝对路径键。
/// @param filePath UTF-8 音频文件路径。
/// @return 规范化路径键；路径解析失败时返回词法规范化结果。
/// @warning 低频加载路径：会访问文件系统，只能在加载音轨时调用。
std::string makeAudioPathSyncKey(const std::string& filePath)
{
    if ( filePath.empty() ) {
        return {};
    }

    std::filesystem::path path = Config::utf8ToPath(filePath);
    std::error_code       filesystemError;
    auto                  canonicalPath =
        std::filesystem::weakly_canonical(path, filesystemError);
    if ( !filesystemError ) {
        path = std::move(canonicalPath);
    }
    return Config::pathToUtf8(path.lexically_normal());
}
}  // namespace

/// @brief IonCachyEngine 播放回调适配器，将音频播放进度转发到事件系统。
class AudioPlayCallBack : public ice::PlayCallBack
{
public:
    /// @brief 播放完成回调。
    /// @param loop 本次播放是否为循环播放。
    void play_done(bool loop) const override
    {
        if ( !loop ) {
            Event::AudioFinishedEvent e;
            e.isLooping = loop;
            Event::EventBus::instance().publish(e);
        }
    }

    /// @brief 帧播放位置更新回调。
    /// @param frame_pos 当前播放帧位置。
    void frameplaypos_updated(size_t frame_pos) override { (void)frame_pos; }

    /// @brief 时间播放位置更新回调。
    /// @param time_pos 当前播放时间位置。
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

/// @brief 主音轨播放回调实例，生命周期覆盖整个音频管理器使用期。
static std::shared_ptr<AudioPlayCallBack> g_callback =
    std::make_shared<AudioPlayCallBack>();

/// @brief 加载主音轨并接入 EQ、变速器和主混音器。
/// @param filePath 音频文件绝对路径。
/// @param config 主音轨配置。
/// @return 加载成功时返回 true。
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

    m_bgmTrack     = track;
    m_bgmPath      = filePath;
    m_bgmSyncKey   = makeAudioPathSyncKey(filePath);
    m_bgmSource    = std::make_shared<ice::SourceNode>(track);
    float finalVol = (m_globalMuted || m_bgmGainMuted)
                         ? 0.0f
                         : m_mainTrackVolume * m_globalVolume * m_bgmGain;
    if ( m_mainTrackMuted ) finalVol = 0.0f;
    m_bgmSource->setvolume(finalVol);
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

/// @brief 卸载当前主音轨并断开相关音频节点。
void AudioManager::unloadBGM()
{
    if ( !m_bgmSource && !m_bgmTrack && !m_stretcher && !m_mainEQ ) {
        return;
    }

    stop();

    if ( m_mainMixer ) {
        if ( m_stretcher ) {
            m_mainMixer->remove_source(m_stretcher);
        } else if ( m_bgmSource ) {
            m_mainMixer->remove_source(m_bgmSource);
        }
    }

    if ( m_preStretcherMixer ) {
        if ( m_mainEQ ) {
            m_preStretcherMixer->remove_source(m_mainEQ);
        } else if ( m_bgmSource ) {
            m_preStretcherMixer->remove_source(m_bgmSource);
        }
    }

    m_mainEQ.reset();
    m_mainEQPreset = EQPreset::None;
    m_stretcher.reset();
    m_bgmSource.reset();
    m_bgmTrack.reset();
    m_bgmPath.clear();
    m_bgmSyncKey.clear();
    m_status = PlaybackStatus::Stopped;
    XINFO("BGM unloaded.");
}

/// @brief 获取当前加载的主音轨数据。
/// @return 当前主音轨数据；未加载时返回空指针。
std::shared_ptr<ice::AudioTrack> AudioManager::getBGMTrack() const
{
    return m_bgmTrack;
}

/// @brief 获取当前加载的 BGM 文件路径。
/// @return 当前 BGM 文件路径；未加载时返回空字符串。
const std::string& AudioManager::getLoadedBGMPath() const
{
    return m_bgmPath;
}

/// @brief 将音轨加载到独立试听通道并接入主混音器。
/// @param filePath 音频文件绝对路径。
/// @param config 试听音轨配置。
/// @return 加载并接入混音图成功时返回 true。
/// @warning 低频资源路径：可能触发音频解码缓存加载，禁止在每帧热路径中调用。
bool AudioManager::loadAuditionTrack(const std::string&      filePath,
                                     const AudioTrackConfig& config)
{
    if ( !m_audioPool || !m_threadPool || !m_mainMixer || filePath.empty() ) {
        return false;
    }

    XINFO("Loading audition track: {}", filePath);
    auto trackWeak = m_audioPool->get_or_load(*m_threadPool, filePath);
    auto track     = trackWeak.lock();
    if ( !track ) {
        XERROR("Failed to load audition track: {}", filePath);
        return false;
    }

    unloadAuditionTrack();

    m_auditionTrack       = std::move(track);
    m_auditionPath        = filePath;
    m_auditionSyncKey     = makeAudioPathSyncKey(filePath);
    m_auditionTrackVolume = std::clamp(config.volume, 0.0f, 1.0f);
    m_auditionTrackMuted  = config.muted;
    m_auditionSource      = std::make_shared<ice::SourceNode>(m_auditionTrack);
    m_auditionStretcher   = std::make_shared<ice::TimeStretcher>();
    m_auditionStretcher->set_inputnode(m_auditionSource);
    m_mainMixer->add_source(m_auditionStretcher);
    m_auditionStatus = PlaybackStatus::Stopped;

    refreshAuditionTrackVolume();
    setAuditionPlaybackSpeed(config.playbackSpeed);
    XINFO("Audition track loaded successfully.");
    return true;
}

/// @brief 卸载独立试听音轨并断开其混音节点。
void AudioManager::unloadAuditionTrack()
{
    stopAudition();

    if ( m_mainMixer && m_auditionStretcher ) {
        m_mainMixer->remove_source(m_auditionStretcher);
    }

    const bool hadAuditionTrack = m_auditionTrack || m_auditionSource ||
                                  m_auditionStretcher ||
                                  !m_auditionPath.empty();
    m_auditionStretcher.reset();
    m_auditionSource.reset();
    m_auditionTrack.reset();
    m_auditionPath.clear();
    m_auditionSyncKey.clear();
    m_auditionStatus      = PlaybackStatus::Stopped;
    m_auditionTrackVolume = 1.0f;
    m_auditionTrackMuted  = false;
    m_auditionSpeed       = 1.0;

    if ( hadAuditionTrack ) {
        XINFO("Audition track unloaded.");
    }
}

/// @brief 获取独立试听通道当前加载的音频路径。
/// @return 音频文件路径；未加载时返回空字符串。
const std::string& AudioManager::getLoadedAuditionPath() const
{
    return m_auditionPath;
}

/// @brief 获取当前 BGM 文件的规范化绝对路径键。
/// @return 与 Session 主音轨同步键格式一致的路径键；未加载时为空。
const std::string& AudioManager::getLoadedBGMSyncKey() const
{
    return m_bgmSyncKey;
}

/// @brief 获取独立试听文件的规范化绝对路径键。
/// @return 与 Session 主音轨同步键格式一致的路径键；未加载时为空。
const std::string& AudioManager::getLoadedAuditionSyncKey() const
{
    return m_auditionSyncKey;
}

/// @brief 使指定音频文件的解码缓存失效。
/// @param filePath UTF-8 音频文件绝对路径。
void AudioManager::invalidateTrackCache(const std::string& filePath)
{
    if ( !m_audioPool || filePath.empty() ) {
        return;
    }
    m_audioPool->invalidate(filePath);
}

/// @brief 加载或复用音频资源池中的轨道，供离线分析工具读取。
/// @param filePath 音频文件绝对路径。
/// @return 加载成功时返回音频轨道；失败时返回空指针。
/// @warning 低频分析路径：可能触发音频解码缓存加载，严禁在每帧
/// UI、渲染或逻辑热路径中调用。
std::shared_ptr<ice::AudioTrack> AudioManager::loadTrackForAnalysis(
    const std::string& filePath)
{
    if ( !m_audioPool || !m_threadPool ) {
        return nullptr;
    }

    auto trackWeak = m_audioPool->get_or_load(*m_threadPool, filePath);
    auto track     = trackWeak.lock();
    if ( !track ) {
        XERROR("Failed to load analysis audio track: {}", filePath);
        return nullptr;
    }

    return track;
}

}  // namespace MMM::Audio
