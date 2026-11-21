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
    /// 恢复横纵缩放、非 Hold 打击特效时长、绑定音效标签、长条填充模式、Note
    /// 贴图位置和 打开项目时的默认调色方案。
    void resetNoteRenderingToDefaults()
    {
        // 临时默认对象集中提供字段初值，避免在重置路径重复维护常量。
        const EditorConfig defaults;
        visual.noteScaleX     = defaults.visual.noteScaleX;
        visual.noteScaleY     = defaults.visual.noteScaleY;
        visual.notePartScales = defaults.visual.notePartScales;
        visual.nonHoldHitEffectDuration =
            defaults.visual.nonHoldHitEffectDuration;
        visual.showBoundSampleLabels = defaults.visual.showBoundSampleLabels;
        visual.noteFillMode          = defaults.visual.noteFillMode;
        // 固定位置与缩放归属同一复位分组，不能保留上次底边设置。
        visual.noteTexturePosition = defaults.visual.noteTexturePosition;
        settings.defaultColorPaletteSchemeName =
            defaults.settings.defaultColorPaletteSchemeName;
    }

    /// @brief 将背景与画布渲染选项恢复为应用默认配置。
    /// @details
    /// 恢复填充模式、背景不透明度、暗化程度、分拍线透明度和检视分拍线延伸比例，
    /// 保留背景电平图配置。
    void resetBackgroundRenderingToDefaults()
    {
        // 只覆盖本分组声明的选项，背景频谱等独立设置保持用户值。
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

/// @brief 同时借用会话行为与视口视觉配置，渲染时不复制配置容器。
/// @details 生命周期不得超过两份被借用配置，不能保存为异步任务状态。
/// 传递此值不改变所有权；调用方需保持引用有效直至本次绘制或坐标计算完成。
/// 仅独立视图会覆盖视觉字段，多窗口共享谱面时音符操作规则仍指向同一会话。
struct EditorConfigView {
    const VisualConfig&   visual;    ///< 当前视口的有效视觉配置。
    const EditorSettings& settings;  ///< 与谱面会话共用的编辑及音频策略。

    /// @brief 普通画布与辅助视图沿用同一份完整配置。
    EditorConfigView(const EditorConfig& config)
        : EditorConfigView(config.visual, config.settings)
    {
    }

    /// @brief 对比视图可提供独立布局，行为仍由谱面会话管理。
    EditorConfigView(const VisualConfig&   visualConfig,
                     const EditorSettings& editorSettings)
        : visual(visualConfig), settings(editorSettings)
    {
    }
};

/// @brief 将完整编辑器配置序列化为 JSON。
void to_json(nlohmann::json& json, const EditorConfig& config);
/// @brief 从 JSON 读取完整编辑器配置。
void from_json(const nlohmann::json& json, EditorConfig& config);

}  // namespace MMM::Config
