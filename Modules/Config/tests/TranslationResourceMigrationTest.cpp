#include "config/skin/TranslationResourceMigration.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <filesystem>
#include <fstream>
#include <string_view>

namespace
{
/// @brief 记录测试断言失败。
/// @param condition 待验证条件。
/// @param message 条件失败时写入日志的说明。
/// @return 条件原值。
bool check(bool condition, std::string_view message)
{
    if ( !condition ) {
        XERROR("TranslationResourceMigrationTest failed: {}", message);
    }
    return condition;
}

/// @brief 写入测试占位文件并创建父目录。
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
}  // namespace

/// @brief 验证旧版皮肤语言文件的一次性无条件清理。
/// @param argc 命令行参数数量。
/// @param argv 命令行参数；附加参数为测试输出目录。
/// @return 全部断言通过时返回 0。
int main(int argc, char* argv[])
{
    if ( argc < 2 || !argv[1] ) {
        XERROR("TranslationResourceMigrationTest requires output path");
        return 1;
    }

    const auto      outputRoot = MMM::Config::utf8ToPath(argv[1]);
    std::error_code filesystemError;
    std::filesystem::remove_all(outputRoot, filesystemError);
    filesystemError.clear();

    const auto skinsRoot        = outputRoot / "assets/skins";
    const auto translationsRoot = outputRoot / "assets/translations";
    const auto markerPath       = outputRoot / "assets/.translation-layout-v2";
    const auto firstEnglish     = skinsRoot / "first/resources/lang/en_us.lua";
    const auto firstChinese     = skinsRoot / "first/resources/lang/zh_cn.lua";
    const auto secondChinese    = skinsRoot / "second/resources/lang/zh_cn.lua";
    const auto unrelatedFile    = skinsRoot / "first/resources/lang/custom.lua";

    const auto incompleteRoot = outputRoot / "incomplete";
    const auto incompleteLegacy =
        incompleteRoot / "skins/old/resources/lang/zh_cn.lua";
    bool ok =
        check(writeFile(incompleteRoot / "translations/en_us.lua", "return {}"),
              "不完整默认翻译测试文件写入失败");
    ok &= check(writeFile(incompleteLegacy, "return { keep = true }"),
                "不完整资源迁移的旧文件写入失败");
    const auto incompleteResult =
        MMM::Config::migrateLegacySkinTranslationFiles(
            incompleteRoot / "skins",
            incompleteRoot / "translations",
            incompleteRoot / ".migration-marker");
    ok &= check(!incompleteResult.completed &&
                    std::filesystem::is_regular_file(incompleteLegacy),
                "新版默认翻译不完整时不得提前删除旧文件");

    ok &= check(writeFile(translationsRoot / "en_us.lua", "return {}"),
                "英文默认翻译测试文件写入失败");
    ok &= check(writeFile(translationsRoot / "zh_cn.lua", "return {}"),
                "中文默认翻译测试文件写入失败");
    ok &= check(writeFile(firstEnglish, "return { modified = true }"),
                "第一份旧英文翻译写入失败");
    ok &= check(writeFile(firstChinese, "return { modified = true }"),
                "第一份旧中文翻译写入失败");
    ok &= check(writeFile(secondChinese, "return { modified = true }"),
                "第二份旧中文翻译写入失败");
    ok &= check(writeFile(unrelatedFile, "return { keep = true }"),
                "无关翻译文件写入失败");
    if ( !ok ) return 1;

    const auto firstResult = MMM::Config::migrateLegacySkinTranslationFiles(
        skinsRoot, translationsRoot, markerPath);
    ok &= check(firstResult.completed, "首次迁移必须成功");
    ok &= check(firstResult.removedFiles.size() == 3,
                "首次迁移必须删除所有已存在的固定路径语言文件");
    ok &= check(!std::filesystem::exists(firstEnglish) &&
                    !std::filesystem::exists(firstChinese) &&
                    !std::filesystem::exists(secondChinese),
                "用户修改过的旧版默认语言文件也必须无条件删除");
    ok &= check(std::filesystem::is_regular_file(unrelatedFile),
                "迁移不得删除其他语言文件");
    ok &= check(std::filesystem::is_regular_file(markerPath),
                "首次迁移必须写入一次性完成标记");

    ok &= check(writeFile(firstChinese, "return { new_override = true }"),
                "迁移后同路径覆写文件写入失败");
    const auto secondResult = MMM::Config::migrateLegacySkinTranslationFiles(
        skinsRoot, translationsRoot, markerPath);
    ok &= check(secondResult.completed && secondResult.removedFiles.empty(),
                "完成标记存在时不得重复执行删除");
    ok &= check(std::filesystem::is_regular_file(firstChinese),
                "一次性迁移不得误删后来创建的覆写文件");
    return ok ? 0 : 1;
}
