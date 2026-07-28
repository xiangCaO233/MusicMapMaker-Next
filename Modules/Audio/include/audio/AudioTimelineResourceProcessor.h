#pragma once

#include "audio/AudioTimelineMixerNode.h"
#include "mmm/project/AudioResource.h"

#include <memory>
#include <string>
#include <string_view>

namespace ice
{
class AudioTrack;
}  // namespace ice

namespace MMM::Audio
{

/// @brief 构造排除资源音量和静音的离线 DSP 缓存键。
/// @param filePath 音频文件稳定路径。
/// @param config 音频资源持久化配置。
/// @return 可在自动采样与 Note HitEffect 之间共享的处理缓存键。
/// @warning 低频资源路径：会序列化配置并分配字符串。
[[nodiscard]] std::string makeAudioResourceProcessingCacheKey(
    std::string_view filePath, const AudioTrackConfig& config);

/// @brief 在非实时线程应用单个音频资源的持久化 DSP 配置。
///
/// 音量和静音不烘焙进 PCM，由时间线片段增益独立应用。playbackSpeed、
/// playbackPitch 和 EQ 会在这里固定为不可变 PCM，使音频回调不再运行逐资源
/// 变速、变调或滤波节点。
/// @param track 已完成缓存的原始音轨。
/// @param config 音频资源持久化配置。
/// @return 成功时返回只读 PCM；资源无数据时返回空指针。
/// @warning
/// 低频资源准备路径：可能等待解码并执行完整离线 Rubber Band 和 EQ
/// 处理，禁止在 UI、逻辑 update 或音频回调热路径调用。
[[nodiscard]] std::shared_ptr<const PreparedTimelineAudio>
prepareAudioTimelineResource(const std::shared_ptr<ice::AudioTrack>& track,
                             const AudioTrackConfig&                 config);

}  // namespace MMM::Audio
