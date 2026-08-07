#pragma once

namespace MMM::Network::Collaboration
{

/// @brief 启用或关闭 libdatachannel 的 WebRTC/ICE 底层诊断日志。
/// @param enabled 开启时记录 Debug 级别日志，关闭时停止底层日志回调。
void setRtcDiagnosticLoggingEnabled(bool enabled);

}  // namespace MMM::Network::Collaboration
