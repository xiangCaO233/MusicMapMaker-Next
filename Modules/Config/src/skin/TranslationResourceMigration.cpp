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
/// 标记位于 assets 根目录，不随单个皮肤删除或更新。
constexpr std::string_view MIGRATION_MARKER_FILE_NAME =
    ".translation-layout-v2-migrated";

/// @brief 迁移前必须存在的新版默认语言文件名。
/// 两份基线同时存在才允许移除皮肤中旧的内嵌副本。
constexpr std::array<std::string_view, 2> DEFAULT_TRANSLATION_FILE_NAMES{
    "en_us.lua", "zh_cn.lua"
};

/// @brief 旧版皮肤中需要无条件删除的默认语言相对路径。
/// 固定相对路径限制删除范围，其他语言与自定义文件一律保留。
constexpr std::array<std::string_view, 2> LEGACY_TRANSLATION_RELATIVE_PATHS{
    "resources/lang/en_us.lua", "resources/lang/zh_cn.lua"
};

/// @brief 生成迁移失败结果。
/// @param message 失败原因。
/// @return 未完成的迁移结果。
TranslationResourceMigrationResult migrationError(std::string message)
{
    // completed 默认保持 false，仅移动错误文本以避免复制长路径诊断。
    TranslationResourceMigrationResult result;
    result.errorMessage = std::move(message);
    return result;
}
}  // namespace

/// @brief 使用应用标准资源目录执行旧版皮肤翻译布局迁移。
/// @return 完成状态、已删除文件和失败信息组成的迁移结果。
/// @warning 启动低频文件系统路径：会遍历皮肤并删除两个固定旧语言文件。
TranslationResourceMigrationResult migrateLegacySkinTranslationFiles()
{
    // 完成标记放在 assets 根，独立于任何单个皮肤的生命周期。
    auto markerPath = AppPaths::assetsRootPath();
    // UTF-8 转换保持非 ASCII 配置根在各平台上的原生路径表示。
    markerPath /= utf8ToPath(std::string(MIGRATION_MARKER_FILE_NAME));
    return migrateLegacySkinTranslationFiles(AppPaths::skinsRootPath(),
                                             AppPaths::translationsRootPath(),
                                             markerPath);
}

/// @brief 在显式目录中一次性删除旧默认翻译副本并写入完成标记。
/// @param skinsRoot 用户皮肤根目录。
/// @param translationsRoot 新版共享默认翻译目录。
/// @param markerPath 成功迁移后创建的一次性标记文件。
/// @return 完成状态、实际删除路径和失败说明。
/// @warning 低频迁移路径：只允许删除常量表列出的皮肤内相对路径。
TranslationResourceMigrationResult migrateLegacySkinTranslationFiles(
    const std::filesystem::path& skinsRoot,
    const std::filesystem::path& translationsRoot,
    const std::filesystem::path& markerPath)
{
    namespace fs = std::filesystem;
    // 全流程复用 error_code，所有文件系统失败都转换为可展示结果。
    std::error_code filesystemError;
    // 标记存在即视为此前完整成功，后来创建的同路径覆写不再删除。
    if ( fs::exists(markerPath, filesystemError) && !filesystemError ) {
        // 快速路径不重新遍历目录，removedFiles 自然保持为空。
        TranslationResourceMigrationResult result;
        result.completed = true;
        return result;
    }
    // 无法查询标记时不能安全判断是否已迁移，立即停止所有删除操作。
    if ( filesystemError ) {
        return migrationError("无法读取翻译资源迁移标记：" +
                              filesystemError.message());
    }

    // 删除旧副本前先验证新版中英文基线齐全，避免用户失去唯一翻译资源。
    for ( const auto fileName : DEFAULT_TRANSLATION_FILE_NAMES ) {
        const auto defaultTranslation =
            translationsRoot / utf8ToPath(std::string(fileName));
        // 每轮 is_regular_file 都会覆盖错误码，错误和非普通文件统一失败。
        if ( !fs::is_regular_file(defaultTranslation, filesystemError) ||
             filesystemError ) {
            // 诊断包含缺失的具体默认文件，便于资源同步流程提示恢复位置。
            return migrationError("新版默认翻译资源尚未完整：" +
                                  pathToUtf8(defaultTranslation));
        }
    }

    // 只有前置资源验证全部通过后才创建结果并开始遍历用户皮肤。
    // removedFiles 默认空，随后只追加 remove 明确返回 true 的路径。
    TranslationResourceMigrationResult result;
    // directory_iterator 构造失败通常代表根目录缺失或权限不足，不能写标记。
    fs::directory_iterator skinDirectoryIt(skinsRoot, filesystemError);
    if ( filesystemError ) {
        // 根目录无法打开时不创建完成标记，否则会永久跳过尚未处理的皮肤。
        return migrationError("无法遍历用户皮肤目录：" +
                              filesystemError.message());
    }

    const fs::directory_iterator end;
    // 使用增量 error_code 迭代，任一目录读取失败都会中止并保留未完成状态。
    for ( ; skinDirectoryIt != end;
          skinDirectoryIt.increment(filesystemError) ) {
        if ( filesystemError ) {
            // 迭代中断可能只处理了部分皮肤，保持未完成以便下次继续重试。
            return migrationError("遍历用户皮肤目录失败：" +
                                  filesystemError.message());
        }
        // 跳过符号链接防止迁移越出 skins 根；普通文件同样不是皮肤目录。
        if ( skinDirectoryIt->is_symlink(filesystemError) || filesystemError ||
             !skinDirectoryIt->is_directory(filesystemError) ) {
            // 清除条目级错误后继续下一项，单个特殊条目不阻止其他皮肤迁移。
            filesystemError.clear();
            continue;
        }

        // 每个真实皮肤只检查固定的 en_us 与 zh_cn 旧相对位置。
        for ( const auto relativePath : LEGACY_TRANSLATION_RELATIVE_PATHS ) {
            const fs::path legacyFile =
                skinDirectoryIt->path() / utf8ToPath(std::string(relativePath));
            // remove 对不存在路径返回 false，仍属于无需记录的成功状态。
            const bool removed = fs::remove(legacyFile, filesystemError);
            if ( filesystemError ) {
                // 删除失败时不写完成标记，使下次启动仍可重试未完成迁移。
                return migrationError("无法删除旧版皮肤翻译文件：" +
                                      pathToUtf8(legacyFile) + "：" +
                                      filesystemError.message());
            }
            // 结果只收集确实删除的路径，供启动日志报告实际迁移数量。
            if ( removed ) result.removedFiles.push_back(legacyFile);
        }
    }

    // 所有皮肤处理成功后再创建标记父目录，标记代表完整事务已结束。
    fs::create_directories(markerPath.parent_path(), filesystemError);
    if ( filesystemError ) {
        // 标记父目录失败发生在删除之后，返回错误促使后续启动再次核对剩余项。
        return migrationError("无法创建翻译资源迁移标记目录：" +
                              filesystemError.message());
    }
    // trunc 确保异常残留标记被标准版本内容完整覆盖。
    std::ofstream markerFile(markerPath, std::ios::binary | std::ios::trunc);
    if ( !markerFile ) {
        // 无法创建标记时不能宣告完成，即使当前旧文件已经全部移除。
        return migrationError("无法写入翻译资源迁移标记：" +
                              pathToUtf8(markerPath));
    }
    // 版本文本供人工诊断，存在性仍是当前迁移流程的快速判断依据。
    markerFile << "translation-layout-v2\n";
    if ( !markerFile.good() ) {
        // 写入失败可能留下空文件；结果仍标为失败供启动界面提示。
        return migrationError("翻译资源迁移标记写入失败：" +
                              pathToUtf8(markerPath));
    }

    // 仅在删除和标记落盘全部成功后发布 completed=true。
    result.completed = true;
    return result;
}

}  // namespace MMM::Config
