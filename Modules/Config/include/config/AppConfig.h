#pragma once

#include "EditorConfig.h"
#include <filesystem>
#include <mutex>

namespace MMM::Config
{

/// @brief 全局应用配置管理器
class AppConfig
{
public:
    static AppConfig& instance();

    /// @brief 获取编辑器配置
    EditorConfig&       getEditorConfig() { return m_editorConfig; }
    const EditorConfig& getEditorConfig() const { return m_editorConfig; }

    /// @brief 获取视觉配置
    VisualConfig&       getVisualConfig() { return m_editorConfig.visual; }
    const VisualConfig& getVisualConfig() const
    {
        return m_editorConfig.visual;
    }

    /// @brief 获取编辑器设置
    EditorSettings& getEditorSettings() { return m_editorConfig.settings; }
    const EditorSettings& getEditorSettings() const
    {
        return m_editorConfig.settings;
    }

    /// @brief 从文件加载配置
    /// @param path 配置文件路径，若为空则使用默认路径
    bool load(const std::filesystem::path& path = "");

    /// @brief 保存配置到文件
    /// @param path 配置文件路径，若为空则使用默认路径
    bool save(const std::filesystem::path& path = "") const;

    /// @brief 重置为默认配置
    void reset();

    /// @brief 添加最近打开的项目
    /// @param path 项目文件夹路径
    void addRecentProject(const std::string& path);

    /// @brief 获取设备屏幕刷新率
    int getDeviceRefreshRate() const { return m_deviceRefreshRate; }

    /// @brief 设置设备屏幕刷新率（由图形模块在启动时写入）
    void setDeviceRefreshRate(int rate) { m_deviceRefreshRate = rate; }

    /// @brief 获取窗口缩放比例 (GLFW 原生报告的比例，用于字体加载等)
    float getNativeContentScale() const { return m_nativeContentScale; }

    /// @brief 设置窗口缩放比例
    void setNativeContentScale(float scale) { m_nativeContentScale = scale; }

    /// @brief 获取 UI 布局缩放比例 (已减去系统自动缩放部分，并包含用户自定义倍率)
    float getUIScale() const
    {
        return m_uiScale * m_editorConfig.settings.uiScaleMultiplier;
    }

    /// @brief 设置 UI 布局缩放比例 (仅设置系统部分)
    void setUIScale(float scale) { m_uiScale = scale; }

    /// @brief 获取窗口缩放比例 (包含用户自定义倍率)
    float getWindowContentScale() const
    {
        return m_uiScale * m_editorConfig.settings.uiScaleMultiplier;
    }

    /// @brief 设置窗口缩放比例 (仅设置系统部分)
    void setWindowContentScale(float scale) { m_uiScale = scale; }

private:
    AppConfig();
    ~AppConfig() = default;

    AppConfig(const AppConfig&)            = delete;
    AppConfig& operator=(const AppConfig&) = delete;

    std::filesystem::path getDefaultConfigPath() const;

    EditorConfig       m_editorConfig;
    mutable std::mutex m_mutex;
    int                m_deviceRefreshRate{ 60 };  // 默认60Hz
    float              m_nativeContentScale{ 1.0f };
    float              m_uiScale{ 1.0f };
};

}  // namespace MMM::Config
