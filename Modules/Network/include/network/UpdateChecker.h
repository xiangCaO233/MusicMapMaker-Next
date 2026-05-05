#pragma once

#include <cstdint>
#include <string>

namespace MMM::Network
{

/// @brief 更新检查结果状态
enum class UpdateStatus : uint8_t {
    kChecking,     ///< 正在检查中
    kUpToDate,     ///< 已是最新版本
    kUpdateFound,  ///< 发现新版本
    kError         ///< 检查出错（网络问题等）
};

/// @brief 版本更新信息
struct UpdateInfo {
    UpdateStatus status{ UpdateStatus::kChecking };
    std::string  latestVersion;      ///< 远程最新版本号
    std::string  currentVersion;     ///< 当前应用版本号
    std::string  changelog;          ///< 更新日志
    std::string  releaseDate;        ///< 发布日期
    std::string  downloadUrl;        ///< 下载地址（平台相关）
    int64_t      downloadSize{ 0 };  ///< 下载文件大小（字节）
    std::string  checksum;           ///< SHA256 校验和
    std::string  errorMessage;       ///< 错误信息
};

/// @brief 应用更新检查器，面向 xiand233.top 的更新 API
class UpdateChecker
{
public:
    UpdateChecker()  = default;
    ~UpdateChecker() = default;

    /// @brief 异步检查更新（非阻塞，在单独线程中执行 HTTP 请求）
    void checkAsync();

    /// @brief 获取当前状态
    UpdateInfo getInfo() const { return m_info; }

    /// @brief 判断是否检查完成
    bool isFinished() const
    {
        return m_info.status == UpdateStatus::kUpToDate ||
               m_info.status == UpdateStatus::kUpdateFound ||
               m_info.status == UpdateStatus::kError;
    }

    /// @brief 在浏览器中打开 URL（平台通用）
    static void openUrlInBrowser(const std::string& url);

private:
    /// @brief 解析语义化版本字符串（如 "gammav0.2"）
    static bool parseVersion(const std::string& verStr, int& major, int& minor,
                             int& patch);

    /// @brief 比较两个版本号（返回 true 表示 remote > local）
    static bool isNewer(const std::string& remote, const std::string& local);

    UpdateInfo m_info;
};

}  // namespace MMM::Network
