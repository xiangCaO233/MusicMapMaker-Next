#include "config/skin/TranslationResourceMigration.h"

#include "config/AppPaths.h"
#include "config/Utf8Path.h"

#include <array>
#include <fstream>
#include <string_view>
#include <utility>

namespace MMM::Config
{
namespace
{
/// @brief 新版默认翻译布局迁移完成标记文件名。
constexpr std::string_view MIGRATION_MARKER_FILE_NAME =
    ".translation-layout-v2-migrated";

/// @brief 迁移前必须存在的新版默认语言文件名。
constexpr std::array<std::string_view, 2> DEFAULT_TRANSLATION_FILE_NAMES{
    "en_us.lua", "zh_cn.lua"
};

/// @brief 旧版皮肤中需要无条件删除的默认语言相对路径。
constexpr std::array<std::string_view, 2> LEGACY_TRANSLATION_RELATIVE_PATHS{
    "resources/lang/en_us.lua", "resources/lang/zh_cn.lua"
};

/// @brief 生成迁移失败结果。
/// @param message 失败原因。
/// @return 未完成的迁移结果。
TranslationResourceMigrationResult migrationError(std::string message)
{
    TranslationResourceMigrationResult result;
    result.errorMessage = std::move(message);
    return result;
}
}  // namespace

TranslationResourceMigrationResult migrateLegacySkinTranslationFiles()
{
    auto markerPath = AppPaths::assetsRootPath();
    markerPath /= utf8ToPath(std::string(MIGRATION_MARKER_FILE_NAME));
    return migrateLegacySkinTranslationFiles(AppPaths::skinsRootPath(),
                                             AppPaths::translationsRootPath(),
                                             markerPath);
}

TranslationResourceMigrationResult migrateLegacySkinTranslationFiles(
    const std::filesystem::path& skinsRoot,
    const std::filesystem::path& translationsRoot,
    const std::filesystem::path& markerPath)
{
    namespace fs = std::filesystem;
    std::error_code filesystemError;
    if ( fs::exists(markerPath, filesystemError) && !filesystemError ) {
        TranslationResourceMigrationResult result;
        result.completed = true;
        return result;
    }
    if ( filesystemError ) {
        return migrationError("无法读取翻译资源迁移标记：" +
                              filesystemError.message());
    }

    for ( const auto fileName : DEFAULT_TRANSLATION_FILE_NAMES ) {
        const auto defaultTranslation =
            translationsRoot / utf8ToPath(std::string(fileName));
        if ( !fs::is_regular_file(defaultTranslation, filesystemError) ||
             filesystemError ) {
            return migrationError("新版默认翻译资源尚未完整：" +
                                  pathToUtf8(defaultTranslation));
        }
    }

    TranslationResourceMigrationResult result;
    fs::directory_iterator skinDirectoryIt(skinsRoot, filesystemError);
    if ( filesystemError ) {
        return migrationError("无法遍历用户皮肤目录：" +
                              filesystemError.message());
    }

    const fs::directory_iterator end;
    for ( ; skinDirectoryIt != end;
          skinDirectoryIt.increment(filesystemError) ) {
        if ( filesystemError ) {
            return migrationError("遍历用户皮肤目录失败：" +
                                  filesystemError.message());
        }
        if ( skinDirectoryIt->is_symlink(filesystemError) || filesystemError ||
             !skinDirectoryIt->is_directory(filesystemError) ) {
            filesystemError.clear();
            continue;
        }

        for ( const auto relativePath : LEGACY_TRANSLATION_RELATIVE_PATHS ) {
            const fs::path legacyFile =
                skinDirectoryIt->path() / utf8ToPath(std::string(relativePath));
            const bool removed = fs::remove(legacyFile, filesystemError);
            if ( filesystemError ) {
                return migrationError("无法删除旧版皮肤翻译文件：" +
                                      pathToUtf8(legacyFile) + "：" +
                                      filesystemError.message());
            }
            if ( removed ) result.removedFiles.push_back(legacyFile);
        }
    }

    fs::create_directories(markerPath.parent_path(), filesystemError);
    if ( filesystemError ) {
        return migrationError("无法创建翻译资源迁移标记目录：" +
                              filesystemError.message());
    }
    std::ofstream markerFile(markerPath, std::ios::binary | std::ios::trunc);
    if ( !markerFile ) {
        return migrationError("无法写入翻译资源迁移标记：" +
                              pathToUtf8(markerPath));
    }
    markerFile << "translation-layout-v2\n";
    if ( !markerFile.good() ) {
        return migrationError("翻译资源迁移标记写入失败：" +
                              pathToUtf8(markerPath));
    }

    result.completed = true;
    return result;
}

}  // namespace MMM::Config
