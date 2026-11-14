#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace MMM::Main
{
/// @brief 各平台本地原始采样缓存的默认字节上限，不包含上传服务的数据。
inline constexpr std::uintmax_t PGO_PROFILE_CACHE_MAX_BYTES =
    100ULL * 1024 * 1024;

/// @brief 单次缓存整理的统计；失败不通过异常传播到启动或退出流程。
struct PGOProfileCacheCleanupResult {
    /// @brief 成功枚举并统计的普通 profraw 文件总字节数。
    std::uintmax_t m_bytesBefore{};
    /// @brief 扣除成功删除项后的已知剩余字节数。
    std::uintmax_t m_bytesAfter{};
    /// @brief 实际成功删除的文件数，跳过的活动文件不计入。
    std::size_t m_removedFiles{};
    /// @brief 枚举、元数据、删除失败或并发变更导致的未完成操作数。
    std::size_t m_errors{};
};

/// @brief 超限时按修改时间淘汰最旧的本地 profraw，直到严格低于上限。
/// @param directory 应用独占的缓存目录；缺失时不创建目录。
/// @param protectedProfile 正在写入或上传的文件；空路径表示无需保护。
/// @param maxBytes 正数容量上限，参数化用于小体积边界回归。
/// @param forceBelowLimit 上轮因活动文件停在上限时，释放保护后继续严格淘汰。
/// @return 最佳努力清理的统计；保护文件过大或文件系统失败时可能仍超限。
/// @details 只处理目录直属的普通 .profraw，不跟随符号链接或递归子目录。
/// 恰好等于上限不会启动淘汰；已经超限后必须清理到严格小于上限。
/// 删除前复核类型、大小和修改时间，避免扫描后更新的文件被当作旧数据删除。
/// @warning 启动和退出低频路径：包含目录枚举、排序和删除，禁止在帧循环调用。
PGOProfileCacheCleanupResult trimPGOProfileCache(
    const std::filesystem::path& directory,
    const std::filesystem::path& protectedProfile = {},
    std::uintmax_t               maxBytes         = PGO_PROFILE_CACHE_MAX_BYTES,
    bool                         forceBelowLimit  = false);
}  // namespace MMM::Main
