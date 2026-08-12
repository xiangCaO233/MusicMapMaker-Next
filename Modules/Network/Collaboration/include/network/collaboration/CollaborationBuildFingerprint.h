#pragma once

#include <string>
#include <string_view>

namespace MMM::Network::Collaboration
{
/// @brief 计算当前客户端主程序二进制的 SHA-256 构建指纹。
/// @return 成功时返回 64 位小写十六进制；无法定位或读取程序时返回空。
/// @warning 仅在创建或加入房间的低频路径首次调用，会读取完整主程序文件；
/// 结果随后缓存在进程内，禁止从 UI 热路径反复计算。
[[nodiscard]] const std::string& collaborationBuildFingerprint();

/// @brief 校验协作握手使用的 SHA-256 构建指纹格式。
/// @param fingerprint 待校验的指纹。
/// @return 仅当输入为 64 位小写十六进制时返回 true。
[[nodiscard]] bool isValidCollaborationBuildFingerprint(
    std::string_view fingerprint);
}  // namespace MMM::Network::Collaboration
