#pragma once

#include "graphic/theme/ImGuiTheme.h"

#include <cstddef>
#include <filesystem>
#include <memory>
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

/// @brief 一次主题插件目录重载结果。
struct ThemePluginReloadResult {
    /// @brief 扫描到的 Lua 插件文件数量。
    std::size_t discoveredPluginFiles{ 0 };
    /// @brief 成功载入内存的自定义主题实例数量。
    std::size_t loadedThemeCount{ 0 };
    /// @brief 未能载入的主题定义数量。
    std::size_t failedThemeCount{ 0 };
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
    /// @return 扫描、载入和错误统计。
    /// @warning 低频插件重载路径：会访问文件系统并执行
    /// Lua，禁止放入渲染热路径。
    ThemePluginReloadResult reloadThemePlugins(
        const std::filesystem::path& pluginDirectory);

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
};

}  // namespace MMM::Graphic
