#pragma once

#include "audio/AudioMarkerService.h"

#include <cstdint>
#include <string_view>

namespace MMM::Audio::MarkerInternal
{
/// @brief 对外部元数据的最大读取预算，防止标签导致无界分配。
constexpr std::size_t MAX_METADATA_BYTES = 16 * 1024 * 1024;
/// @brief 章节与整拍数量预算，避免异常 BPM 生成数亿个标记。
constexpr std::size_t MAX_MARKERS = 65536;
/// @brief 嵌入普通 comment 字段的版本化载荷边界。
constexpr std::string_view PAYLOAD_BEGIN = "[MMM_TIMING_V1]";
/// @brief 载荷结束标记，保留边界之外原有的用户备注。
constexpr std::string_view PAYLOAD_END = "[/MMM_TIMING_V1]";

/// @brief 验证所有字段，不通过钳位悄悄改变用户的时间或 BPM。
bool validData(const AudioMarkerData& data);
/// @brief 编码精确测量数据，使用 ASCII 转义兼容旧容器备注编码。
std::string encodeData(const AudioMarkerData& data);
/// @brief 严格解析版本化数据，失败时不提交任何部分结果。
bool decodeData(std::string_view text, AudioMarkerData& data);
/// @brief 为外部章节或 WAV cue 生成非负、去重且有界的时间点。
bool makeVisibleChapters(const AudioMarkerExportOptions& options,
                         double duration, std::vector<AudioChapter>& chapters);
/// @brief 将配套名附加到完整音频文件名，避免同名不同格式互相覆盖。
std::filesystem::path sidecarPath(const std::filesystem::path& audio);
/// @brief 读取标准 RIFF/WAVE cue、标签及精确私有块。
AudioMarkerReadResult readWave(const std::filesystem::path& path);
/// @brief 流式复制 WAVE 块，仅替换标记块，PCM 字节保持原样。
bool writeWave(const std::filesystem::path& input,
               const std::filesystem::path& output, const AudioMarkerData& data,
               const std::vector<AudioChapter>& visible, std::string& error);
/// @brief 从 FFmpeg 容器标签和标准章节读取标记。
AudioMarkerReadResult readContainer(const std::filesystem::path& path);
/// @brief 无解码重封装；返回失败时调用者可使用原格式配套文件。
bool remuxContainer(const std::filesystem::path&     input,
                    const std::filesystem::path&     output,
                    const AudioMarkerData&           data,
                    const std::vector<AudioChapter>& visible,
                    std::string&                     error);
/// @brief 查询容器原始时长，供拍线展开和章节终点计算使用。
double audioDuration(const std::filesystem::path& path);
}  // namespace MMM::Audio::MarkerInternal
