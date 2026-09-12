#include "config/AppPaths.h"
#include "config/Utf8Path.h"

#include <filesystem>

/// @brief 验证应用路径解析与本机多客户端配置根隔离。
/// @param argc 参数数量。
/// @param argv 唯一参数为通过 MMM_CONFIG_ROOT 注入的预期基础目录。
int main(int argc, char* argv[])
{
    // CTest 必须显式传入隔离根目录，避免测试误读用户的真实配置。
    if ( argc != 2 || argv[1] == nullptr ) return 1;

    // AppPaths 会在覆盖根下追加固定应用目录名，测试构造同一预期布局。
    std::filesystem::path expected = MMM::Config::utf8ToPath(argv[1]);
    expected /= "mmm";
    // 分别读取配置根和可执行目录，覆盖环境解析与程序位置解析两条路径。
    const auto actual = MMM::Config::AppPaths::configRootPath();
    const auto executableDirectory =
        MMM::Config::AppPaths::executableDirectoryPath();
    // 使用 error_code 查询，测试失败应通过返回码表达而不是抛出异常。
    std::error_code directoryError;
    // 可执行目录必须是当前机器上的绝对现存目录，而非依赖工作目录的相对值。
    const bool validExecutableDirectory =
        executableDirectory.is_absolute() &&
        std::filesystem::is_directory(executableDirectory, directoryError) &&
        !directoryError;
    // 用户配置文件必须直接位于隔离后的应用根目录，不能回落到默认用户路径。
    // 路径比较先做词法规范化，消除输入尾部分隔符等无语义差异。
    return validExecutableDirectory &&
                   actual.lexically_normal() == expected.lexically_normal() &&
                   MMM::Config::AppPaths::userConfigFilePath().parent_path() ==
                       actual
               ? 0
               : 1;
}
