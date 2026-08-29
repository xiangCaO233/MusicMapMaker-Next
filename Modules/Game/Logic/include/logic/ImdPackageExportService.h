#pragma once

#include "audio/AudioManager.h"

#include <filesystem>
#include <string>
#include <vector>

namespace MMM
{
class BeatMap;
}

namespace MMM::Logic
{

/// @brief 单谱面 RM/IMD 资源包导出结果。
struct ImdPackageExportResult {
    /// @brief 是否成功写出完整资源包。
    bool success{ false };

    /// @brief 失败时可直接展示给用户的原因。
    std::string errorMessage;

    /// @brief 包内实际使用的 IMD 文件名。
    std::string beatmapFileName;

    /// @brief 包内实际使用的 MP3 文件名。
    std::string audioFileName;

    /// @brief 包内实际使用的背景图片文件名。
    std::string coverFileName;
};

/// @brief 将当前谱面、拼装音频和背景图导出为独立 RM/IMD 资源包。
class ImdPackageExportService final
{
public:
    /// @brief 生成只含同名 IMD、MP3 和背景图的 zip 资源包。
    /// @param beatMap 已同步的当前谱面。
    /// @param audioEvents 自动采样和物件绑定音效组成的完整时间线事件。
    /// @param chartEndSeconds 玩家物件和 Timing 决定的最短音频时长。
    /// @param coverPath 背景图片的实际文件路径。
    /// @param outputPath 目标 zip 文件路径。
    /// @return 导出状态、错误原因和三个包内文件名。
    /// @warning 用户触发的低频导出路径：会完整序列化谱面、混合音频、读取
    /// 背景图并压缩归档，禁止在逻辑 update 或 UI 渲染热路径调用。
    [[nodiscard]] static ImdPackageExportResult exportPackage(
        const BeatMap&                                    beatMap,
        const std::vector<Audio::AudioTimelineLoadEvent>& audioEvents,
        double chartEndSeconds, const std::filesystem::path& coverPath,
        const std::filesystem::path& outputPath);
};

}  // namespace MMM::Logic
