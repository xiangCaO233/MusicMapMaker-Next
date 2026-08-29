#pragma once

#include "audio/AudioManager.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace MMM::Audio
{

/// @brief 复合音频时间线离线导出参数。
struct AudioTimelineExportOptions {
    /// @brief 需要按时间线拼装的全部音频事件。
    std::vector<AudioTimelineLoadEvent> events;

    /// @brief 玩家物件和 Timing 决定的最短输出时长，单位为秒。
    double chartEndSeconds{ 0.0 };

    /// @brief 目标音频文件路径；编码格式由扩展名决定。
    std::filesystem::path outputPath;
};

/// @brief 复合音频时间线离线导出结果。
struct AudioTimelineExportResult {
    /// @brief 是否成功完成编码。
    bool success{ false };

    /// @brief 失败时可直接展示给用户的原因。
    std::string errorMessage;

    /// @brief 实际提交给编码器的音频帧数。
    std::size_t outputFrames{ 0U };
};

/// @brief 将多段自动采样和物件音效离线拼装为单个音频文件。
class AudioTimelineExportService final
{
public:
    /// @brief 按统一时间线混合并编码全部音频事件。
    /// @param options 时间线事件、最短时长和目标路径。
    /// @return 导出状态、错误信息和输出帧数。
    /// @warning 用户触发的低频导出路径：会访问文件系统、等待完整解码并执行
    /// 资源级离线 DSP 和编码，禁止在 UI、逻辑 update 或音频回调热路径调用。
    [[nodiscard]] static AudioTimelineExportResult exportMixedAudio(
        const AudioTimelineExportOptions& options);
};

}  // namespace MMM::Audio
