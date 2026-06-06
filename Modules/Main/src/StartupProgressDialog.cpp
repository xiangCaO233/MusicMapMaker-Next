#include "main/StartupProgressDialog.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <cstdlib>

#ifndef _WIN32
#    include <csignal>
#endif

namespace MMM::Main
{

namespace
{

/// @brief 检查当前环境是否能显示 zenity 窗口。
bool canUseZenity()
{
#ifdef _WIN32
    return false;
#else
    const char* display        = std::getenv("DISPLAY");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if ( (!display || display[0] == '\0') &&
         (!waylandDisplay || waylandDisplay[0] == '\0') ) {
        return false;
    }

    const int result = std::system("command -v zenity >/dev/null 2>&1");
    return result == 0;
#endif
}

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
#ifndef _WIN32
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
    if ( !m_pipe ) return;

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
    if ( !m_pipe ) return;

    writeProgress(100, "资源包已准备完成，正在启动...");
    pclose(m_pipe);
    m_pipe = nullptr;
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
    if ( !m_pipe ) return;

    const std::string safeText = singleLineText(text);
    if ( std::fprintf(m_pipe, "# %s\n%d\n", safeText.c_str(), percent) < 0 ) {
        pclose(m_pipe);
        m_pipe = nullptr;
        return;
    }
    std::fflush(m_pipe);
}

}  // namespace MMM::Main
