#include "BackgroundSpectrumAnalyzer.h"
#include "audio/AudioManager.h"
#include "audio/AudioTimelineMixerNode.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/project/AudioResource.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

#include <ice/core/MixBus.hpp>
#include <ice/core/SourceNode.hpp>
#include <ice/core/effect/GraphicEqualizer.hpp>
#include <ice/core/effect/TimeStretcher.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/manage/AudioTrack.hpp>

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

/// @brief 将秒数安全转换为统一时间线帧。
/// @param seconds 秒数，允许为负。
/// @return 限制在 AudioTimelineFrame 可表达范围内的最近帧。
AudioTimelineFrame secondsToTimelineFrame(double seconds) noexcept
{
    if ( !std::isfinite(seconds) ) return 0;

    const long double frames =
        static_cast<long double>(seconds) *
        static_cast<long double>(ice::ICEConfig::internal_format.samplerate);
    constexpr auto MIN_FRAME = static_cast<long double>(
        std::numeric_limits<AudioTimelineFrame>::min());
    constexpr auto MAX_FRAME = static_cast<long double>(
        std::numeric_limits<AudioTimelineFrame>::max());
    if ( frames <= MIN_FRAME ) {
        return std::numeric_limits<AudioTimelineFrame>::min();
    }
    if ( frames >= MAX_FRAME ) {
        return std::numeric_limits<AudioTimelineFrame>::max();
    }
    return static_cast<AudioTimelineFrame>(std::llround(frames));
}

/// @brief 追加与指定采样事件关联的加载诊断。
/// @param result 接收诊断的加载结果。
/// @param code 诊断类型。
/// @param event 诊断关联的采样事件。
void appendTimelineDiagnostic(AudioTimelineLoadResult&        result,
                              AudioTimelineLoadDiagnosticCode code,
                              const AudioTimelineLoadEvent&   event)
{
    const char* message = "未知音频时间线加载问题";
    switch ( code ) {
    case AudioTimelineLoadDiagnosticCode::AudioSystemUnavailable:
        message = "音频系统尚未初始化";
        break;
    case AudioTimelineLoadDiagnosticCode::MissingResource:
        message = "音频资源缺失或无法解码";
        break;
    case AudioTimelineLoadDiagnosticCode::InvalidStartTime:
        message = "采样实际起播时间无效，已按 0 秒载入";
        break;
    case AudioTimelineLoadDiagnosticCode::UnsupportedResourcePlaybackSpeed:
        message = "暂不支持逐资源 playbackSpeed，未应用到复合时间线";
        break;
    case AudioTimelineLoadDiagnosticCode::UnsupportedResourcePitch:
        message = "暂不支持逐资源 pitch，未应用到复合时间线";
        break;
    case AudioTimelineLoadDiagnosticCode::UnsupportedResourceEqualizer:
        message = "暂不支持逐资源 EQ，未应用到复合时间线";
        break;
    }
    result.diagnostics.push_back(AudioTimelineLoadDiagnostic{
        .code        = code,
        .eventId     = event.eventId,
        .resourceKey = event.resourceKey,
        .filePath    = event.filePath,
        .message     = message,
    });
}

/// @brief 将资源配置音量规范化到其声明的单位范围。
/// @param volume 资源配置中的线性音量。
/// @return 有限的 0 到 1 音量。
float sanitizedResourceVolume(float volume) noexcept
{
    return std::isfinite(volume) ? std::clamp(volume, 0.0F, 1.0F) : 0.0F;
}

/// @brief 将采样物件音量规范化为可混音的非负线性倍率。
/// @param volume 采样物件线性音量。
/// @return 有限非负倍率。
float sanitizedEventVolume(float volume) noexcept
{
    return std::isfinite(volume) ? std::max(volume, 0.0F) : 0.0F;
}
}  // namespace

/// @brief 在非实时路径准备全部资源并替换复合音频时间线。
/// @param events 自动采样事件。
/// @param chartEndSeconds 非音频谱面内容结束时间。
/// @param fingerprint 完整时间线稳定指纹。
/// @return 图替换结果及逐事件诊断。
/// @warning 低频资源路径：会访问文件系统并等待资源解码。
AudioTimelineLoadResult AudioManager::loadAudioTimeline(
    const std::vector<AudioTimelineLoadEvent>& events, double chartEndSeconds,
    const std::string& fingerprint)
{
    AudioTimelineLoadResult result;
    if ( !m_audioPool || !m_threadPool || !m_mainMixer ||
         !m_preStretcherMixer ) {
        result.diagnostics.push_back(AudioTimelineLoadDiagnostic{
            .code    = AudioTimelineLoadDiagnosticCode::AudioSystemUnavailable,
            .message = "音频系统尚未初始化",
        });
        return result;
    }

    const EQPreset     previousEqPreset = m_mainEQPreset;
    std::vector<float> previousEqGains;
    std::vector<float> previousEqQs;
    if ( m_mainEQ && previousEqPreset != EQPreset::None ) {
        const std::size_t bandCount = getMainTrackEQBandCount();
        previousEqGains.reserve(bandCount);
        previousEqQs.reserve(bandCount);
        for ( std::size_t bandIndex = 0U; bandIndex < bandCount; ++bandIndex ) {
            previousEqGains.push_back(getMainTrackEQBandGain(bandIndex));
            previousEqQs.push_back(getMainTrackEQBandQ(bandIndex));
        }
    }

    std::vector<PreparedTimelineClip> preparedClips;
    preparedClips.reserve(events.size());
    std::unordered_map<std::string, std::shared_ptr<ice::AudioTrack>>
        tracksByPath;
    tracksByPath.reserve(events.size());
    std::shared_ptr<ice::AudioTrack> firstLoadedTrack;

    for ( const auto& event : events ) {
        double startSeconds = event.effectiveStartSeconds;
        if ( !std::isfinite(startSeconds) ) {
            appendTimelineDiagnostic(
                result,
                AudioTimelineLoadDiagnosticCode::InvalidStartTime,
                event);
            startSeconds = 0.0;
        }

        if ( !std::isfinite(event.resourceConfig.playbackSpeed) ||
             std::abs(event.resourceConfig.playbackSpeed - 1.0F) > 1.0e-6F ) {
            appendTimelineDiagnostic(result,
                                     AudioTimelineLoadDiagnosticCode::
                                         UnsupportedResourcePlaybackSpeed,
                                     event);
        }
        if ( !std::isfinite(event.resourceConfig.playbackPitch) ||
             std::abs(event.resourceConfig.playbackPitch) > 1.0e-6F ) {
            appendTimelineDiagnostic(
                result,
                AudioTimelineLoadDiagnosticCode::UnsupportedResourcePitch,
                event);
        }
        if ( event.resourceConfig.eqEnabled ||
             event.resourceConfig.eqPreset !=
                 static_cast<int>(EQPreset::None) ) {
            appendTimelineDiagnostic(
                result,
                AudioTimelineLoadDiagnosticCode::UnsupportedResourceEqualizer,
                event);
        }

        std::shared_ptr<ice::AudioTrack> track;
        if ( !event.filePath.empty() ) {
            const auto existingTrack = tracksByPath.find(event.filePath);
            if ( existingTrack != tracksByPath.end() ) {
                track = existingTrack->second;
            } else {
                track = m_audioPool->get_or_load(*m_threadPool, event.filePath)
                            .lock();
                if ( track && track->num_frames() == 0U ) {
                    track.reset();
                }
                tracksByPath.emplace(event.filePath, track);
            }
        }

        if ( !track ) {
            ++result.missingClipCount;
            appendTimelineDiagnostic(
                result,
                AudioTimelineLoadDiagnosticCode::MissingResource,
                event);
            continue;
        }

        if ( !firstLoadedTrack ) firstLoadedTrack = track;
        const float resourceVolume =
            event.resourceConfig.muted
                ? 0.0F
                : sanitizedResourceVolume(event.resourceConfig.volume);
        preparedClips.push_back(PreparedTimelineClip{
            .eventId    = event.eventId,
            .sourceKey  = event.resourceKey,
            .startFrame = secondsToTimelineFrame(startSeconds),
            .volume = resourceVolume * sanitizedEventVolume(event.eventVolume),
            .track  = std::move(track),
        });
    }

    const double normalizedChartEnd =
        std::isfinite(chartEndSeconds) ? std::max(chartEndSeconds, 0.0) : 0.0;
    auto timelineNode = std::make_shared<AudioTimelineMixerNode>(
        std::move(preparedClips),
        secondsToTimelineFrame(normalizedChartEnd),
        std::max<std::size_t>(ice::ICEConfig::default_buffer_size, 1U));

    unloadAudioTimeline();

    m_audioTimelineNode             = std::move(timelineNode);
    m_audioTimelineFingerprint      = fingerprint;
    m_audioTimelineClipCount        = m_audioTimelineNode->clipCount();
    m_missingAudioTimelineClipCount = result.missingClipCount;
    m_bgmTrack                      = std::move(firstLoadedTrack);
    m_bgmPath = events.size() == 1U ? events.front().filePath : std::string{};
    m_bgmSyncKey = fingerprint;
    m_bgmSpectrumCapture =
        std::make_shared<BackgroundSpectrumCaptureNode>(m_audioTimelineNode);
    m_stretcher = std::make_shared<ice::TimeStretcher>();
    m_stretcher->set_inputnode(m_preStretcherMixer);
    if ( previousEqPreset != EQPreset::None ) {
        createMainTrackEQ(previousEqPreset);
        const std::size_t bandCount =
            std::min(previousEqGains.size(), previousEqQs.size());
        for ( std::size_t bandIndex = 0U; bandIndex < bandCount; ++bandIndex ) {
            setMainTrackEQBandGain(bandIndex, previousEqGains[bandIndex]);
            setMainTrackEQBandQ(bandIndex, previousEqQs[bandIndex]);
        }
    } else {
        m_preStretcherMixer->add_source(m_bgmSpectrumCapture);
    }
    m_mainMixer->add_source(m_stretcher);

    refreshAudioTimelineVolume();
    setPlaybackSpeed(m_speed);
    setPlaybackPitch(m_playbackPitch);
    setPlaybackQuality(m_playbackQuality);

    result.success         = true;
    result.loadedClipCount = m_audioTimelineClipCount;
    XINFO(
        "Audio timeline loaded: clips={}, missing={}, end={}s, fingerprint={}",
        result.loadedClipCount,
        result.missingClipCount,
        getTotalTime(),
        fingerprint);
    return result;
}

/// @brief 停止并卸载当前复合音频时间线。
void AudioManager::unloadAudioTimeline()
{
    if ( !m_audioTimelineNode && !m_bgmTrack && !m_stretcher ) {
        return;
    }

    if ( m_audioTimelineNode ) {
        m_audioTimelineNode->stop();
    }
    clearAllScheduledSoundEffects();
    if ( m_mainMixer ) {
        if ( m_stretcher ) {
            m_mainMixer->remove_source(m_stretcher);
        } else if ( m_audioTimelineNode ) {
            m_mainMixer->remove_source(m_audioTimelineNode);
        }
    }
    if ( m_preStretcherMixer ) {
        if ( m_mainEQ ) {
            m_preStretcherMixer->remove_source(m_mainEQ);
        } else if ( m_bgmSpectrumCapture ) {
            m_preStretcherMixer->remove_source(m_bgmSpectrumCapture);
        } else if ( m_audioTimelineNode ) {
            m_preStretcherMixer->remove_source(m_audioTimelineNode);
        }
    }

    m_mainEQ.reset();
    m_mainEQPreset = EQPreset::None;
    m_stretcher.reset();
    m_bgmSpectrumCapture.reset();
    m_audioTimelineNode.reset();
    m_audioTimelineFingerprint.clear();
    m_audioTimelineClipCount        = 0U;
    m_missingAudioTimelineClipCount = 0U;
    m_bgmTrack.reset();
    m_bgmPath.clear();
    m_bgmSyncKey.clear();
    XINFO("Audio timeline unloaded.");
}

/// @brief 获取当前完整时间线稳定指纹。
/// @return 未加载时为空字符串。
const std::string& AudioManager::getLoadedAudioTimelineFingerprint() const
{
    return m_audioTimelineFingerprint;
}

/// @brief 获取当前有效采样片段数量。
/// @return 调度表中的片段数量。
std::size_t AudioManager::getLoadedAudioTimelineClipCount() const
{
    return m_audioTimelineClipCount;
}

/// @brief 获取上次加载时缺失的片段数量。
/// @return 缺失或无法解码的事件数量。
std::size_t AudioManager::getMissingAudioTimelineClipCount() const
{
    return m_missingAudioTimelineClipCount;
}

/// @brief 判断当前是否已构造时间线时钟。
/// @return 即使零片段时间线也在已构造时返回 true。
bool AudioManager::hasLoadedAudioTimeline() const
{
    return static_cast<bool>(m_audioTimelineNode);
}

/// @brief 提交主时间线半开循环范围。
/// @param startSeconds 循环起点。
/// @param endSeconds 排除结束点。
/// @return 参数和时间线均有效时返回 true。
bool AudioManager::setAudioTimelineLoop(double startSeconds, double endSeconds)
{
    if ( !m_audioTimelineNode || !std::isfinite(startSeconds) ||
         !std::isfinite(endSeconds) || startSeconds >= endSeconds ) {
        return false;
    }
    resetMainTimeStretcher();
    return m_audioTimelineNode->setLoop({ secondsToTimelineFrame(startSeconds),
                                          secondsToTimelineFrame(endSeconds) });
}

/// @brief 关闭主时间线循环并清除拉伸历史。
void AudioManager::clearAudioTimelineLoop()
{
    if ( m_audioTimelineNode ) {
        resetMainTimeStretcher();
        m_audioTimelineNode->clearLoop();
    }
}

/// @brief 将旧单 BGM 请求包装为零秒单事件时间线。
/// @param filePath 音频文件路径。
/// @param config 资源配置；仅 volume 和 muted 逐片段生效。
/// @return 单片段成功载入时返回 true。
bool AudioManager::loadBGM(const std::string&      filePath,
                           const AudioTrackConfig& config)
{
    const auto fingerprint = makeAudioPathSyncKey(filePath);
    const auto result      = loadAudioTimeline({ AudioTimelineLoadEvent{
                                                   .eventId     = 0U,
                                                   .resourceKey = fingerprint,
                                                   .filePath    = filePath,
                                                   .effectiveStartSeconds = 0.0,
                                                   .eventVolume           = 1.0F,
                                                   .resourceConfig        = config,
                                               } },
                                               0.0,
                                               fingerprint);
    return result.success && result.loadedClipCount == 1U;
}

/// @brief 兼容入口：卸载当前复合时间线。
void AudioManager::unloadBGM()
{
    unloadAudioTimeline();
}

/// @brief 获取当前时间线首个成功加载的音轨，供旧可视化入口兼容。
/// @return 没有有效采样时返回空指针。
std::shared_ptr<ice::AudioTrack> AudioManager::getBGMTrack() const
{
    return m_bgmTrack;
}

/// @brief 获取单片段兼容时间线的文件路径。
/// @return 复合时间线或未加载时返回空字符串。
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

/// @brief 获取当前时间线兼容同步键。
/// @return 完整时间线指纹；未加载时为空。
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
