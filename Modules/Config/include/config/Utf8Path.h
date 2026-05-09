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
/// @details 在 Windows 上构造 path 时应传入 UTF-8 字符串，而非 ANSI 字符串。
///          例如从 JSON 配置中读出的路径字段应通过本函数转换后再拼合。
/// @param s UTF-8 编码的路径字符串
/// @return std::filesystem::path 对象
inline std::filesystem::path utf8ToPath(const std::string& s)
{
    return std::filesystem::path(reinterpret_cast<const char8_t*>(s.c_str()));
}

}  // namespace MMM::Config
