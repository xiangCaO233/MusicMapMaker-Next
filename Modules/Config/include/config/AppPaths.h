#pragma once

#include <filesystem>

namespace MMM::Config
{

/// @brief 应用级路径工具，集中管理配置目录和资源包目录。
class AppPaths
{
public:
    /// @brief 获取本应用在用户 .config 下的根目录。
    /// @return 用户配置根目录，Windows 下为 用户目录/.config/mmm。
    static std::filesystem::path configRootPath();

    /// @brief 确保用户配置根目录及其父路径存在。
    /// @return 目录存在或创建成功时返回 true。
    static bool ensureConfigRootPath();

    /// @brief 获取用户配置文件路径。
    /// @return user_config.json 在用户 .config 根目录下的完整路径。
    static std::filesystem::path userConfigFilePath();

    /// @brief 获取 ImGui 布局配置文件路径。
    /// @return imgui.ini 在用户 .config 根目录下的完整路径。
    static std::filesystem::path imguiIniFilePath();

    /// @brief 获取用户资源包根目录。
    /// @return assets 资源包在用户 .config 根目录下的完整路径。
    static std::filesystem::path assetsRootPath();

    /// @brief 获取默认皮肤入口脚本路径。
    /// @return mmm-default 皮肤的 skin.lua 完整路径。
    static std::filesystem::path defaultSkinFilePath();

    /// @brief 获取窗口图标路径。
    /// @return 默认资源包中的窗口图标完整路径。
    static std::filesystem::path windowIconFilePath();

    /// @brief 获取旧版用户配置文件路径。
    /// @return 当前工作目录下的旧 user_config.json 路径，用于一次性迁移。
    static std::filesystem::path legacyUserConfigFilePath();
};

}  // namespace MMM::Config
