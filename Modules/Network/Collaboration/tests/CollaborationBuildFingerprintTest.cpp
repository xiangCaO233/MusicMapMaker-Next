#include "network/collaboration/CollaborationBuildFingerprint.h"

#include "runtime/AppThreadPool.h"

#include <chrono>
#include <thread>

namespace
{
/// @brief 确保测试在线程池依赖的日志系统析构前完成显式关闭。
/// @note 守卫声明在 main 栈上，使所有提前返回路径执行相同清理。
struct AppThreadPoolShutdownGuard {
    /// @brief 待关闭的应用线程池单例。
    /// @note 引用不拥有线程池，单例生命周期覆盖整个测试进程。
    MMM::Runtime::AppThreadPool& pool;

    /// @brief 在 main 返回前关闭线程池，避免静态析构阶段继续写日志。
    ~AppThreadPoolShutdownGuard()
    {
        // shutdown 等待已调度指纹任务结束，保证测试不遗留后台工作。
        pool.shutdown();
    }
};
}  // namespace

/// @brief 验证构建指纹后台计算不会阻塞调用方并最终发布正确格式。
/// @return 调度、非阻塞读取、最终格式和幂等初始化均通过时返回零。
int main()
{
    using namespace MMM::Network::Collaboration;

    // 使用应用真实线程池，覆盖指纹初始化在正式运行时采用的调度路径。
    auto& appThreadPool = MMM::Runtime::AppThreadPool::instance();
    // 初始化发生在计时前，避免把线程池首次创建成本误算为调度阻塞。
    appThreadPool.init();
    // 守卫必须晚于 init 建立，确保之后每个错误码分支都完成关闭。
    AppThreadPoolShutdownGuard shutdownGuard{ appThreadPool };

    // 只测量初始化 API 的提交耗时，不等待后台 SHA-256 计算完成。
    const auto scheduleStartedAt = std::chrono::steady_clock::now();
    // 首次调用必须成功接受初始化请求，否则无法进入后台状态机。
    if ( !startCollaborationBuildFingerprintInitialization() ) return 1;
    const auto scheduleElapsed =
        std::chrono::steady_clock::now() - scheduleStartedAt;
    // 一秒上限宽于正常调度抖动，但能捕获错误的同步全量哈希实现。
    if ( scheduleElapsed >= std::chrono::seconds(1) ) return 2;

    // 调度返回后状态只允许仍在计算或已经快速完成。
    const auto initialState = collaborationBuildFingerprintState();
    if ( initialState != CollaborationBuildFingerprintState::Calculating &&
         initialState != CollaborationBuildFingerprintState::Ready ) {
        // Uninitialized 或 Failed 均表示调度未建立有效计算任务。
        return 3;
    }
    if ( initialState == CollaborationBuildFingerprintState::Calculating ) {
        // 计算期间读取缓存不得等待后台任务，也不得暴露部分指纹。
        const auto cachedReadStartedAt = std::chrono::steady_clock::now();
        if ( !collaborationBuildFingerprint().empty() ) return 4;
        const auto cachedReadElapsed =
            std::chrono::steady_clock::now() - cachedReadStartedAt;
        // 100ms 上限捕获锁等待，同时给慢速测试主机留出调度余量。
        if ( cachedReadElapsed >= std::chrono::milliseconds(100) ) return 5;
    }

    // 真实构建树文件较多，允许后台任务最多运行 30 秒后判定超时。
    constexpr auto FINGERPRINT_TIMEOUT = std::chrono::seconds(30);
    const auto     deadline =
        std::chrono::steady_clock::now() + FINGERPRINT_TIMEOUT;
    // 测试等待只发生在独立进程；5ms 轮询不会进入应用运行时路径。
    while ( collaborationBuildFingerprintState() ==
                CollaborationBuildFingerprintState::Calculating &&
            std::chrono::steady_clock::now() < deadline ) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // 超时、失败或未初始化状态都不能作为可用于协作握手的结果。
    if ( collaborationBuildFingerprintState() !=
         CollaborationBuildFingerprintState::Ready ) {
        return 6;
    }
    // Ready 状态必须对应固定格式的完整指纹，而不是空或部分字符串。
    if ( !isValidCollaborationBuildFingerprint(
             collaborationBuildFingerprint()) ) {
        // 格式验证覆盖固定 SHA-256 长度和十六进制字符约束。
        return 7;
    }
    // 重复初始化应幂等成功，不能再次启动竞争的后台任务。
    return startCollaborationBuildFingerprintInitialization() ? 0 : 8;
}
