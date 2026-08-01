#include "config/EditorSettings.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <array>
#include <cstdint>
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

/// @brief 写入发光被关闭的旧版内置 IVM 测试皮肤。
/// @param path 输出文件路径；父目录必须使用内置皮肤目录名 ivm。
/// @return 文件成功写入时返回 true。
bool writeLegacyIvmSkin(const std::filesystem::path& path)
{
    std::ofstream file(path, std::ios::binary);
    if ( !file ) return false;
    file << "return {\n"
            "  meta = { name = 'IVM', author = 'Test', version = '1.0' },\n"
            "  langs = {},\n"
            "  fonts = {},\n"
            "  assets = {},\n"
            "  audios = {},\n"
            "  layout = {},\n"
            "  theme = 'IVM',\n"
            "  effects = { glow = { passes = 0, intensity = 0.0 } }\n"
            "}\n";
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
    ok &= check(skinManager.getHitEffectLayoutMode() ==
                    MMM::Config::HitEffectLayoutMode::Fixed,
                "未声明布局的旧皮肤必须保持固定尺寸打击特效");
    return ok;
}

/// @brief 验证旧版内置 IVM 在资源文件未更新时仍恢复交互发光。
/// @param path 位于 ivm 目录内的旧版测试皮肤路径。
/// @return 悬浮与选中共用的发光配置已迁移时返回 true。
bool verifyLegacyIvmGlowMigration(const std::filesystem::path& path)
{
    auto& skinManager = MMM::Config::SkinManager::instance();
    bool  ok = check(skinManager.loadSkin(MMM::Config::pathToUtf8(path)),
                     "旧版 IVM 测试皮肤应成功加载");
    ok &= check(skinManager.getGlowPasses() == 6,
                "旧版 IVM 必须恢复交互发光轮次");
    ok &= check(skinManager.getGlowIntensity() == 0.5F,
                "旧版 IVM 必须恢复交互发光强度");
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
    const nlohmann::json        pluginJson{
        { "theme", "example.twilight" },
        { "disabledPluginIds",
          nlohmann::json::array({ "themes/example.lua" }) },
    };
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
    ok &= check(pluginSettings.disabledPluginIds ==
                    std::vector<std::string>{ "themes/example.lua" },
                "禁用插件 ID 应原样载入");
    ok &= check(legacyCeciliaSettings.theme == "Cecilia",
                "旧版 MmmDefault 主题应迁移为 Cecilia");
    ok &= check(serializedAutomatic.value("theme", std::string()) == "Auto",
                "自动主题序列化哨兵必须保持为 Auto");
    ok &= check(
        serializedPlugin.value("theme", std::string()) == "example.twilight",
        "插件主题稳定 ID 应原样序列化");
    ok &= check(serializedPlugin.value("disabledPluginIds",
                                       std::vector<std::string>()) ==
                    std::vector<std::string>{ "themes/example.lua" },
                "禁用插件 ID 应原样序列化");
    return ok;
}

/// @brief 从 PNG 文件头读取像素尺寸。
/// @param path PNG 文件路径。
/// @param width 成功时写入宽度。
/// @param height 成功时写入高度。
/// @return 文件包含有效 PNG 签名和 IHDR 尺寸时返回 true。
bool readPngDimensions(const std::filesystem::path& path, std::uint32_t& width,
                       std::uint32_t& height)
{
    std::ifstream file(path, std::ios::binary);
    if ( !file ) return false;

    std::array<std::uint8_t, 24> header{};
    file.read(reinterpret_cast<char*>(header.data()),
              static_cast<std::streamsize>(header.size()));
    if ( file.gcount() != static_cast<std::streamsize>(header.size()) ) {
        return false;
    }

    constexpr std::array<std::uint8_t, 8> PNG_SIGNATURE{ 0x89U, 0x50U, 0x4eU,
                                                         0x47U, 0x0dU, 0x0aU,
                                                         0x1aU, 0x0aU };
    if ( !std::equal(
             PNG_SIGNATURE.begin(), PNG_SIGNATURE.end(), header.begin()) ||
         header[12] != 'I' || header[13] != 'H' || header[14] != 'D' ||
         header[15] != 'R' ) {
        return false;
    }

    auto readBigEndian = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(header[offset]) << 24U |
               static_cast<std::uint32_t>(header[offset + 1U]) << 16U |
               static_cast<std::uint32_t>(header[offset + 2U]) << 8U |
               static_cast<std::uint32_t>(header[offset + 3U]);
    };
    width  = readBigEndian(16U);
    height = readBigEndian(20U);
    return width > 0U && height > 0U;
}

/// @brief 验证 IVM 内置皮肤的固定主题、颜色和纹理几何约束。
/// @param skinPath 仓库中 IVM 皮肤入口路径。
/// @return 皮肤配置与全部自维护资源符合设计时返回 true。
bool verifyIvmSkin(const std::filesystem::path& skinPath)
{
    auto& skinManager = MMM::Config::SkinManager::instance();
    bool  ok = check(skinManager.loadSkin(MMM::Config::pathToUtf8(skinPath)),
                     "IVM 内置皮肤应成功加载");
    ok &= check(skinManager.getData().themeName == "IVM", "IVM 皮肤名称不匹配");
    ok &= check(skinManager.getDefaultTheme(
                    MMM::Config::SkinThemeAppearance::Light) == "IVM" &&
                    skinManager.getDefaultTheme(
                        MMM::Config::SkinThemeAppearance::Dark) == "IVM",
                "IVM 皮肤亮暗分支都必须绑定内置 IVM 主题");
    ok &= check(skinManager.getHitEffectLayoutMode() ==
                    MMM::Config::HitEffectLayoutMode::TrackFill,
                "IVM 皮肤必须启用整轨填充打击特效");

    const auto holdColor = skinManager.getColor("note_hold");
    const auto nodeColor = skinManager.getColor("note_node");
    ok &= check(holdColor.r == nodeColor.r && holdColor.g == nodeColor.g &&
                    holdColor.b == nodeColor.b && holdColor.a == nodeColor.a,
                "IVM 节点颜色必须与 Body 完全一致");

    const auto beatHead = skinManager.getColor("beat_lines.beat_1");
    ok &= check(beatHead.r == 1.0f && beatHead.g == 0.0f &&
                    beatHead.b == 0.0f && beatHead.a == 1.0f,
                "IVM 拍头线必须是完全不透明的纯红色");

    const auto referenceBeatLine = skinManager.getColor("beat_lines.beat_2");
    constexpr std::array<std::string_view, 8> BEAT_LINE_KEYS{
        "beat_lines.beat_2",  "beat_lines.beat_3",  "beat_lines.beat_4",
        "beat_lines.beat_6",  "beat_lines.beat_8",  "beat_lines.beat_12",
        "beat_lines.beat_16", "beat_lines.default",
    };
    ok &= check(referenceBeatLine.r == referenceBeatLine.g &&
                    referenceBeatLine.g == referenceBeatLine.b &&
                    referenceBeatLine.a == 1.0f,
                "IVM 默认分拍线必须是完全不透明的灰色");
    for ( std::string_view key : BEAT_LINE_KEYS ) {
        const auto color = skinManager.getColor(std::string(key));
        ok &= check(color.r == referenceBeatLine.r &&
                        color.g == referenceBeatLine.g &&
                        color.b == referenceBeatLine.b &&
                        color.a == referenceBeatLine.a,
                    "IVM 全部分拍线槽位必须使用同一灰色");
    }

    /// @brief 单张 IVM 纹理应暴露的资产键与像素尺寸。
    struct TextureExpectation {
        /// @brief SkinManager 中的资产键。
        std::string_view key;
        /// @brief 期望像素宽度。
        std::uint32_t width;
        /// @brief 期望像素高度。
        std::uint32_t height;
    };
    constexpr std::array<TextureExpectation, 8> TEXTURE_EXPECTATIONS{
        TextureExpectation{ "note.note", 256U, 128U },
        TextureExpectation{ "note.node", 24U, 24U },
        TextureExpectation{ "note.holdbodyvertical", 24U, 128U },
        TextureExpectation{ "note.holdbodyhorizontal", 256U, 24U },
        TextureExpectation{ "note.holdend", 24U, 12U },
        TextureExpectation{ "note.arrowleft", 128U, 96U },
        TextureExpectation{ "note.arrowright", 128U, 96U },
        TextureExpectation{ "panel.track.judgearea", 256U, 128U },
    };
    for ( const auto& expectation : TEXTURE_EXPECTATIONS ) {
        std::uint32_t width  = 0U;
        std::uint32_t height = 0U;
        const auto    path =
            skinManager.getAssetPath(std::string(expectation.key));
        ok &= check(readPngDimensions(path, width, height),
                    "IVM 纹理必须是有效 PNG");
        ok &= check(width == expectation.width && height == expectation.height,
                    "IVM 纹理尺寸不符合约定");
    }

    /// @brief IVM 特效序列应暴露的键、目录和帧数。
    struct EffectExpectation {
        /// @brief SkinManager 中的特效序列键。
        std::string_view key;
        /// @brief IVM 资源目录下的序列子目录。
        std::string_view directory;
        /// @brief 期望序列帧数。
        std::size_t frameCount;
    };
    constexpr std::array<EffectExpectation, 2> EFFECT_EXPECTATIONS{
        EffectExpectation{ "note.effect.note", "note", 6U },
        EffectExpectation{ "note.effect.flick", "flick", 16U },
    };
    const auto effectRoot =
        skinPath.parent_path() / "resources/image/note/effect";
    for ( const auto& expectation : EFFECT_EXPECTATIONS ) {
        const auto* sequence =
            skinManager.getEffectSequence(std::string(expectation.key));
        ok &= check(sequence != nullptr, "IVM 特效序列必须存在");
        if ( !sequence ) continue;

        ok &= check(sequence->frames.size() == expectation.frameCount,
                    "IVM 特效序列帧数不符合约定");
        const auto expectedDirectory =
            (effectRoot / expectation.directory).lexically_normal();
        for ( const auto& framePath : sequence->frames ) {
            std::uint32_t width  = 0U;
            std::uint32_t height = 0U;
            ok &= check(
                framePath.parent_path().lexically_normal() == expectedDirectory,
                "IVM 特效不得复用默认皮肤资源");
            ok &= check(readPngDimensions(framePath, width, height),
                        "IVM 特效帧必须是有效 PNG");
            ok &= check(width == 32U && height == 256U,
                        "IVM 特效帧必须使用纵向渐变纹理尺寸");
        }
    }

    const auto      fontPath = skinManager.getFontPath("ascii");
    std::error_code fontError;
    ok &= check(fontPath.filename() == "LiberationSans-Regular.ttf" &&
                    std::filesystem::is_regular_file(fontPath, fontError) &&
                    !fontError,
                "IVM 必须使用随皮肤分发的 Windows 风格字体");
    fontError.clear();
    ok &= check(std::filesystem::is_regular_file(
                    fontPath.parent_path() / "OFL-1.1.txt", fontError) &&
                    !fontError,
                "IVM 字体必须随附 SIL OFL 1.1 许可证");
    ok &= check(skinManager.getGlowPasses() == 6 &&
                    skinManager.getGlowIntensity() == 0.5f,
                "IVM 悬浮或选中物件必须启用发光");
    return ok;
}
}  // namespace

/// @brief 皮肤亮暗主题绑定与旧配置兼容回归测试入口。
/// @param argc 命令行参数数量。
/// @param argv 命令行参数；附加参数为测试输出目录和 IVM 皮肤入口。
/// @return 全部断言通过时返回 0。
int main(int argc, char* argv[])
{
    if ( argc < 3 || !argv[1] || !argv[2] ) {
        XERROR("SkinThemeBindingTest requires output and IVM skin entry paths");
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
    const std::filesystem::path legacyIvmDirectory = outputDirectory / "ivm";
    std::filesystem::create_directories(legacyIvmDirectory, filesystemError);
    if ( filesystemError ) {
        XERROR("Failed to create legacy IVM test directory: {}",
               filesystemError.message());
        return 1;
    }
    const std::filesystem::path legacyIvmSkinPath =
        legacyIvmDirectory / "skin.lua";

    bool ok = check(writeTestSkin(legacySkinPath, "'Cecilia'"),
                    "旧格式测试皮肤写入失败");
    ok &= check(writeTestSkin(pairedSkinPath,
                              "{ light = 'Cecilia', dark = 'Moonlight' }"),
                "亮暗双主题测试皮肤写入失败");
    ok &= check(
        writeTestSkin(lightOnlySkinPath, "{ light = 'ComfortableLight' }"),
        "单分支测试皮肤写入失败");
    ok &= check(writeLegacyIvmSkin(legacyIvmSkinPath),
                "旧版 IVM 测试皮肤写入失败");
    if ( !ok ) return 1;

    ok &= verifyThemeBinding(legacySkinPath, "Cecilia", "Cecilia");
    ok &= verifyThemeBinding(pairedSkinPath, "Cecilia", "Moonlight");
    ok &= verifyThemeBinding(
        lightOnlySkinPath, "ComfortableLight", "ComfortableLight");
    ok &= verifyLegacyIvmGlowMigration(legacyIvmSkinPath);
    ok &= verifyLegacyAppConfigSemantics();
    ok &= verifyIvmSkin(MMM::Config::utf8ToPath(argv[2]));
    return ok ? 0 : 1;
}
