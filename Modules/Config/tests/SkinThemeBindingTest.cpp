#include "config/EditorSettings.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace
{
/// @brief 记录测试断言失败。
/// @param condition 待验证条件。
/// @param message 条件失败时写入日志的说明。
/// @return 条件原值。
bool check(bool condition, std::string_view message)
{
    if ( !condition ) XERROR("SkinThemeBindingTest failed: {}", message);
    return condition;
}

/// @brief 写入用于皮肤主题解析测试的最小 Lua 文件。
/// @param path 输出文件路径。
/// @param themeExpression theme 字段对应的 Lua 表达式。
/// @return 文件成功写入时返回 true。
bool writeTestSkin(const std::filesystem::path& path,
                   std::string_view             themeExpression)
{
    std::ofstream file(path, std::ios::binary);
    if ( !file ) return false;
    file << "return {\n"
            "  meta = { name = 'Theme Test', author = 'Test', version = "
            "'1.0' },\n"
            "  langs = {},\n"
            "  fonts = {},\n"
            "  assets = {},\n"
            "  audios = {},\n"
            "  layout = {},\n"
            "  theme = "
         << themeExpression << "\n}\n";
    return file.good();
}

/// @brief 加载测试皮肤并核对亮暗主题绑定。
/// @param path 测试皮肤入口路径。
/// @param expectedLight 期望亮色主题。
/// @param expectedDark 期望暗色主题。
/// @return 加载与两个分支断言均成功时返回 true。
bool verifyThemeBinding(const std::filesystem::path& path,
                        std::string_view             expectedLight,
                        std::string_view             expectedDark)
{
    auto& skinManager = MMM::Config::SkinManager::instance();
    bool  ok = check(skinManager.loadSkin(MMM::Config::pathToUtf8(path)),
                     "皮肤应成功加载");
    ok &= check(skinManager.getDefaultTheme(
                    MMM::Config::SkinThemeAppearance::Light) == expectedLight,
                "亮色主题绑定不匹配");
    ok &= check(skinManager.getDefaultTheme(
                    MMM::Config::SkinThemeAppearance::Dark) == expectedDark,
                "暗色主题绑定不匹配");
    return ok;
}

/// @brief 验证旧版应用配置能区分自动与手动主题偏好。
/// @return 兼容语义与序列化值均正确时返回 true。
bool verifyLegacyAppConfigSemantics()
{
    MMM::Config::EditorSettings automaticSettings;
    const nlohmann::json        automaticJson{ { "theme", "Auto" } };
    from_json(automaticJson, automaticSettings);

    MMM::Config::EditorSettings manualSettings;
    const nlohmann::json        manualJson{ { "theme", "Moonlight" } };
    from_json(manualJson, manualSettings);

    MMM::Config::EditorSettings pluginSettings;
    const nlohmann::json        pluginJson{ { "theme", "example.twilight" } };
    from_json(pluginJson, pluginSettings);

    MMM::Config::EditorSettings legacyCeciliaSettings;
    const nlohmann::json        legacyCeciliaJson{ { "theme", "MmmDefault" } };
    from_json(legacyCeciliaJson, legacyCeciliaSettings);

    nlohmann::json serializedAutomatic;
    to_json(serializedAutomatic, automaticSettings);
    nlohmann::json serializedPlugin;
    to_json(serializedPlugin, pluginSettings);

    bool ok = check(automaticSettings.theme == MMM::Config::UI_THEME_AUTO_ID,
                    "旧版 Auto 应继续表示未手动指定主题");
    ok &= check(manualSettings.theme == "Moonlight",
                "旧版非 Auto 主题应继续表示用户手动选择");
    ok &= check(pluginSettings.theme == "example.twilight",
                "插件主题稳定 ID 应原样载入");
    ok &= check(legacyCeciliaSettings.theme == "Cecilia",
                "旧版 MmmDefault 主题应迁移为 Cecilia");
    ok &= check(serializedAutomatic.value("theme", std::string()) == "Auto",
                "自动主题序列化哨兵必须保持为 Auto");
    ok &= check(
        serializedPlugin.value("theme", std::string()) == "example.twilight",
        "插件主题稳定 ID 应原样序列化");
    return ok;
}
}  // namespace

/// @brief 皮肤亮暗主题绑定与旧配置兼容回归测试入口。
/// @param argc 命令行参数数量。
/// @param argv 命令行参数；首个附加参数为测试输出目录。
/// @return 全部断言通过时返回 0。
int main(int argc, char* argv[])
{
    if ( argc < 2 || !argv[1] ) {
        XERROR("SkinThemeBindingTest requires an output directory");
        return 1;
    }

    const std::filesystem::path outputDirectory =
        MMM::Config::utf8ToPath(argv[1]);
    std::error_code filesystemError;
    std::filesystem::remove_all(outputDirectory, filesystemError);
    filesystemError.clear();
    std::filesystem::create_directories(outputDirectory, filesystemError);
    if ( filesystemError ) {
        XERROR("Failed to create SkinThemeBindingTest output directory: {}",
               filesystemError.message());
        return 1;
    }

    const std::filesystem::path legacySkinPath =
        outputDirectory / "legacy-skin.lua";
    const std::filesystem::path pairedSkinPath =
        outputDirectory / "paired-skin.lua";
    const std::filesystem::path lightOnlySkinPath =
        outputDirectory / "light-only-skin.lua";

    bool ok = check(writeTestSkin(legacySkinPath, "'Cecilia'"),
                    "旧格式测试皮肤写入失败");
    ok &= check(writeTestSkin(pairedSkinPath,
                              "{ light = 'Cecilia', dark = 'Moonlight' }"),
                "亮暗双主题测试皮肤写入失败");
    ok &= check(
        writeTestSkin(lightOnlySkinPath, "{ light = 'ComfortableLight' }"),
        "单分支测试皮肤写入失败");
    if ( !ok ) return 1;

    ok &= verifyThemeBinding(legacySkinPath, "Cecilia", "Cecilia");
    ok &= verifyThemeBinding(pairedSkinPath, "Cecilia", "Moonlight");
    ok &= verifyThemeBinding(
        lightOnlySkinPath, "ComfortableLight", "ComfortableLight");
    ok &= verifyLegacyAppConfigSemantics();
    return ok ? 0 : 1;
}
