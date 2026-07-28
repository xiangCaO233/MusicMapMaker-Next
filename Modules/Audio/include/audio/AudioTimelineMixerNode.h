#pragma once

#include "audio/AudioTimelineTransport.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ice/core/IAudioNode.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <memory>
#include <string>
#include <vector>

namespace ice
{
class AudioTrack;
}  // namespace ice

namespace MMM::Audio
{

/// @brief 已在非实时线程完成解析和缓存的时间线音频片段。
struct PreparedTimelineClip {
    /// @brief 谱面采样物件的稳定标识。
    AudioTimelineEventId eventId{ 0U };
    /// @brief 用于诊断和资源映射的稳定资源键。
    std::string sourceKey;
    /// @brief 片段在统一音频时间线上的起始帧，允许为负数。
    AudioTimelineFrame startFrame{ 0 };
    /// @brief 片段自身的线性音量。
    float volume{ 1.0F };
    /// @brief 已完整缓存的音频数据。
    ///
    /// @warning
    /// 共享所有权仅用于保证音频回调期间缓存不被资源线程释放；该指针只在构造
    /// 和销毁节点时复制，process 热路径不会复制 shared_ptr。
    std::shared_ptr<ice::AudioTrack> track;
};

/// @brief 将多个自动采样按统一时间线混合的实时音频节点。
///
/// 片段表在构造期间完成过滤和排序，之后保持不可变。播放控制由单个逻辑线程
/// 写入序列锁邮箱，并在音频 block 边界应用；回调内不执行分配、排序、锁、
/// 文件访问或资源解码。
class AudioTimelineMixerNode final : public ice::IAudioNode
{
public:
    /// @brief 构造多采样时间线节点。
    /// @param clips 已在非实时线程完整缓存的音频片段。
    /// @param requestedTimelineEndFrame
    /// 谱面物件决定的排除结束帧；最终结束帧还会
    ///        自动包含所有片段的实际结束位置。
    /// @param maximumProcessFrames 回调内单次缓存读取的最大帧数。
    AudioTimelineMixerNode(std::vector<PreparedTimelineClip> clips,
                           AudioTimelineFrame requestedTimelineEndFrame,
                           std::size_t        maximumProcessFrames);

    /// @brief 请求从当前位置播放。
    /// @warning 仅允许单个非实时逻辑线程写入控制邮箱。
    void play() noexcept;

    /// @brief 请求冻结整条时间线。
    /// @warning 仅允许单个非实时逻辑线程写入控制邮箱。
    void pause() noexcept;

    /// @brief 请求停止并回到时间线零点。
    /// @warning 仅允许单个非实时逻辑线程写入控制邮箱。
    void stop() noexcept;

    /// @brief 请求跳转到指定有符号时间线帧。
    /// @param frame 目标时间线帧。
    /// @warning 仅允许单个非实时逻辑线程写入控制邮箱。
    void seek(AudioTimelineFrame frame) noexcept;

    /// @brief 请求启用半开区间循环。
    /// @param range 有效范围必须满足 startFrame 小于 endFrame。
    /// @return 请求有效时返回 true。
    /// @warning 仅允许单个非实时逻辑线程写入控制邮箱。
    [[nodiscard]] bool setLoop(AudioTimelineLoopRange range) noexcept;

    /// @brief 请求关闭循环。
    /// @warning 仅允许单个非实时逻辑线程写入控制邮箱。
    void clearLoop() noexcept;

    /// @brief 获取最近一个已完成音频 block 的时间线位置。
    /// @return 下一 block 起始的有符号时间线帧。
    [[nodiscard]] AudioTimelineFrame positionFrame() const noexcept;

    /// @brief 获取最近一个已完成音频 block 的播放状态。
    [[nodiscard]] AudioTimelinePlaybackState state() const noexcept;

    /// @brief 获取逻辑线程最近请求的播放状态。
    /// @return 尚未到达 block 边界时也立即反映最新控制请求。
    [[nodiscard]] AudioTimelinePlaybackState requestedState() const noexcept;

    /// @brief 获取最近一个已完成音频 block 的传输纪元。
    [[nodiscard]] std::uint64_t epoch() const noexcept;

    /// @brief 判断非循环时间线是否已到达复合结束位置。
    [[nodiscard]] bool finished() const noexcept;

    /// @brief 获取谱面与全部采样共同决定的排除结束帧。
    [[nodiscard]] AudioTimelineFrame timelineEndFrame() const noexcept;

    /// @brief 获取排序并过滤后的有效采样数量。
    [[nodiscard]] std::size_t clipCount() const noexcept;

    /// @brief 设置复合时间线在逐事件音量之后应用的主增益。
    /// @param gain 非负线性增益；非有限值按零处理。
    /// @warning
    /// 逻辑线程写、音频线程逐 block 读取；relaxed 原子用于避免播放热路径加锁。
    void setMasterGain(float gain) noexcept;

    /// @brief 获取当前请求的复合时间线主增益。
    [[nodiscard]] float masterGain() const noexcept;

    /// @brief 获取复合时间线最近 block 的左声道峰值。
    [[nodiscard]] float leftLevel() const noexcept;

    /// @brief 获取复合时间线最近 block 的右声道峰值。
    [[nodiscard]] float rightLevel() const noexcept;

    /// @brief 生成一个设备音频 block。
    /// @param buffer 由上游预分配的输出缓冲区。
    /// @warning
    /// 音频回调热路径；禁止引入分配、锁、文件系统、资源解码、完整排序或日志。
    void process(ice::AudioBuffer& buffer) override;

private:
    /// @brief 控制邮箱中的目标播放命令。
    enum class PlaybackCommand : std::uint8_t {
        Stop,
        Play,
        Pause,
    };

    /// @brief 过滤、规范化并稳定排序预备片段。
    static std::vector<PreparedTimelineClip> prepareClips(
        std::vector<PreparedTimelineClip> clips);

    /// @brief 从已排序片段构造传输调度描述。
    static std::vector<TimelineClipSpec> buildClipSpecs(
        const std::vector<PreparedTimelineClip>& clips);

    /// @brief 计算谱面与全部音频片段的复合结束帧。
    static AudioTimelineFrame calculateTimelineEndFrame(
        const std::vector<PreparedTimelineClip>& clips,
        AudioTimelineFrame requestedTimelineEndFrame) noexcept;

    /// @brief 在 block 边界读取稳定控制快照并更新传输状态。
    /// @warning 音频热路径；仅执行有界原子读取和常数时间状态修改。
    void applyPendingControls() noexcept;

    /// @brief 发布回调线程的最新传输快照。
    /// @warning 音频热路径；仅执行 relaxed 原子写入。
    void publishTransportSnapshot() noexcept;

    /// @brief 混合当前位置起始且不跨越循环或结束边界的一段输出。
    /// @param output 输出缓冲区。
    /// @param outputStartFrame 输出缓冲区内的起始偏移。
    /// @param frameCount 本段帧数，必须不超过预分配缓存容量。
    /// @warning 音频热路径；只读取完整缓存并写入预分配内存。
    void mixSegment(ice::AudioBuffer& output, std::size_t outputStartFrame,
                    std::size_t frameCount);

    /// @brief 应用主增益并发布当前输出 block 峰值。
    /// @param output 已完成逐片段混合的输出缓冲。
    /// @warning 音频热路径；只执行固定声道样本遍历和 relaxed 原子写入。
    void applyMasterGainAndPublishLevels(ice::AudioBuffer& output) noexcept;

    /// @brief 请求播放命令并发布一个完整序列锁写入。
    void requestPlaybackCommand(PlaybackCommand command) noexcept;

    /// @brief 有效且已排序的预备片段。
    std::vector<PreparedTimelineClip> m_clips;
    /// @brief 只由音频回调线程推进的确定性传输核心。
    AudioTimelineTransport m_transport;
    /// @brief 谱面和采样共同决定的排除结束帧。
    AudioTimelineFrame m_timelineEndFrame{ 0 };
    /// @brief 单次缓存读取允许的最大帧数。
    std::size_t m_maximumProcessFrames{ 1U };
    /// @brief 回调外预分配的音频源读取缓存。
    ice::AudioBuffer m_sourceScratch;
    /// @brief 回调外预分配的活跃片段结果缓存。
    std::vector<AudioTimelineActiveSpan> m_activeSpanScratch;

    /// @brief 播放命令序列锁版本。
    ///
    /// @warning
    /// 单个逻辑线程写入、音频线程读取；原子操作用于避免控制锁阻塞实时回调。
    std::atomic<std::uint64_t> m_playbackCommandSequence{ 0U };
    /// @brief 播放命令邮箱值。
    ///
    /// @warning 受 m_playbackCommandSequence 序列锁保护。
    std::atomic<PlaybackCommand> m_requestedPlaybackCommand{
        PlaybackCommand::Stop
    };
    /// @brief 音频线程最后应用的播放命令版本。
    std::uint64_t m_appliedPlaybackCommandSequence{ 0U };

    /// @brief Seek 命令序列锁版本。
    ///
    /// @warning
    /// 单个逻辑线程写入、音频线程读取；原子操作用于避免控制锁阻塞实时回调。
    std::atomic<std::uint64_t> m_seekSequence{ 0U };
    /// @brief Seek 目标帧邮箱值。
    ///
    /// @warning 受 m_seekSequence 序列锁保护。
    std::atomic<AudioTimelineFrame> m_requestedSeekFrame{ 0 };
    /// @brief 音频线程最后应用的 Seek 版本。
    std::uint64_t m_appliedSeekSequence{ 0U };

    /// @brief 循环命令序列锁版本。
    ///
    /// @warning
    /// 单个逻辑线程写入、音频线程读取；原子操作用于避免控制锁阻塞实时回调。
    std::atomic<std::uint64_t> m_loopSequence{ 0U };
    /// @brief 循环起始帧邮箱值。
    ///
    /// @warning 受 m_loopSequence 序列锁保护。
    std::atomic<AudioTimelineFrame> m_requestedLoopStartFrame{ 0 };
    /// @brief 循环排除结束帧邮箱值。
    ///
    /// @warning 受 m_loopSequence 序列锁保护。
    std::atomic<AudioTimelineFrame> m_requestedLoopEndFrame{ 0 };
    /// @brief 循环启用邮箱值。
    ///
    /// @warning 受 m_loopSequence 序列锁保护。
    std::atomic<bool> m_requestedLoopEnabled{ false };
    /// @brief 音频线程最后应用的循环命令版本。
    std::uint64_t m_appliedLoopSequence{ 0U };

    /// @brief 回调发布给逻辑线程的最新位置。
    ///
    /// @warning
    /// 音频线程写入、逻辑线程读取；relaxed 顺序足以提供独立状态快照。
    std::atomic<AudioTimelineFrame> m_publishedPositionFrame{ 0 };
    /// @brief 回调发布给逻辑线程的最新状态。
    ///
    /// @warning
    /// 音频线程写入、逻辑线程读取；relaxed 顺序足以提供独立状态快照。
    std::atomic<AudioTimelinePlaybackState> m_publishedState{
        AudioTimelinePlaybackState::Stopped
    };
    /// @brief 回调发布给逻辑线程的最新纪元。
    ///
    /// @warning
    /// 音频线程写入、逻辑线程读取；relaxed 顺序足以提供独立状态快照。
    std::atomic<std::uint64_t> m_publishedEpoch{ 0U };
    /// @brief 回调发布给逻辑线程的自然结束标记。
    ///
    /// @warning
    /// 音频线程写入、逻辑线程读取；relaxed 顺序足以提供独立状态快照。
    std::atomic<bool> m_publishedFinished{ false };

    /// @brief 逐事件音量之后应用的复合时间线主增益。
    ///
    /// @warning
    /// 逻辑线程写、音频线程逐 block 读取；relaxed 顺序只要求获取最新独立值。
    std::atomic<float> m_masterGain{ 1.0F };

    /// @brief 最近输出 block 的左声道峰值。
    ///
    /// @warning 音频线程写、逻辑线程读；relaxed 顺序只提供独立电平快照。
    std::atomic<float> m_leftLevel{ 0.0F };

    /// @brief 最近输出 block 的右声道峰值。
    ///
    /// @warning 音频线程写、逻辑线程读；relaxed 顺序只提供独立电平快照。
    std::atomic<float> m_rightLevel{ 0.0F };
};

}  // namespace MMM::Audio
