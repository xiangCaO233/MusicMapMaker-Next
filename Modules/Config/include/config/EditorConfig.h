#pragma once

#include "EditorSettings.h"
#include "VisualConfig.h"

#include <string>
#include <vector>

namespace MMM::Config
{

/// @brief 编辑器全局配置容器 (持久化于 用户目录/.config/mmm/user_config.json)
/// @details 包含视觉表现和编辑行为两大类配置
struct EditorConfig {
    /// @brief 视觉、布局与渲染相关的配置
    VisualConfig visual;

    /// @brief 编辑逻辑、同步与音效策略相关的设置
    EditorSettings settings;

    /// @brief 最近打开的项目路径列表
    std::vector<std::string> recentProjects;

    /// @brief 将物件渲染选项恢复为应用默认配置。
    /// @details 恢复横纵缩放、长条填充模式和打开项目时的默认调色方案。
    void resetNoteRenderingToDefaults()
    {
        const EditorConfig defaults;
        visual.noteScaleX   = defaults.visual.noteScaleX;
        visual.noteScaleY   = defaults.visual.noteScaleY;
        visual.noteFillMode = defaults.visual.noteFillMode;
        settings.defaultColorPaletteSchemeName =
            defaults.settings.defaultColorPaletteSchemeName;
    }

    /// @brief 将背景图像渲染选项恢复为应用默认配置。
    /// @details 仅恢复填充模式、不透明度和暗化程度，保留背景电平图配置。
    void resetBackgroundRenderingToDefaults()
    {
        const EditorConfig defaults;
        visual.background.fillMode = defaults.visual.background.fillMode;
        visual.background.opaque_ratio =
            defaults.visual.background.opaque_ratio;
        visual.background.darken_ratio =
            defaults.visual.background.darken_ratio;
    }
};

inline void to_json(nlohmann::json& j, const EditorConfig& c)
{
    j = nlohmann::json{ { "visual", c.visual },
                        { "settings", c.settings },
                        { "recentProjects", c.recentProjects } };
}

inline void from_json(const nlohmann::json& j, EditorConfig& c)
{
    c.visual         = j.value("visual", VisualConfig());
    c.settings       = j.value("settings", EditorSettings());
    c.recentProjects = j.value("recentProjects", std::vector<std::string>());
}

}  // namespace MMM::Config
