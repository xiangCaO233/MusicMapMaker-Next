#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace MMM::Utils
{

/// @brief 音频文件中可用于项目展示的基础元数据。
struct AudioInfo {
    /// @brief 媒体标题；缺失时探测器回退为文件名主干。
    std::string title;
    /// @brief 媒体艺术家；源文件未提供时保持为空。
    std::string artist;
    /// @brief 依据帧数和采样率计算的时长，单位为秒。
    double duration = 0.0;
};

/// @brief 使用 IonCachyEngine 的解码器工厂读取音频文件元数据和时长。
class AudioInfoUtils
{
public:
    /// @brief 探测指定音频文件的标题、艺术家和时长。
    /// @param filePath 待探测音频文件的本地路径。
    /// @return 文件存在且解码器探测成功时返回元数据，否则返回空。
    /// @warning 该函数执行文件系统查询和媒体探测，只能在低频加载路径调用。
    static std::optional<AudioInfo> probeAudioInfo(
        const std::filesystem::path& filePath);
};

}  // namespace MMM::Utils
