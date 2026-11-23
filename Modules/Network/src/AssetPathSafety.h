#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace MMM::Network::AssetPaths
{

/// @brief 判断相对资源路径是否安全。
/// @details
/// 禁止绝对路径、Windows 反斜杠/盘符、空段、`.` 与 `..` 段，确保同一清单在
/// 各平台解析为相同的项目内路径。该词法检查之后仍需结合实际根目录验证。
/// @par 拒绝样式
/// - `/absolute/path` 等 POSIX 绝对路径。
/// - `\\server` 或包含反斜杠的 Windows 路径。
/// - `C:/path` 或含冒号的驱动器、URI 形式。
/// - `a//b`、`a/` 等含空路径段的形式。
/// - `a/./b` 与 `a/../b` 等点段导航。
/// @par 允许样式
/// 只接受由非空普通段和正斜杠组成的相对资源路径；字符编码本身保持 UTF-8，
/// 由 Config::utf8ToPath 在最终平台路径组合时转换。
inline bool isSafeRelativeAssetPath(const std::string& path)
{
    if ( path.empty() || path.front() == '/' || path.front() == '\\' ) {
        // 空路径和两种平台绝对路径起始符不能映射到资源根内文件。
        return false;
    }
    if ( path.find('\\') != std::string::npos ||
         path.find(':') != std::string::npos ) {
        // 统一只接受正斜杠，并拒绝 Windows 驱动器或 URI 冒号。
        return false;
    }

    std::size_t start = 0;
    while ( start <= path.size() ) {
        // 逐段检查同时拒绝连续斜杠和末尾斜杠产生的空段。
        const std::size_t end     = path.find('/', start);
        const auto        segment = path.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        // Windows 会规范化尾空格和尾点；先拒绝这些别名再检查组合路径。
        if ( segment.empty() || segment.back() == ' ' ||
             segment.back() == '.' ) {
            return false;
        }
        if ( end == std::string::npos ) break;
        start = end + 1;
    }
    return true;
}

/// @brief 判断目标路径是否仍处在根目录内。
/// @details 两侧词法规范化后计算 target 相对 root 的路径；空结果、绝对结果或
/// 任一 `..` 段都表示目标不是严格根内子项。
inline bool isPathInsideRoot(const std::filesystem::path& root,
                             const std::filesystem::path& target)
{
    const auto normalizedRoot   = root.lexically_normal();
    const auto normalizedTarget = target.lexically_normal();
    const auto relativePath =
        normalizedTarget.lexically_relative(normalizedRoot);
    // 资源必须是根内具体文件，根目录本身不算合法目标。
    if ( relativePath.empty() || relativePath.is_absolute() ) return false;
    for ( const auto& part : relativePath ) {
        if ( part == ".." ) return false;
    }
    return true;
}

}  // namespace MMM::Network::AssetPaths
