#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace MMM::Audio
{
/// @brief 音频中的命名章节或时间标记，位置以原始音频秒数表示。
struct AudioChapter {
    /// @brief 相对文件开头的秒数；不受项目倍速、视觉偏移影响。
    double seconds{ 0.0 };
    /// @brief 原始章节名称，可使用 UTF-8 文本。
    std::string title;
};

/// @brief 一段恒定 BPM 的起始锚点，与音频章节独立保存。
struct AudioBpmSegment {
    /// @brief 首拍秒数；允许保存测量工具支持的负首拍。
    double seconds{ 0.0 };
    /// @brief 该段每分钟拍数，必须有限且位于公共 BPM 范围。
    double bpm{ 120.0 };
};

/// @brief 可独立于谱面往返的音频测量数据。
struct AudioMarkerData {
    /// @brief 章节名称和起点；不由 BPM 数值推断名称。
    std::vector<AudioChapter> chapters;
    /// @brief 有序变速段，精确双精度值保存在版本化元数据中。
    std::vector<AudioBpmSegment> bpmSegments;
};

/// @brief 标记读取结果；普通无标记音频也属于成功读取。
struct AudioMarkerReadResult {
    /// @brief 是否完成读取且未遇到损坏的测量元数据。
    bool success{ false };
    /// @brief 失败原因；无标记音频留空。
    std::string error;
    /// @brief 解析出的章节和 BPM，不假定每个章节都有 BPM。
    AudioMarkerData data;
};

/// @brief 标记导出参数，始终写入新的路径。
struct AudioMarkerExportOptions {
    /// @brief 原始音频，只读访问且禁止与输出指向同一文件。
    std::filesystem::path inputPath;
    /// @brief 目标扩展名优先沿用来源；显式 WAV/MP3 转换会重新编码。
    std::filesystem::path outputPath;
    /// @brief 冻结后的测量快照，后台线程不访问工具状态。
    AudioMarkerData data;
    /// @brief 是否额外为每个整拍生成外部软件可见的标记。
    bool includeBeats{ false };
};

/// @brief 导出状态，区分内嵌标记和配套文件。
struct AudioMarkerExportResult {
    /// @brief 音频及标记均成功写出并完成回读校验。
    bool success{ false };
    /// @brief 失败原因，失败时不发布部分输出。
    std::string error;
    /// @brief 不支持内嵌的容器使用此配套路径，必须与音频一起传递。
    std::filesystem::path sidecarPath;
};

/// @brief 音频元数据读写服务，不向音频样本混入节拍声。
class AudioMarkerService
{
public:
    /// @brief 导入项目时一并复制配套标记；无配套文件视为成功。
    /// @param source 已有源音频，配套数据必须通过读取校验。
    /// @param destination 已复制到项目的新音频位置，不覆盖已有标记。
    /// @return 无错误表示无需复制或已成功，失败由调用方回滚新音频。
    /// @warning 低频导入事务中调用，包含文件读取与复制。
    static std::error_code copyCompanion(
        const std::filesystem::path& source,
        const std::filesystem::path& destination);
    /// @brief 读取内嵌章节、测量 BPM 或版本化配套文件。
    /// @warning 低频文件路径；需在后台线程或显式文件操作中调用。
    static AudioMarkerReadResult read(const std::filesystem::path& path);
    /// @brief 保持原格式复制编码数据并写入标记，必要时使用配套文件。
    /// @warning 后台文件事务；会遍历音频包，但不修改原文件或执行实时 DSP。
    static AudioMarkerExportResult exportFile(
        const AudioMarkerExportOptions& options);
};
}  // namespace MMM::Audio
