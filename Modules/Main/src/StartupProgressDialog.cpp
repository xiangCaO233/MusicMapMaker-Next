#include "main/StartupProgressDialog.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <commctrl.h>
#    include <windows.h>
#else
#    include <csignal>
#endif

namespace MMM::Main
{

namespace
{

#ifdef _WIN32
constexpr const wchar_t* kStartupProgressWindowClass =
    L"MMMStartupProgressDialogWindow";

/// @brief 将 UTF-8 文本转换为 Win32 宽字符文本。
/// @param text UTF-8 文本。
/// @return 可传给 Win32 W 接口的宽字符文本。
std::wstring utf8ToWide(std::string_view text)
{
    if ( text.empty() ) return {};

    const int inputSize = static_cast<int>(text.size());
    int       wideSize  = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), inputSize, nullptr, 0);
    if ( wideSize <= 0 ) {
        wideSize =
            MultiByteToWideChar(CP_UTF8, 0, text.data(), inputSize, nullptr, 0);
    }
    if ( wideSize <= 0 ) return L"MusicMapMaker";

    std::wstring wideText(static_cast<std::size_t>(wideSize), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.data(), inputSize, wideText.data(), wideSize);
    return wideText;
}

/// @brief 按当前 Windows DPI 缩放像素值。
/// @param value 96 DPI 下的像素值。
/// @return 缩放后的像素值。
int scaleForWindowsDpi(int value)
{
    HDC screenDc = GetDC(nullptr);
    if ( !screenDc ) return value;

    const int dpi = GetDeviceCaps(screenDc, LOGPIXELSX);
    ReleaseDC(nullptr, screenDc);
    return MulDiv(value, dpi > 0 ? dpi : 96, 96);
}

/// @brief 设置 Win32 子控件默认 GUI 字体。
/// @param window 控件句柄。
void setDefaultGuiFont(HWND window)
{
    if ( !window ) return;

    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if ( font ) {
        SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

/// @brief 处理启动进度窗口消息。
/// @param window 窗口句柄。
/// @param message 消息类型。
/// @param wParam 消息参数。
/// @param lParam 消息参数。
/// @return Win32 消息处理结果。
LRESULT CALLBACK startupProgressWindowProc(HWND window, UINT message,
                                           WPARAM wParam, LPARAM lParam)
{
    if ( message == WM_CLOSE ) return 0;
    return DefWindowProcW(window, message, wParam, lParam);
}

/// @brief 注册启动进度窗口类。
/// @return 成功或已注册时返回 true。
bool registerStartupProgressWindowClass()
{
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc   = startupProgressWindowProc;
    windowClass.hInstance     = GetModuleHandleW(nullptr);
    windowClass.hCursor       = LoadCursorW(nullptr, IDC_WAIT);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kStartupProgressWindowClass;

    if ( RegisterClassW(&windowClass) != 0 ) return true;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

/// @brief 处理启动进度窗口积压的 Win32 消息。
void pumpWindowsMessages()
{
    MSG message{};
    while ( PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) ) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
#endif

#ifndef _WIN32
/// @brief 检查当前环境是否能显示 zenity 窗口。
bool canUseZenity()
{
    const char* display        = std::getenv("DISPLAY");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if ( (!display || display[0] == '\0') &&
         (!waylandDisplay || waylandDisplay[0] == '\0') ) {
        return false;
    }

    const int result = std::system("command -v zenity >/dev/null 2>&1");
    return result == 0;
}
#endif

/// @brief 将进度文本压成单行，避免破坏 zenity 进度输入。
std::string singleLineText(std::string text)
{
    for ( char& c : text ) {
        if ( c == '\n' || c == '\r' ) c = ' ';
    }
    return text;
}

}  // namespace

StartupProgressDialog::StartupProgressDialog()
{
#ifdef _WIN32
    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(INITCOMMONCONTROLSEX);
    commonControls.dwICC  = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&commonControls);

    if ( !registerStartupProgressWindowClass() ) {
        XWARN("StartupProgressDialog: failed to register Win32 window class");
        return;
    }

    const int width  = scaleForWindowsDpi(480);
    const int height = scaleForWindowsDpi(150);
    const int x      = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y      = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                  kStartupProgressWindowClass,
                                  L"MusicMapMaker",
                                  WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                  x,
                                  y,
                                  width,
                                  height,
                                  nullptr,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  nullptr);
    if ( !window ) {
        XWARN("StartupProgressDialog: failed to create Win32 progress window");
        return;
    }

    HMENU systemMenu = GetSystemMenu(window, FALSE);
    if ( systemMenu ) {
        EnableMenuItem(
            systemMenu, SC_CLOSE, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
    }

    const int margin    = scaleForWindowsDpi(22);
    const int textY     = scaleForWindowsDpi(22);
    const int textH     = scaleForWindowsDpi(32);
    const int progressY = scaleForWindowsDpi(72);
    const int progressH = scaleForWindowsDpi(22);
    const int childW    = width - margin * 2;

    HWND text     = CreateWindowExW(0,
                                    L"STATIC",
                                    L"正在准备资源包...",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    margin,
                                    textY,
                                    childW,
                                    textH,
                                    window,
                                    nullptr,
                                    GetModuleHandleW(nullptr),
                                    nullptr);
    HWND progress = CreateWindowExW(0,
                                    PROGRESS_CLASSW,
                                    nullptr,
                                    WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                    margin,
                                    progressY,
                                    childW,
                                    progressH,
                                    window,
                                    nullptr,
                                    GetModuleHandleW(nullptr),
                                    nullptr);
    if ( !text || !progress ) {
        DestroyWindow(window);
        XWARN(
            "StartupProgressDialog: failed to create Win32 progress controls");
        return;
    }

    setDefaultGuiFont(text);
    SendMessageW(progress, PBM_SETRANGE32, 0, 100);
    SendMessageW(progress, PBM_SETPOS, 0, 0);

    m_windowHandle   = window;
    m_textHandle     = text;
    m_progressHandle = progress;

    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    writeProgress(0, "正在准备资源包...");
#else
    if ( !canUseZenity() ) return;

    std::signal(SIGPIPE, SIG_IGN);
    m_pipe = popen(
        "zenity --progress --title=\"MusicMapMaker\" "
        "--text=\"正在准备资源包...\" --percentage=0 --auto-close "
        "--no-cancel 2>/dev/null",
        "w");
    if ( !m_pipe ) {
        XWARN("StartupProgressDialog: failed to open zenity progress dialog");
    }
#endif
}

StartupProgressDialog::~StartupProgressDialog()
{
    close();
}

bool StartupProgressDialog::shouldOpenFor(
    const Network::AssetSyncProgress& progress)
{
    return progress.stage == Network::AssetSyncProgressStage::kCheckingFiles ||
           progress.stage ==
               Network::AssetSyncProgressStage::kDownloadingPackage ||
           progress.stage ==
               Network::AssetSyncProgressStage::kExtractingPackage ||
           progress.stage == Network::AssetSyncProgressStage::kDownloadingFile;
}

void StartupProgressDialog::update(const Network::AssetSyncProgress& progress)
{
    if ( !isOpen() ) return;

    const int         percent = progressPercent(progress);
    const std::string text    = progressText(progress);
    const auto        now     = std::chrono::steady_clock::now();

    if ( percent == m_lastPercent && text == m_lastText &&
         now - m_lastUpdateTime < std::chrono::milliseconds(200) ) {
        return;
    }

    writeProgress(percent, text);
    m_lastPercent    = percent;
    m_lastText       = text;
    m_lastUpdateTime = now;
}

void StartupProgressDialog::close()
{
    if ( !isOpen() ) return;

    writeProgress(100, "资源包已准备完成，正在启动...");
#ifdef _WIN32
    DestroyWindow(static_cast<HWND>(m_windowHandle));
    m_windowHandle   = nullptr;
    m_textHandle     = nullptr;
    m_progressHandle = nullptr;
    pumpWindowsMessages();
#else
    pclose(m_pipe);
    m_pipe = nullptr;
#endif
}

bool StartupProgressDialog::isOpen() const
{
#ifdef _WIN32
    return m_windowHandle != nullptr;
#else
    return m_pipe != nullptr;
#endif
}

int StartupProgressDialog::progressPercent(
    const Network::AssetSyncProgress& progress) const
{
    if ( progress.stage ==
         Network::AssetSyncProgressStage::kExtractingPackage ) {
        return 98;
    }
    if ( progress.stage == Network::AssetSyncProgressStage::kFinished ) {
        return 100;
    }
    if ( progress.stage == Network::AssetSyncProgressStage::kCheckingFiles &&
         progress.totalFileCount > 0 ) {
        const auto percent = static_cast<int>((progress.currentFileIndex * 35) /
                                              progress.totalFileCount);
        return std::clamp(percent, 0, 35);
    }
    if ( progress.stage == Network::AssetSyncProgressStage::kDownloadingFile &&
         progress.totalFileCount > 0 ) {
        const auto completedFiles =
            progress.currentFileIndex > 0 ? progress.currentFileIndex - 1 : 0;
        int filePercent = 0;
        if ( progress.totalBytes > 0 ) {
            filePercent = static_cast<int>(
                (progress.currentBytes * 100) /
                std::max<std::int64_t>(progress.totalBytes, 1));
        }
        const auto percent =
            35 + static_cast<int>(
                     ((completedFiles * 100 + std::clamp(filePercent, 0, 100)) *
                      60) /
                     (progress.totalFileCount * 100));
        return std::clamp(percent, 35, 95);
    }
    if ( progress.totalBytes > 0 ) {
        const auto percent =
            static_cast<int>((progress.currentBytes * 100) /
                             std::max<std::int64_t>(progress.totalBytes, 1));
        return std::clamp(percent, 0, 100);
    }
    return 0;
}

std::string StartupProgressDialog::progressText(
    const Network::AssetSyncProgress& progress) const
{
    switch ( progress.stage ) {
    case Network::AssetSyncProgressStage::kDownloadingPackage:
        if ( progress.totalBytes > 0 ) {
            return "正在下载资源包 " +
                   std::to_string(progress.currentBytes / 1024 / 1024) + "/" +
                   std::to_string(progress.totalBytes / 1024 / 1024) + " MB";
        }
        return "正在下载资源包...";
    case Network::AssetSyncProgressStage::kExtractingPackage:
        return "正在解压资源包...";
    case Network::AssetSyncProgressStage::kDownloadingFile:
        return "正在更新资源文件 " + std::to_string(progress.currentFileIndex) +
               "/" + std::to_string(progress.totalFileCount);
    case Network::AssetSyncProgressStage::kCheckingManifest:
        return "正在检查资源更新...";
    case Network::AssetSyncProgressStage::kCheckingFiles:
        if ( progress.totalFileCount > 0 ) {
            return "正在校验本地资源 " +
                   std::to_string(progress.currentFileIndex) + "/" +
                   std::to_string(progress.totalFileCount);
        }
        return "正在校验本地资源...";
    case Network::AssetSyncProgressStage::kFinished:
        return "资源包已准备完成，正在启动...";
    default: break;
    }
    return singleLineText(progress.message);
}

void StartupProgressDialog::writeProgress(int percent, const std::string& text)
{
    const std::string safeText = singleLineText(text);
#ifdef _WIN32
    HWND window   = static_cast<HWND>(m_windowHandle);
    HWND textCtrl = static_cast<HWND>(m_textHandle);
    HWND progress = static_cast<HWND>(m_progressHandle);
    if ( !window || !textCtrl || !progress ) return;

    const int  clampedPercent = std::clamp(percent, 0, 100);
    const auto wideText       = utf8ToWide(safeText);
    SetWindowTextW(textCtrl, wideText.c_str());
    SendMessageW(progress, PBM_SETPOS, clampedPercent, 0);
    UpdateWindow(textCtrl);
    UpdateWindow(progress);
    UpdateWindow(window);

    pumpWindowsMessages();
#else
    if ( !m_pipe ) return;

    if ( std::fprintf(m_pipe, "# %s\n%d\n", safeText.c_str(), percent) < 0 ) {
        pclose(m_pipe);
        m_pipe = nullptr;
        return;
    }
    std::fflush(m_pipe);
#endif
}

}  // namespace MMM::Main
