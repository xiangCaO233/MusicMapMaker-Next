#pragma once

#include "mmm/project/AudioResource.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace MMM::Logic
{

/// @brief 音效资源按需加载登记请求。
struct AudioRegistrationRequest {
    /// @brief 需要登记的项目音频资源。
    AudioResource m_resource;

    /// @brief 音频资源在文件系统中的绝对路径。
    std::filesystem::path m_absolutePath;
};

/// @brief 打开项目后的结果信息。
struct OpenProjectResult {
    /// @brief 是否成功打开项目。
    bool m_opened{ false };

    /// @brief 实际打开的项目目录路径。
    std::filesystem::path m_actualProjectPath;

    /// @brief 打开项目时若传入谱面文件，则记录需要自动打开的谱面路径。
    std::filesystem::path m_targetBeatmapPath;

    /// @brief 项目显示标题。
    std::string m_projectTitle;

    /// @brief 项目内谱面数量。
    std::size_t m_beatmapCount{ 0 };

    /// @brief 打开项目后需要登记到音频引擎的按需加载音效资源。
    std::vector<AudioRegistrationRequest> m_effectRegistrations;
};

/// @brief 新建项目时需要写入项目描述文件的初始设置。
struct ProjectCreationOptions {
    /// @brief 项目显示标题。
    std::string m_title;

    /// @brief 项目曲作者或艺术家。
    std::string m_artist;

    /// @brief 项目谱师。
    std::string m_mapper;

    /// @brief 项目默认调色方案；空字符串表示继承软件默认。
    std::string m_colorPaletteSchemeName;

    /// @brief 新项目首次打开时的侧边栏页签名称。
    std::string m_sidebarActiveTab;
};

/// @brief 当前临时项目的运行时信息。
struct TemporaryProjectInfo {
    /// @brief 是否存在临时项目。
    bool m_isTemporary{ false };

    /// @brief 用户拖拽打开的原始谱面包路径。
    std::filesystem::path m_sourcePackagePath;

    /// @brief 当前临时项目缓存目录。
    std::filesystem::path m_cacheProjectPath;
};

}  // namespace MMM::Logic
