#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace MMM::Config
{

/// @brief 旧版皮肤翻译资源清理结果。
struct TranslationResourceMigrationResult {
    /// @brief 迁移已完成或此前已完成。
    bool completed{ false };

    /// @brief 本次迁移无条件删除的旧版默认语言文件。
    std::vector<std::filesystem::path> removedFiles;

    /// @brief 迁移未完成时的诊断信息。
    std::string errorMessage;
};

/// @brief 清理用户皮肤中旧版固定路径下的默认语言文件。
/// @return 迁移完成状态、删除列表及错误信息。
/// @warning 启动期一次性文件系统迁移；会无条件删除每个皮肤下的
/// resources/lang/en_us.lua 与 resources/lang/zh_cn.lua。
TranslationResourceMigrationResult migrateLegacySkinTranslationFiles();

/// @brief 在指定测试或受控目录中清理旧版皮肤默认语言文件。
/// @param skinsRoot 用户皮肤根目录。
/// @param translationsRoot 新版默认翻译资源根目录。
/// @param markerPath 一次性迁移完成标记路径。
/// @return 迁移完成状态、删除列表及错误信息。
/// @warning 仅应在默认翻译资源完整后调用；完成标记存在时不会重复删除。
TranslationResourceMigrationResult migrateLegacySkinTranslationFiles(
    const std::filesystem::path& skinsRoot,
    const std::filesystem::path& translationsRoot,
    const std::filesystem::path& markerPath);

}  // namespace MMM::Config
