/// @file PGOProfiler.h
/// @brief PGO instrumentation 运行时 — profile 写入与异步上传
///
/// 仅在 -DMMM_PGO_INSTRUMENT=ON 构建中生效。
/// initPGOProfiler() 设置 profile 输出路径，
/// shutdownPGOProfiler() 在用户允许时强制写出 profile，并按运行时间阈值上传。

#pragma once

#include <cstdint>
#include <string>

namespace MMM::Main
{

/// @brief PGO 退出上传阶段。
enum class PGOProfilerShutdownStage {
    Idle,       ///< 尚未开始。
    Writing,    ///< 正在写出 profraw 文件。
    Skipped,    ///< 已跳过上传。
    Uploading,  ///< 正在上传 profraw 文件。
    Succeeded,  ///< 上传成功。
    Failed      ///< 写出或上传失败。
};

/// @brief PGO 退出上传进度快照。
struct PGOProfilerShutdownProgress {
    /// @brief 当前退出上传阶段。
    PGOProfilerShutdownStage stage{ PGOProfilerShutdownStage::Idle };

    /// @brief 退出 PGO 流程是否已经结束。
    bool finished{ false };

    /// @brief 本次是否实际尝试上传。
    bool uploadAttempted{ false };

    /// @brief 用户配置是否允许上传。
    bool uploadAllowed{ false };

    /// @brief 本次运行时长，单位秒。
    long long runtimeSeconds{ 0 };

    /// @brief 允许上传的最短运行时长，单位秒。
    long long minRuntimeSeconds{ 600 };

    /// @brief 已上传字节数。
    std::uint64_t uploadedBytes{ 0 };

    /// @brief 预计上传总字节数；未知时为 0。
    std::uint64_t totalBytes{ 0 };

    /// @brief 当前状态说明。
    std::string message;
};

/// @brief 初始化 PGO profile 收集
///
/// 设置 profile 文件的输出目录和文件名 (含版本号、时间戳、PID)。
/// 应在 main() 中尽早调用。
void initPGOProfiler();

/// @brief 写出 profile，并在允许时按运行时长阈值同步上传。
///
/// @param uploadAllowed 用户是否允许自动上传 profile。
/// 调用 __llvm_profile_write_file() 强制写出 .profraw 文件，
/// 随后通过 HTTP multipart POST 上传到配置的服务器。
/// 应在 main() 返回前调用；该函数位于退出低频路径，允许执行短时网络上传。
void shutdownPGOProfiler(bool uploadAllowed);

/// @brief 写出 profile，并在需要上传时启动后台上传任务。
/// @param uploadAllowed 用户是否允许自动上传 profile。
/// @return 已启动后台上传任务时返回 true；无需展示进度窗口时返回 false。
bool beginShutdownPGOProfilerAsync(bool uploadAllowed);

/// @brief 获取 PGO 退出上传进度快照。
/// @return 当前进度状态。
PGOProfilerShutdownProgress getPGOProfilerShutdownProgress();

/// @brief 判断 PGO 退出流程是否已经结束。
/// @return 已结束时返回 true。
bool isShutdownPGOProfilerFinished();

/// @brief 等待 PGO 退出上传后台任务结束。
void waitForShutdownPGOProfiler();

}  // namespace MMM::Main
