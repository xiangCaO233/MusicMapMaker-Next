#pragma once

#include <algorithm>
#include <cctype>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>

namespace MMM::UI::Utils
{
/// @brief 新建谱面支持导入的资源类别。
enum class ProjectResourceType {
    Unsupported,  ///< 不支持的扩展名。
    Audio,        ///< 主音频。
    Image,        ///< 封面或背景图片。
    Video         ///< 背景视频。
};

/// @brief 根据扩展名分类资源；仅在选择文件或拖放完成时调用，不访问磁盘。
inline ProjectResourceType classifyProjectResource(
    const std::filesystem::path& path)
{
    auto extension = path.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if ( extension == ".mp3" || extension == ".ogg" || extension == ".wav" ||
         extension == ".flac" || extension == ".opus" || extension == ".aac" ||
         extension == ".m4a" )
        return ProjectResourceType::Audio;
    if ( extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
         extension == ".bmp" )
        return ProjectResourceType::Image;
    if ( extension == ".mp4" || extension == ".avi" || extension == ".mkv" ||
         extension == ".webm" || extension == ".mov" || extension == ".flv" ||
         extension == ".m4v" )
        return ProjectResourceType::Video;
    return ProjectResourceType::Unsupported;
}

/// @brief 将外部资源复制到项目，项目内文件直接复用，同名文件追加数字后缀。
/// @return 成功时返回项目相对路径，失败时返回文件系统错误。
/// @warning 仅在用户确认导入的低频路径调用；执行文件系统检查和文件复制。
inline std::expected<std::filesystem::path, std::error_code>
importProjectResource(const std::filesystem::path& projectRoot,
                      const std::filesystem::path& source)
{
    std::error_code error;
    const auto      root = std::filesystem::canonical(projectRoot, error);
    if ( error ) return std::unexpected(error);
    const auto input = std::filesystem::canonical(source, error);
    if ( error ) return std::unexpected(error);
    if ( !std::filesystem::is_regular_file(input, error) ) {
        return std::unexpected(
            error ? error : std::make_error_code(std::errc::invalid_argument));
    }
    const auto relative = input.lexically_relative(root);
    if ( !relative.empty() && *relative.begin() != ".." &&
         !relative.is_absolute() ) {
        return relative;
    }
    auto filename = input.filename();
    for ( unsigned suffix = 1;; ++suffix ) {
        if ( std::filesystem::copy_file(input, root / filename, error) )
            return filename;
        if ( error != std::errc::file_exists ) return std::unexpected(error);
        error.clear();
        filename = input.stem();
        filename += "_" + std::to_string(suffix);
        filename += input.extension();
    }
}
}  // namespace MMM::UI::Utils
