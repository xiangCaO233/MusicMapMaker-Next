#include "config/skin/SkinPackageService.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <miniz.h>
#include <string>
#include <utility>
#include <vector>

namespace
{

/// @brief 写入测试文本文件。
bool writeTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::error_code createError;
    std::filesystem::create_directories(path.parent_path(), createError);
    if ( createError ) return false;

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if ( !output ) return false;
    output << text;
    return output.good();
}

/// @brief 读取测试文本文件。
std::string readTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if ( !input ) return {};
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

/// @brief 创建包含指定条目的 ZIP 兼容 MSK 文件。
bool writeMskFile(
    const std::filesystem::path&                            outputPath,
    const std::vector<std::pair<std::string, std::string>>& entries)
{
    mz_zip_archive archive{};
    if ( !mz_zip_writer_init_file(
             &archive, MMM::Config::pathToUtf8(outputPath).c_str(), 0) ) {
        return false;
    }

    bool success = true;
    for ( const auto& [name, content] : entries ) {
        if ( !mz_zip_writer_add_mem(&archive,
                                    name.c_str(),
                                    content.data(),
                                    content.size(),
                                    MZ_BEST_SPEED) ) {
            success = false;
            break;
        }
    }
    if ( success && !mz_zip_writer_finalize_archive(&archive) ) {
        success = false;
    }
    mz_zip_writer_end(&archive);
    return success;
}

/// @brief 记录布尔断言结果。
int expectTrue(bool condition, const char* label)
{
    if ( condition ) {
        XINFO("[skin-package] PASS: {}", label);
        return 0;
    }
    XERROR("[skin-package] FAIL: {}", label);
    return 1;
}

/// @brief 验证皮肤目录导出、导入及同名保护。
int testExportAndImport(const std::filesystem::path& root)
{
    int        failures = 0;
    const auto source   = root / "source" / "FancySkin";
    const auto package  = root / "FancySkin.msk";
    const auto target   = root / "target";

    failures += expectTrue(
        writeTextFile(source / "skin.lua", "return { name = 'Fancy' }"),
        "write skin.lua fixture");
    failures +=
        expectTrue(writeTextFile(source / "resources" / "note.txt", "note"),
                   "write nested resource fixture");

    std::string exportError;
    failures += expectTrue(MMM::Config::SkinPackageService::exportPackage(
                               source, package, exportError),
                           "export skin package");

    const auto imported =
        MMM::Config::SkinPackageService::importPackage(package, target);
    failures += expectTrue(imported.success, "import exported skin package");
    failures += expectTrue(imported.skinDirectoryName == "FancySkin",
                           "preserve exported skin directory name");
    failures += expectTrue(readTextFile(target / "FancySkin" / "skin.lua") ==
                               "return { name = 'Fancy' }",
                           "restore skin.lua content");
    failures += expectTrue(
        readTextFile(target / "FancySkin" / "resources" / "note.txt") == "note",
        "restore nested resource content");

    const auto duplicate =
        MMM::Config::SkinPackageService::importPackage(package, target);
    failures +=
        expectTrue(!duplicate.success, "reject duplicate skin directory");
    failures += expectTrue(
        readTextFile(target / "FancySkin" / "resources" / "note.txt") == "note",
        "preserve existing skin after duplicate import");

    failures +=
        expectTrue(writeTextFile(source / "resources" / "note.txt", "updated"),
                   "update export fixture");
    failures += expectTrue(MMM::Config::SkinPackageService::exportPackage(
                               source, package, exportError),
                           "overwrite exported package");
    const auto updatedImport = MMM::Config::SkinPackageService::importPackage(
        package, root / "updated-target");
    failures +=
        expectTrue(updatedImport.success, "import overwritten skin package");
    failures += expectTrue(readTextFile(root / "updated-target" / "FancySkin" /
                                        "resources" / "note.txt") == "updated",
                           "overwrite package contains updated resource");
    return failures;
}

/// @brief 验证根级 skin.lua 使用包文件名作为安装目录。
int testRootSkinLua(const std::filesystem::path& root)
{
    const auto package  = root / "RootSkin.msk";
    const auto target   = root / "root-target";
    int        failures = 0;
    failures += expectTrue(writeMskFile(package,
                                        { { "skin.lua", "return {}" },
                                          { "resources/icon.txt", "icon" } }),
                           "write root-level skin package");

    const auto imported =
        MMM::Config::SkinPackageService::importPackage(package, target);
    failures += expectTrue(imported.success, "import root-level skin package");
    failures += expectTrue(imported.skinDirectoryName == "RootSkin",
                           "derive root skin name from package");
    failures += expectTrue(
        readTextFile(target / "RootSkin" / "resources" / "icon.txt") == "icon",
        "copy root-level skin resources");
    return failures;
}

/// @brief 验证危险路径和多入口皮肤包会被拒绝。
int testInvalidPackages(const std::filesystem::path& root)
{
    int        failures     = 0;
    const auto unsafe       = root / "Unsafe.msk";
    const auto ambiguous    = root / "Ambiguous.msk";
    const auto unsafeTarget = root / "unsafe-target";

    failures += expectTrue(writeMskFile(unsafe,
                                        { { "../evil.txt", "evil" },
                                          { "Safe/skin.lua", "return {}" } }),
                           "write unsafe package");
    const auto unsafeResult =
        MMM::Config::SkinPackageService::importPackage(unsafe, unsafeTarget);
    failures +=
        expectTrue(!unsafeResult.success, "reject path traversal package");
    failures += expectTrue(!std::filesystem::exists(root / "evil.txt"),
                           "do not write path traversal entry");

    failures += expectTrue(writeMskFile(ambiguous,
                                        { { "One/skin.lua", "return {}" },
                                          { "Two/skin.lua", "return {}" } }),
                           "write ambiguous package");
    const auto ambiguousResult = MMM::Config::SkinPackageService::importPackage(
        ambiguous, root / "ambiguous-target");
    failures += expectTrue(!ambiguousResult.success,
                           "reject multiple skin.lua entries");
    return failures;
}

}  // namespace

/// @brief 运行 MSK 皮肤包导入导出测试。
/// @param argc 参数数量。
/// @param argv 参数数组；首个参数为测试输出目录。
/// @return 所有检查通过时返回 0。
int main(int argc, char** argv)
{
    if ( argc < 2 || argv[1] == nullptr ) return 1;

    const std::filesystem::path root(argv[1]);
    std::error_code             removeError;
    std::filesystem::remove_all(root, removeError);
    std::error_code createError;
    std::filesystem::create_directories(root, createError);
    if ( createError ) return 1;

    const int failures = testExportAndImport(root) + testRootSkinLua(root) +
                         testInvalidPackages(root);
    return failures == 0 ? 0 : 1;
}
