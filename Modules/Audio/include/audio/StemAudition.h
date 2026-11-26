#pragma once

#include "audio/AudioTimelineTransport.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ice
{
class AudioBuffer;
class ThreadPool;
}  // namespace ice

namespace MMM::Audio
{
class AudioTimelineMixerNode;
class PreparedTimelineAudio;
struct PreparedTimelineClip;

/// @brief 已对齐到内部采样率的独立试听音轨。
struct StemAuditionTrack {
    /// @brief 调用方提供的稳定 ID，文件名和显示名可独立变化。
    std::string id;
    /// @brief 音轨首帧在共享源时间轴上的位置。
    AudioTimelineFrame startFrame{};
    /// @brief 保留源电平的 PCM，采用与音频图一致的声道和采样率。
    std::shared_ptr<const PreparedTimelineAudio> audio;
};

/// @brief 音轨组、选择或区间不满足请求契约。
enum class StemAuditionError {
    DecodeFailed,     ///< 指定文件未解码出可用的完整 PCM。
    InvalidTrack,     ///< 空 ID、缺失 PCM 或当前音轨超出源时间轴。
    DuplicateTrack,   ///< 源目录或请求中有重复 ID，可能导致重复混音。
    UnknownTrack,     ///< 请求的 ID 不在当前目录中。
    InvalidInterval,  ///< 区间、缓冲格式或单次输出帧数无效。
};

/// @brief 为试听节点与离线摘录提供同一份不可变、源帧对齐的音轨目录。
/// @details 原曲或其他参考版可独立建组。调用方负责准备 PCM 和管理设备。
class StemAudition final
{
public:
    /// @brief 建立有明确结束位置的目录，拒绝重复 ID 和越界 PCM。
    /// @param sourceEndFrame 源时间轴排除端点，也是全关时的播放终点。
    /// @param tracks 已转换为内部 PCM 格式的已知音轨。
    [[nodiscard]] static std::expected<StemAudition, StemAuditionError> create(
        AudioTimelineFrame             sourceEndFrame,
        std::vector<StemAuditionTrack> tracks);

    /// @brief 列表中的文件使用共同零点，以最长音轨的排除端点结束。
    /// @param paths 完整 UTF-8 路径，作为本目录内的稳定 ID。
    /// @param decodePool 与当前任务所在池分离的解码池。
    /// @warning 仅供后台文件任务调用，等待完整解码期间可能阻塞。
    [[nodiscard]] static std::expected<StemAudition, StemAuditionError>
    loadAlignedFiles(std::span<const std::string> paths,
                     ice::ThreadPool&             decodePool);

    /// @brief 按注册顺序列出可选音轨，视图随目录失效。
    [[nodiscard]] std::span<const StemAuditionTrack> tracks() const noexcept;

    /// @brief 返回音轨组的排除端点，单位为内部采样帧。
    [[nodiscard]] AudioTimelineFrame endFrame() const noexcept;

    /// @brief 按稳定 ID 构建实时播放节点，空选择生成保持时钟的静音节点。
    /// @warning 在控制线程使用。节点接管独立的 PCM 所有权。
    [[nodiscard]] std::expected<std::shared_ptr<AudioTimelineMixerNode>,
                                StemAuditionError>
    makePlayback(std::span<const std::string> ids,
                 std::size_t                  maximumBlockFrames) const;

    /// @brief 在下一音频块切换选择，保留已有的播放位置与循环。
    /// @param playback 由当前目录通过 makePlayback 创建的节点。
    /// @param ids 新的全部选择，空列表表示静音。
    /// @param maximumBlockFrames 调用方为新调度预备的块容量。
    /// @param gains 与目录同序的线性推子值。空列表使用单位增益。
    /// @return 新调度代次或错误。无效选择不影响当前调度。
    /// @warning 仅允许控制线程调用，并按节点契约回收已退役的调度。
    [[nodiscard]] std::expected<std::uint64_t, StemAuditionError> select(
        AudioTimelineMixerNode& playback, std::span<const std::string> ids,
        std::size_t            maximumBlockFrames,
        std::span<const float> gains = {}) const;

    /// @brief 在原曲帧坐标取样，output 活动帧数决定半开区间的长度。
    /// @details 区间必须完全属于源时间轴，输出采用内部 PCM 格式。
    /// @warning 仅供后台线程使用，可能分配临时混音节点及读取离线 PCM。
    [[nodiscard]] std::expected<void, StemAuditionError> sample(
        std::span<const std::string> ids, AudioTimelineFrame firstFrame,
        ice::AudioBuffer& output) const;

private:
    /// @brief 解析选择并按固定求和顺序生成时间线调度片段。
    [[nodiscard]] std::expected<std::vector<PreparedTimelineClip>,
                                StemAuditionError>
    makeClips(std::span<const std::string> ids,
              std::span<const float>       gains = {}) const;

    /// @brief 创建通过时间轴与 ID 校验的目录。
    StemAudition(AudioTimelineFrame             sourceEndFrame,
                 std::vector<StemAuditionTrack> tracks);

    /// @brief 原曲排除端点，避免静音或短轨改变试听长度。
    AudioTimelineFrame m_sourceEndFrame;
    /// @brief 固定输入顺序确定浮点求和顺序。
    std::vector<StemAuditionTrack> m_tracks;
};
}  // namespace MMM::Audio
