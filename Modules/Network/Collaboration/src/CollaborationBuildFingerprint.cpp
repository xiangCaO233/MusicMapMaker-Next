#include "network/collaboration/CollaborationBuildFingerprint.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "network/UpdateChecker.h"
#include "runtime/AppThreadPool.h"

#include <ice/thread/ThreadPool.hpp>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <utility>

namespace MMM::Network::Collaboration
{
namespace
{
/// @brief 单次后台计算读取的块大小，限制构建指纹任务的峰值内存。
constexpr std::size_t FINGERPRINT_READ_BLOCK_BYTES = 256U * 1024U;

/// @brief 构建指纹后台计算状态。
/// @warning 后台任务写入，启动线程与 UI 线程读取；状态发布使用 release/acquire
/// 保证 Ready 对缓存内容的可见性，避免任何等待。
std::atomic<CollaborationBuildFingerprintState> s_fingerprintState{
    CollaborationBuildFingerprintState::Uninitialized
};
/// @brief 已完成计算的主程序二进制 SHA-256。
std::string s_fingerprint;

/// @brief 将 SHA-256 摘要格式化为小写十六进制字符串。
/// @param digest 32 字节 SHA-256 摘要。
/// @return 64 位小写十六进制字符串。
[[nodiscard]] std::string formatSha256(
    const std::array<std::uint8_t, 32>& digest)
{
    constexpr char HEX_DIGITS[] = "0123456789abcdef";
    std::string    result(digest.size() * 2U, '0');
    for ( std::size_t index = 0; index < digest.size(); ++index ) {
        result[index * 2U]      = HEX_DIGITS[digest[index] >> 4U];
        result[index * 2U + 1U] = HEX_DIGITS[digest[index] & 0x0FU];
    }
    return result;
}

/// @brief 分块读取当前主程序并计算二进制 SHA-256。
/// @param executablePath 启动任务时固定的主程序路径。
/// @return 成功时返回 64 位小写十六进制；读取或摘要失败时返回空。
/// @warning 后台任务路径；允许顺序文件读取和加密摘要计算，禁止访问 UI 状态。
[[nodiscard]] std::string calculateFingerprint(
    const std::filesystem::path& executablePath)
{
    std::ifstream input(executablePath, std::ios::binary);
    if ( !input ) return {};

    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    if ( mbedtls_sha256_starts(&context, 0) != 0 ) {
        mbedtls_sha256_free(&context);
        return {};
    }

    std::array<std::uint8_t, FINGERPRINT_READ_BLOCK_BYTES> block{};
    bool                                                   success = true;
    while ( input ) {
        input.read(reinterpret_cast<char*>(block.data()),
                   static_cast<std::streamsize>(block.size()));
        const auto count = input.gcount();
        if ( count > 0 &&
             mbedtls_sha256_update(&context,
                                   block.data(),
                                   static_cast<std::size_t>(count)) != 0 ) {
            success = false;
            break;
        }
    }

    std::array<std::uint8_t, 32> digest{};
    if ( input.bad() || !success ||
         mbedtls_sha256_finish(&context, digest.data()) != 0 ) {
        success = false;
    }
    mbedtls_sha256_free(&context);
    return success ? formatSha256(digest) : std::string{};
}

/// @brief 执行后台指纹计算并发布不可变结果。
/// @param executablePath 启动任务时固定的主程序路径。
void initializeFingerprintStorage(std::filesystem::path executablePath)
{
    auto fingerprint = calculateFingerprint(executablePath);
    if ( !isValidCollaborationBuildFingerprint(fingerprint) ) {
        // 后台写入单调地终止于 Failed；UI 只在 acquire 读到 Ready 后访问缓存。
        s_fingerprintState.store(CollaborationBuildFingerprintState::Failed,
                                 std::memory_order_release);
        XERROR("Failed to calculate collaboration client build fingerprint");
        return;
    }
    s_fingerprint = std::move(fingerprint);
    // release 发布指纹字符串的最终内容，之后字符串保持只读直至进程退出。
    s_fingerprintState.store(CollaborationBuildFingerprintState::Ready,
                             std::memory_order_release);
    XINFO("Collaboration client build fingerprint: {}",
          collaborationBuildFingerprint().substr(0, 12));
}
}  // namespace

bool startCollaborationBuildFingerprintInitialization()
{
    auto state = s_fingerprintState.load(std::memory_order_acquire);
    if ( state != CollaborationBuildFingerprintState::Uninitialized ) {
        return true;
    }

    auto* appThreadPool = Runtime::AppThreadPool::instance().get();
    if ( !appThreadPool ) return false;

    const auto executablePath = Network::UpdateChecker::currentExecutablePath();
    if ( executablePath.empty() ) {
        s_fingerprintState.store(CollaborationBuildFingerprintState::Failed,
                                 std::memory_order_release);
        XERROR(
            "Failed to locate executable for collaboration build fingerprint");
        return true;
    }

    if ( !s_fingerprintState.compare_exchange_strong(
             state,
             CollaborationBuildFingerprintState::Calculating,
             std::memory_order_acq_rel,
             std::memory_order_acquire) ) {
        return true;
    }
    appThreadPool->enqueue_void([path = Config::utf8ToPath(executablePath)]() {
        initializeFingerprintStorage(path);
    });
    return true;
}

CollaborationBuildFingerprintState collaborationBuildFingerprintState()
{
    return s_fingerprintState.load(std::memory_order_acquire);
}

std::string collaborationBuildFingerprint()
{
    if ( collaborationBuildFingerprintState() !=
         CollaborationBuildFingerprintState::Ready ) {
        return {};
    }
    return s_fingerprint;
}

bool isValidCollaborationBuildFingerprint(std::string_view fingerprint)
{
    return fingerprint.size() == 64U &&
           std::all_of(fingerprint.begin(), fingerprint.end(), [](char value) {
               return (value >= '0' && value <= '9') ||
                      (value >= 'a' && value <= 'f');
           });
}
}  // namespace MMM::Network::Collaboration
