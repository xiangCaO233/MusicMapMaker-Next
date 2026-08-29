#pragma once

#ifdef _WIN32
#    include <windows.h>
#    include <dbghelp.h>
#    include <shlobj.h>
#    include <string>
#    include <ctime>

namespace MMM
{

/// @brief 崩溃回调函数
inline LONG WINAPI mmm_unhandled_exception_filter(EXCEPTION_POINTERS* pExceptionPointers)
{
    // 获取当前时间作为文件名一部分
    std::time_t t  = std::time(nullptr);
    char        timeStr[64];
    std::strftime(timeStr, sizeof(timeStr), "%Y%m%d_%H%M%S", std::localtime(&t));

    std::string dumpPath = "crash_" + std::string(timeStr) + ".dmp";

    HANDLE hDumpFile = CreateFileA(dumpPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if ( hDumpFile != INVALID_HANDLE_VALUE ) {
        MINIDUMP_EXCEPTION_INFORMATION dumpInfo;
        dumpInfo.ExceptionPointers = pExceptionPointers;
        dumpInfo.ThreadId          = GetCurrentThreadId();
        dumpInfo.ClientPointers    = TRUE;

        // 写入 MiniDump
        MiniDumpWriteDump(GetCurrentProcess(),
                          GetCurrentProcessId(),
                          hDumpFile,
                          MiniDumpNormal,
                          &dumpInfo,
                          NULL,
                          NULL);
        CloseHandle(hDumpFile);
    }

    // 弹出提示（可选）
    MessageBoxA(NULL, ("Application crashed! Dump saved to: " + dumpPath).c_str(), "Fatal Error", MB_OK | MB_ICONERROR);

    // 返回 EXCEPTION_EXECUTE_HANDLER 表示异常已处理，程序将静默退出
    return EXCEPTION_EXECUTE_HANDLER;
}

/// @brief 注册崩溃处理器
inline void register_crash_handler()
{
    SetUnhandledExceptionFilter(mmm_unhandled_exception_filter);
}

}  // namespace MMM

#else

namespace MMM
{
inline void register_crash_handler()
{
    // Linux/macOS 下暂未实现，可使用 signal 处理
}
}  // namespace MMM

#endif
