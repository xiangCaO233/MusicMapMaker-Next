#pragma once

namespace MMM::Config
{

// 三个类型均由会话或视口持有，借用接口只需要其声明。
struct EditorConfig;
struct EditorSettings;
struct VisualConfig;

/// @brief 借用会话行为、全局视觉偏好与视口布局，渲染时不复制配置容器。
/// @details 生命周期不得超过被借用的配置，不能保存为异步任务状态。
/// 整体视图仅包含三条引用，不进入持久化配置结构。
/// 传递此值不改变所有权；调用方需保持引用有效直至本次绘制或坐标计算完成。
/// 仅独立视图会覆盖视觉字段，多窗口共享谱面时音符操作规则仍指向同一会话。
struct EditorConfigView {
    const VisualConfig&   visual;    ///< 当前视口的有效视觉配置。
    const EditorSettings& settings;  ///< 与谱面会话共用的编辑及音频策略。
    /// @brief 会话共享的分拍线显示模式与光标渐隐范围，跟随全局工具栏。
    const VisualConfig& sharedVisual;

    /// @brief 普通画布与辅助视图沿用同一份完整配置。
    /// @param config 在视图使用期间保持有效的拥有型配置。
    EditorConfigView(const EditorConfig& config);

    /// @brief 对比视图可提供独立布局，行为仍由谱面会话管理。
    EditorConfigView(const VisualConfig&   visualConfig,
                     const EditorSettings& editorSettings)
        : EditorConfigView(visualConfig, editorSettings, visualConfig)
    {
    }

    /// @brief 保留对比视口本地布局，同时借用会话最新的共享显示偏好。
    /// @note 普通视口可将同一份视觉配置同时用作布局与共享来源。
    EditorConfigView(const VisualConfig&   visualConfig,
                     const EditorSettings& editorSettings,
                     const VisualConfig&   sharedVisualConfig)
        : visual(visualConfig)
        , settings(editorSettings)
        , sharedVisual(sharedVisualConfig)
    {
    }
};

}  // namespace MMM::Config
