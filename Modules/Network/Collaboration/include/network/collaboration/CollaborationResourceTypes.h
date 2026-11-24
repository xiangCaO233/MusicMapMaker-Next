#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace MMM
{
class Project;
}

namespace MMM::Network::Collaboration
{
/// @brief 协作项目资源同步阶段。
/// @details 房间驱动进度，UI 读取这些值以反馈当前资源同步状态。
/// 准备与缓存比对结束前，已完成字节数仅代表当前阶段的进度。
enum class CollaborationResourceSyncPhase {
    Idle,
    Preparing,
    WaitingManifest,
    ComparingCache,
    Downloading,
    Verifying,
    Ready,
    Error,
};

/// @brief UI 可直接读取的资源同步进度快照。
/// @details 文件计数和字节数属于生成快照时的同一进度样本。
/// 房主报告清单大小，访客只对仍需传输的内容累计下载字节。
struct CollaborationResourceSyncProgress {
    /// @brief 当前阶段。
    CollaborationResourceSyncPhase phase{
        CollaborationResourceSyncPhase::Idle
    };
    /// @brief 清单中的总文件数。
    std::uint32_t totalFiles = 0;
    /// @brief 已通过 SHA-256 校验的文件数。
    std::uint32_t completedFiles = 0;
    /// @brief 已完成本地缓存比对的文件数。
    std::uint32_t comparedFiles = 0;
    /// @brief 直接命中本地内容缓存的文件数。
    std::uint32_t cachedFiles = 0;
    /// @brief 访客缺失文件的总字节数；房主就绪时为清单总字节数。
    std::uint64_t totalBytes = 0;
    /// @brief 已接收并落入临时文件的字节数。
    std::uint64_t transferredBytes = 0;
    /// @brief 当前处理的缓存相对路径。
    std::string currentFile;
    /// @brief 错误或补充状态文本。
    std::string detail;
};

/// @brief 已完成并可绑定给访客会话的只读项目资源。
struct CollaborationResourceBundle {
    /// @brief 以内容缓存为根、只登记本谱引用音频的临时项目。
    std::shared_ptr<::MMM::Project> project;
    /// @brief 房主谱面相对路径到访客缓存相对路径的映射。
    std::unordered_map<std::string, std::string> pathRemap;
};

}  // namespace MMM::Network::Collaboration
