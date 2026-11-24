#pragma once

#ifdef _WIN32
#    include <cstddef>
#    include <cwchar>
// MinGW 的 dbghelp.h 依赖 windows.h 类型定义，并且 SDK 文件名区分大小写。
// clang-format off
#    include <windows.h>
#    include <dbghelp.h>
// clang-format on

namespace MMM
{
namespace CrashHandlerDetail
{

/// @brief 区分建立文件与提交转储两个阶段，提示不会把失败当成已保存。
enum class DumpStatus {
    /// @brief 无法构造文件名或无法在当前目录新建文件。
    createFailed,
    /// @brief 转储生成、刷新文件或关闭写句柄失败。
    writeFailed,
    /// @brief 转储成功写入、刷新并关闭。
    saved,
};

/// @brief 生成当前目录中的转储文件名，末尾加入 PID 和 TID 避免并发覆盖。
/// @param path 接收以空字符结尾的文件名。
/// @return 格式化成功时返回 true。
/// @note 文件名使用当前进程和故障线程 ID。同一毫秒内的同线程重复调用
/// 仍交由系统独占创建判定，不能覆盖早先留下的诊断数据。
template<std::size_t N> bool makeDumpPath(wchar_t (&path)[N]) noexcept
{
    // GetLocalTime 直接填充已拆分的本地时间，不依赖 CRT 的共享 tm 缓冲区。
    SYSTEMTIME local{};
    GetLocalTime(&local);
    // 即使时钟被拨回，也由创建文件时的 CREATE_NEW 保护以前的转储。
    const auto length =
        std::swprintf(path,
                      N,
                      L"crash_%04u%02u%02u_%02u%02u%02u_%03u_%lu_%lu.dmp",
                      unsigned(local.wYear),
                      unsigned(local.wMonth),
                      unsigned(local.wDay),
                      unsigned(local.wHour),
                      unsigned(local.wMinute),
                      unsigned(local.wSecond),
                      unsigned(local.wMilliseconds),
                      GetCurrentProcessId(),
                      GetCurrentThreadId());
    return length > 0 && static_cast<std::size_t>(length) < N;
}

/// @brief 新建转储文件，只有写入、刷新和关闭都成功才报告已保存。
/// @param path 原生 Windows 转储路径。
/// @param pointers 当前进程中借用的异常上下文，允许为空。
/// @param writer DbgHelp 写入入口，回归测试可提供确定的失败结果。
/// @note 失败时尽力删除不完整的文件，已有同名文件由 CREATE_NEW 保护。
inline DumpStatus writeDump(
    const wchar_t* path, EXCEPTION_POINTERS* pointers,
    decltype(&MiniDumpWriteDump) writer = &MiniDumpWriteDump) noexcept
{
    // 当前工作目录可能只读，例如从 Program Files 启动。
    // 保持原来的目录选择策略，但失败时不再显示一个并不存在的路径。
    const HANDLE dump = CreateFileW(path,
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if ( dump == INVALID_HANDLE_VALUE ) return DumpStatus::createFailed;

    // 当前进程同时是写入方和被转储方，异常指针从调用者本机地址空间读取。
    MINIDUMP_EXCEPTION_INFORMATION context{ GetCurrentThreadId(),
                                            pointers,
                                            FALSE };
    // MiniDumpNormal 限制转储范围。写入成功后还要刷新，以捕获延迟的文件写错误。
    const bool written = writer(GetCurrentProcess(),
                                GetCurrentProcessId(),
                                dump,
                                MiniDumpNormal,
                                pointers ? &context : nullptr,
                                nullptr,
                                nullptr) != FALSE;
    const bool flushed = written && FlushFileBuffers(dump) != FALSE;
    // 无论前面哪一步失败都关闭写句柄，失败文件不应作为有效诊断数据使用。
    const bool closed = CloseHandle(dump) != FALSE;
    if ( !flushed || !closed ) {
        // 清理失败也保持 writeFailed。调用方不会把残留的部分转储当成已保存。
        DeleteFileW(path);
        return DumpStatus::writeFailed;
    }
    return DumpStatus::saved;
}

/// @brief 根据实际结果生成消息，仅成功时注明转储路径。
/// @param status 文件操作结果。
/// @param path 仅在成功时读取，须指向已生成的有效文件名。
/// @param message 调用者提供的成功提示缓冲区。
template<std::size_t N>
const wchar_t* dumpMessage(DumpStatus status, const wchar_t* path,
                           wchar_t (&message)[N]) noexcept
{
    // 两条失败提示使用固定文本，不读取失败时可能已截断的路径字符串。
    // 提示只解释已知的操作结果，不猜测错误源是磁盘、权限还是 DbgHelp。
    switch ( status ) {
    case DumpStatus::createFailed:
        return L"Application crashed! Could not create a dump file in the "
               L"current directory.";
    case DumpStatus::writeFailed:
        return L"Application crashed! Could not finish writing the dump file.";
    case DumpStatus::saved:
        // 只有转储已保存才展示路径。缓冲不足时放弃路径文案，仍报告正确状态。
        const auto length = std::swprintf(
            message, N, L"Application crashed! Dump saved to: %ls", path);
        if ( length > 0 && static_cast<std::size_t>(length) < N )
            return message;
        return L"Application crashed! Dump saved in the current directory.";
    }
    return L"Application crashed!";
}

}  // namespace CrashHandlerDetail

/// @brief 捕获 Windows 未处理异常并尝试写出最小转储文件。
/// @param exceptionPointers 操作系统提供的异常上下文。
/// @return EXCEPTION_EXECUTE_HANDLER，通知系统终止当前异常流程。
/// @note 异常路径不构造堆字符串，提示区分创建、写入失败和保存成功。
/// 仍在操作系统选定的异常线程上调用 DbgHelp，保持现有进程内诊断策略。
/// 不调用项目日志系统，避免在故障线程已持有日志锁时再次获取它。
inline LONG WINAPI
mmm_unhandled_exception_filter(EXCEPTION_POINTERS* exceptionPointers)
{
    using namespace CrashHandlerDetail;
    // 缓冲区分别覆盖最坏情况的时间戳和两个 32 位 ID，以及完整的提示前缀。
    // 格式化结果未完整写入时不会进入创建阶段，防止为截断的后缀生成转储。
    wchar_t    path[96]{};
    const auto status = makeDumpPath(path) ? writeDump(path, exceptionPointers)
                                           : DumpStatus::createFailed;
    wchar_t    message[160]{};
    // 统一通过同一个状态选择器生成提示，成功与失败状态不会混用。
    MessageBoxW(nullptr,
                dumpMessage(status, path, message),
                L"Fatal Error",
                MB_OK | MB_ICONERROR);
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
