#include "config/AppPaths.h"
#include "config/Utf8Path.h"

#include <filesystem>

/// @brief 验证应用路径解析与本机多客户端配置根隔离。
/// @param argc 参数数量。
/// @param argv 唯一参数为通过 MMM_CONFIG_ROOT 注入的预期基础目录。
int main(int argc, char* argv[])
{
    if ( argc != 2 || argv[1] == nullptr ) return 1;

    std::filesystem::path expected = MMM::Config::utf8ToPath(argv[1]);
    expected /= "mmm";
    const auto actual = MMM::Config::AppPaths::configRootPath();
    const auto executableDirectory =
        MMM::Config::AppPaths::executableDirectoryPath();
    std::error_code directoryError;
    const bool      validExecutableDirectory =
        executableDirectory.is_absolute() &&
        std::filesystem::is_directory(executableDirectory, directoryError) &&
        !directoryError;
    return validExecutableDirectory &&
                   actual.lexically_normal() == expected.lexically_normal() &&
                   MMM::Config::AppPaths::userConfigFilePath().parent_path() ==
                       actual
               ? 0
               : 1;
}
