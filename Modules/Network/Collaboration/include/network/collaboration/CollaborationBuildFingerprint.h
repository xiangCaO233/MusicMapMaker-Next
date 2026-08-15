#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace MMM::Network::Collaboration
{
/// @brief 当前主程序构建指纹的后台计算状态。
enum class CollaborationBuildFingerprintState : std::uint8_t {
    /// @brief 尚未提交后台任务。
    Uninitialized,
    /// @brief 后台线程正在读取并计算摘要。
    Calculating,
    /// @brief 指纹已经固定并可供只读复制。
    Ready,
    /// @brief 无法定位、读取或计算当前主程序摘要。
    Failed,
};

/// @brief 在共享线程池中启动当前主程序二进制 SHA-256 构建指纹计算。
/// @return 已经启动、正在计算或已经得到结果时返回 true；线程池不可用时返回
/// false。
/// @warning
/// 启动路径仅解析当前程序位置并投递后台任务，不读取程序内容、不等待计算完成；必须
/// 在共享线程池初始化后从应用启动路径调用，禁止放入 UI 热路径。
[[nodiscard]] bool startCollaborationBuildFingerprintInitialization();

/// @brief 查询当前主程序构建指纹的后台计算状态。
/// @return 当前状态快照。
/// @warning UI 热路径可每帧调用；仅执行一次 acquire 原子读取，不等待后台任务。
[[nodiscard]] CollaborationBuildFingerprintState
collaborationBuildFingerprintState();

/// @brief 非阻塞读取已经缓存的客户端主程序二进制 SHA-256 构建指纹。
/// @return 准备完成时返回 64 位小写十六进制；尚未完成或失败时返回空。
/// @warning
/// UI 热路径仅在状态为 Ready 后调用；acquire 状态发布保证只读缓存已经固定。
/// 函数只复制 64 字节结果，不会触发文件读取、互斥锁或等待后台计算。
[[nodiscard]] std::string collaborationBuildFingerprint();

/// @brief 校验协作握手使用的 SHA-256 构建指纹格式。
/// @param fingerprint 待校验的指纹。
/// @return 仅当输入为 64 位小写十六进制时返回 true。
[[nodiscard]] bool isValidCollaborationBuildFingerprint(
    std::string_view fingerprint);
}  // namespace MMM::Network::Collaboration
