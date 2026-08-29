#include "BackgroundSpectrumAnalyzer.h"
#include "audio/AudioManager.h"
#include "audio/AudioTimelineMixerNode.h"
#include "audio/AudioTimelineResourceProcessor.h"
#include "audio/KeySoundControl.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/project/AudioResource.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <thread>
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

/// @brief 等待音频回调接管指定时间线调度，并回收被替换的旧调度。
/// @param node 持有新旧调度的时间线节点。
/// @param generation 需要等待发布的调度代次。
/// @return 在超时前观察到目标代次时返回 true。
/// @warning 项目卸载低频路径：最多阻塞 500 毫秒等待音频 block 边界，禁止
/// 在音频回调、UI 渲染或逻辑热路径中调用。
bool waitForTimelineScheduleRetirement(AudioTimelineMixerNode& node,
                                       std::uint64_t           generation)
{
    using namespace std::chrono_literals;
    constexpr auto TIMEOUT  = 500ms;
    const auto     deadline = std::chrono::steady_clock::now() + TIMEOUT;
    while ( std::chrono::steady_clock::now() < deadline ) {
        if ( const auto snapshot = node.clockSnapshot();
             snapshot.valid && snapshot.scheduleGeneration == generation ) {
            static_cast<void>(node.reclaimRetiredSchedules());
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    static_cast<void>(node.reclaimRetiredSchedules());
    return false;
}

}  // namespace

/// @brief 查找或建立自动采样与 HitEffect 共用的资源 DSP PCM。
/// @warning 低频控制路径：缓存未命中且无候选时会执行完整离线 DSP。
std::shared_ptr<const PreparedTimelineAudio>
AudioManager::getOrPrepareAudioTimelineResource(
    const std::string& filePath, const std::shared_ptr<ice::AudioTrack>& track,
    const AudioTrackConfig&                      resourceConfig,
    std::shared_ptr<const PreparedTimelineAudio> preparedCandidate)
{
    if ( !track || track->num_frames() == 0U ) return {};

    const auto processingCacheKey =
        makeAudioResourceProcessingCacheKey(filePath, resourceConfig);
    const auto existingPreparedAudio =
        m_audioTimelineResourceCache.find(processingCacheKey);
    if ( existingPreparedAudio != m_audioTimelineResourceCache.end() &&
         existingPreparedAudio->second.sourceTrack.lock() == track ) {
        if ( auto prepared =
                 existingPreparedAudio->second.preparedAudio.lock() ) {
            return prepared;
        }
    }

    auto preparedAudio = std::move(preparedCandidate);
    if ( !preparedAudio ) {
        preparedAudio = prepareAudioTimelineResource(track, resourceConfig);
    }
    if ( preparedAudio ) {
        m_audioTimelineResourceCache.insert_or_assign(
            processingCacheKey,
            CachedTimelineResourceAudio{
                .sourceTrack   = track,
                .preparedAudio = preparedAudio,
            });
    }
    return preparedAudio;
}

/// @brief 在非实时路径准备全部资源并替换复合音频时间线。
/// @param events 自动采样事件。
/// @param chartEndSeconds 非音频谱面内容结束时间。
/// @param fingerprint 完整时间线稳定指纹。
/// @return 图替换结果及逐事件诊断。
/// @warning 低频资源路径：会访问文件系统、等待资源解码并执行资源级离线 DSP。
AudioTimelineLoadResult AudioManager::loadAudioTimeline(
    const std::vector<AudioTimelineLoadEvent>& events, double chartEndSeconds,
    const std::string& fingerprint)
{
    AudioTimelineLoadResult result;
    if ( !m_audioPool || !m_threadPool || !m_audioTimelineNode ) {
        result.diagnostics.push_back(AudioTimelineLoadDiagnostic{
            .code    = AudioTimelineLoadDiagnosticCode::AudioSystemUnavailable,
            .message = "音频系统尚未初始化",
        });
        return result;
    }

    static_cast<void>(m_audioTimelineNode->reclaimRetiredSchedules());

    std::vector<PreparedTimelineClip> preparedClips;
    preparedClips.reserve(events.size());
    std::unordered_map<std::string, std::shared_ptr<ice::AudioTrack>>
        tracksByPath;
    tracksByPath.reserve(events.size());
    std::unordered_map<std::string,
                       std::shared_ptr<const PreparedTimelineAudio>>
        preparedAudioByProcessingKey;
    preparedAudioByProcessingKey.reserve(events.size());
    std::erase_if(m_audioTimelineResourceCache, [](const auto& cacheEntry) {
        return cacheEntry.second.sourceTrack.expired() ||
               cacheEntry.second.preparedAudio.expired();
    });
    std::shared_ptr<ice::AudioTrack> firstLoadedTrack;

    // 先提交全部唯一文件的解码任务，避免逐文件启动后立即等待导致串行化。
    for ( const auto& event : events ) {
        if ( event.filePath.empty() || tracksByPath.contains(event.filePath) ) {
            continue;
        }
        auto track =
            m_audioPool->get_or_load(*m_threadPool, event.filePath).lock();
        tracksByPath.emplace(event.filePath, std::move(track));
    }
    result.requestedSourceCount = tracksByPath.size();

    // 全部解码任务均已提交后，才按事件顺序等待并准备唯一 DSP 结果。
    for ( const auto& event : events ) {
        double startSeconds = event.effectiveStartSeconds;
        if ( !std::isfinite(startSeconds) ) {
            appendTimelineDiagnostic(
                result,
                AudioTimelineLoadDiagnosticCode::InvalidStartTime,
                event);
            startSeconds = 0.0;
        }

        std::shared_ptr<ice::AudioTrack> track;
        if ( !event.filePath.empty() ) {
            const auto existingTrack = tracksByPath.find(event.filePath);
            if ( existingTrack != tracksByPath.end() ) {
                track = existingTrack->second;
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

        const auto processingCacheKey = makeAudioResourceProcessingCacheKey(
            event.filePath, event.resourceConfig);
        std::shared_ptr<const PreparedTimelineAudio> preparedAudio;
        const auto                                   existingPreparedAudio =
            preparedAudioByProcessingKey.find(processingCacheKey);
        if ( existingPreparedAudio != preparedAudioByProcessingKey.end() ) {
            preparedAudio = existingPreparedAudio->second;
        } else {
            preparedAudio = getOrPrepareAudioTimelineResource(
                event.filePath, track, event.resourceConfig);
            preparedAudioByProcessingKey.emplace(processingCacheKey,
                                                 preparedAudio);
            if ( preparedAudio ) {
                ++result.preparedResourceCount;
            }
        }
        if ( !preparedAudio ) {
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
            .eventId       = event.eventId,
            .sourceKey     = event.resourceKey,
            .startFrame    = secondsToTimelineFrame(startSeconds),
            .bgmTrackIndex = event.bgmTrackIndex,
            .volume = resourceVolume * sanitizedEventVolume(event.eventVolume),
            .audio  = std::move(preparedAudio),
        });
    }

    const double normalizedChartEnd =
        std::isfinite(chartEndSeconds) ? std::max(chartEndSeconds, 0.0) : 0.0;
    m_audioTimelineBaseClips = std::move(preparedClips);
    m_audioTimelineRequestedEndFrame =
        secondsToTimelineFrame(normalizedChartEnd);
    m_audioTimelineMaximumProcessFrames =
        std::max<std::size_t>(ice::ICEConfig::default_buffer_size, 1U);
    result.scheduleGeneration = m_audioTimelineNode->replaceSchedule(
        m_audioTimelineBaseClips,
        m_audioTimelineRequestedEndFrame,
        m_audioTimelineMaximumProcessFrames);
    resetMainTimeStretcher();
    m_audioTimelineLoaded           = true;
    m_audioTimelineFingerprint      = fingerprint;
    m_audioTimelineClipCount        = m_audioTimelineNode->clipCount();
    m_missingAudioTimelineClipCount = result.missingClipCount;
    m_bgmTrack                      = std::move(firstLoadedTrack);
    m_bgmPath = events.size() == 1U ? events.front().filePath : std::string{};
    m_bgmSyncKey = fingerprint;

    refreshAudioTimelineVolume();
    setPlaybackSpeed(m_speed);
    setPlaybackPitch(m_playbackPitch);
    setPlaybackQuality(m_playbackQuality);

    result.success         = true;
    result.loadedClipCount = m_audioTimelineClipCount;
    XINFO(
        "Audio timeline loaded: sources={}, prepared={}, clips={}, missing={}, "
        "end={}s, fingerprint={}",
        result.requestedSourceCount,
        result.preparedResourceCount,
        result.loadedClipCount,
        result.missingClipCount,
        getTotalTime(),
        fingerprint);
    return result;
}

/// @brief 停止并卸载当前复合音频时间线。
void AudioManager::unloadAudioTimeline()
{
    if ( !m_audioTimelineNode || !m_audioTimelineLoaded ) {
        return;
    }

    static_cast<void>(m_audioTimelineNode->reclaimRetiredSchedules());
    m_audioTimelineNode->stop();
    m_audioTimelineNode->clearLoop();
    clearAllScheduledSoundEffects();
    const std::uint64_t emptyScheduleGeneration =
        m_audioTimelineNode->replaceSchedule(
            {},
            0,
            std::max<std::size_t>(ice::ICEConfig::default_buffer_size, 1U));
    m_audioTimelineBaseClips.clear();
    m_audioTimelineRequestedEndFrame    = 0;
    m_audioTimelineMaximumProcessFrames = 1U;
    resetMainTimeStretcher();
    m_audioTimelineLoaded = false;
    m_audioTimelineFingerprint.clear();
    m_audioTimelineClipCount        = 0U;
    m_missingAudioTimelineClipCount = 0U;
    m_bgmTrack.reset();
    m_bgmPath.clear();
    m_bgmSyncKey.clear();
    if ( m_stretcher ) {
        // 停止状态下拉伸器不会拉取上游；短暂恢复空时间线分支，确保音频
        // 回调能在下一 block 接管空调度，期间不会产生可听输出。
        m_stretcher->set_paused(false);
    }
    if ( !waitForTimelineScheduleRetirement(*m_audioTimelineNode,
                                            emptyScheduleGeneration) ) {
        XWARN(
            "Timed out waiting for the audio callback to retire the unloaded "
            "timeline schedule.");
    }
    if ( m_stretcher ) {
        m_stretcher->set_paused(true);
    }
    static_cast<void>(releaseUnusedTrackCache());
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
    return m_audioTimelineLoaded;
}

/// @brief 设置整个玩家打击音区的运行时静音覆盖。
void AudioManager::setPlayerKeySoundAreaMuted(bool muted) noexcept
{
    m_keySoundControls->setPlayerAreaMuted(muted);
}

/// @brief 查询整个玩家打击音区的运行时静音覆盖。
bool AudioManager::isPlayerKeySoundAreaMuted() const noexcept
{
    return m_keySoundControls->isPlayerAreaMuted();
}

/// @brief 设置指定玩家轨道的 Key 音静音状态。
void AudioManager::setPlayerKeySoundTrackMuted(std::uint32_t trackIndex,
                                               bool          muted) noexcept
{
    m_keySoundControls->setPlayerTrackMuted(trackIndex, muted);
}

/// @brief 查询指定玩家轨道是否已静音。
bool AudioManager::isPlayerKeySoundTrackMuted(
    std::uint32_t trackIndex) const noexcept
{
    return m_keySoundControls->isPlayerTrackMuted(trackIndex);
}

/// @brief 设置指定玩家轨道的 Key 音线性增益。
void AudioManager::setPlayerKeySoundTrackGain(std::uint32_t trackIndex,
                                              float         gain) noexcept
{
    m_keySoundControls->setPlayerTrackGain(trackIndex, gain);
}

/// @brief 查询指定玩家轨道的 Key 音线性增益。
float AudioManager::getPlayerKeySoundTrackGain(
    std::uint32_t trackIndex) const noexcept
{
    return m_keySoundControls->getPlayerTrackGain(trackIndex);
}

/// @brief 设置整个 BGM 轨道区的 Key 音静音状态。
void AudioManager::setBgmKeySoundAreaMuted(bool muted) noexcept
{
    m_keySoundControls->setBgmAreaMuted(muted);
}

/// @brief 查询整个 BGM 轨道区是否已静音。
bool AudioManager::isBgmKeySoundAreaMuted() const noexcept
{
    return m_keySoundControls->isBgmAreaMuted();
}

/// @brief 设置整个 BGM Key 音区的线性增益。
void AudioManager::setBgmKeySoundAreaGain(float gain) noexcept
{
    m_keySoundControls->setBgmAreaGain(gain);
}

/// @brief 查询整个 BGM Key 音区的线性增益。
float AudioManager::getBgmKeySoundAreaGain() const noexcept
{
    return m_keySoundControls->getBgmAreaGain();
}

/// @brief 设置指定 BGM 轨道的 Key 音静音状态。
void AudioManager::setBgmKeySoundTrackMuted(std::uint32_t trackIndex,
                                            bool          muted) noexcept
{
    m_keySoundControls->setBgmTrackMuted(trackIndex, muted);
}

/// @brief 查询指定 BGM 轨道是否已静音。
bool AudioManager::isBgmKeySoundTrackMuted(
    std::uint32_t trackIndex) const noexcept
{
    return m_keySoundControls->isBgmTrackMuted(trackIndex);
}

/// @brief 设置指定 BGM 轨道的 Key 音线性增益。
void AudioManager::setBgmKeySoundTrackGain(std::uint32_t trackIndex,
                                           float         gain) noexcept
{
    m_keySoundControls->setBgmTrackGain(trackIndex, gain);
}

/// @brief 查询指定 BGM 轨道的 Key 音线性增益。
float AudioManager::getBgmKeySoundTrackGain(
    std::uint32_t trackIndex) const noexcept
{
    return m_keySoundControls->getBgmTrackGain(trackIndex);
}

/// @brief 设置未绑定或绑定打击音效类别的运行时静音覆盖。
void AudioManager::setKeySoundEffectGroupMuted(KeySoundEffectGroup group,
                                               bool muted) noexcept
{
    m_keySoundControls->setEffectGroupMuted(group, muted);
}

/// @brief 查询未绑定或绑定打击音效类别的运行时静音覆盖。
bool AudioManager::isKeySoundEffectGroupMuted(
    KeySoundEffectGroup group) const noexcept
{
    return m_keySoundControls->isEffectGroupMuted(group);
}

/// @brief 设置未绑定或绑定打击音效类别的线性增益。
void AudioManager::setKeySoundEffectGroupGain(KeySoundEffectGroup group,
                                              float               gain) noexcept
{
    m_keySoundControls->setEffectGroupGain(group, gain);
}

/// @brief 查询未绑定或绑定打击音效类别的线性增益。
float AudioManager::getKeySoundEffectGroupGain(
    KeySoundEffectGroup group) const noexcept
{
    return m_keySoundControls->getEffectGroupGain(group);
}

/// @brief 提交主时间线半开循环范围。
/// @param startSeconds 循环起点。
/// @param endSeconds 排除结束点。
/// @return 参数和时间线均有效时返回 true。
bool AudioManager::setAudioTimelineLoop(double startSeconds, double endSeconds)
{
    if ( !m_audioTimelineLoaded || !m_audioTimelineNode ||
         !std::isfinite(startSeconds) || !std::isfinite(endSeconds) ||
         startSeconds >= endSeconds ) {
        return false;
    }
    resetMainTimeStretcher();
    return m_audioTimelineNode->setLoop({ secondsToTimelineFrame(startSeconds),
                                          secondsToTimelineFrame(endSeconds) });
}

/// @brief 关闭主时间线循环并清除拉伸历史。
void AudioManager::clearAudioTimelineLoop()
{
    if ( m_audioTimelineLoaded && m_audioTimelineNode ) {
        resetMainTimeStretcher();
        m_audioTimelineNode->clearLoop();
    }
}

/// @brief 将旧单 BGM 请求包装为零秒单事件时间线。
/// @param filePath 音频文件路径。
/// @param config 完整资源配置；高级 DSP 离线应用且不覆盖全局预览参数。
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

/// @brief 释放只剩音频池自身持有的完整解码音轨。
/// @return 本次释放的缓存音轨数量。
/// @warning 低频资源卸载路径：可能析构完整 PCM，禁止在音频回调、UI
/// 渲染或逻辑热路径中调用。
std::size_t AudioManager::releaseUnusedTrackCache()
{
    if ( !m_audioPool ) return 0U;

    std::erase_if(m_audioTimelineResourceCache, [](const auto& cacheEntry) {
        return cacheEntry.second.sourceTrack.expired() ||
               cacheEntry.second.preparedAudio.expired();
    });
    const std::size_t releasedCount = m_audioPool->release_unused();
    if ( releasedCount > 0U ) {
        XDEBUG("Released {} unused decoded audio track(s).", releasedCount);
    }
    return releasedCount;
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
