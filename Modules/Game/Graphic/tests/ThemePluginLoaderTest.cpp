#include "graphic/theme/ImGuiThemeRegistry.h"

#include "log/colorful-log.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>

namespace
{

/// @brief 记录主题插件测试断言失败。
/// @param condition 待验证条件。
/// @param message 失败说明。
/// @return 条件原值。
bool check(bool condition, std::string_view message)
{
    if ( !condition ) {
        XERROR("ThemePluginLoaderTest failed: {}", message);
    }
    return condition;
}

/// @brief 判断两个浮点值近似相等。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 误差小于阈值时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 0.0001f;
}

/// @brief 写入 Lua 测试插件。
/// @param path 输出路径。
/// @param content Lua 源码。
/// @return 写入成功时返回 true。
bool writePlugin(const std::filesystem::path& path, std::string_view content)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if ( !file ) return false;
    file << content;
    return file.good();
}

/// @brief 注册测试使用的最小 DeepDark 内置主题。
/// @param registry 目标注册表。
/// @return 注册成功时返回 true。
bool registerBaseTheme(MMM::Graphic::ImGuiThemeRegistry& registry)
{
    return registry.registerBuiltInTheme(
        std::make_unique<MMM::Graphic::ImGuiTheme>(
            "DeepDark",
            "DeepDark",
            MMM::Graphic::ImGuiThemeOrigin::BuiltIn,
            std::string(),
            std::filesystem::path(),
            [](ImGuiStyle& style) {
                style.Alpha                 = 0.9f;
                style.WindowPadding         = { 1.0f, 2.0f };
                style.Colors[ImGuiCol_Text] = ImVec4(0.1f, 0.2f, 0.3f, 1.0f);
                style.Colors[ImGuiCol_WindowBg] =
                    ImVec4(0.4f, 0.5f, 0.6f, 1.0f);
            }));
}

/// @brief 验证 Lua 主题字段解析与内置基底继承。
/// @param registry 主题注册表。
/// @param pluginDirectory 测试插件目录。
/// @return 首轮加载和样式断言全部成功时返回 true。
bool testPluginLoad(MMM::Graphic::ImGuiThemeRegistry& registry,
                    const std::filesystem::path&      pluginDirectory)
{
    const auto validPlugin   = pluginDirectory / "01-valid.lua";
    const auto invalidPlugin = pluginDirectory / "02-invalid.lua";
    bool       ok            = check(writePlugin(validPlugin,
                                                 R"lua(return {
    type = "theme",
    id = "test.ocean",
    name = "Test Ocean",
    base = "DeepDark",
    style = {
        Alpha = 0.75,
        WindowPadding = { 12.0, 13.0 },
        WindowMenuButtonPosition = "Right",
        AntiAliasedFill = false,
        Colors = {
            Text = { 0.9, 0.8, 0.7, 1.0 },
        },
    },
})lua"),
                                     "有效插件写入失败");
    ok &= check(writePlugin(invalidPlugin,
                            R"lua(return {
    type = "theme",
    id = "test.invalid",
    name = "Invalid",
    base = "MissingBuiltIn",
    style = {},
})lua"),
                "无效插件写入失败");
    if ( !ok ) return false;

    const auto result = registry.reloadThemePlugins(pluginDirectory);
    ok &= check(result.discoveredPluginFiles == 2, "插件扫描数量不匹配");
    ok &= check(result.loadedThemeCount == 1, "有效主题载入数量不匹配");
    ok &= check(result.failedThemeCount == 1, "无效主题数量不匹配");
    ok &= check(registry.findTheme("test.ocean") != nullptr,
                "有效主题实例未进入注册表");
    ok &= check(registry.findTheme("test.invalid") == nullptr,
                "无效主题实例不应进入注册表");

    ImGuiStyle style;
    ok &= check(registry.applyTheme("test.ocean", style), "自定义主题应用失败");
    ok &= check(near(style.Alpha, 0.75f), "Alpha 覆盖失败");
    ok &= check(near(style.WindowPadding.x, 12.0f) &&
                    near(style.WindowPadding.y, 13.0f),
                "WindowPadding 覆盖失败");
    ok &= check(style.WindowMenuButtonPosition == ImGuiDir_Right,
                "方向字段覆盖失败");
    ok &= check(!style.AntiAliasedFill, "布尔字段覆盖失败");
    ok &= check(near(style.Colors[ImGuiCol_Text].x, 0.9f), "颜色字段覆盖失败");
    ok &= check(near(style.Colors[ImGuiCol_WindowBg].x, 0.4f),
                "未覆盖颜色应继承内置基底");
    return ok;
}

/// @brief 验证重载会删除旧插件主题实例。
/// @param registry 主题注册表。
/// @param pluginDirectory 测试插件目录。
/// @return 旧实例删除且新实例载入时返回 true。
bool testReloadClearsOldThemes(MMM::Graphic::ImGuiThemeRegistry& registry,
                               const std::filesystem::path& pluginDirectory)
{
    std::error_code filesystemError;
    std::filesystem::remove(pluginDirectory / "02-invalid.lua",
                            filesystemError);
    if ( filesystemError ) {
        return check(false, "无法删除无效测试插件");
    }

    const bool written = writePlugin(pluginDirectory / "01-valid.lua",
                                     R"lua(return {
    type = "theme",
    id = "test.reloaded",
    name = "Reloaded Theme",
    base = "DeepDark",
    style = {
        FrameRounding = 8.0,
    },
})lua");
    if ( !check(written, "重载插件写入失败") ) return false;

    const auto result = registry.reloadThemePlugins(pluginDirectory);
    bool       ok     = check(result.success(), "第二次重载不应失败");
    ok &= check(result.loadedThemeCount == 1, "第二次重载数量不匹配");
    ok &= check(registry.findTheme("test.ocean") == nullptr,
                "重载后旧主题实例仍然存在");
    ok &= check(registry.findTheme("test.reloaded") != nullptr,
                "重载后新主题实例不存在");
    return ok;
}

/// @brief 验证文档附带的完整示例始终符合实际加载接口。
/// @param examplePath 仓库内示例插件路径。
/// @param pluginDirectory 独立测试插件目录。
/// @return 示例被复制、解析并注册时返回 true。
bool testDocumentedExample(const std::filesystem::path& examplePath,
                           const std::filesystem::path& pluginDirectory)
{
    std::error_code filesystemError;
    std::filesystem::remove_all(pluginDirectory, filesystemError);
    filesystemError.clear();
    std::filesystem::create_directories(pluginDirectory, filesystemError);
    if ( filesystemError ) {
        return check(false, "无法创建示例插件测试目录");
    }
    std::filesystem::copy_file(
        examplePath,
        pluginDirectory / "theme-example.lua",
        std::filesystem::copy_options::overwrite_existing,
        filesystemError);
    if ( filesystemError ) {
        return check(false, "无法复制文档示例插件");
    }

    MMM::Graphic::ImGuiThemeRegistry registry;
    bool ok = check(registerBaseTheme(registry), "示例测试基底主题注册失败");
    const auto result = registry.reloadThemePlugins(pluginDirectory);
    ok &= check(result.success(), "文档示例插件应成功载入");
    ok &= check(result.loadedThemeCount == 1, "文档示例应创建一个主题实例");
    ok &= check(registry.findTheme("example.twilight") != nullptr,
                "文档示例主题实例未进入注册表");
    return ok;
}

}  // namespace

/// @brief 主题 Lua 插件加载与重载回归测试入口。
/// @param argc 命令行参数数量。
/// @param argv 首个附加参数为测试输出目录，第二个为文档示例插件路径。
/// @return 全部断言通过时返回 0。
int main(int argc, char* argv[])
{
    if ( argc < 3 || !argv[1] || !argv[2] ) {
        XERROR(
            "ThemePluginLoaderTest requires output and example plugin paths");
        return 1;
    }

    const std::filesystem::path pluginDirectory = argv[1];
    const std::filesystem::path examplePath     = argv[2];
    std::error_code             filesystemError;
    std::filesystem::remove_all(pluginDirectory, filesystemError);
    filesystemError.clear();
    std::filesystem::create_directories(pluginDirectory, filesystemError);
    if ( filesystemError ) {
        XERROR("Failed to prepare theme plugin test directory: {}",
               filesystemError.message());
        return 1;
    }

    MMM::Graphic::ImGuiThemeRegistry registry;
    bool ok = check(registerBaseTheme(registry), "内置基底主题注册失败");
    ok &= testPluginLoad(registry, pluginDirectory);
    ok &= testReloadClearsOldThemes(registry, pluginDirectory);
    ok &= testDocumentedExample(
        examplePath, pluginDirectory.parent_path() / "theme_plugin_example");
    return ok ? 0 : 1;
}
