#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::Network
{

/// @brief 单个资源文件的远程清单记录。
struct AssetFileEntry {
    std::string   path;       ///< 相对 assets 根目录的文件路径。
    std::string   url;        ///< 可下载该文件的远程 URL。
    std::string   sha256;     ///< 文件内容 SHA256，固定为 64 位小写十六进制。
    std::uint64_t size{ 0 };  ///< 文件大小，单位字节。
};

/// @brief 默认资源包清单。
struct AssetManifest {
    std::string                 version;        ///< 资源包版本号。
    std::string                 packageUrl;     ///< 完整资源包下载 URL。
    std::string                 packageSha256;  ///< 完整资源包 SHA256。
    std::vector<AssetFileEntry> files;          ///< 可增量更新的文件列表。
};

/// @brief 资源同步运行选项。
struct AssetSyncOptions {
    std::filesystem::path assetsRootPath;  ///< 用户配置目录中的 assets 根路径。
    std::string           baseUrl;         ///< 网站根 URL，用于补全相对 URL。
    std::string           manifestUrl;     ///< 资源清单 URL。
    std::string           packageUrl;      ///< 清单不可用时的完整资源包 URL。
};

/// @brief 资源同步结果状态。
enum class AssetSyncStatus : std::uint8_t {
    kReady,       ///< 本地资源已就绪，无需更新。
    kDownloaded,  ///< 已下载并解压完整资源包。
    kUpdated,     ///< 已按清单增量更新资源。
    kError        ///< 同步失败。
};

/// @brief 资源同步结果。
struct AssetSyncResult {
    AssetSyncStatus status{ AssetSyncStatus::kReady };
    std::string     errorMessage;           ///< 失败原因。
    std::size_t     checkedFileCount{ 0 };  ///< 清单中参与校验的文件数。
    std::size_t     updatedFileCount{ 0 };  ///< 本次下载更新的文件数。
    std::string     remoteVersion;          ///< 远程资源版本。
};

/// @brief 启动时默认资源包下载、校验和增量更新服务。
class AssetSyncService
{
public:
    /// @brief 构造默认同步选项。
    /// @return 使用官方网站和用户配置 assets 路径的同步选项。
    static AssetSyncOptions defaultOptions();

    /// @brief 同步用户配置目录中的资源包。
    /// @param options 同步选项。
    /// @return 同步结果；失败时包含面向日志和弹窗的错误信息。
    static AssetSyncResult sync(const AssetSyncOptions& options);

    /// @brief 将清单中的下载 URL 补全为 libcurl 可使用的绝对 URL。
    /// @param baseUrl 网站根 URL。
    /// @param url 清单中的绝对 URL 或相对 URL。
    /// @return 已补全的下载 URL。
    static std::string resolveDownloadUrl(const std::string& baseUrl,
                                          const std::string& url);

    /// @brief 解析资源清单 JSON。
    /// @param manifestText 清单 JSON 文本。
    /// @param errorMessage 解析失败时写入原因。
    /// @return 成功时返回清单，否则返回空。
    static std::optional<AssetManifest> parseManifest(
        std::string_view manifestText, std::string& errorMessage);

    /// @brief 收集本地缺失或 SHA256 不一致的资源文件。
    /// @param manifest 远程资源清单。
    /// @param assetsRootPath 本地 assets 根路径。
    /// @return 需要下载替换的文件列表。
    static std::vector<AssetFileEntry> collectOutdatedFiles(
        const AssetManifest&         manifest,
        const std::filesystem::path& assetsRootPath);

    /// @brief 将 zip 资源包安全解压到指定目录。
    /// @param zipPath zip 文件路径。
    /// @param destinationRoot 解压目标根目录。
    /// @param errorMessage 失败时写入原因。
    /// @return 解压成功返回 true。
    static bool extractZipArchive(const std::filesystem::path& zipPath,
                                  const std::filesystem::path& destinationRoot,
                                  std::string&                 errorMessage);

    /// @brief 计算文件 SHA256。
    /// @param path 文件路径。
    /// @return 成功时返回 64 位小写十六进制 SHA256，失败时返回空字符串。
    static std::string sha256File(const std::filesystem::path& path);
};

}  // namespace MMM::Network
