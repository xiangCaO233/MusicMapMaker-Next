#pragma once

#include "mmm/beatmap/BeatMap.h"

#include <filesystem>
#include <string>

namespace MMM
{

/// @brief 谱面倍速副本生成参数。
struct BeatmapSpeedTransformOptions {
    /// @brief 倍速倍率，必须大于 0。
    double speed{ 1.0 };

    /// @brief 新谱面的项目相对谱面文件路径。
    std::filesystem::path mapPath;

    /// @brief 新谱面的项目相对主音频路径。
    std::filesystem::path audioPath;

    /// @brief 新谱面内部名称。
    std::string name;

    /// @brief 新谱面难度名。
    std::string version;
};

/// @brief 谱面倍速副本生成结果。
struct BeatmapSpeedTransformResult {
    /// @brief 是否成功生成。
    bool success{ false };

    /// @brief 失败原因。
    std::string errorMessage;

    /// @brief 生成出的谱面副本。
    BeatMap beatmap;
};

/// @brief 谱面倍速副本生成工具。
class BeatmapSpeedTransform
{
public:
    /// @brief 计算谱面实际内容末尾时间。
    /// @param beatmap 谱面。
    /// @return 所有物件、时间线与自动采样实际触发时间的最大值，单位毫秒。
    static double calculateContentEndTime(const BeatMap& beatmap);

    /// @brief 生成与变速音频同步的谱面副本。
    /// @param source 原谱面。
    /// @param options 生成参数。
    /// @return 生成结果。
    static BeatmapSpeedTransformResult createSpeedVersion(
        const BeatMap& source, const BeatmapSpeedTransformOptions& options);
};

}  // namespace MMM
