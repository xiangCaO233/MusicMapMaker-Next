#pragma once
#include "AudioResource.h"
#include "ProjectSettings.h"
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace MMM
{

/// @brief 项目元数据 (持久化于项目描述文件)
struct ProjectMetadata {
    /// @brief 项目显示的标题
    std::string m_title{ "Untitled Project" };
    /// @brief 曲作者/艺术家
    std::string m_artist{ "Unknown" };
    /// @brief 谱面作者
    std::string m_mapper{ "Unknown" };
    /// @brief 项目版本 (用于兼容性检查)
    std::string m_version{ "1.0.0" };

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ProjectMetadata, m_title, m_artist, m_mapper,
                                   m_version)
};

/// @brief 核心项目容器：管理多个谱面实例、多个音频资源、以及特定偏好设置
class Project
{
public:
    Project() = default;

    /// @brief 项目内管理的谱面入口信息
    struct BeatmapEntry {
        /// @brief 难度名称或版本名 (如 "Easy", "Remix Ver.")
        std::string m_name;

        /// @brief 谱面定义文件路径 (相对于项目根目录)
        std::string m_filePath;

        /// @brief 该谱面关联的主音轨 ID
        std::string m_audioTrackId;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(BeatmapEntry, m_name, m_filePath,
                                       m_audioTrackId)
    };

    // --- 数据成员 (参与 JSON 序列化) ---

    /// @brief 项目基本元数据
    ProjectMetadata m_metadata;

    /// @brief 项目特定的编辑器偏好设置与状态
    ProjectSettings m_settings;

    /// @brief 项目管理的所有音频资源库
    std::vector<AudioResource> m_audioResources;

    /// @brief 项目内包含的所有谱面入口列表
    std::vector<BeatmapEntry> m_beatmaps;

    // --- 运行时状态 (不参与序列化) ---

    /// @brief 项目实际在文件系统中的根路径
    std::filesystem::path m_projectRoot;

    /// @brief 序列化宏
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(Project, m_metadata, m_settings,
                                   m_audioResources, m_beatmaps)
};

}  // namespace MMM
