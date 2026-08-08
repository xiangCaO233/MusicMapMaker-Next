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
    /// @details
    /// 恢复横纵缩放、非 Hold 打击特效时长、绑定音效标签、长条填充模式和
    /// 打开项目时的默认调色方案。
    void resetNoteRenderingToDefaults()
    {
        const EditorConfig defaults;
        visual.noteScaleX = defaults.visual.noteScaleX;
        visual.noteScaleY = defaults.visual.noteScaleY;
        visual.nonHoldHitEffectDuration =
            defaults.visual.nonHoldHitEffectDuration;
        visual.showBoundSampleLabels = defaults.visual.showBoundSampleLabels;
        visual.noteFillMode          = defaults.visual.noteFillMode;
        settings.defaultColorPaletteSchemeName =
            defaults.settings.defaultColorPaletteSchemeName;
    }

    /// @brief 将背景与画布渲染选项恢复为应用默认配置。
    /// @details
    /// 恢复填充模式、背景不透明度、暗化程度、分拍线透明度和检视分拍线延伸比例，
    /// 保留背景电平图配置。
    void resetBackgroundRenderingToDefaults()
    {
        const EditorConfig defaults;
        visual.background.fillMode = defaults.visual.background.fillMode;
        visual.background.opaque_ratio =
            defaults.visual.background.opaque_ratio;
        visual.background.darken_ratio =
            defaults.visual.background.darken_ratio;
        visual.beatLineAlpha = defaults.visual.beatLineAlpha;
        visual.hoverSubdivisionLineExtensionRatio =
            defaults.visual.hoverSubdivisionLineExtensionRatio;
    }
};

/// @brief 将完整编辑器配置序列化为 JSON。
void to_json(nlohmann::json& json, const EditorConfig& config);
/// @brief 从 JSON 读取完整编辑器配置。
void from_json(const nlohmann::json& json, EditorConfig& config);

}  // namespace MMM::Config
