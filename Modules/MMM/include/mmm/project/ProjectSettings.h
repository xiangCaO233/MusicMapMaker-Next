#pragma once
#include "config/EditorSettings.h"
#include "config/VisualConfig.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace MMM
{

/// @brief 项目级的特定偏好设置
/// @details 包含可以覆盖全局配置的可选项，以及最后一次的状态记录
struct ProjectSettings {
    /// @brief 覆盖全局视觉配置 (若为 nullopt 则继承全局 user_config.json)
    std::optional<Config::VisualConfig> m_visualOverride;

    /// @brief 覆盖全局编辑器行为 (若为 nullopt 则继承全局 user_config.json)
    std::optional<Config::EditorSettings> m_editorOverride;

    /// @brief 项目中最后一次打开的谱面名称 (BeatmapEntry::m_name)
    std::string m_lastOpenedBeatmap;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ProjectSettings, m_visualOverride,
                                   m_editorOverride, m_lastOpenedBeatmap)
};

}  // namespace MMM
