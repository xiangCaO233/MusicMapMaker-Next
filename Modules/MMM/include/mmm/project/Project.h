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

        /// @brief 旧项目记录的单主音轨 ID，仅供加载迁移定位资源。
        std::string m_audioTrackId;

        /// @brief 序列化当前谱面入口，不再写出旧版单主音轨字段。
        /// @param json 输出 JSON 对象。
        /// @param entry 待序列化的谱面入口。
        friend void to_json(nlohmann::json& json, const BeatmapEntry& entry)
        {
            json = nlohmann::json{ { "m_name", entry.m_name },
                                   { "m_filePath", entry.m_filePath } };
        }

        /// @brief 反序列化谱面入口，并读取旧版单主音轨迁移字段。
        /// @param json 输入 JSON 对象。
        /// @param entry 接收反序列化结果的谱面入口。
        friend void from_json(const nlohmann::json& json, BeatmapEntry& entry)
        {
            entry = BeatmapEntry{};
            if ( !json.is_object() ) return;

            entry.m_name         = json.value("m_name", std::string{});
            entry.m_filePath     = json.value("m_filePath", std::string{});
            entry.m_audioTrackId = json.value("m_audioTrackId", std::string{});
        }
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

    /// @brief 手动从项目中移除并排除自动同步的谱面相对路径列表。
    std::vector<std::string> m_excludedBeatmapPaths;

    /// @brief 手动从项目中移除并排除自动同步的音频相对路径列表。
    std::vector<std::string> m_excludedAudioPaths;

    // --- 运行时状态 (不参与序列化) ---

    /// @brief 项目实际在文件系统中的根路径
    std::filesystem::path m_projectRoot;

    /// @brief 当前项目是否为拖拽谱面包创建的临时只读项目。
    bool m_isTemporaryProject{ false };

    /// @brief 临时项目对应的原始谱面包路径。
    std::filesystem::path m_temporarySourcePackagePath;

    /// @brief 序列化项目配置。
    friend void to_json(nlohmann::json& j, const Project& p)
    {
        j = nlohmann::json{
            { "m_metadata", p.m_metadata },
            { "m_settings", p.m_settings },
            { "m_audioResources", p.m_audioResources },
            { "m_beatmaps", p.m_beatmaps },
            { "m_excludedBeatmapPaths", p.m_excludedBeatmapPaths },
            { "m_excludedAudioPaths", p.m_excludedAudioPaths }
        };
    }

    /// @brief 反序列化项目配置，并兼容旧项目文件中缺失的排除列表。
    friend void from_json(const nlohmann::json& j, Project& p)
    {
        j.at("m_metadata").get_to(p.m_metadata);
        j.at("m_settings").get_to(p.m_settings);
        j.at("m_audioResources").get_to(p.m_audioResources);
        j.at("m_beatmaps").get_to(p.m_beatmaps);
        p.m_excludedBeatmapPaths =
            j.value("m_excludedBeatmapPaths", std::vector<std::string>{});
        p.m_excludedAudioPaths =
            j.value("m_excludedAudioPaths", std::vector<std::string>{});
    }
};

}  // namespace MMM
