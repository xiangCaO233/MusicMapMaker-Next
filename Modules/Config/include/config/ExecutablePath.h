#pragma once

#include <filesystem>

namespace MMM::Config
{

/// @brief 查询当前进程的可执行文件路径。
/// @return 成功时为绝对路径，系统查询失败或平台不支持时为空。
/// @note 可以在日志系统及业务单例初始化前调用。
/// @note Linux 和 macOS 尽量解析实际目标，避免依赖符号链接所在目录。
[[nodiscard]] std::filesystem::path currentExecutablePath();

}  // namespace MMM::Config
