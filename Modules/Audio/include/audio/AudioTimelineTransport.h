#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::Audio
{

/// @brief 音频时间线统一使用的有符号采样帧位置。
using AudioTimelineFrame = std::int64_t;

/// @brief 音频时间线中单个采样片段的稳定标识。
using AudioTimelineEventId = std::uint64_t;

/// @brief 与谱面模型解耦的音频时间线片段描述。
struct TimelineClipSpec {
    /// @brief 片段的稳定事件标识。
    AudioTimelineEventId eventId{ 0U };
    /// @brief 由上层资源系统解析的音频源键。
    std::string sourceKey;
    /// @brief 片段在时间线上的起始帧，允许为负数。
    AudioTimelineFrame startFrame{ 0 };
    /// @brief 音频源参与播放的总帧数，非正值片段会被调度器忽略。
    AudioTimelineFrame durationFrames{ 0 };
    /// @brief 片段的线性音量。
    float volume{ 1.0F };
};

/// @brief 音频时间线传输状态。
enum class AudioTimelinePlaybackState : std::uint8_t {
    Stopped,
    Playing,
    Paused,
};

/// @brief 半开区间形式的音频循环范围。
struct AudioTimelineLoopRange {
    /// @brief 循环区间的包含端点。
    AudioTimelineFrame startFrame{ 0 };
    /// @brief 循环区间的排除端点。
    AudioTimelineFrame endFrame{ 0 };
};

/// @brief 一个输出区间内需要从音频源读取的连续片段。
struct AudioTimelineActiveSpan {
    /// @brief 片段在排序后不可变调度表中的索引。
    std::size_t clipIndex{ 0U };
    /// @brief 片段的稳定事件标识。
    AudioTimelineEventId eventId{ 0U };
    /// @brief 指向不可变调度表内部的音频源键。
    std::string_view sourceKey;
    /// @brief 本次输出缓冲区内的起始帧偏移。
    AudioTimelineFrame outputStartFrame{ 0 };
    /// @brief 音频源内部的起始帧偏移。
    AudioTimelineFrame sourceStartFrame{ 0 };
    /// @brief 本次连续读取的帧数。
    AudioTimelineFrame frameCount{ 0 };
    /// @brief 片段的线性音量。
    float volume{ 1.0F };
    /// @brief 产生此片段的传输纪元。
    std::uint64_t epoch{ 0U };
};

/// @brief 无分配区间查询的统计结果。
struct AudioTimelineSpanQueryResult {
    /// @brief 实际写入调用方缓冲区的片段数量。
    std::size_t writtenSpanCount{ 0U };
    /// @brief 完整查询本应产生的片段数量。
    std::size_t totalSpanCount{ 0U };
    /// @brief 调用方缓冲区不足以容纳全部片段时为 true。
    bool truncated{ false };
};

/// @brief 与音频设备和谱面对象解耦的确定性时间线传输与片段调度核心。
///
/// 构造后调度表不可变，并按 startFrame、eventId 稳定排序。查询接口由调用方
/// 提供输出缓冲区，因此后续接入音频回调时不需要在热路径中分配内存。
class AudioTimelineTransport final
{
public:
    /// @brief 构造不可变音频片段调度表。
    /// @param clips 尚未排序的片段描述；非正 durationFrames 会被忽略。
    explicit AudioTimelineTransport(std::vector<TimelineClipSpec> clips);

    /// @brief 获取排序后的不可变片段调度表。
    /// @return 生命周期受当前传输对象约束的只读片段视图。
    [[nodiscard]] std::span<const TimelineClipSpec> clips() const noexcept;

    /// @brief 获取当前传输状态。
    /// @return 当前播放、暂停或停止状态。
    [[nodiscard]] AudioTimelinePlaybackState state() const noexcept;

    /// @brief 获取当前有符号时间线帧位置。
    /// @return 下一次 consumeActiveSpans 开始处理的位置。
    [[nodiscard]] AudioTimelineFrame positionFrame() const noexcept;

    /// @brief 获取当前传输纪元。
    /// @return seek、stop 或循环回绕后递增的纪元。
    [[nodiscard]] std::uint64_t epoch() const noexcept;

    /// @brief 获取当前循环范围。
    /// @return 未启用循环时为空。
    [[nodiscard]] const std::optional<AudioTimelineLoopRange>&
    loopRange() const noexcept;

    /// @brief 从当前位置开始或继续播放。
    void play() noexcept;

    /// @brief 冻结当前位置，后续消费调用不推进时间线。
    void pause() noexcept;

    /// @brief 停止播放并回到时间线零点，同时开启新纪元。
    void stop() noexcept;

    /// @brief 跳转到指定时间线帧并开启新纪元。
    /// @param frame 目标有符号帧；启用循环且目标不小于排除端点时回到循环起点。
    void seek(AudioTimelineFrame frame) noexcept;

    /// @brief 配置半开循环区间。
    /// @param range startFrame 必须严格小于 endFrame。
    /// @return 范围有效并成功启用时返回 true。
    [[nodiscard]] bool setLoop(AudioTimelineLoopRange range) noexcept;

    /// @brief 关闭循环但保留当前播放位置和纪元。
    void clearLoop() noexcept;

    /// @brief 查询任意线性时间区间内的活跃片段，不改变传输状态。
    /// @param startFrame 查询区间的起始时间线帧。
    /// @param frameCount 查询区间的帧数；非正值产生空结果。
    /// @param output 调用方提供的片段输出缓冲区。
    /// @return 写入数量、完整数量和截断状态。
    /// @warning
    /// 音频热路径查询；禁止在此函数及其调用链引入内存分配、锁或文件操作。
    [[nodiscard]] AudioTimelineSpanQueryResult queryActiveSpans(
        AudioTimelineFrame startFrame, AudioTimelineFrame frameCount,
        std::span<AudioTimelineActiveSpan> output) const noexcept;

    /// @brief 按当前播放状态消费输出帧，处理循环边界并推进传输位置。
    /// @param frameCount 需要生成的输出帧数；非正值不推进。
    /// @param output 调用方提供的片段输出缓冲区。
    /// @return 写入数量、完整数量和截断状态。
    /// @warning 音频热路径入口；禁止引入内存分配、锁、阻塞或文件操作。
    [[nodiscard]] AudioTimelineSpanQueryResult consumeActiveSpans(
        AudioTimelineFrame                 frameCount,
        std::span<AudioTimelineActiveSpan> output) noexcept;

private:
    /// @brief 在指定线性区间追加活跃片段，并叠加输出偏移和纪元。
    /// @param startFrame 查询区间起点。
    /// @param frameCount 查询帧数。
    /// @param outputStartFrame 输出缓冲区中的基准偏移。
    /// @param queryEpoch 当前分段对应的传输纪元。
    /// @param output 调用方输出缓冲区。
    /// @param result 跨循环分段累计的查询统计。
    /// @warning 音频热路径内部函数；仅执行不可变索引查询和顺序写入。
    void appendActiveSpans(AudioTimelineFrame                 startFrame,
                           AudioTimelineFrame                 frameCount,
                           AudioTimelineFrame                 outputStartFrame,
                           std::uint64_t                      queryEpoch,
                           std::span<AudioTimelineActiveSpan> output,
                           AudioTimelineSpanQueryResult& result) const noexcept;

    /// @brief 排序后不可变的片段调度表。
    std::vector<TimelineClipSpec> m_clips;
    /// @brief 与 m_clips 同索引的片段排除端点。
    std::vector<AudioTimelineFrame> m_clipEndFrames;
    /// @brief 用于跳过已结束前缀的单调最大排除端点索引。
    std::vector<AudioTimelineFrame> m_prefixMaxEndFrames;
    /// @brief 当前传输状态。
    AudioTimelinePlaybackState m_state{ AudioTimelinePlaybackState::Stopped };
    /// @brief 下一次消费开始的有符号时间线帧。
    AudioTimelineFrame m_positionFrame{ 0 };
    /// @brief 用于区分 seek、stop 和循环迭代的传输纪元。
    std::uint64_t m_epoch{ 0U };
    /// @brief 当前半开循环区间，空值表示禁用循环。
    std::optional<AudioTimelineLoopRange> m_loopRange;
};

}  // namespace MMM::Audio
