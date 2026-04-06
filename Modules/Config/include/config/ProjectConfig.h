#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace MMM::Config
{

/// @brief 项目（谱面）级别的配置信息
struct ProjectConfig {
    /// @brief 歌曲标题
    std::string title{ "Untitled" };
    /// @brief 艺术家/曲作者
    std::string artist{ "Unknown" };
    /// @brief 谱面作者
    std::string mapper{ "Unknown" };

    /// @brief 歌曲文件路径 (相对于谱面根目录)
    std::string audioPath;
    /// @brief 背景图路径 (相对于谱面根目录)
    std::string backgroundPath;

    /// @brief 基础 BPM
    double baseBPM{ 120.0 };

    /// @brief 选曲预览开始时间 (秒)
    double previewStart{ 0.0 };
    /// @brief 选曲预览时长 (秒)
    double previewDuration{ 15.0 };

    // TODO: 后续可在此添加难度列表、各难度特定配置等

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ProjectConfig, title, artist, mapper,
                                   audioPath, backgroundPath, baseBPM,
                                   previewStart, previewDuration)
};

}  // namespace MMM::Config
