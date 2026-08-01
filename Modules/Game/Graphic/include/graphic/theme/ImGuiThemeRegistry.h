#pragma once

#include "graphic/theme/ImGuiTheme.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::Graphic
{

/// @brief 单个主题插件加载错误。
struct ThemePluginLoadError {
    /// @brief 产生错误的 Lua 插件文件。
    std::filesystem::path sourcePath;
    /// @brief 面向日志和状态提示的错误说明。
    std::string message;
};

/// @brief 单个主题插件文件最近一次扫描与加载状态。
struct ThemePluginInfo {
    /// @brief 配置根目录相对稳定 ID，例如 themes/example.lua。
    std::string id;
    /// @brief 插件 Lua 文件完整路径。
    std::filesystem::path sourcePath;
    /// @brief 本次扫描时插件是否启用。
    bool enabled{ true };
    /// @brief 本文件成功创建的主题实例数量。
    std::size_t loadedThemeCount{ 0 };
    /// @brief 本文件本次加载产生的错误数量。
    std::size_t errorCount{ 0 };
    /// @brief 本文件首个加载错误，供插件列表直接显示。
    std::string firstError;
};

/// @brief 一次主题插件目录重载结果。
struct ThemePluginReloadResult {
    /// @brief 扫描到的 Lua 插件文件数量。
    std::size_t discoveredPluginFiles{ 0 };
    /// @brief 成功载入内存的自定义主题实例数量。
    std::size_t loadedThemeCount{ 0 };
    /// @brief 未能载入的主题定义数量。
    std::size_t failedThemeCount{ 0 };
    /// @brief 扫描到但因用户开关而跳过的插件文件数量。
    std::size_t disabledPluginFiles{ 0 };
    /// @brief 文件或主题定义对应的详细错误。
    std::vector<ThemePluginLoadError> errors;

    /// @brief 判断本次重载是否没有错误。
    /// @return 无错误时返回 true。
    [[nodiscard]] bool success() const { return errors.empty(); }
};

/// @brief 按实例持有内置与 Lua 插件 ImGui 主题的注册表。
class ImGuiThemeRegistry final
{
public:
    /// @brief 注册一个内置主题实例。
    /// @param theme 待接管的主题实例。
    /// @return ID 合法且未重复时返回 true。
    bool registerBuiltInTheme(std::unique_ptr<ImGuiTheme> theme);

    /// @brief 删除所有已载入插件主题并重新扫描指定目录。
    /// @param pluginDirectory Lua 主题插件根目录。
    /// @param disabledPluginIds 需要跳过执行的配置根目录相对插件 ID。
    /// @return 扫描、载入和错误统计。
    /// @warning 低频插件重载路径：会访问文件系统并执行
    /// Lua，禁止放入渲染热路径。
    ThemePluginReloadResult reloadThemePlugins(
        const std::filesystem::path& pluginDirectory,
        std::span<const std::string> disabledPluginIds = {});

    /// @brief 按配置根目录相对 ID 查找插件文件状态。
    /// @param id 插件稳定 ID。
    /// @return 找到时返回观察指针；状态由注册表持有到下一次重载。
    [[nodiscard]] const ThemePluginInfo* findPlugin(std::string_view id) const;

    /// @brief 获取最近一次扫描到的插件文件列表。
    /// @return 按插件路径排序的只读状态列表。
    [[nodiscard]] const std::vector<ThemePluginInfo>& plugins() const
    {
        return m_plugins;
    }

    /// @brief 按稳定 ID 查找主题实例。
    /// @param id 主题 ID。
    /// @return 找到时返回观察指针；实例由注册表持有。
    [[nodiscard]] const ImGuiTheme* findTheme(std::string_view id) const;

    /// @brief 判断指定主题 ID 是否已注册。
    /// @param id 主题 ID。
    /// @return 已存在时返回 true。
    [[nodiscard]] bool contains(std::string_view id) const
    {
        return findTheme(id) != nullptr;
    }

    /// @brief 将指定主题及其内置基底写入目标样式。
    /// @param id 主题 ID。
    /// @param style 待覆盖的样式实例。
    /// @return 主题存在且继承关系有效时返回 true。
    bool applyTheme(std::string_view id, ImGuiStyle& style) const;

    /// @brief 获取当前主题实例列表。
    /// @return 按内置注册顺序和插件路径顺序排列的实例列表。
    [[nodiscard]] const std::vector<std::unique_ptr<ImGuiTheme>>& themes() const
    {
        return m_themes;
    }

private:
    /// @brief 注册一个 Lua 插件主题实例。
    /// @param theme 待接管的插件主题实例。
    /// @return ID、基底和重复检查通过时返回 true。
    bool registerPluginTheme(std::unique_ptr<ImGuiTheme> theme);

    /// @brief 删除所有插件来源主题实例。
    void clearPluginThemes();

    /// @brief 验证主题 ID 可用于持久化和查找。
    /// @param id 待验证 ID。
    /// @return ID 合法时返回 true。
    static bool isValidThemeId(std::string_view id);

    /// @brief 当前注册的全部主题实例。
    std::vector<std::unique_ptr<ImGuiTheme>> m_themes;

    /// @brief 最近一次扫描到的全部主题插件文件状态。
    std::vector<ThemePluginInfo> m_plugins;
};

}  // namespace MMM::Graphic
