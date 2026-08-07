#pragma once

#include <string>
#include <string_view>

namespace MMM::Network::Collaboration::Detail
{

/// @brief 脱敏 libdatachannel 日志中的 TURN URI 长期凭据。
/// @param message libdatachannel 产生的原始日志文本。
/// @return 保留连接诊断信息但隐藏 TURN 用户名和密码的日志文本。
[[nodiscard]] std::string redactRtcDiagnosticMessage(std::string_view message);

}  // namespace MMM::Network::Collaboration::Detail
