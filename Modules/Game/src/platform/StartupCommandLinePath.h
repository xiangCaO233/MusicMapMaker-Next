#pragma once

#include <filesystem>

namespace MMM
{

/// @brief 取得启动命令行中的首个路径参数。
/// @param argc C 运行时提供的参数数量，包含程序名。
/// @param argv C 运行时提供的参数数组，有效期覆盖本次启动调用。
/// @return 以平台原生字符保存的路径；缺少参数或平台解析失败时返回空路径。
/// @details 适配器只解析参数，路径检查和打开事件由应用启动流程负责。
std::filesystem::path readStartupCommandLinePath(int argc, char** argv);

}  // namespace MMM
