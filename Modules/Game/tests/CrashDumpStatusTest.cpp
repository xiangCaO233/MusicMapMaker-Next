#include "game/CrashHandler.h"

#include <sddl.h>

#include <filesystem>
#include <string_view>
#include <system_error>

namespace
{
// 模拟入口与原生 DbgHelp 并用同一套状态，避免复制一份供测试专用的判定。
using MMM::CrashHandlerDetail::DumpStatus;

/// @brief 本进程内只执行一个部分写入场景，记录夹具是否成功制造残留字节。
bool partialWritten{};

/// @brief 对写了一部分后的错误进行可重复注入，覆盖真实文件清理。
/// @param file 由受测函数建立的真实、独占写句柄。
/// @return 固定失败，用来模拟 DbgHelp 在写出前缀后停止。
/// @note 调用约定与 DbgHelp 相同，直接经过生产函数的失败和清理分支。
BOOL WINAPI rejectAfterWrite(HANDLE, DWORD, HANDLE file, MINIDUMP_TYPE,
                             PMINIDUMP_EXCEPTION_INFORMATION,
                             PMINIDUMP_USER_STREAM_INFORMATION,
                             PMINIDUMP_CALLBACK_INFORMATION)
{
    // 固定前缀不会偶然形成 minidump 签名，文件必须被清除才能通过。
    constexpr char partial[] = "unfinished";
    DWORD          written{};
    // 如果注入本身没有写入夹具，测试不得把空文件清理当成覆盖了部分写入。
    partialWritten =
        WriteFile(file, partial, sizeof(partial), &written, nullptr) != FALSE &&
        written == sizeof(partial);
    return FALSE;
}

/// @brief 创建拒绝所有用户创建文件的临时目录，父目录仍可清理测试夹具。
/// @param path 测试新建的父目录下的子目录，不会重设已存在目录的权限。
/// @return 仅在带有显式安全描述符的目录创建成功时返回 true。
/// @details 不依赖安装目录或用户名，也不更改本机其他文件的访问控制。
bool createDeniedDirectory(const std::filesystem::path& path)
{
    // 文件创建权限与目录本身能否被枚举和清理独立。否定项只限制前者。
    PSECURITY_DESCRIPTOR descriptor{};
    // FILE_ADD_FILE 为 0x2。拒绝项必须在允许项之前，使其覆盖继承的写权限。
    if ( !ConvertStringSecurityDescriptorToSecurityDescriptorW(
             L"D:P(D;;0x2;;;WD)(A;;FA;;;WD)",
             SDDL_REVISION_1,
             &descriptor,
             nullptr) )
        return false;
    // 安全描述符只在创建目录时借用，创建失败也释放本地分配。
    // P 阻止继承父目录的 ACL，使否定规则不受执行环境影响。
    SECURITY_ATTRIBUTES attributes{ sizeof(SECURITY_ATTRIBUTES),
                                    descriptor,
                                    FALSE };
    const bool created = CreateDirectoryW(path.c_str(), &attributes) != FALSE;
    LocalFree(descriptor);
    return created;
}

/// @brief 读取真实 DbgHelp 转储的文件头，确认成功路径生成了有效诊断数据。
/// @return 长度达到转储头要求、签名正确且存在至少一个流时返回 true。
/// @note 只读转储头，测试不会记录其中包含的进程数据。
bool hasDumpHeader(const std::filesystem::path& path)
{
    // 重新打开文件也验证了受测函数已关闭独占写句柄。
    const HANDLE file = CreateFileW(path.c_str(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if ( file == INVALID_HANDLE_VALUE ) return false;
    // 读取固定大小头部便足以区分文件是否具有 DbgHelp 生成的结构。
    // 不读取其余页，避免还需要按系统版本解析可选数据流。
    MINIDUMP_HEADER header{};
    DWORD           read{};
    // 只检查存在性会接收空文件。这里用签名及流数量和失败清理区分。
    const bool valid =
        ReadFile(file, &header, sizeof(header), &read, nullptr) != FALSE &&
        read == sizeof(header) && header.Signature == MINIDUMP_SIGNATURE &&
        header.NumberOfStreams > 0;
    CloseHandle(file);
    return valid;
}

/// @brief 使用生产路径和提示选择验证不可写目录、部分写入、实际保存三种场景。
/// @param root 此次进程独占创建的输出目录。
/// @return 零表示全部成功，其余返回码标识首个失败的场景。
/// @details 通过状态选择器取得消息内容而无需弹出对话框，正常写入仍调用系统库。
int verifyDumpStatuses(const std::filesystem::path& root)
{
    using namespace MMM::CrashHandlerDetail;
    const auto denied = root / L"denied";
    if ( !createDeniedDirectory(denied) ) return 1;
    // 对 ACL 受限的目录只验证建立新的文件；不修改权限来制造清理失败。
    const auto deniedDump = denied / L"crash.dmp";
    wchar_t    message[160]{};
    // 读取 Win32 原始错误以排除错误路径、不存在的目录等替代性原因。
    const auto  openFailure = writeDump(deniedDump.c_str(), nullptr);
    const DWORD openError   = GetLastError();
    if ( openFailure != DumpStatus::createFailed ||
         openError != ERROR_ACCESS_DENIED ||
         std::wstring_view(
             dumpMessage(openFailure, deniedDump.c_str(), message))
                 .find(L"Could not create") == std::wstring_view::npos )
        return 2;

    // 不同目标文件名防止前一用例的 ACL 先拒绝本次创建，掩盖写入失败分支。
    const auto partialDump = root / L"partial.dmp";
    const auto writeFailure =
        writeDump(partialDump.c_str(), nullptr, rejectAfterWrite);
    // 同时验证部分内容存在过、文件随后被移除，并且提示没有使用成功状态。
    // GetFileAttributes
    // 的失败必须发生在同一测试路径；另一个文件的不存在不构成覆盖。
    if ( !partialWritten || writeFailure != DumpStatus::writeFailed ||
         GetFileAttributesW(partialDump.c_str()) != INVALID_FILE_ATTRIBUTES ||
         std::wstring_view(
             dumpMessage(writeFailure, partialDump.c_str(), message))
                 .find(L"Could not finish writing") == std::wstring_view::npos )
        return 3;

    // 使用状态选择器接收成功后的路径，写入调用仍走系统自带 DbgHelp。
    // 用生产命名函数验证时间格式化路径。结尾必须保留完整扩展名。
    wchar_t name[96]{};
    if ( !makeDumpPath(name) || !std::wstring_view(name).ends_with(L".dmp") )
        return 4;
    const auto completeDump = root / name;
    // 本测试没有崩溃上下文，DbgHelp 仍应提供正常的进程转储头和数据流。
    // 不需要触发真正的崩溃，也不依赖图形会话和调试器。
    const auto success = writeDump(completeDump.c_str(), nullptr);
    // 生产提示在成功时使用相对于工作目录的文件名，反馈末尾必须完整保留它。
    const std::wstring_view feedback = dumpMessage(success, name, message);
    if ( success != DumpStatus::saved || !hasDumpHeader(completeDump) ||
         !feedback.ends_with(name) ||
         feedback.find(L"Dump saved to:") == std::wstring_view::npos )
        return 5;
    // 冲突文件必须仍是原来的诊断数据，不会被用于测试失败分支的写入覆盖。
    // 通过第二次读取实际文件头确认 CREATE_NEW 没有替换先前写入的内容。
    if ( writeDump(completeDump.c_str(), nullptr, rejectAfterWrite) !=
             DumpStatus::createFailed ||
         !hasDumpHeader(completeDump) )
        return 6;
    return 0;
}
}  // namespace

/// @brief 为每个测试进程建立独立的夹具目录，结束后移除实际生成的转储。
/// @return 失败代码区分初始化、操作验证与清理，便于从 CTest 判断故障阶段。
/// @note CTest 指定构建输出目录作为进程工作目录，诊断文件不写入源码树。
int main()
{
    std::error_code error;
    wchar_t         name[96]{};
    if ( !MMM::CrashHandlerDetail::makeDumpPath(name) ) return 1;
    // 与其他测试进程共享输出根时，使用 PID 加时间避免目录名冲突。
    const std::filesystem::path root(name);
    // 重复执行时如果旧夹具名称已经存在，直接退出，不在其中创建任何文件。
    if ( !std::filesystem::create_directory(root, error) ) return 1;
    const int result = verifyDumpStatuses(root);
    // 不删除用户已有目录。只有独占创建成功后才会进入这条清理路径。
    // 同时清除权限受限目录和写成功的转储，原生机器不会积累含进程数据的夹具。
    std::filesystem::remove_all(root, error);
    // 如果两阶段均失败，优先报告真正的转储状态断言。
    return result != 0 ? result : (error ? 7 : 0);
}
