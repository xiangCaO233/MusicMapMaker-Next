#include "game/GlobDefs.h"
#include "config/ExecutablePath.h"
#include "log/colorful-log.h"
#include "platform/StartupLocale.h"

#include <filesystem>
#include <system_error>

#ifdef _WIN32
// 必须在 CrashHandler 间接引入 SDK 前启用精简声明。
// NOMINMAX 还需要保护此文件所用的标准库函数名。
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
// 崩溃处理及控制台配置在此处共用相同的 SDK 宏设置。
#    include "game/CrashHandler.h"
#    include <windows.h>
#endif

namespace MMM
{

/// @brief 配置启动环境，并在日志初始化完成后记录字符设置或工作目录失败。
RTTILogger::RTTILogger()
{
#ifdef _WIN32
    // Windows 控制台统一输出 UTF-8，避免中文日志使用系统代码页乱码。
    SetConsoleOutputCP(CP_UTF8);

    // 为标准输出和错误输出启用 ANSI 虚拟终端颜色支持。
    /// @brief 尝试给指定标准句柄启用虚拟终端处理位。
    /// @param stdHandle STD_OUTPUT_HANDLE 或 STD_ERROR_HANDLE。
    auto enableVT = [](DWORD stdHandle) {
        // 无效或不可查询的句柄保持原状态，应用仍可继续启动。
        HANDLE hOut = GetStdHandle(stdHandle);
        if ( hOut == INVALID_HANDLE_VALUE ) return;
        DWORD dwMode = 0;
        // 重定向到文件或管道时无法查询控制台模式，直接保持句柄原状。
        if ( !GetConsoleMode(hOut, &dwMode) ) return;
        // 保留进程原有模式位，只补上 ANSI 转义序列所需的解析模式。
        SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    };
    // 两个标准流可能分别重定向，必须各自检查句柄与模式。
    enableVT(STD_OUTPUT_HANDLE);
    enableVT(STD_ERROR_HANDLE);

    // 崩溃处理器必须在后续业务初始化前注册，以覆盖早期故障。
    register_crash_handler();
#endif
    // 只在启动线程切换字符类别，在日志系统可用之前记录设置结果。
    // 使用平台候选值，避免宿主环境变量将字符转换改回非 UTF-8。
    // 文件路径继续用平台原生表示，数字读写维持自己的解析规则。
    // 字符设置失败与路径失败分别诊断，便于区分运行库和文件系统问题。
    const bool characterLocaleReady =
        Platform::initializeStartupCharacterLocale();

    // 资源定位和启动目录使用同一个查询，分别处理查询和目录切换的失败。
    const auto      executablePath      = Config::currentExecutablePath();
    const auto      executableDirectory = executablePath.parent_path();
    std::error_code currentPathError;
    if ( executableDirectory.is_absolute() ) {
        // 必须在主循环和后台任务启动前切换，避免改变运行中的相对路径解析。
        // 不支持该目录的系统会留下错误码，并保留启动者提供的工作目录。
        std::filesystem::current_path(executableDirectory, currentPathError);
    }

    // 路径切换必须先于日志初始化，使相对日志路径仍以启动工作目录为基准。
    // 若切换失败，日志后端也继续使用调用者原有的工作目录。
    XLogger::init("MMM");
    if ( !characterLocaleReady ) {
        // 裁剪过 locale 数据的设备仍可启动，字符转换沿用原进程设置。
        XWARN(
            "UTF-8 LC_CTYPE is unavailable; keeping the existing character "
            "locale");
    }
    if ( !executableDirectory.is_absolute() ) {
        // 空路径代表平台查询失败，路径服务自身不依赖已经初始化的日志。
        XWARN(
            "Failed to set working directory: failed to query executable path");
    } else if ( currentPathError ) {
        // 目录权限或长度受系统限制时保留原始诊断，仍允许主程序启动。
        XWARN("Failed to set working directory: {}",
              currentPathError.message());
    }
}

/// @brief 在进程退出时收尾，保证日志缓冲在静态对象生命周期结束前写出。
RTTILogger::~RTTILogger()
{
    XLogger::shutdown();
}

}  // namespace MMM
