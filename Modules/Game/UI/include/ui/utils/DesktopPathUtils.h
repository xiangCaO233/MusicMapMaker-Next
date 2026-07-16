#pragma once

#include <filesystem>

namespace MMM::UI::DesktopPathUtils
{

/// @brief 在系统文件管理器中打开目录或定位文件。
/// @param path 需要打开或定位的路径。
/// @param selectItem 路径为文件时是否请求文件管理器定位该文件。
/// @return 成功启动系统文件管理器时返回 true。
/// @warning
/// 低频用户操作路径：会访问文件系统并启动系统进程，禁止放入每帧调用链。
bool openInFileManager(const std::filesystem::path& path, bool selectItem);

}  // namespace MMM::UI::DesktopPathUtils
