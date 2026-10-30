#pragma once

#include <filesystem>

namespace MMM
{
/// @brief 判断项目相对路径是否属于内部持久化目录，不能作为发布资源。
/// @param relativePath 项目根目录下的文件或目录，调用方另外执行越界检查。
/// @return 首段为保留的 .mmm 目录时为真，普通 chart.mmm 文件仍可打包。
/// @details 自动备份保存在 .mmm/backups；只按扩展名扫描会把历史副本当难度。
/// 目录级规则同时供 UI 扫描和实际打包预检使用，不能仅取消默认勾选。
/// @note 纯路径判断，不枚举文件，也不解析符号链接；用于低频打包准备。
[[nodiscard]] inline bool isInternalPackagePath(
    const std::filesystem::path& relativePath)
{
    const auto normalized = relativePath.lexically_normal();
    // 仅根目录下的保留目录属于项目内部状态；不误伤普通文件名中的 .mmm。
    return !normalized.empty() && *normalized.begin() == ".mmm";
}
}  // namespace MMM
