#include "StartupCommandLinePath.h"

namespace MMM
{

/// @brief 按 POSIX 文件系统的原生字节解释首个用户参数。
/// @param argc 包含程序名的参数数量。
/// @param argv 进程参数数组，参数内容由调用者保持有效。
/// @return 首个用户参数的路径，缺少用户参数时返回空路径。
std::filesystem::path readStartupCommandLinePath(int argc, char** argv)
{
    // 直接保留 argv 的原生字节，避免依赖进程 locale 重新编码路径。
    if ( argc < 2 ) return {};
    return std::filesystem::path(argv[1]);
}

}  // namespace MMM
