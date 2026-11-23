#include "common/MessageBox.h"

#include "log/colorful-log.h"

#include <spawn.h>
#include <sys/wait.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <string>

/// @brief 继承进程环境以查找当前桌面会话的通知工具。
extern char** environ;

/// @file MessageBoxLinux.cpp
/// @brief 通过 Linux 桌面工具显示启动阶段的致命诊断。
/// @details 进程通过 argv 接收 UTF-8 文本，标题和正文不参与 shell 命令解释。
/// 未安装通知工具、权限错误或没有桌面会话时，应用日志仍记录错误。
namespace MMM::UI
{
namespace
{

/// @brief 直接以参数数组启动桌面工具，并等待退出状态。
/// @param arguments 包含工具名和后续参数的可写存储，生命周期覆盖 posix_spawnp。
/// @return 工具成功启动且正常退出时为真，其他情况由上层尝试下一工具。
/// @warning 同步等待桌面工具，仅限致命初始化失败。
template<std::size_t Count>
bool runDesktopTool(std::array<std::string, Count> arguments)
{
    // 多一个值初始化的槽位作为 argv 结束符，保留正文中的引号和换行。
    // 参数数组按值传入，所有字符缓冲在启动系统调用返回前保持稳定。
    std::array<char*, Count + 1> argv{};
    for ( std::size_t index = 0; index < Count; ++index ) {
        argv[index] = arguments[index].data();
    }

    pid_t child{};
    // 通过 PATH 查找工具，正文与标题始终作为普通参数传入。
    if ( posix_spawnp(
             &child, argv.front(), nullptr, nullptr, argv.data(), environ) !=
         0 ) {
        // 创建失败时尚无子进程，继续等待会掩盖下一工具的后备机会。
        return false;
    }

    int status = 0;
    // EINTR 仅影响当前等待，同一个已启动的工具不可重复执行。
    while ( waitpid(child, &status, 0) < 0 ) {
        if ( errno != EINTR ) return false;
    }
    // 非零退出或信号终止表示没有完成通知，交由调用者尝试下一工具。
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace

/// @brief 先记录致命原因，再按桌面环境支持情况显示提示。
/// @details 桌面会话不可用时，已发出的日志仍记录完整错误。
/// @warning 可能等待对话框关闭，不可用于逐帧或后台轮询。
void showFatalError(const std::string& title, const std::string& message)
{
    // 先写入本次初始化的致命诊断，不能等待通知工具自行报告。
    XERROR("{}: {}", title, message);

    // 兼容 GNOME、KDE 和通用桌面通知。前一工具成功时不会重复显示。
    if ( runDesktopTool(std::array<std::string, 4>{
             "zenity", "--error", "--title=" + title, "--text=" + message }) ||
         runDesktopTool(std::array<std::string, 5>{
             "kdialog", "--error", message, "--title", title }) ) {
        return;
    }
    // notify-send 使用位置参数，结束选项解析以保留任意标题前缀。
    if ( !runDesktopTool(std::array<std::string, 4>{
             "notify-send", "--", title, message }) ) {
        XWARN("Unable to show a desktop fatal error notification");
    }
}

}  // namespace MMM::UI
