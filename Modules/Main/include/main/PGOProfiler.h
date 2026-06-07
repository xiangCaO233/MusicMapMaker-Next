/// @file PGOProfiler.h
/// @brief PGO instrumentation 运行时 — profile 写入与异步上传
///
/// 仅在 -DMMM_PGO_INSTRUMENT=ON 构建中生效。
/// initPGOProfiler() 设置 profile 输出路径，
/// shutdownPGOProfiler() 强制写出 profile，并在运行时间达到阈值后上传到服务器。

#pragma once

namespace MMM::Main
{

/// @brief 初始化 PGO profile 收集
///
/// 设置 profile 文件的输出目录和文件名 (含版本号、时间戳、PID)。
/// 应在 main() 中尽早调用。
void initPGOProfiler();

/// @brief 写出 profile 并按运行时长阈值上传
///
/// 调用 __llvm_profile_write_file() 强制写出 .profraw 文件，
/// 随后通过 HTTP multipart POST 上传到配置的服务器。
/// 应在 main() 返回前调用；该函数位于退出低频路径，允许执行短时网络上传。
void shutdownPGOProfiler();

}  // namespace MMM::Main
