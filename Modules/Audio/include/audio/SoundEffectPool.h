#pragma once

#include "audio/KeySoundTypes.h"
#include "audio/StereoGainEnvelope.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ice
{
class AudioTrack;
class MixBus;
}  // namespace ice

namespace MMM::Audio
{

class PreparedTimelineAudio;
class KeySoundControlBank;

/// @brief 音效预定起播采用的帧时钟域。
enum class SoundEffectScheduleMode : std::uint8_t {
    AbsoluteTimelineFrame,
    RelativeOutputDelay,
};

/// @brief 已换算到目标音频路由时钟域的音效调度。
struct SoundEffectSchedulePlan {
    /// @brief 应由 SourceNode 使用的调度模式。
    SoundEffectScheduleMode mode{
        SoundEffectScheduleMode::AbsoluteTimelineFrame
    };

    /// @brief 绝对时间线目标帧或相对输出延迟帧。
    std::size_t frame{ 0U };
};

/// @brief 根据打击音效路由将时间线帧差换算为实际起播计划。
/// @param targetTimelineFrame 目标时间线输入帧。
/// @param currentTimelineFrame 当前时间线输入帧。
/// @param previewSpeed 全局预览播放倍率。
/// @param syncSpeed 打击音效是否位于全局变速器之前。
/// @return 同步路由使用绝对时间线帧；非同步路由使用设备输出帧延迟。
[[nodiscard]] SoundEffectSchedulePlan planSoundEffectSchedule(
    std::size_t targetTimelineFrame, std::size_t currentTimelineFrame,
    double previewSpeed, bool syncSpeed) noexcept;

/**
 * @brief 音效池，用于管理同一音效的多个并发播放实例
 */
class SoundEffectPool
{
public:
    /// @brief 不持有上下文的时间线参考位置读取函数。
    using ReferencePositionReader =
        std::size_t (*)(const void* context) noexcept;

    /// @brief 构造音效池
    /// @param track 音轨资源
    /// @param keySoundControls 生命周期覆盖本池的 Key 音控制库。
    SoundEffectPool(std::shared_ptr<ice::AudioTrack> track,
                    const KeySoundControlBank* keySoundControls = nullptr);

    /// @brief 从已完成资源级 DSP 的只读 PCM 构造音效池。
    /// @param audio 与自动采样时间线共享的预处理 PCM。
    /// @param keySoundControls 生命周期覆盖本池的 Key 音控制库。
    explicit SoundEffectPool(
        std::shared_ptr<const PreparedTimelineAudio> audio,
        const KeySoundControlBank* keySoundControls = nullptr);

    ~SoundEffectPool();

    /// @brief 获取该音效池的局部混音器输出节点，供外部路由
    std::shared_ptr<ice::MixBus> getMixer() const;

    /// @brief 预分配节点 (必须在构造后调用一次)
    /// @param count 初始数量
    void init(int count = 8);

    /// @brief 播放一次音效。
    /// @param volumeFactor 本次播放相对资源基础音量的额外倍率。
    void play(float volumeFactor);

    /// @brief 使用指定音高播放一次音效。
    /// @param volumeFactor 本次播放相对资源基础音量的额外倍率。
    /// @param pitchSemitones 音高偏移，单位为半音。
    void play(float volumeFactor, double pitchSemitones);

    /// @brief 在指定时间播放音效
    /// @param volumeFactor 本次播放相对资源基础音量的额外倍率。
    /// @param targetFrame 目标帧位置（基于 BGM）
    /// @param referenceContext 生命周期覆盖预定播放的参考时钟上下文。
    /// @param referenceReader 无异常、无阻塞、无分配的参考位置读取函数。
    /// @param stereoEnvelope 本次播放的线性双声道增益包络。
    /// @param scheduledDelayFrames 从当前参考位置到目标帧的预计间隔。
    /// @param playbackControl 玩家轨道与打击音类别的运行时控制。
    void playScheduled(float volumeFactor, std::size_t targetFrame,
                       const void*                    referenceContext,
                       ReferencePositionReader        referenceReader,
                       const StereoGainEnvelope&      stereoEnvelope,
                       std::size_t                    scheduledDelayFrames,
                       const KeySoundPlaybackControl& playbackControl = {});

    /// @brief 按节点输出帧域中的相对延迟播放音效。
    /// @param volumeFactor 本次播放相对资源基础音量的额外倍率。
    /// @param outputDelayFrames 起播前需要经过的设备输出帧数。
    /// @param stereoEnvelope 本次播放的线性双声道增益包络。
    /// @param playbackControl 玩家轨道与打击音类别的运行时控制。
    void playScheduledRelative(
        float volumeFactor, std::size_t outputDelayFrames,
        const StereoGainEnvelope&      stereoEnvelope  = {},
        const KeySoundPlaybackControl& playbackControl = {});

    /// @brief 停止所有正在播放或预定的音效，并重置状态
    void stopAll();

    /// @brief 获取是否正在播放
    bool isPlaying() const;

    /// @brief 获取是否暂停中
    bool isPaused() const;

    /// @brief 暂停播放
    void pause();

    /// @brief 恢复播放
    void resume();

    /// @brief 设置池内所有节点的音量 (由 AudioManager 调用，会考虑全局音量)
    void setVolume(float volume);

    /// @brief 更新当前所有播放节点的音量 (当全局音量变化或静音状态变化时)
    void updateEffectiveVolume(float globalVolume, bool muted);

    /// @brief 获取池内音量 (原始音量，不含全局增益)
    float getVolume() const { return m_volume; }

    /// @brief 获取音效总时长 (秒)
    double getDuration() const;

    /// @brief 判断音效池是否读取指定的预处理 PCM。
    /// @param audio 待比较的非拥有 PCM 地址。
    /// @return 地址与池内共享资源相同时返回 true。
    [[nodiscard]] bool usesPreparedAudio(
        const PreparedTimelineAudio* audio) const noexcept;

    /// @brief 获取最近一次播放的进度 (秒)
    double getLatestPlaybackTime() const;

private:
    /// @brief 单个可复用的音效播放实例。
    struct SFXPlayInstance;

    /// @brief 创建一个接入本地混音器的播放实例。
    std::shared_ptr<SFXPlayInstance> createInstance();

    /// @brief 从池中取出一个可播放实例，必要时扩容。
    std::shared_ptr<SFXPlayInstance> acquireInstance();

    /// @brief 与自动采样时间线共享的已预处理 PCM。
    std::shared_ptr<const PreparedTimelineAudio> m_audio;

    /// @brief 音频回调读取的稳定 Key 音控制库观察指针。
    /// @warning AudioManager 持有者生命周期必须覆盖本池及其所有
    /// 实例；回调每个活跃实例每 block 解引用。
    const KeySoundControlBank* m_keySoundControls{ nullptr };

    std::shared_ptr<ice::MixBus> m_localMixer;

    std::vector<std::shared_ptr<SFXPlayInstance>> m_allInstances;
    std::shared_ptr<SFXPlayInstance>              m_latestInstance;
    mutable std::mutex                            m_mtx;

    /// @brief 资源基础音量。
    float m_volume{ 1.0f };

    /// @brief 已合并资源、全局和总线静音状态的当前基础增益。
    float m_effectiveVolume{ 1.0F };
};

}  // namespace MMM::Audio
