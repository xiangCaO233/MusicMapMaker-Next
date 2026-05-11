#pragma once

#include <filesystem>
#include <string>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

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
inline std::filesystem::path utf8ToPath(const std::string& s)
{
#ifdef _WIN32
    // 将 UTF-8 转换为宽字符串，再用 wchar_t* 构造 path。
    // 这对 MSVC STL 和 libc++ on Windows 均有效：
    //   - MSVC STL: 原生使用 wchar_t 存储路径
    //   - libc++: 内部调用 __char_to_wide，wchar_t* 构造不会触发窄字符编码猜测
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if ( len <= 0 ) {
        return {};
    }
    std::wstring wstr(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, wstr.data(), len);
    return std::filesystem::path(wstr);
#else
    return std::filesystem::path(s);
#endif
}

}  // namespace MMM::Config
