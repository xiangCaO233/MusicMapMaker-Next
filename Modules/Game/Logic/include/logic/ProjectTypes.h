#pragma once

#include "mmm/project/AudioResource.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace MMM::Logic
{

/// @brief 音效资源按需加载登记请求。
/// @details 描述待登记资源，不持有解码器；由打开流程调用方交给音频系统。
struct AudioRegistrationRequest {
    /// @brief 需要登记的项目音频资源。
    AudioResource m_resource;

    /// @brief 音频资源在文件系统中的绝对路径。
    std::filesystem::path m_absolutePath;
};

/// @brief 打开项目后的结果信息。
/// @details 项目本身由控制器持有，此值仅交付后续会话和音频接入所需信息。
struct OpenProjectResult {
    /// @brief 是否成功打开项目。
    bool m_opened{ false };

    /// @brief 实际打开的项目目录路径。
    std::filesystem::path m_actualProjectPath;

    /// @brief 待自动打开的谱面；文件入口取指定文件，临时谱包可取首个谱面。
    std::filesystem::path m_targetBeatmapPath;

    /// @brief 项目显示标题。
    std::string m_projectTitle;

    /// @brief 项目内谱面数量。
    std::size_t m_beatmapCount{ 0 };

    /// @brief 打开项目后需要登记到音频引擎的按需加载音效资源。
    std::vector<AudioRegistrationRequest> m_effectRegistrations;
};

/// @brief 新建项目时需要写入项目描述文件的初始设置。
/// @note 只用于无既有配置的项目；空标题、曲作者、谱师由创建入口补默认值。
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
/// @note 路径快照不管理缓存寿命；持有此值不阻止项目关闭或缓存清理。
struct TemporaryProjectInfo {
    /// @brief 是否存在临时项目。
    bool m_isTemporary{ false };

    /// @brief 用户拖拽打开的原始谱面包路径。
    std::filesystem::path m_sourcePackagePath;

    /// @brief 当前临时项目缓存目录。
    std::filesystem::path m_cacheProjectPath;
};

}  // namespace MMM::Logic
