#pragma once

#include <filesystem>
#include <iosfwd>
#include <string_view>

namespace MMM::Update
{
/// @brief 原生入口转交给同一安装流程，保持路径的原生字符类型。
int runUpdater(int argc, std::filesystem::path::value_type* argv[]);

/// @brief 写入更新器诊断，平台失败和共同流程使用同一输出通道。
void writeStderr(std::string_view message);

/// @brief 解析当前平台命令行中的正数 PID。无效返回 0。
long parseParentPid(
    std::basic_string_view<std::filesystem::path::value_type> text);

/// @brief 将参数规范化为实际安装目标，失败返回 false。macOS 会向上定位 .app。
bool normalizeTarget(std::filesystem::path& path);

/// @brief 有限等待父进程明确退出。
/// @warning 只允许独立更新器等待系统进程。
bool waitForParent(long pid);

/// @brief 从下载路径安装新版本。各平台处理暂存、恢复与清理。
bool install(const std::filesystem::path& source,
             const std::filesystem::path& target);

/// @brief 使用平台原生路径启动新版本。
bool launchTarget(const std::filesystem::path& target);

/// @brief 返回本次安装的成功标记路径。空路径表示跳过该标记。
std::filesystem::path updateSuccessMarkerPath(
    const std::filesystem::path& target);

/// @brief 按安装身份协议写出标记正文。
void writeMarkerContents(std::ostream&                marker,
                         const std::filesystem::path& target);
}  // namespace MMM::Update
