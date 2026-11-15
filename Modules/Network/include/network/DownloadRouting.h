#pragma once

#include <cstdint>
#include <cstdio>
#include <curl/curl.h>
#include <functional>
#include <string>
#include <vector>

namespace MMM::Network
{
/// @brief 本次应用会话共用的下载线路选择。
/// @note 手动 R2 仍可优选该域名的 IP，但不会回退到源站。
enum class DownloadRoute : std::uint8_t {
    Automatic,  ///< 按速度优选，并在失败或持续慢速时回退。
    R2,         ///< 只使用 R2 下载域名。
    Origin      ///< 使用官方服务器同路径副本。
};

/// @brief 读取当前会话选线，不访问配置文件或网络。
/// @warning UI 每帧与后台下载回调读取单一枚举，不发布其他共享状态；relaxed
/// 足够。
[[nodiscard]] DownloadRoute getDownloadRoute();

/// @brief 由 UI 发布选线，正在传输的官方文件在下一次回调退出后重新下载。
/// @param route 本次会话共用的选项，不修改系统 DNS 或代理。
/// @warning UI 只写单一枚举；网络任务不能在持 UI 锁时等待切换结果。
void setDownloadRoute(DownloadRoute route);

/// @brief 按选择构造官方文件地址；第三方及本地 URL 保持原样。
/// @note 保留原始路径和摘要参数，不接受下载命名空间以外的地址。
[[nodiscard]] std::string downloadUrlForRoute(const std::string& url,
                                              DownloadRoute      route);

/// @brief 后台文件下载的选路、取消和进度契约。
/// @details 仅优化官方 R2 下载域名，不改变系统 DNS、代理或 TLS 信任配置。
struct DownloadRoutingOptions {
    std::function<bool()> m_cancelled;  ///< 返回 true 后不再尝试其他线路。
    std::function<void(std::int64_t, std::int64_t)>
                  m_progress;               ///< 已写入量与总量。
    std::uint64_t m_expectedSize{ 0 };      ///< 清单提供的总长；零表示未知。
    long          m_timeoutSeconds{ 600 };  ///< 包含测速和重试的整个任务超时。
    bool m_probeRoutes{ true };  ///< 小更新器可关闭主动测速，仍复用已选线路。
    bool m_allowOriginFallback{ true };  ///< 官方同路径直连副本作为最后退路。
    /// @brief 可选的可信解析结果；为空时使用系统 DNS，非公网地址不参与优选。
    /// @note 用于已有解析器及验收工具；不允许通过此字段关闭证书校验。
    std::vector<std::string> m_addresses;
};

/// @brief 判断 URL 是否属于可优选的官方 HTTPS 下载路径。
/// @note 严格限定主机和路径，不能把相似域名、用户信息或任意端口当作镜像。
bool isR2DownloadUrl(const std::string& url);

/// @brief 后台下载到调用方已打开的文件，必要时按吞吐量选路和安全续传。
/// @param handle 已配置平台证书、URL 和 User-Agent 的独立 easy handle。
/// @param url 原始下载 URL；连接地址变化不改变 Host、SNI 或证书主机名。
/// @param output 以二进制写模式打开的普通文件，调用方负责关闭及失败清理。
/// @param options 整个同步调用期间保持有效的回调与策略。
/// @return 传输结果；文件完成后调用方仍须执行原有完整 SHA256 校验。
/// @warning 仅用于低频后台下载：包含 DNS、网络和文件 I/O，禁止从
/// UI/逻辑热路径调用。
/// @warning 系统 DNS 解析受操作系统超时约束；不得持有 UI 或会话锁调用。
CURLcode performRoutedDownload(CURL* handle, const std::string& url,
                               FILE*                         output,
                               const DownloadRoutingOptions& options = {});
}  // namespace MMM::Network
