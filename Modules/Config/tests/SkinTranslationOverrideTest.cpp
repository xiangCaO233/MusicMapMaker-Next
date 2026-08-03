#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
/// @brief 记录测试断言失败。
/// @param condition 待验证条件。
/// @param message 条件失败时写入日志的说明。
/// @return 条件原值。
bool check(bool condition, std::string_view message)
{
    if ( !condition ) {
        XERROR("SkinTranslationOverrideTest failed: {}", message);
    }
    return condition;
}

/// @brief 写入测试文件并创建父目录。
/// @param path 输出文件路径。
/// @param content 文件内容。
/// @return 文件成功写入时返回 true。
bool writeFile(const std::filesystem::path& path, std::string_view content)
{
    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if ( filesystemError ) return false;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if ( !file ) return false;
    file << content;
    return file.good();
}

/// @brief 写入包含指定 lang_overrides 表达式的最小皮肤。
/// @param path 皮肤入口路径。
/// @param overridesExpression lang_overrides 对应 Lua 表达式。
/// @return 文件成功写入时返回 true。
bool writeSkin(const std::filesystem::path& path,
               std::string_view             overridesExpression)
{
    std::string content =
        "return {\n"
        "  meta = { name = 'Translation Test', author = 'Test', version = "
        "'1.0' },\n"
        "  langs = { zh_cn = 'resources/lang/zh_cn.lua' },\n"
        "  lang_overrides = ";
    content += overridesExpression;
    content +=
        ",\n"
        "  fonts = {}, assets = {}, audios = {}, layout = {}\n"
        "}\n";
    return writeFile(path, content);
}
}  // namespace

/// @brief 验证皮肤翻译覆写、旧 langs 忽略和路径边界。
/// @param argc 命令行参数数量。
/// @param argv 命令行参数；附加参数为测试输出目录。
/// @return 全部断言通过时返回 0。
int main(int argc, char* argv[])
{
    if ( argc < 2 || !argv[1] ) {
        XERROR("SkinTranslationOverrideTest requires output path");
        return 1;
    }

    const auto      outputRoot = MMM::Config::utf8ToPath(argv[1]);
    std::error_code filesystemError;
    std::filesystem::remove_all(outputRoot, filesystemError);
    filesystemError.clear();

    const auto translationsRoot = outputRoot / "translations";
    const auto skinRoot         = outputRoot / "skins/custom";
    const auto skinPath         = skinRoot / "skin.lua";
    const auto overridePath = skinRoot / "custom/translations/zh-custom.lua";

    bool ok =
        check(writeFile(translationsRoot / "en_us.lua",
                        "return { ['ui.sample'] = 'Default EN', ['ui.keep'] = "
                        "'Keep EN' }"),
              "英文默认翻译写入失败");
    ok &= check(writeFile(translationsRoot / "zh_cn.lua",
                          "return { ['ui.sample'] = '默认中文', ['ui.keep'] = "
                          "'保留中文' }"),
                "中文默认翻译写入失败");
    ok &=
        check(writeFile(overridePath,
                        "return { ['ui.sample'] = '皮肤覆写', ['ui.missing'] = "
                        "'不应插入' }"),
              "皮肤翻译覆写写入失败");
    ok &= check(
        writeSkin(skinPath, "{ zh_cn = 'custom/translations/zh-custom.lua' }"),
        "测试皮肤写入失败");
    if ( !ok ) return 1;

    auto& skinManager = MMM::Config::SkinManager::instance();
    ok &= check(skinManager.loadSkin(MMM::Config::pathToUtf8(skinPath),
                                     translationsRoot),
                "包含翻译覆写的皮肤必须成功加载");
    ok &= check(skinManager.getTranslator().switchLang("zh_cn"),
                "中文默认语言必须可切换");
    ok &=
        check(std::string(skinManager.getTranslator().translate(
                  MMM::Hash::hash_str("ui.sample"), "ui.sample")) == "皮肤覆写",
              "默认字典已有字段必须被皮肤覆写");
    ok &= check(std::string(skinManager.getTranslator().translate(
                    MMM::Hash::hash_str("ui.keep"), "ui.keep")) == "保留中文",
                "未覆写字段必须保留默认翻译");
    ok &= check(
        std::string(skinManager.getTranslator().translate(
            MMM::Hash::hash_str("ui.missing"), "ui.missing")) == "ui.missing",
        "默认字典不存在的字段不得被皮肤插入");
    ok &= check(skinManager.getData().missingTranslationOverrideFields ==
                    std::vector<std::string>{ "zh_cn:ui.missing" },
                "未知覆写字段必须保留语言 ID 供启动提示");
    const auto overrideIt =
        skinManager.getData().langOverrideLuaPaths.find("zh_cn");
    ok &=
        check(overrideIt != skinManager.getData().langOverrideLuaPaths.end() &&
                  overrideIt->second.lexically_normal() ==
                      overridePath.lexically_normal(),
              "覆写文件应允许位于皮肤包内任意子路径");

    const auto escapedOverridePath = outputRoot / "skins/escaped.lua";
    const auto escapedSkinPath     = skinRoot / "escaped-skin.lua";
    ok &= check(
        writeFile(escapedOverridePath, "return { ['ui.sample'] = '越界覆写' }"),
        "越界覆写测试文件写入失败");
    ok &= check(writeSkin(escapedSkinPath, "{ zh_cn = '../escaped.lua' }"),
                "越界覆写测试皮肤写入失败");
    ok &= check(skinManager.loadSkin(MMM::Config::pathToUtf8(escapedSkinPath),
                                     translationsRoot),
                "越界覆写应被忽略而不阻断皮肤加载");
    ok &= check(skinManager.getData().langOverrideLuaPaths.empty(),
                "皮肤包外的覆写路径必须被拒绝");
    ok &= check(skinManager.getTranslator().switchLang("zh_cn"),
                "拒绝越界覆写后中文默认语言仍应可切换");
    ok &=
        check(std::string(skinManager.getTranslator().translate(
                  MMM::Hash::hash_str("ui.sample"), "ui.sample")) == "默认中文",
              "越界覆写不得修改默认字典");
    return ok ? 0 : 1;
}
