#pragma once

#ifdef _WIN32
#    include <ctime>
#    include <string>
// MinGW 的 dbghelp.h 依赖 windows.h 类型定义，并且 SDK 文件名区分大小写。
// clang-format off
#    include <windows.h>
#    include <dbghelp.h>
// clang-format on

namespace MMM
{

/// @brief 捕获 Windows 未处理异常并写出最小转储文件。
/// @param pExceptionPointers 操作系统提供的异常上下文。
/// @return EXCEPTION_EXECUTE_HANDLER，通知系统终止当前异常流程。
///
/// 转储文件写入当前工作目录，名称带本地时间戳。任何写文件失败都不得在异常
/// 路径继续抛出；提示框仍显示预期路径，便于用户定位或报告权限问题。
inline LONG WINAPI
mmm_unhandled_exception_filter(EXCEPTION_POINTERS* pExceptionPointers)
{
    // 时间戳减少连续崩溃覆盖同一转储文件的概率。
    std::time_t t = std::time(nullptr);
    char        timeStr[64];
    std::strftime(
        timeStr, sizeof(timeStr), "%Y%m%d_%H%M%S", std::localtime(&t));

    // 文件名只使用数字和下划线，适用于 Windows 常见文件系统。
    std::string dumpPath = "crash_" + std::string(timeStr) + ".dmp";

    // CreateFileA 创建或覆盖本次时间戳对应文件，不共享写句柄。
    HANDLE hDumpFile = CreateFileA(dumpPath.c_str(),
                                   GENERIC_WRITE,
                                   0,
                                   NULL,
                                   CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL,
                                   NULL);

    if ( hDumpFile != INVALID_HANDLE_VALUE ) {
        // MiniDumpWriteDump 需要当前线程 ID 和异常指针描述故障现场。
        MINIDUMP_EXCEPTION_INFORMATION dumpInfo;
        dumpInfo.ExceptionPointers = pExceptionPointers;
        dumpInfo.ThreadId          = GetCurrentThreadId();
        dumpInfo.ClientPointers    = TRUE;

        // MiniDumpNormal 控制文件大小，避免异常路径执行高成本完整内存转储。
        MiniDumpWriteDump(GetCurrentProcess(),
                          GetCurrentProcessId(),
                          hDumpFile,
                          MiniDumpNormal,
                          &dumpInfo,
                          NULL,
                          NULL);
        // 无论写入是否成功都关闭已创建句柄。
        CloseHandle(hDumpFile);
    }

    // 异常处理末尾使用系统提示框告知用户转储文件位置。
    MessageBoxA(NULL,
                ("Application crashed! Dump saved to: " + dumpPath).c_str(),
                "Fatal Error",
                MB_OK | MB_ICONERROR);

    // 返回执行处理器语义，阻止系统继续搜索其他未处理异常过滤器。
    return EXCEPTION_EXECUTE_HANDLER;
}

/// @brief 把 MusicMapMaker 未处理异常过滤器注册到当前 Windows 进程。
/// @note 应在业务线程和图形后端启动前调用，覆盖尽可能多的崩溃阶段。
inline void register_crash_handler()
{
    SetUnhandledExceptionFilter(mmm_unhandled_exception_filter);
}

}  // namespace MMM

#else

namespace MMM
{
/// @brief 非 Windows 平台的崩溃处理注册占位入口。
///
/// 保持跨平台启动代码拥有统一 API；当前不安装 signal handler，也不改变
/// 系统默认 core dump 行为。
inline void register_crash_handler()
{
    // Linux/macOS 暂由系统默认信号与 core dump 机制处理。
}
}  // namespace MMM

#endif
