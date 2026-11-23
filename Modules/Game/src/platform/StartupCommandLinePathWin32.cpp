#include "StartupCommandLinePath.h"

#include <windows.h>

// Shell32 的参数声明依赖 Windows 基础类型，放在 windows.h 之后。
#include <shellapi.h>

#include <memory>

namespace MMM
{

/// @brief 从 Win32 的 UTF-16 命令行取得路径，保留中文和未经裁剪的长路径。
/// @return 缺少路径参数或系统解析失败时返回空路径，启动流程可以继续。
std::filesystem::path readStartupCommandLinePath(int /*argc*/, char** /*argv*/)
{
    // 直接读取宽字符进程命令行，避免窄字符 CRT 参数受当前代码页影响。
    int argumentCount = 0;
    // 参数和字符串共享一块系统分配；所有返回路径由同一个 LocalFree 回收。
    const std::unique_ptr<LPWSTR, decltype(&LocalFree)> arguments(
        CommandLineToArgvW(GetCommandLineW(), &argumentCount), &LocalFree);
    // API 失败时没有参数块，即使输出计数异常也不能访问参数。
    if ( !arguments || argumentCount < 2 ) return {};
    // path 拷贝原生字符，参数块释放后调用方仍可使用。
    return std::filesystem::path(arguments.get()[1]);
}

}  // namespace MMM
