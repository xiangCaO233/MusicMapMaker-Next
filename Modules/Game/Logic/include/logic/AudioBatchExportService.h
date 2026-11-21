#pragma once

#include "audio/AudioSpeedExportService.h"

#include <filesystem>
#include <string>
#include <vector>

namespace MMM::Logic
{
/// @brief 一次批量转换的值语义请求，交给文件任务线程独占。
struct AudioBatchExportRequest {
    /// @brief 用户选中的原始音频和可选谱面绝对路径。
    std::vector<std::filesystem::path> m_inputs, m_charts;
    /// @brief 音频输出目录，已有同名文件用编号避让。
    std::filesystem::path m_directory;
    /// @brief 目标容器后缀，包含开头的点。
    std::string m_extension;
    /// @brief 本批共用的编码参数，输入和输出路径由服务逐项填充。
    Audio::AudioSpeedExportOptions m_options;
};

/// @brief 音频及改链谱面副本的生成结果。
struct AudioBatchExportResult {
    /// @brief 实际写出的音频和谱面副本数量。
    std::size_t m_audioWritten{}, m_chartsWritten{};
    /// @brief 没有引用已成功转码资源的所选谱面数。
    std::size_t m_chartsUnchanged{};
    /// @brief 各失败文件及处理阶段，后续文件可独立成功。
    std::vector<std::string> m_errors;
};

/// @brief 依次转码文件，再按实际输出路径生成更新引用的谱面副本。
/// @param request 输入、可选谱面、目标目录、格式和编码参数。
/// @return 每项独立提交，失败时只映射已完整写出的音频。
/// @warning 文件线程低频任务，执行完整解码与磁盘读写。
[[nodiscard]] AudioBatchExportResult exportAudioBatch(
    AudioBatchExportRequest request);
}  // namespace MMM::Logic
