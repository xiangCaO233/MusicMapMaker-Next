#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace MMM::Update
{
/// @brief 从应用包或包内可执行文件路径取得最接近的 .app 根。
/// @param path 应用包本身或其 Contents/MacOS 下的可执行文件。
/// @return 找不到包根时为空。
/// @note 仅做词法处理；调用者在需要文件身份时另行规范化。
/// 从叶节点向上遍历时使用最近的包根，使嵌套应用不能消费外层应用的事件。
inline std::filesystem::path macApplicationRoot(std::filesystem::path path)
{
    while ( !path.empty() ) {
        if ( path.extension() == ".app" ) return path;
        const auto parent = path.parent_path();
        // 根节点没有更上一级，不能让未打包的程序陷入循环。
        if ( parent == path ) break;
        path = parent;
    }
    return {};
}

/// @brief 由真实安装路径生成跨进程稳定的临时标记名。
/// @param app 存在的应用包根目录。
/// @param temporary 系统临时目录，测试可显式注入隔离目录。
/// @return 规范化失败时为空，调用方不能回退到共享写入位置。
/// @details 同名应用副本在不同目录时必须拥有独立文件名。
/// 两个进程可用同一路径的不同符号链接启动。真实路径保持双方使用同一标记。
inline std::filesystem::path macUpdateMarkerPath(
    const std::filesystem::path& app, const std::filesystem::path& temporary)
{
    if ( app.empty() || temporary.empty() ) return {};
    std::error_code error;
    const auto      identity = std::filesystem::canonical(app, error);
    if ( error ) return {};

    // FNV-1a 只生成稳定文件名，不能用哈希作为标记所属安装的最终判据。
    constexpr std::uint64_t OFFSET = 14695981039346656037ULL;
    constexpr std::uint64_t PRIME  = 1099511628211ULL;
    auto                    hash   = OFFSET;
    for ( unsigned char byte : identity.generic_u8string() ) {
        hash ^= byte;
        hash *= PRIME;
    }
    // 一个 64 位值最多需要十六个十六进制字符，无须终止符即可构造文件名。
    // 固定进制且不走区域格式化，更新器和主程序会生成相同字节。
    std::array<char, 16> hex{};
    const auto           converted =
        std::to_chars(hex.data(), hex.data() + hex.size(), hash, 16);
    return temporary /
           ("MusicMapMaker-Next." + std::string(hex.data(), converted.ptr) +
            ".mm_update_success");
}

/// @brief 仅消费实际属于当前安装副本的成功标记。
/// @param executable 启动程序的原生路径，可经过符号链接。
/// @param temporary 两端共享的系统临时目录。
/// @return 确认标记归属且成功消费时为真。
/// @details 优先检查新式独立标记；旧更新器写入全局文件，仍需验证包身份。
/// 哈希冲突、文件内容不完整或另一个安装的旧标记都不会触发成功提示。
inline bool consumeMacUpdateMarker(const std::filesystem::path& executable,
                                   const std::filesystem::path& temporary)
{
    namespace fs = std::filesystem;
    // 启动路径可能穿过整个应用包的符号链接，先解析再寻找 bundle 根。
    std::error_code error;
    const auto      realExecutable = fs::canonical(executable, error);
    if ( error ) return false;
    // 开发树的未打包可执行文件没有安装身份，不能消费任一正式安装的标记。
    const auto app       = macApplicationRoot(realExecutable);
    const auto ownMarker = macUpdateMarkerPath(app, temporary);
    if ( ownMarker.empty() ) return false;

    // 新旧标记顺序固定。共享旧文件验证失败后保留给真正的目标安装。
    for ( const auto& marker :
          { ownMarker, temporary / "MusicMapMaker-Next.mm_update_success" } ) {
        // 只读取有界的普通路径文本，保留路径间可能存在的换行字节。
        if ( !fs::is_regular_file(marker, error) || error ) continue;
        // 限额覆盖普通系统路径，损坏的大文件不得增加启动时的内存开销。
        constexpr std::size_t MAX_PATH_BYTES = 32768;
        const auto            size           = fs::file_size(marker, error);
        if ( error || size == 0 || size > MAX_PATH_BYTES ) continue;
        // 正文是 UTF-8 原生路径。按长度读取而非按行，合法路径允许包含换行。
        std::u8string text(static_cast<std::size_t>(size), u8'\0');
        {
            // 如果读取期间文件长度改变，丢弃本次观察而非消费部分写入的标记。
            std::ifstream stream(marker, std::ios::binary);
            if ( !stream.read(reinterpret_cast<char*>(text.data()),
                              static_cast<std::streamsize>(size)) ||
                 stream.peek() != std::char_traits<char>::eof() )
                continue;
        }
        // 旧更新器可能使用符号链接路径；按目录身份比较可以保留这一兼容性。
        // 拒绝内嵌 NUL，不能让系统路径调用只比较标记正文的前缀。
        if ( text.find(u8'\0') != std::u8string::npos ||
             !fs::equivalent(fs::path(text), app, error) || error )
            continue;
        // 流先关闭，文件系统才允许按名字删除已经验证的文件。
        // 两个同安装的进程同时启动时，只有成功删除的观察者报告一次更新。
        return fs::remove(marker, error) && !error;
    }
    return false;
}
}  // namespace MMM::Update
