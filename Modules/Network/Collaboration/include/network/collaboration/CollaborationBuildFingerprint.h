#pragma once

#include <string>
#include <string_view>

namespace MMM::Network::Collaboration
{
/// @brief 在进程入口固定当前主程序二进制的 SHA-256 构建指纹。
/// @return 成功读取并缓存当前运行映像对应文件时返回 true。
/// @warning
/// 仅允许在应用启动的低频路径调用一次；会读取完整主程序文件，必须先于可能替换磁盘
/// 上可执行文件的更新或构建流程执行。
[[nodiscard]] bool initializeCollaborationBuildFingerprint();

/// @brief 计算当前客户端主程序二进制的 SHA-256 构建指纹。
/// @return 成功时返回 64 位小写十六进制；无法定位或读取程序时返回空。
/// @warning
/// 主程序会在进程入口预先固定缓存；嵌入方若跳过初始化，本函数首次调用时会在低频
/// 路径读取完整主程序文件，禁止从 UI 热路径反复调用。
[[nodiscard]] const std::string& collaborationBuildFingerprint();

/// @brief 校验协作握手使用的 SHA-256 构建指纹格式。
/// @param fingerprint 待校验的指纹。
/// @return 仅当输入为 64 位小写十六进制时返回 true。
[[nodiscard]] bool isValidCollaborationBuildFingerprint(
    std::string_view fingerprint);
}  // namespace MMM::Network::Collaboration
