#pragma once
#include "config/EditorSettings.h"
#include "config/VisualConfig.h"
#include "mmm/project/ProjectWorkspaceState.h"
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace MMM
{

/**
 * @file ProjectSettings.h
 * @brief 定义随项目保存的覆盖配置与编辑工作区快照。
 *
 * 工作区字段只描述再次打开项目时可恢复的编辑现场，不参与谱面内容语义。
 * 项目覆盖配置仅保存允许随项目迁移的选项；软件级隐私、诊断、欢迎页与工具栏
 * 外观偏好必须继续由全局配置拥有。反序列化需要兼容旧项目缺失字段，并在读取
 * 后规整非有限数值、未知枚举文本与越界位掩码，避免持久化数据直接污染运行态。
 */

/// @brief 项目级的特定偏好设置
/// @details
/// 包含可以覆盖全局配置的可选项，以及最后一次的状态记录。软件级隐私、诊断、
/// PGO 上传、欢迎页和工具栏外观不会随项目传播，序列化时显式剔除这些字段。
struct ProjectSettings {
    /// @brief 覆盖全局视觉配置 (若为 nullopt 则继承全局 .config/mmm 配置)
    std::optional<Config::VisualConfig> m_visualOverride;

    /// @brief 覆盖全局编辑器行为 (若为 nullopt 则继承全局 .config/mmm 配置)
    std::optional<Config::EditorSettings> m_editorOverride;

    /// @brief 覆盖软件级谱面自动备份配置；空值表示继承软件配置。
    std::optional<Config::AutoBackupConfig> m_autoBackupOverride;

    /// @brief 项目中最后一次打开的谱面名称 (BeatmapEntry::m_name)
    std::string m_lastOpenedBeatmap;

    /// @brief 项目打开时应用的调色方案；新项目默认留空以继承软件默认。
    std::string m_colorPaletteSchemeName;

    /// @brief 项目级工作区状态。
    ProjectWorkspaceState m_workspace;

    /// @brief 序列化项目设置。
    /// @param j 接收对象的 JSON 节点。
    /// @param settings 待保存的项目覆盖配置和工作区。
    friend void to_json(nlohmann::json& j, const ProjectSettings& settings);

    /// @brief 反序列化项目设置，并兼容缺少工作区字段的旧项目。
    /// @param j 来源 JSON 对象。
    /// @param settings 接收并完成软件级字段隔离后的项目设置。
    friend void from_json(const nlohmann::json& j, ProjectSettings& settings);
};

}  // namespace MMM
