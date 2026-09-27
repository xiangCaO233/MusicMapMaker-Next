#pragma once
#include <cstdlib>
#include <string>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#endif

namespace MMM::UI
{

/// @brief 在程序图形界面不可用时显示跨平台致命错误提示。
/// @param title 弹窗标题。
/// @param message 需要展示的错误信息。
/// @note Windows 使用原生消息框，其他平台依次尝试常见桌面通知工具。
/// @warning 非 Windows 分支会同步启动外部命令，只能用于低频致命错误路径。
/// @warning 非 Windows 分支通过 shell 拼接参数，调用方只能传入可信诊断文本。
inline void showFatalError(const std::string& title, const std::string& message)
{
#ifdef _WIN32
    // Windows 分支在编译期剔除外部命令依赖，适合启动失败阶段使用。
    // 启动早期不依赖 ImGui，直接使用系统错误图标阻塞提示用户。
    MessageBoxA(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
#else
    // 非 Windows 分支不假设具体桌面环境，仅依赖系统 PATH 查找可用工具。
    // 按桌面环境兼容性依次尝试 zenity、kdialog 与 notify-send。
    // 前一工具失败才执行后一工具，保证通常只展示一次错误提示。
    std::string cmd = "zenity --error --title=\"" + title + "\" --text=\"" +
                      message +
                      "\" 2>/dev/null || "
                      "kdialog --error \"" +
                      message + "\" --title \"" + title +
                      "\" 2>/dev/null || "
                      "notify-send \"" +
                      title + "\" \"" + message + "\"";
    // 致命错误提示不参与业务错误恢复，忽略外部通知工具的退出状态。
    (void)std::system(cmd.c_str());
#endif
}

}  // namespace MMM::UI
