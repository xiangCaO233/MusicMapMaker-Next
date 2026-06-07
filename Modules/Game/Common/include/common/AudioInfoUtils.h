#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace MMM::Utils
{

struct AudioInfo {
    std::string title;
    std::string artist;
    double      duration = 0.0;
};

/// @brief 使用 IonCachyEngine 的解码器工厂读取音频文件元数据和时长。
class AudioInfoUtils
{
public:
    /// @brief 探测指定音频文件的标题、艺术家和时长。
    static std::optional<AudioInfo> probeAudioInfo(
        const std::filesystem::path& filePath);
};

}  // namespace MMM::Utils
