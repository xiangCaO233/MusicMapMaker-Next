#include "common/MessageBox.h"

#include "log/colorful-log.h"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <limits>
#include <optional>
#include <string>

/// @file MessageBoxWin32.cpp
/// @brief 通过 Win32 原生接口呈现图形初始化失败。
/// @details Windows 最终收到 UTF-16；UTF-8 是应用日志和其他平台共用的输入协议。
/// 窗口尚未成功创建，消息框没有上级窗口，失败原因以先行写入的日志为准。
namespace MMM::UI
{
namespace
{

/// @brief 根据明确的 UTF-8 字节数生成 Win32 所需宽字符文本。
/// @param input 程序内部诊断文本，必须适合传给以空字符结尾的消息框接口。
/// @return 有效输入的 UTF-16 字符串，转换错误时空值。
/// @details 首次查询返回所需 UTF-16 元素数，按此分配后才执行转换。
/// 明确拒绝不完整的 UTF-8 字节序列，避免生成误导性的截断提示。
std::optional<std::wstring> toDialogText(const std::string& input)
{
    // Win32 接口的字节计数是 int，内嵌空字符也会让对话框提前截断。
    if ( input.size() >
             static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
         input.find('\0') != std::string::npos ) {
        return std::nullopt;
    }
    // 零长度转换是 Win32 的错误参数，空标题和空正文在这里保留为空。
    if ( input.empty() ) return std::wstring{};

    const auto length = static_cast<int>(input.size());
    // 先计算 UTF-16 单元容量，再填充精确大小的缓冲。wstring 自带终止符。
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), length, nullptr, 0);
    // 查询失败时不分配缓冲，调用方按字段使用固定可读的后备消息。
    if ( needed == 0 ) return std::nullopt;
    // 使用可写字符串区间承接 Win32，保持宽字符所有权直至消息框返回。
    std::wstring value(static_cast<std::size_t>(needed), L'\0');
    if ( MultiByteToWideChar(CP_UTF8,
                             MB_ERR_INVALID_CHARS,
                             input.data(),
                             length,
                             value.data(),
                             needed) != needed ) {
        return std::nullopt;
    }
    // 按所需长度构造的 wstring 在有效内容之后提供终止符。
    return value;
}

}  // namespace

/// @brief 写入并显示致命诊断，绕开当前系统代码页。
/// @param title 协议约定为 UTF-8 的标题，转换失败时使用程序名。
/// @param message UTF-8 正文，转换失败时指引用户查看先前写入的启动日志。
/// @details 编码转换与弹窗都发生在记录日志之后，任何错误都不更改退出状态。
/// @warning 原生消息框可能阻塞直到用户确认，仅限初始化失败。
void showFatalError(const std::string& title, const std::string& message)
{
    // 日志先于编码转换及窗口创建，可在无桌面会话时保留完整诊断。
    XERROR("{}: {}", title, message);
    const auto wideTitle   = toDialogText(title);
    const auto wideMessage = toDialogText(message);
    // 两个字段独立转换，单项不可用时尽量保留另一项诊断。
    const wchar_t* caption = wideTitle ? wideTitle->c_str() : L"MusicMapMaker";
    const wchar_t* details = wideMessage
                                 ? wideMessage->c_str()
                                 : L"Fatal error. See the application log.";
    // 此处不使用 GLFW/ImGui 句柄，因为调用点可能连窗口都没有建立。
    if ( MessageBoxW(nullptr, details, caption, MB_OK | MB_ICONERROR) == 0 ) {
        // 立即取错误码，避免后续诊断调用覆盖线程的 Win32 错误状态。
        XWARN("Unable to show the Win32 fatal error dialog ({})",
              GetLastError());
    }
}

}  // namespace MMM::UI
