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

/**
 * @brief 使用 ffprobe 获取音频文件的元数据和时长
 */
class AudioInfoUtils
{
public:
    static std::optional<AudioInfo> probeAudioInfo(
        const std::filesystem::path& filePath);
};

}  // namespace MMM::Utils
