#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace MMM::Audio
{

/// @brief 音频倍速导出进度快照。
struct AudioSpeedExportProgress {
    /// @brief 0 到 1 的进度值。
    float progress{ 0.0f };

    /// @brief 面向 UI 的进度文本。
    std::string message;
};

/// @brief 音频倍速导出参数。
struct AudioSpeedExportOptions {
    /// @brief 输入音频文件路径。
    std::filesystem::path inputPath;

    /// @brief 输出 WAV 文件路径。
    std::filesystem::path outputPath;

    /// @brief 倍速倍率，必须大于 0。
    double speed{ 1.0 };

    /// @brief 是否保持原音高；false 时音高随倍速改变。
    bool preservePitch{ true };

    /// @brief 最低输出时长，单位秒；用于补齐谱面尾部。
    double minimumDurationSeconds{ 0.0 };

    /// @brief 低频进度回调。
    std::function<void(const AudioSpeedExportProgress&)> progressCallback;
};

/// @brief 音频倍速导出结果。
struct AudioSpeedExportResult {
    /// @brief 是否导出成功。
    bool success{ false };

    /// @brief 失败原因。
    std::string errorMessage;

    /// @brief 实际写出的音频帧数。
    std::size_t outputFrames{ 0 };

    /// @brief 实际写出的音频时长，单位秒。
    double outputDurationSeconds{ 0.0 };
};

/// @brief 音频倍速导出服务。
class AudioSpeedExportService
{
public:
    /// @brief 导出与谱面倍速同步的 WAV 音频。
    /// @param options 导出参数。
    /// @return 导出结果。
    /// @warning 后台耗时路径：会完整读取/重采样音频，只能由用户手动触发。
    static AudioSpeedExportResult exportWav(
        const AudioSpeedExportOptions& options);
};

}  // namespace MMM::Audio
