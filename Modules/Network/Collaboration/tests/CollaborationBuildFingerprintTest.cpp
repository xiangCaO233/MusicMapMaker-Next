#include "network/collaboration/CollaborationBuildFingerprint.h"

#include "runtime/AppThreadPool.h"

#include <chrono>
#include <thread>

namespace
{
/// @brief 确保测试在线程池依赖的日志系统析构前完成显式关闭。
struct AppThreadPoolShutdownGuard {
    /// @brief 待关闭的应用线程池单例。
    MMM::Runtime::AppThreadPool& pool;

    /// @brief 在 main 返回前关闭线程池，避免静态析构阶段继续写日志。
    ~AppThreadPoolShutdownGuard() { pool.shutdown(); }
};
}  // namespace

/// @brief 验证构建指纹后台计算不会阻塞调用方并最终发布正确格式。
int main()
{
    using namespace MMM::Network::Collaboration;

    auto& appThreadPool = MMM::Runtime::AppThreadPool::instance();
    appThreadPool.init();
    AppThreadPoolShutdownGuard shutdownGuard{ appThreadPool };

    const auto scheduleStartedAt = std::chrono::steady_clock::now();
    if ( !startCollaborationBuildFingerprintInitialization() ) return 1;
    const auto scheduleElapsed =
        std::chrono::steady_clock::now() - scheduleStartedAt;
    if ( scheduleElapsed >= std::chrono::seconds(1) ) return 2;

    const auto initialState = collaborationBuildFingerprintState();
    if ( initialState != CollaborationBuildFingerprintState::Calculating &&
         initialState != CollaborationBuildFingerprintState::Ready ) {
        return 3;
    }
    if ( initialState == CollaborationBuildFingerprintState::Calculating ) {
        const auto cachedReadStartedAt = std::chrono::steady_clock::now();
        if ( !collaborationBuildFingerprint().empty() ) return 4;
        const auto cachedReadElapsed =
            std::chrono::steady_clock::now() - cachedReadStartedAt;
        if ( cachedReadElapsed >= std::chrono::milliseconds(100) ) return 5;
    }

    constexpr auto FINGERPRINT_TIMEOUT = std::chrono::seconds(30);
    const auto     deadline =
        std::chrono::steady_clock::now() + FINGERPRINT_TIMEOUT;
    while ( collaborationBuildFingerprintState() ==
                CollaborationBuildFingerprintState::Calculating &&
            std::chrono::steady_clock::now() < deadline ) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if ( collaborationBuildFingerprintState() !=
         CollaborationBuildFingerprintState::Ready ) {
        return 6;
    }
    if ( !isValidCollaborationBuildFingerprint(
             collaborationBuildFingerprint()) ) {
        return 7;
    }
    return startCollaborationBuildFingerprintInitialization() ? 0 : 8;
}
