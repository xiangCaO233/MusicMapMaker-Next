/// @file PGOProfiler.h
/// @brief PGO instrumentation 运行时 — profile 写入与异步上传
///
/// 仅在 -DMMM_PGO_INSTRUMENT=ON 构建中生效。
/// initPGOProfiler() 设置 profile 输出路径，
/// shutdownPGOProfiler() 强制写出 profile 并通过 curl 异步上传到服务器。

#pragma once

namespace MMM::Main
{

/// @brief 初始化 PGO profile 收集
///
/// 设置 profile 文件的输出目录和文件名 (含版本号、时间戳、PID)。
/// 应在 main() 中尽早调用。
void initPGOProfiler();

/// @brief 写出 profile 并异步上传
///
/// 调用 __llvm_profile_write_file() 强制写出 .profraw 文件，
/// 随后启动独立线程通过 HTTP multipart POST 上传到配置的服务器。
/// 应在 main() 返回前调用，不阻塞主线程退出。
void shutdownPGOProfiler();

}  // namespace MMM::Main
