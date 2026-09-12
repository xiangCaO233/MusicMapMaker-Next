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
    // 每个夹具自行创建父目录，测试主体只关注皮肤目录结构语义。
    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if ( filesystemError ) return false;
    // 截断模式确保重复执行不会残留上一次更长的 Lua 内容。
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
    // 只拼接被测字段，其余必需字段保持最小且固定，便于定位加载失败。
    std::string content =
        "return {\n"
        "  meta = { name = 'Translation Test', author = 'Test', version = "
        "'1.0' },\n"
        "  langs = { zh_cn = 'resources/lang/zh_cn.lua' },\n"
        "  lang_overrides = ";
    // 表达式由各用例提供，可覆盖合法 table 与越界路径两种场景。
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
    // 输出根由 CTest 指定，禁止测试回退到用户配置或源码资源目录。
    if ( argc < 2 || !argv[1] ) {
        XERROR("SkinTranslationOverrideTest requires output path");
        return 1;
    }

    const auto outputRoot = MMM::Config::utf8ToPath(argv[1]);
    // 清理仅限专用 test_output 子目录，保证重复执行使用全新夹具。
    std::error_code filesystemError;
    std::filesystem::remove_all(outputRoot, filesystemError);
    filesystemError.clear();

    // 默认翻译模拟应用资源，皮肤及覆写文件则位于独立包根内。
    const auto translationsRoot = outputRoot / "translations";
    const auto skinRoot         = outputRoot / "skins/custom";
    const auto skinPath         = skinRoot / "skin.lua";
    const auto overridePath = skinRoot / "custom/translations/zh-custom.lua";

    // 两种默认语言共享相同字段集合，用于验证皮肤只覆写指定语言。
    bool ok =
        check(writeFile(translationsRoot / "en_us.lua",
                        "return { ['ui.sample'] = 'Default EN', ['ui.keep'] = "
                        "'Keep EN' }"),
              "英文默认翻译写入失败");
    ok &= check(writeFile(translationsRoot / "zh_cn.lua",
                          "return { ['ui.sample'] = '默认中文', ['ui.keep'] = "
                          "'保留中文' }"),
                "中文默认翻译写入失败");
    // 默认文件只声明公共字段，未知字段仅由皮肤夹具引入。
    // 覆写同时包含合法字段和未知字段，覆盖合并与诊断两条路径。
    ok &=
        check(writeFile(overridePath,
                        "return { ['ui.sample'] = '皮肤覆写', ['ui.missing'] = "
                        "'不应插入' }"),
              "皮肤翻译覆写写入失败");
    // 覆写路径故意不采用约定目录名，验证实现按配置路径解析。
    ok &= check(
        writeSkin(skinPath, "{ zh_cn = 'custom/translations/zh-custom.lua' }"),
        "测试皮肤写入失败");
    // 任一夹具失败都停止后续加载，避免把准备错误误报为业务断言失败。
    if ( !ok ) return 1;

    // 单例在测试进程内首次加载该皮肤，不继承真实应用的皮肤状态。
    auto& skinManager = MMM::Config::SkinManager::instance();
    ok &= check(skinManager.loadSkin(MMM::Config::pathToUtf8(skinPath),
                                     translationsRoot),
                "包含翻译覆写的皮肤必须成功加载");
    // loadSkin 同时装载默认翻译和皮肤覆写，是本测试需要覆盖的集成入口。
    // 显式切换中文后，后续断言都针对被覆写的活动字典。
    ok &= check(skinManager.getTranslator().switchLang("zh_cn"),
                "中文默认语言必须可切换");
    // 仅在切换成功后读取统计，确保快照描述中文活动字典。
    // 切换语言会完整预热指针缓存，并保留所有译文的稳定地址。
    const auto cacheStats = skinManager.getTranslator().getCacheStats();
    ok &= check(cacheStats.activeDictionaryEntryCount ==
                    cacheStats.pointerCacheEntryCount,
                "语言切换后必须预热全部当前字典缓存");
    // 条目数相等比检查单键更能发现缓存重建遗漏。
    // 已存在字段接受皮肤值，确认覆写优先级高于默认翻译。
    // 当前活动字典条目都应在语言切换时进入热缓存，避免首帧逐项写锁。
    ok &= check(
        cacheStats.stableStringCount >= cacheStats.activeDictionaryEntryCount,
        "稳定字符串池必须覆盖当前语言字典");
    // 稳定池可能同时包含其他语言和旧覆写文本，因此这里只要求下界。
    ok &= check(
        skinManager.getTranslator()
                .translate(MMM::Hash::hashString("ui.sample"), "ui.sample")
                .view() == "皮肤覆写",
        "默认字典已有字段必须被皮肤覆写");
    // 哈希使用与 TR 宏相同的生产入口，避免测试绕过实际键解析规则。
    // 使用精确 UTF-8 文本比较，覆盖 Lua 读取到稳定池再返回视图的完整链路。
    // 未出现在覆写文件中的字段必须继续来自默认字典。
    ok &= check(skinManager.getTranslator()
                        .translate(MMM::Hash::hashString("ui.keep"), "ui.keep")
                        .view() == "保留中文",
                "未覆写字段必须保留默认翻译");
    // 保留字段也验证覆写采用逐字段合并，而不是替换整个语言字典。
    // 未知字段不得扩展公共翻译接口，查找只能回退到键名。
    ok &= check(
        skinManager.getTranslator()
                .translate(MMM::Hash::hashString("ui.missing"), "ui.missing")
                .view() == "ui.missing",
        "默认字典不存在的字段不得被皮肤插入");
    // 回退文本与键名相同，可直接识别未知字段是否错误进入了字典。
    // 诊断保留语言前缀，多个语言同时出错时仍可给出明确提示。
    ok &= check(skinManager.getData().missingTranslationOverrideFields ==
                    std::vector<std::string>{ "zh_cn:ui.missing" },
                "未知覆写字段必须保留语言 ID 供启动提示");
    // 未知字段报告必须去重并保持确定顺序，本夹具验证单字段基础形式。
    // SkinData 保存规范化后的真实覆写路径，供诊断和后续重载复用。
    const auto overrideIt =
        skinManager.getData().langOverrideLuaPaths.find("zh_cn");
    ok &=
        check(overrideIt != skinManager.getData().langOverrideLuaPaths.end() &&
                  overrideIt->second.lexically_normal() ==
                      overridePath.lexically_normal(),
              "覆写文件应允许位于皮肤包内任意子路径");
    // 比较前做词法规范化，避免等价的分隔符或点段导致平台差异。

    // 第二个皮肤通过 .. 指向包目录之外，专门验证路径穿越防护。
    const auto escapedOverridePath = outputRoot / "skins/escaped.lua";
    const auto escapedSkinPath     = skinRoot / "escaped-skin.lua";
    // 文件实际存在，确保拒绝原因来自边界校验而非简单的文件缺失。
    ok &= check(
        writeFile(escapedOverridePath, "return { ['ui.sample'] = '越界覆写' }"),
        "越界覆写测试文件写入失败");
    ok &= check(writeSkin(escapedSkinPath, "{ zh_cn = '../escaped.lua' }"),
                "越界覆写测试皮肤写入失败");
    // escaped-skin.lua 仍位于合法皮肤根，唯一非法因素是其覆写目标。
    // 相对路径从皮肤入口目录解析，规范化后必须仍位于同一皮肤根内。
    // 非关键覆写错误不应阻断基础皮肤加载，只忽略危险配置。
    ok &= check(skinManager.loadSkin(MMM::Config::pathToUtf8(escapedSkinPath),
                                     translationsRoot),
                "越界覆写应被忽略而不阻断皮肤加载");
    // 拒绝的路径不能残留到 SkinData，避免其他调用方绕过校验使用它。
    ok &= check(skinManager.getData().langOverrideLuaPaths.empty(),
                "皮肤包外的覆写路径必须被拒绝");
    // 空映射也证明失败路径没有保存未经验证的原始相对字符串。
    ok &= check(skinManager.getTranslator().switchLang("zh_cn"),
                "拒绝越界覆写后中文默认语言仍应可切换");
    // 成功切换证明危险覆写只影响可选扩展，不破坏默认语言加载。
    // 第二次 loadSkin 会清空旧字典和缓存，重新装载默认翻译再尝试覆写。
    // 重新切换后应观察全新的默认字典，证明上一个皮肤覆写未泄漏。
    ok &= check(
        skinManager.getTranslator()
                .translate(MMM::Hash::hashString("ui.sample"), "ui.sample")
                .view() == "默认中文",
        "越界覆写不得修改默认字典");
    // 返回码只汇总断言结果，详细失败原因已经由 check 写入项目日志。
    return ok ? 0 : 1;
}
