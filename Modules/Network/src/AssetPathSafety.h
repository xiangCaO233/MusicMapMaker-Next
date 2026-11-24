#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

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
         path.find(':') != std::string::npos ||
         path.find('\0') != std::string::npos ) {
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

/// @note 词法路径与文件系统归属各自独立。清单和 ZIP 均先校验名称，
/// 再通过下列函数检查安装树，避免把合法的相对字符串误当作安全文件位置。
/// @brief 解析用户指定的资源根；根本身可以位于用户配置的目录链接上。
inline std::filesystem::path resolveRoot(const std::filesystem::path& root,
                                         std::error_code&             error)
{
    // 先转绝对路径；首次安装还没有正式目录，weakly_canonical 对缺失尾段
    // 保留词法规范结果，但仅有相对路径时无法固定后续检查的基准。
    const auto absolute = std::filesystem::absolute(
        root.empty() ? std::filesystem::path(".") : root, error);
    if ( error ) return {};
    // 只解析调用方信任的根；条目内部的符号链接不参与该操作。
    // 如果根尚不存在，规范化尾段用于随后建立安装目录。
    return std::filesystem::weakly_canonical(absolute, error);
}

/// @brief 从可信根逐级检查目标，拒绝链接、重解析点和中间非目录。
/// @param root 已解析的可信安装根，不能从清单条目派生。
/// @param relative 已通过 UTF-8 路径词法验证的相对条目。
/// @param error 区分重定向、非目录与文件系统查询失败。
/// @return 已有节点均能直接访问，或目标尚未创建时为真。
/// @details 已存在的祖先按顺序检查，不存在的层级允许通过；缺失
/// 子树由安装器创建。在打开同卷暂存或发布之前调用，避免跟随安装内的链接。
inline bool isUnredirectedDestination(const std::filesystem::path& root,
                                      const std::filesystem::path& relative,
                                      std::error_code&             error)
{
    error.clear();
    if ( relative.empty() || relative.has_root_path() ) {
        error = std::make_error_code(std::errc::invalid_argument);
        return false;
    }
    // 遍历必须从根向下进行，直接查询深层叶子会在检查前穿过其祖先链接。
    // 逐级查询也能明确拒绝指向同一安装树其他位置的别名。
    auto current = root;
    for ( auto it = relative.begin(); it != relative.end(); ++it ) {
        if ( it->empty() || *it == "." || *it == ".." ) {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }
        current /= *it;
        // 这里只读路径元数据。早先的词法检查也保证不会越过可信根。
        const auto status = std::filesystem::symlink_status(current, error);
        // 查询对象自身，悬空链接会被识别为链接，和真正缺失区分。
        if ( error == std::errc::no_such_file_or_directory ) error.clear();
        if ( error ) return false;
        if ( !std::filesystem::exists(status) ) continue;
        // 检查叶子本身是因为复制或摘要读取也不能跟随重定向；
        // 父目录检查与普通文件类型检查各有独立失败条件。
        bool redirected = std::filesystem::is_symlink(status);
#if defined(_WIN32)
        // Windows junction 未必由标准库分类为链接，额外检查对象的重解析属性。
        const DWORD attributes = GetFileAttributesW(current.c_str());
        // Win32 查询失败时无法判定该项的类型，不能将它看作普通目录。
        if ( attributes == INVALID_FILE_ATTRIBUTES ) {
            error = std::error_code(static_cast<int>(GetLastError()),
                                    std::system_category());
            return false;
        }
        redirected = redirected || (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#endif
        if ( redirected ) {
            error =
                std::make_error_code(std::errc::too_many_symbolic_link_levels);
            return false;
        }
        // 叶子按归档或清单的发布规则处理，中间项须为可以进入的普通目录。
        auto next = it;
        if ( ++next != relative.end() &&
             !std::filesystem::is_directory(status) ) {
            error = std::make_error_code(std::errc::not_a_directory);
            return false;
        }
    }
    return true;
}

/// @note 纯词法算法也用于公开的差异查询；需要写入的入口还应检查真实祖先。
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
