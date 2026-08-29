#pragma once

#include <cstddef>
#include <filesystem>

#include <string>

namespace MMM::Audio
{

/// @brief 把音频内容按首拍相位裁头或补静音到时间原点的参数。
struct AudioOriginAlignmentOptions {
    /// @brief 原始音频路径。
    std::filesystem::path inputPath;

    /// @brief 对齐后音频路径；编码格式由扩展名决定。
    std::filesystem::path outputPath;

    /// @brief 首红线折回一拍内后的有符号相位，单位毫秒。
    /// 正值裁掉音频开头，负值在音频开头补等长静音。
    double phaseMilliseconds{ 0.0 };
};

/// @brief 音频原点对齐结果。
struct AudioOriginAlignmentResult {
    /// @brief 是否成功完成音频编码。
    bool success{ false };

    /// @brief 失败时可直接记录或展示的原因。
    std::string errorMessage;

    /// @brief 实际提交给编码器的音频帧数。
    std::size_t outputFrames{ 0U };
};

/// @brief 为 MCZ 非 OGG 主音轨生成首拍位于时间原点的音频副本。
class AudioOriginAlignmentService final
{
public:
    /// @brief 根据相位裁掉开头或在开头补静音，并按目标扩展名重新编码。
    /// @param options 输入、输出路径和首拍相位。
    /// @return 导出状态、错误信息和输出帧数。
    /// @warning 用户触发的低频导出路径：会访问文件系统、等待完整解码和编码，
    /// 禁止在 UI、逻辑 update 或音频回调热路径调用。
    [[nodiscard]] static AudioOriginAlignmentResult alignToOrigin(
        const AudioOriginAlignmentOptions& options);
};

}  // namespace MMM::Audio
