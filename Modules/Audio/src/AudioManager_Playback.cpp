#include "audio/AudioManager.h"
#include "audio/AudioTimelineMixerNode.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include <ice/core/MixBus.hpp>
#include <ice/core/SourceNode.hpp>
#include <ice/core/effect/TimeStretcher.hpp>

namespace MMM::Audio
{
namespace
{
// 本文件只负责已经构造完成的主时间线和试听通道的播放控制。资源解码、图替换
// 与设备管理位于其他实现文件，因此这里的公开操作应保持轻量，不访问文件系统。
//
// 主时间线的可观察状态来自两层：
//
// - AudioTimelineMixerNode 保存谱面帧位置、请求状态和完成标记；
// - TimeStretcher 可能仍在排空已经接收的 final 输入。
//
// 因而节点到达 finished 时不一定立刻对外报告 Stopped。只有拉伸器也排空后，
// getStatus 和 clockSnapshot 才发布最终停止状态，防止 UI 提前结束进度显示。
//
// Seek 分为连续 ScrubUpdate 和提交两种语义。连续更新仅写时间线邮箱，依靠 epoch
// 让拉伸器在 block 边界丢弃历史；提交 Seek 额外清除排定音效。这样拖动期间不
// 遍历所有音效池，鼠标释放时仍能保证旧位置的音效不会继续播放。
//
// 试听通道使用独立 SourceNode 与 TimeStretcher，不参与主时间线时钟。其状态
// 缓存用于区分 SourceNode 当前未播放时的 Paused 与 Stopped，因为底层单一
// isplaying 标志无法表达这两个业务状态。
// 所有 getter 都允许在未加载或关闭过程中调用，并以零值或 Stopped 作为稳定
// 退路，避免 UI 为生命周期过渡额外持有音频节点。

/// @brief 将秒数安全转换为主时间线帧。
/// @param seconds 目标秒数，允许负值。
/// @return 最近的可表达时间线帧；非有限值回退为零。
AudioTimelineFrame playbackSecondsToFrame(double seconds) noexcept
{
    // 使用 long double 完成乘法，先钳制再转换，避免超范围整数转换未定义行为。
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

/// @brief 将项目拉伸质量转换为 IonCachyEngine 质量。
/// @param quality 项目全局预览质量。
/// @return 对应引擎枚举。
ice::TimeStretchQuality toIceStretchQuality(
    AudioManager::StretchQuality quality) noexcept
{
    switch ( quality ) {
    case AudioManager::StretchQuality::Fast:
        return ice::TimeStretchQuality::Fast;
    case AudioManager::StretchQuality::Balanced:
        return ice::TimeStretchQuality::Balanced;
    case AudioManager::StretchQuality::Finer:
        return ice::TimeStretchQuality::Finer;
    case AudioManager::StretchQuality::Best:
        return ice::TimeStretchQuality::Best;
    }
    return ice::TimeStretchQuality::Finer;
}
}  // namespace

/// @brief 开始或恢复复合音频时间线。
///
/// 已自然结束的流需要先清除拉伸历史；暂停恢复则保留当前历史以连续输出。
void AudioManager::play()
{
    if ( m_audioTimelineLoaded && m_audioTimelineNode ) {
        const bool restartFinishedStream = m_audioTimelineNode->finished();
        // finished 表示上次 final 已提交，重新播放必须建立新的 discontinuity。
        if ( restartFinishedStream ) {
            resetMainTimeStretcher();
        }
        m_audioTimelineNode->play();
        if ( m_stretcher ) {
            // 先发布时间线请求，再解除下游暂停，防止拉取到旧停止状态。
            m_stretcher->set_paused(false);
        }
    }
}

/// @brief 暂停复合音频时间线。
///
/// 同时暂停时间线时钟和拉伸器输出；恢复时不重置位置或 DSP 历史。
void AudioManager::pause()
{
    if ( m_audioTimelineLoaded && m_audioTimelineNode ) {
        m_audioTimelineNode->pause();
        if ( m_stretcher ) {
            // 下游暂停阻止已缓存采样在时间线冻结后继续送往设备。
            m_stretcher->set_paused(true);
        }
    }
}

/// @brief 停止复合音频时间线并回到起始位置。
///
/// Stop 是完整提交操作：重置时间线、拉伸历史和全部排定音效。
void AudioManager::stop()
{
    if ( m_audioTimelineLoaded && m_audioTimelineNode ) {
        m_audioTimelineNode->stop();
        // stop 改变上游 epoch，手动请求同时覆盖下游尚未观察到新 epoch 的窗口。
        resetMainTimeStretcher();
        if ( m_stretcher ) {
            m_stretcher->set_paused(true);
        }
        clearAllScheduledSoundEffects();
        // 旧时间点触发的长音效不能跨越一次显式停止继续发声。
    }
}

/// @brief 跳转复合音频时间线播放位置。
/// @param seconds 目标时间，单位为秒。
/// @param mode 完整提交或连续拖动中间更新。
/// @warning 逻辑控制路径：连续拖动中间更新只写入无锁 seek 邮箱；完整提交
/// 才允许遍历并停止音效池。
void AudioManager::seek(double seconds, AudioSeekMode mode)
{
    if ( m_audioTimelineLoaded && m_audioTimelineNode ) {
        m_audioTimelineNode->seek(playbackSecondsToFrame(seconds));
        // 节点内部负责钳制到有效循环或时间线范围。
        if ( mode == AudioSeekMode::ScrubUpdate ) {
            // 时间线应用 seek 时会增加 epoch，主拉伸器的代际读取器会在音频
            // block 边界清理历史；中间帧无需重复发送手动 discontinuity。
            return;
        }
        resetMainTimeStretcher();
        // 提交路径立即通知拉伸器，并清理独立音效池的旧调度。
        clearAllScheduledSoundEffects();
    }
}

/// @brief 获取当前播放状态。
/// @return 当前播放状态。
///
/// 优先使用回调发布的完整快照；快照尚未产生时根据请求状态和拉伸器状态构造
/// 保守结果。该回退主要覆盖刚加载、刚替换调度和后端尚未拉取首个 block。
PlaybackStatus AudioManager::getStatus() const
{
    const auto snapshot = getAudioTimelineClockSnapshot();
    if ( !snapshot.valid ) {
        // 没有有效图时统一报告停止，不暴露内部未初始化状态。
        if ( !m_audioTimelineLoaded || !m_audioTimelineNode ) {
            return PlaybackStatus::Stopped;
        }
        const auto requestedState = m_audioTimelineNode->requestedState();
        if ( m_stretcher && m_stretcher->is_paused() ) {
            // 下游暂停时，节点的 Playing 请求尚不能形成可听播放。
            return requestedState == AudioTimelinePlaybackState::Paused
                       ? PlaybackStatus::Paused
                       : PlaybackStatus::Stopped;
        }
        if ( m_audioTimelineNode->finished() ) {
            // final 输入仍在拉伸器时继续报告 Playing，直到输出完全排空。
            return m_stretcher && !m_stretcher->is_final_input_drained()
                       ? PlaybackStatus::Playing
                       : PlaybackStatus::Stopped;
        }
        switch ( requestedState ) {
        // 回调尚未发布时，请求状态是最接近用户操作的稳定退路。
        case AudioTimelinePlaybackState::Stopped:
            return PlaybackStatus::Stopped;
        case AudioTimelinePlaybackState::Paused: return PlaybackStatus::Paused;
        case AudioTimelinePlaybackState::Playing:
            return PlaybackStatus::Playing;
        }
        return PlaybackStatus::Stopped;
    }
    switch ( snapshot.state ) {
    // 有效快照已经合并回调位置和状态，可以直接映射业务枚举。
    case AudioTimelinePlaybackState::Stopped: return PlaybackStatus::Stopped;
    case AudioTimelinePlaybackState::Paused: return PlaybackStatus::Paused;
    case AudioTimelinePlaybackState::Playing: return PlaybackStatus::Playing;
    }
    return PlaybackStatus::Stopped;
}

/// @brief 获取复合音频时间线当前播放时间。
/// @return 当前播放时间，单位为秒。
///
/// 优先读取同一代次的时钟快照。回调尚未发布快照时才读取节点请求位置，避免
/// 在正常播放中组合来自不同时间点的状态和帧数。
double AudioManager::getCurrentTime() const
{
    const auto snapshot = getAudioTimelineClockSnapshot();
    if ( snapshot.valid ) return snapshot.positionSeconds();
    // 回退换算仍验证采样率，防止异常配置产生无穷或除零结果。
    if ( !m_audioTimelineLoaded || !m_audioTimelineNode ) return 0.0;
    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( sampleRate <= 0.0 ) return 0.0;
    return static_cast<double>(m_audioTimelineNode->positionFrame()) /
           sampleRate;
}

AudioTimelineClockSnapshot
AudioManager::getAudioTimelineClockSnapshot() const noexcept
{
    // 空时间线仍可有效；只有尚未 load 或节点不存在时返回空快照。
    if ( !m_audioTimelineLoaded || !m_audioTimelineNode ) return {};

    auto snapshot = m_audioTimelineNode->clockSnapshot();
    snapshot.sampleRate =
        static_cast<std::uint32_t>(ice::ICEConfig::internal_format.samplerate);
    snapshot.playbackRate = m_speed;
    // 节点负责谱面帧，管理器补充全局预览倍率和引擎采样率。
    snapshot.valid = snapshot.valid && snapshot.sampleRate > 0U &&
                     std::isfinite(snapshot.playbackRate) &&
                     snapshot.playbackRate > 0.0;
    if ( !snapshot.valid ) return {};

    if ( m_stretcher && m_stretcher->is_paused() ) {
        // 下游暂停优先于上游 Playing 请求，确保 UI 不显示仍在输出。
        snapshot.state = snapshot.state == AudioTimelinePlaybackState::Paused
                             ? AudioTimelinePlaybackState::Paused
                             : AudioTimelinePlaybackState::Stopped;
        return snapshot;
    }
    if ( snapshot.finished ) {
        // finished 后等待 stretcher drain，保持自然尾音和视觉状态一致。
        snapshot.state = m_stretcher && !m_stretcher->is_final_input_drained()
                             ? AudioTimelinePlaybackState::Playing
                             : AudioTimelinePlaybackState::Stopped;
    }
    return snapshot;
}

/// @brief 获取谱面内容与所有自动采样共同决定的复合时长。
/// @return 总时长，单位为秒。
///
/// 时长属于资源定义，不除以全局播放倍率；倍率只改变实际经过的墙钟时间。
double AudioManager::getTotalTime() const
{
    if ( !m_audioTimelineLoaded || !m_audioTimelineNode ) return 0.0;
    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( sampleRate <= 0.0 ) return 0.0;
    return static_cast<double>(m_audioTimelineNode->timelineEndFrame()) /
           sampleRate;
}

/// @brief 请求在下一个 block 边界清除 Rubber Band 历史采样。
/// @warning 低频播放控制路径：只写入 lock-free discontinuity 邮箱。
///
/// 没有拉伸器时静默返回，覆盖初始化失败或 shutdown 过程中的控制调用。
void AudioManager::resetMainTimeStretcher()
{
    if ( !m_stretcher ) return;
    static_cast<void>(m_stretcher->request_discontinuity());
}

/// @brief 设置复合时间线全局预览播放倍率。
/// @param speed 目标播放倍率。
///
/// 有限值钳制到引擎支持范围，非有限输入回退到中性倍率。该参数不重新准备资源
/// PCM，也不改变 getTotalTime 返回的谱面时长。
void AudioManager::setPlaybackSpeed(double speed)
{
    m_speed = std::isfinite(speed) ? std::clamp(speed, 0.1, 4.0) : 1.0;
    // 成员始终先更新，即使图尚未初始化也能在 init 时恢复用户请求。
    if ( m_stretcher ) {
        m_stretcher->set_playback_ratio(m_speed);
    }
}

/// @brief 获取用户配置的播放倍率。
/// @return 当前播放倍率。
double AudioManager::getPlaybackSpeed() const
{
    return m_speed;
}

/// @brief 获取音频拉伸器实际生效的播放倍率。
/// @return 实际播放倍率。
double AudioManager::getActualPlaybackSpeed() const
{
    if ( m_stretcher ) {
        return m_stretcher->get_actual_playback_ratio();
    }
    return m_speed;
}

/// @brief 设置复合时间线全局预览音高偏移。
/// @param semitones 半音偏移量。
///
/// 全局音高由主拉伸器实时应用，与资源配置中的离线音高处理相互独立。
void AudioManager::setPlaybackPitch(double semitones)
{
    m_playbackPitch =
        std::isfinite(semitones) ? std::clamp(semitones, -24.0, 24.0) : 0.0;
    if ( m_stretcher ) {
        m_stretcher->set_pitch_semitones(m_playbackPitch);
    }
}

/// @brief 获取复合时间线全局预览音高偏移。
/// @return 半音偏移量。
double AudioManager::getPlaybackPitch() const
{
    return m_playbackPitch;
}

/// @brief 设置复合时间线全局预览拉伸质量。
/// @param quality 目标拉伸质量。
///
/// 先验证项目枚举，未知值回退 Finer；随后统一映射为 ICE 枚举。
void AudioManager::setPlaybackQuality(StretchQuality quality)
{
    switch ( quality ) {
    // 显式列出有效枚举，防止来自旧配置的任意整数穿透到引擎。
    case StretchQuality::Fast:
    case StretchQuality::Balanced:
    case StretchQuality::Finer:
    case StretchQuality::Best: m_playbackQuality = quality; break;
    default: m_playbackQuality = StretchQuality::Finer; break;
    }
    if ( m_stretcher ) {
        m_stretcher->set_quality(toIceStretchQuality(m_playbackQuality));
    }
}

/// @brief 获取复合时间线全局预览拉伸质量。
/// @return 当前拉伸质量。
AudioManager::StretchQuality AudioManager::getPlaybackQuality() const
{
    return m_playbackQuality;
}

/// @brief 开始或恢复独立试听音轨播放。
///
/// 若试听已经到达尾部则先回零，使“播放”操作具备重新开始语义。
void AudioManager::playAudition()
{
    if ( !m_auditionSource ) {
        return;
    }

    const double totalTime = getAuditionTotalTime();
    // 一毫秒容差吸收采样帧换算误差，避免尾帧停留时无法重播。
    if ( totalTime > 0.0 && getAuditionCurrentTime() >= totalTime - 0.001 ) {
        seekAudition(0.0);
    }
    m_auditionSource->play();
    m_auditionStatus = PlaybackStatus::Playing;
}

/// @brief 暂停独立试听音轨播放。
///
/// 已停止对象收到 pause 时保持 Stopped，不伪造一次暂停会话。
void AudioManager::pauseAudition()
{
    if ( !m_auditionSource ) {
        return;
    }

    m_auditionSource->pause();
    if ( m_auditionStatus != PlaybackStatus::Stopped ) {
        m_auditionStatus = PlaybackStatus::Paused;
    }
}

/// @brief 停止独立试听音轨并回到起始位置。
///
/// SourceNode 没有独立 stop，使用 pause 加零帧位置实现业务停止。
void AudioManager::stopAudition()
{
    if ( m_auditionSource ) {
        m_auditionSource->pause();
        m_auditionSource->set_playpos(static_cast<size_t>(0));
    }
    m_auditionStatus = PlaybackStatus::Stopped;
}

/// @brief 跳转独立试听音轨播放位置。
/// @param seconds 目标时间，单位为秒。
///
/// 目标钳制到音轨范围。Seek 不改变播放与暂停状态；停止状态显式保持停止。
void AudioManager::seekAudition(double seconds)
{
    if ( !m_auditionSource ) {
        return;
    }

    const PlaybackStatus statusBeforeSeek = getAuditionStatus();
    // 先读取状态再修改位置，避免底层 Seek 瞬间影响 isplaying 判断。
    const double clampedTime =
        std::clamp(seconds, 0.0, std::max(0.0, getAuditionTotalTime()));
    m_auditionSource->set_playpos(std::chrono::duration<double>(clampedTime));
    if ( statusBeforeSeek == PlaybackStatus::Stopped ) {
        m_auditionStatus = PlaybackStatus::Stopped;
    }
}

/// @brief 获取独立试听音轨的当前播放状态。
/// @return 当前试听播放状态。
///
/// 底层正在推进时优先报告 Playing，否则使用管理器缓存区分暂停和自然停止。
PlaybackStatus AudioManager::getAuditionStatus() const
{
    if ( !m_auditionSource ) {
        return PlaybackStatus::Stopped;
    }
    if ( m_auditionSource->isplaying() ) {
        return PlaybackStatus::Playing;
    }
    return m_auditionStatus == PlaybackStatus::Paused ? PlaybackStatus::Paused
                                                      : PlaybackStatus::Stopped;
}

/// @brief 获取独立试听音轨当前播放时间。
/// @return 当前播放时间，单位为秒。
///
/// SourceNode 暴露帧索引，本层按内部统一采样率换算成业务秒数。
double AudioManager::getAuditionCurrentTime() const
{
    if ( !m_auditionSource ) {
        return 0.0;
    }

    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    if ( sampleRate <= 0.0 ) {
        return 0.0;
    }
    return static_cast<double>(m_auditionSource->get_playpos()) / sampleRate;
}

/// @brief 获取独立试听音轨总时长。
/// @return 总时长，单位为秒。
///
/// 使用引擎提供的 duration，避免自行假设声道数或 PCM 帧布局。
double AudioManager::getAuditionTotalTime() const
{
    if ( !m_auditionSource ) {
        return 0.0;
    }
    return std::chrono::duration_cast<std::chrono::duration<double>>(
               m_auditionSource->total_time())
        .count();
}

/// @brief 设置独立试听音轨播放倍率。
/// @param speed 目标播放倍率。
///
/// 试听倍率与主时间线倍率独立保存；调用方可在编辑同时预听原速资源。
void AudioManager::setAuditionPlaybackSpeed(double speed)
{
    m_auditionSpeed = std::clamp(speed, 0.1, 4.0);
    // 试听接口由受控 UI 数值调用，沿用既有有限输入前置条件并钳制范围。
    if ( m_auditionStretcher ) {
        m_auditionStretcher->set_playback_ratio(m_auditionSpeed);
    }
}

/// @brief 获取独立试听音轨请求的播放倍率。
/// @return 当前请求的播放倍率。
double AudioManager::getAuditionPlaybackSpeed() const
{
    return m_auditionSpeed;
}

/// @brief 获取独立试听拉伸器实际生效的播放倍率。
/// @return 当前实际播放倍率。
double AudioManager::getActualAuditionPlaybackSpeed() const
{
    if ( m_auditionStretcher ) {
        return m_auditionStretcher->get_actual_playback_ratio();
    }
    return m_auditionSpeed;
}

}  // namespace MMM::Audio
