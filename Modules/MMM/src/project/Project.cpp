#include "mmm/project/Project.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace MMM
{

/// @file
/// @brief 项目持久化的字段转换实现，仅在打开和保存项目时调用。
/// @details 模型仍按值存储；JSON 模板实例化不再传播到只访问数据的调用端。

/// @brief 写出 ProjectMetadata 的既有必需字段。
/// @param j 接收字段的 JSON 节点，已有的无关字段继续保留。
/// @param metadata 待保存的值对象。
void to_json(nlohmann::json& j, const ProjectMetadata& metadata)
{
    // 保留原宏逐字段赋值的行为，不能用整体赋值清除调用方已有的键。
    j["m_title"]   = metadata.m_title;
    j["m_artist"]  = metadata.m_artist;
    j["m_mapper"]  = metadata.m_mapper;
    j["m_version"] = metadata.m_version;
}

/// @brief 读取 ProjectMetadata 的既有必需字段。
/// @param j 来源 JSON 对象，字段缺失继续由 at 报告失败。
/// @param metadata 接收读取结果的值对象。
void from_json(const nlohmann::json& j, ProjectMetadata& metadata)
{
    // 不引入默认回退或整对象重置，保持旧宏的必需字段及更新顺序。
    j.at("m_title").get_to(metadata.m_title);
    j.at("m_artist").get_to(metadata.m_artist);
    j.at("m_mapper").get_to(metadata.m_mapper);
    j.at("m_version").get_to(metadata.m_version);
}

/// @brief 序列化草稿轨组，不写出进程内版本。
/// @param json 输出可跨进程恢复的草稿对象。
/// @param group 已绑定谱面路径或尚待迁移的旧草稿组。
void to_json(nlohmann::json& json, const ProjectDraftLaneGroup& group)
{
    json = nlohmann::json{ { "m_notePayload", group.m_notePayload },
                           { "m_trackCount", group.m_trackCount } };
    if ( !group.m_beatmapFilePath.empty() ) {
        // 新数据只写谱面路径，确保同主音频的不同谱面拥有独立草稿。
        json["m_beatmapFilePath"] = group.m_beatmapFilePath;
    } else if ( !group.m_legacyMainAudioResourceId.empty() ) {
        // 尚未被任一谱面认领的旧组保持原键，避免打开项目时自动保存丢失。
        json["m_mainAudioResourceId"] = group.m_legacyMainAudioResourceId;
    }
}

/// @brief 反序列化草稿轨组并重置进程内版本。
/// @param json 当前或旧版草稿对象。
/// @param group 接收持久化字段，不继承此前的运行时修订号。
void from_json(const nlohmann::json& json, ProjectDraftLaneGroup& group)
{
    // 谱面路径优先于历史音轨键，避免同一草稿同时进入两套归属索引。
    group.m_beatmapFilePath = json.value("m_beatmapFilePath", std::string{});
    group.m_legacyMainAudioResourceId =
        group.m_beatmapFilePath.empty()
            ? json.value("m_mainAudioResourceId", std::string{})
            : std::string{};
    group.m_notePayload     = json.value("m_notePayload", std::string{});
    group.m_trackCount      = std::max(0, json.value("m_trackCount", 0));
    group.m_runtimeRevision = 0U;
}

/// @brief 序列化当前谱面入口，不再写出旧版单主音轨字段。
/// @param json 输出 JSON 对象。
/// @param entry 待序列化的谱面入口。
void to_json(nlohmann::json& json, const Project::BeatmapEntry& entry)
{
    // 旧版单音轨字段只读不写，保存后自然迁移到资源引用模型。
    json = nlohmann::json{ { "m_name", entry.m_name },
                           { "m_filePath", entry.m_filePath } };
}

/// @brief 反序列化谱面入口，并读取旧版单主音轨迁移字段。
/// @param json 输入 JSON 对象。
/// @param entry 接收反序列化结果的谱面入口。
void from_json(const nlohmann::json& json, Project::BeatmapEntry& entry)
{
    // 先重置对象，防止复用实例残留新文件未提供的字段。
    entry = Project::BeatmapEntry{};
    // 非对象节点不是有效入口，保持刚建立的空值。
    if ( !json.is_object() ) return;

    entry.m_name         = json.value("m_name", std::string{});
    entry.m_filePath     = json.value("m_filePath", std::string{});
    entry.m_audioTrackId = json.value("m_audioTrackId", std::string{});
}

/// @brief 序列化项目配置。
/// @param j 输出项目描述对象，替换此前的完整内容。
/// @param p 项目值快照，运行时根路径不属于可移植格式。
void to_json(nlohmann::json& j, const Project& p)
{
    // 运行时根目录和临时包身份不写入可迁移的项目描述文件。
    j = nlohmann::json{ { "m_metadata", p.m_metadata },
                        { "m_settings", p.m_settings },
                        { "m_audioResources", p.m_audioResources },
                        { "m_beatmaps", p.m_beatmaps },
                        { "m_draftLaneGroups", p.m_draftLaneGroups },
                        { "m_excludedBeatmapPaths", p.m_excludedBeatmapPaths },
                        { "m_excludedAudioPaths", p.m_excludedAudioPaths } };
}

/// @brief 反序列化项目配置，并兼容旧项目文件中缺失的排除列表。
/// @param j 项目描述对象，核心集合必须存在。
/// @param p 接收项目内容；文件系统身份仍由项目打开流程管理。
void from_json(const nlohmann::json& j, Project& p)
{
    // 核心旧字段保持必需，缺失时由 nlohmann 明确报告损坏项目。
    j.at("m_metadata").get_to(p.m_metadata);
    j.at("m_settings").get_to(p.m_settings);
    j.at("m_audioResources").get_to(p.m_audioResources);
    j.at("m_beatmaps").get_to(p.m_beatmaps);
    // 后增字段使用 value 提供空集合，保证旧项目可直接升级读取。
    p.m_draftLaneGroups =
        j.value("m_draftLaneGroups", std::vector<ProjectDraftLaneGroup>{});
    p.m_excludedBeatmapPaths =
        j.value("m_excludedBeatmapPaths", std::vector<std::string>{});
    p.m_excludedAudioPaths =
        j.value("m_excludedAudioPaths", std::vector<std::string>{});
}

}  // namespace MMM
