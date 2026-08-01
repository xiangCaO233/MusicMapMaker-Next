#pragma once

#include <filesystem>
#include <string>

namespace MMM::Config
{

/// @brief 皮肤包导入结果。
struct SkinPackageImportResult {
    bool                  success{ false };    ///< 导入成功时为 true。
    std::string           skinDirectoryName;   ///< 导入后的皮肤目录名。
    std::filesystem::path installedDirectory;  ///< 导入后的完整皮肤目录。
    std::string           errorMessage;        ///< 导入失败时的错误详情。
};

/// @brief 管理 MSK 皮肤包的安全导入与导出。
class SkinPackageService final
{
public:
    /// @brief 将 MSK 包中的皮肤安装到用户皮肤根目录。
    /// @param packagePath 待导入的 MSK 文件。
    /// @param skinsRoot 用户配置目录下的 skins 根目录。
    /// @return 导入结果；包内必须且只能存在一个 skin.lua。
    [[nodiscard]] static SkinPackageImportResult importPackage(
        const std::filesystem::path& packagePath,
        const std::filesystem::path& skinsRoot);

    /// @brief 将皮肤目录打包为 MSK 文件。
    /// @param skinDirectory 包含 skin.lua 的皮肤目录。
    /// @param outputPath 输出 MSK 文件。
    /// @param errorMessage 失败时写入错误详情。
    /// @return 打包并写入成功时返回 true。
    [[nodiscard]] static bool exportPackage(
        const std::filesystem::path& skinDirectory,
        const std::filesystem::path& outputPath, std::string& errorMessage);
};

}  // namespace MMM::Config
