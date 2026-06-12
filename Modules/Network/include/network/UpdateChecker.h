#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace MMM::Network
{

/// @brief 更新检查与下载状态
enum class UpdateStatus : uint8_t {
    kChecking,     ///< 正在检查更新
    kUpToDate,     ///< 已是最新版本
    kUpdateFound,  ///< 发现新版本，等待下载
    kDownloading,  ///< 正在下载更新
    kDownloaded,   ///< 下载完成，等待重启
    kError         ///< 出错
};

/// @brief 版本更新信息（线程安全：主线程只读，工作线程写入）
struct UpdateInfo {
    UpdateStatus status{ UpdateStatus::kChecking };
    std::string  latestVersion;      ///< 远程最新版本号
    std::string  currentVersion;     ///< 当前应用版本号
    std::string  changelog;          ///< 更新日志
    std::string  releaseDate;        ///< 发布日期
    std::string  downloadUrl;        ///< 下载地址（主程序，平台相关）
    int64_t      downloadSize{ 0 };  ///< 下载文件总大小（字节）
    std::string  checksum;           ///< SHA256 校验和
    std::string  errorMessage;       ///< 错误信息

    // 更新器（独立下载，负责替换主程序）
    std::string updaterUrl;       ///< 更新器下载地址
    std::string updaterFilePath;  ///< 更新器下载后的临时文件路径

    // 下载进度
    int64_t     downloadedBytes{ 0 };     ///< 已下载字节数
    double      downloadProgress{ 0.0 };  ///< 下载进度 0.0~1.0
    std::string downloadedFilePath;  ///< 下载完成后的临时文件路径（主程序）
};

/// @brief 应用更新检查器，面向 xiand233.top 的更新 API
class UpdateChecker
{
public:
    UpdateChecker()  = default;
    ~UpdateChecker() = default;

    /// @brief 异步检查更新（非阻塞，在单独线程中执行 HTTP 请求）
    void checkAsync();

    /// @brief 异步下载更新（非阻塞，下载到临时文件）
    void downloadAsync();

    /// @brief 获取当前状态。
    /// @warning UI 每帧轮询路径：只复制一份小型状态快照，禁止执行阻塞操作。
    UpdateInfo getInfo() const;

    /// @brief 判断是否检查完成
    bool isFinished() const
    {
        const auto info = getInfo();
        return info.status == UpdateStatus::kUpToDate ||
               info.status == UpdateStatus::kUpdateFound ||
               info.status == UpdateStatus::kDownloaded ||
               info.status == UpdateStatus::kError;
    }

    /// @brief 在浏览器中打开 URL（平台通用）
    static void openUrlInBrowser(const std::string& url);

    /// @brief 获取当前可执行文件的绝对路径
    static std::string currentExecutablePath();

    /// @brief 应用更新并重启（成功启动 Updater 后立即退出当前进程）
    /// @param downloadedFilePath 已下载的更新文件路径（主程序）
    /// @param updaterFilePath 更新器文件路径（负责替换主程序）
    /// @param errorMessage 启动失败时写入错误信息，可为空。
    /// @return 成功启动更新器时返回 true；失败时返回 false。
    static bool applyUpdateAndRestart(const std::string& downloadedFilePath,
                                      const std::string& updaterFilePath,
                                      std::string* errorMessage = nullptr);

    /// @brief 检查启动时的更新成功标记（更新后首次启动）
    /// @return 如果存在标记则返回 true，并将标记删除；否则返回 false
    static bool checkStartupUpdateMarker();

    /// @brief 解析语义化版本字符串（如 "v0.2.0"）
    static bool parseVersion(const std::string& verStr, int& major, int& minor,
                             int& patch);

    /// @brief 比较两个版本号（返回 true 表示 remote > local）
    static bool isNewer(const std::string& remote, const std::string& local);

private:
    /// @brief 更新状态互斥锁，保护下载线程与 UI 线程之间的状态快照。
    /// @warning UI 每帧读、下载线程写；仅保护小型状态结构，不得在持锁时执行
    /// 网络、文件系统或进程启动操作。
    mutable std::mutex m_infoMutex;

    /// @brief 当前更新状态快照。
    /// @warning 只能在持有 m_infoMutex 时读写。
    UpdateInfo m_info;
};

}  // namespace MMM::Network
