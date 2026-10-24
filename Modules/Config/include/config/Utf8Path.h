#pragma once

#include <filesystem>
#include <string>

namespace MMM::Config
{

/// @brief 将 std::filesystem::path 转换为 UTF-8 编码的 std::string
/// @details 在 Windows 上，path::string() 和 path::generic_string() 返回 ANSI
///          编码（随系统区域设置变化），中文路径会乱码。本函数始终返回 UTF-8
///          编码字符串，可安全用于日志、C API 调用、ImGui 显示等场景。
///          路径分隔符使用平台原生格式（Windows: \\，Linux/macOS: /）。
/// @param p 文件路径
/// @return UTF-8 编码的路径字符串（原生分隔符）
inline std::string pathToUtf8(const std::filesystem::path& p)
{
    auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
}

/// @brief 将 std::filesystem::path 转换为 UTF-8 编码的
/// std::string（泛用分隔符）
/// @details 与 pathToUtf8 类似，但路径分隔符统一为 /（正斜杠）。
///          适用于需要跨平台一致的字符串存储/比较场景。
/// @param p 文件路径
/// @return UTF-8 编码的路径字符串（/ 分隔符）
inline std::string pathToUtf8Generic(const std::filesystem::path& p)
{
    auto u8 = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
}

/// @brief 将 UTF-8 编码的 std::string 转换为 std::filesystem::path
/// @details 在 Windows 上，MSVC STL 支持通过 char8_t* 构造 UTF-8 路径，
///          而 libc++ 则需要通过 WideChar 转换。本函数为跨编译器和 STL
///          实现统一了此行为。
/// @param s UTF-8 编码的路径字符串
/// @return std::filesystem::path 对象
/// @note 平台转换实现在独立路径库中，公共头不引入 Windows SDK 或其宏。
std::filesystem::path utf8ToPath(const std::string& s);

}  // namespace MMM::Config
