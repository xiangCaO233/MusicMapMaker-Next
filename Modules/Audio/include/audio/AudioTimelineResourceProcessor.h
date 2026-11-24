#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ice
{
class AudioTrack;
}  // namespace ice

namespace MMM
{
struct AudioTrackConfig;
}  // namespace MMM

namespace MMM::Audio
{

class PreparedTimelineAudio;

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

/// @brief 判断资源配置是否会改变原始 PCM。
/// @return 中性参数和无效 EQ 预设均可直接复用原声。
[[nodiscard]] bool audioTimelineResourceNeedsProcessing(
    const AudioTrackConfig& config) noexcept;

/// @brief 原位处理同步导出的一组等长声道，不持有原音轨。
/// @param channels 只包含当前处理配置的输入；处理失败后不再使用。
/// @param config 资源 EQ、变速和变调参数。
/// @param pcmBudgetBytes 输入和变速输出平面的合计字节上限。
/// @return 预计长度或实际输出超过预算时返回 false。
/// @warning 只用于同步导出，可能处理完整资源。
[[nodiscard]] bool processExportTimelineResource(
    std::vector<std::vector<float>>& channels, const AudioTrackConfig& config,
    std::size_t pcmBudgetBytes);

}  // namespace MMM::Audio
