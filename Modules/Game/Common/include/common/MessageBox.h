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

/**
 * @brief 显示跨平台错误弹窗
 * @param title 弹窗标题
 * @param message 错误信息
 */
inline void showFatalError(const std::string& title, const std::string& message)
{
#ifdef _WIN32
    MessageBoxA(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
#else
    // Linux 下尝试使用 zenity, kdialog 或 notify-send
    std::string cmd = "zenity --error --title=\"" + title + "\" --text=\"" +
                      message +
                      "\" 2>/dev/null || "
                      "kdialog --error \"" +
                      message + "\" --title \"" + title +
                      "\" 2>/dev/null || "
                      "notify-send \"" +
                      title + "\" \"" + message + "\"";
    (void)std::system(cmd.c_str());
#endif
}

}  // namespace MMM::UI
