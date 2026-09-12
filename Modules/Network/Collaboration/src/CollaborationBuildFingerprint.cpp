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
/// @note 仅后台任务在发布 Ready 前写入，之后所有线程只读。
std::string s_fingerprint;

/// @brief 将 SHA-256 摘要格式化为小写十六进制字符串。
/// @param digest 32 字节 SHA-256 摘要。
/// @return 64 位小写十六进制字符串。
[[nodiscard]] std::string formatSha256(
    const std::array<std::uint8_t, 32>& digest)
{
    // 查表格式化避免流式格式器的区域设置、分配和大小写差异。
    constexpr char HEX_DIGITS[] = "0123456789abcdef";
    // SHA-256 每字节固定展开为两个字符，结果长度始终为 64。
    std::string result(digest.size() * 2U, '0');
    for ( std::size_t index = 0; index < digest.size(); ++index ) {
        // 高四位和低四位分别映射，保证字节前导零不会丢失。
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
    // 二进制模式避免 Windows 文本换行转换造成跨平台摘要偏差。
    std::ifstream input(executablePath, std::ios::binary);
    // 路径不存在或权限不足均以空结果交由状态机统一标记失败。
    if ( !input ) return {};

    // mbedTLS 上下文要求无论后续步骤成功与否都显式释放。
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    // 第二个参数为零，明确选择 SHA-256 而非 SHA-224。
    if ( mbedtls_sha256_starts(&context, 0) != 0 ) {
        mbedtls_sha256_free(&context);
        return {};
    }

    // 固定块缓冲位于后台任务栈上，不随可执行文件大小增长。
    std::array<std::uint8_t, FINGERPRINT_READ_BLOCK_BYTES> block{};
    bool                                                   success = true;
    // 依赖流状态终止循环，同时保留末尾不足整块的数据。
    while ( input ) {
        input.read(reinterpret_cast<char*>(block.data()),
                   static_cast<std::streamsize>(block.size()));
        const auto count = input.gcount();
        // EOF 前最后一次 read 可同时设置 failbit 并返回有效尾块。
        if ( count > 0 &&
             mbedtls_sha256_update(&context,
                                   block.data(),
                                   static_cast<std::size_t>(count)) != 0 ) {
            // 摘要库失败后不再读取文件，但仍走统一上下文释放路径。
            success = false;
            break;
        }
    }

    // digest 只在完整读取并成功 finalize 后具有可发布语义。
    std::array<std::uint8_t, 32> digest{};
    // badbit 表示真实 I/O 错误；普通 EOF/failbit 不应误判最后一块失败。
    if ( input.bad() || !success ||
         mbedtls_sha256_finish(&context, digest.data()) != 0 ) {
        success = false;
    }
    // 在格式化或返回空结果前释放 mbedTLS 内部状态。
    mbedtls_sha256_free(&context);
    // 空字符串是后台状态机识别失败的唯一计算结果。
    return success ? formatSha256(digest) : std::string{};
}

/// @brief 执行后台指纹计算并发布不可变结果。
/// @param executablePath 启动任务时固定的主程序路径。
void initializeFingerprintStorage(std::filesystem::path executablePath)
{
    // 路径按值传入，避免启动线程的临时对象在线程执行前失效。
    auto fingerprint = calculateFingerprint(executablePath);
    // 除计算错误外也验证格式，禁止发布不完整或异常摘要。
    if ( !isValidCollaborationBuildFingerprint(fingerprint) ) {
        // 后台写入单调地终止于 Failed；UI 只在 acquire 读到 Ready 后访问缓存。
        s_fingerprintState.store(CollaborationBuildFingerprintState::Failed,
                                 std::memory_order_release);
        XERROR("Failed to calculate collaboration client build fingerprint");
        return;
    }
    // 字符串先完整移动到进程缓存，再通过 release 状态公开可见性。
    s_fingerprint = std::move(fingerprint);
    // release 发布指纹字符串的最终内容，之后字符串保持只读直至进程退出。
    s_fingerprintState.store(CollaborationBuildFingerprintState::Ready,
                             std::memory_order_release);
    XINFO("Collaboration client build fingerprint: {}",
          collaborationBuildFingerprint().substr(0, 12));
}
}  // namespace

/// @brief 幂等地把当前可执行文件的指纹计算提交到应用线程池。
/// @return 已提交或已有状态时返回 true；线程池尚不可用时返回 false。
/// @warning 启动阶段低频调用；函数本身不读文件，后台任务负责所有 I/O。
bool startCollaborationBuildFingerprintInitialization()
{
    // acquire 与后台 release 配对；已进入任何终态时都保持幂等。
    auto state = s_fingerprintState.load(std::memory_order_acquire);
    if ( state != CollaborationBuildFingerprintState::Uninitialized ) {
        return true;
    }

    // 指纹任务依赖应用统一线程池；未初始化时由调用方稍后重试。
    auto* appThreadPool = Runtime::AppThreadPool::instance().get();
    if ( !appThreadPool ) return false;

    // 在调用线程只解析当前程序路径，不进行任何文件内容读取。
    const auto executablePath = Network::UpdateChecker::currentExecutablePath();
    if ( executablePath.empty() ) {
        // 无可恢复路径可供同一进程重试，因此直接发布 Failed 终态。
        s_fingerprintState.store(CollaborationBuildFingerprintState::Failed,
                                 std::memory_order_release);
        XERROR(
            "Failed to locate executable for collaboration build fingerprint");
        return true;
    }

    // CAS 保证并发启动者中只有一个负责向线程池提交任务。
    if ( !s_fingerprintState.compare_exchange_strong(
             state,
             CollaborationBuildFingerprintState::Calculating,
             std::memory_order_acq_rel,
             std::memory_order_acquire) ) {
        // 竞争失败表示其他调用已经推进状态，当前调用仍视为成功。
        return true;
    }
    // 捕获转换后的路径值，后台任务不依赖临时 UTF-8 字符串生命周期。
    appThreadPool->enqueue_void([path = Config::utf8ToPath(executablePath)]() {
        initializeFingerprintStorage(path);
    });
    // 返回只表示任务已提交；结果必须通过状态查询异步观察。
    return true;
}

/// @brief 以 acquire 语义读取后台指纹状态快照。
/// @return 当前单调状态；Ready 同时保证缓存字符串可见。
CollaborationBuildFingerprintState collaborationBuildFingerprintState()
{
    return s_fingerprintState.load(std::memory_order_acquire);
}

/// @brief 在 Ready 状态下复制已冻结的构建指纹。
/// @return 未完成或失败时返回空，完成时返回 64 位摘要副本。
std::string collaborationBuildFingerprint()
{
    // 状态的 acquire 读取既避免等待，也同步后台字符串写入。
    if ( collaborationBuildFingerprintState() !=
         CollaborationBuildFingerprintState::Ready ) {
        return {};
    }
    // Ready 发布后缓存不再修改，因此复制过程无需额外互斥锁。
    return s_fingerprint;
}

/// @brief 校验握手指纹是否符合固定的小写 SHA-256 文本格式。
/// @param fingerprint 待检查的指纹视图。
/// @return 长度与所有字符均合法时返回 true。
bool isValidCollaborationBuildFingerprint(std::string_view fingerprint)
{
    // 严格限制小写字符，避免同一摘要产生多种线协议文本表示。
    return fingerprint.size() == 64U &&
           std::all_of(fingerprint.begin(), fingerprint.end(), [](char value) {
               // 直接范围比较不依赖区域设置，也不接受大写 A-F。
               return (value >= '0' && value <= '9') ||
                      (value >= 'a' && value <= 'f');
           });
}
}  // namespace MMM::Network::Collaboration
