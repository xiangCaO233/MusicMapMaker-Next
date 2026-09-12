#pragma once

#include <algorithm>
#include <cctype>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>

/// @file ProjectResourceImport.h
/// @brief 新建谱面流程使用的资源扩展名分类与安全复制辅助函数。
/// @note 头文件内联实现供向导和拖放入口共享，所有错误通过返回值表达。

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
/// @param path 待分类的文件路径。
/// @return 音频、图片、视频或不支持类别。
/// @note 扩展名比较不区分 ASCII 大小写，复合后缀只检查最后一段。
inline ProjectResourceType classifyProjectResource(
    const std::filesystem::path& path)
{
    // 只读取路径字符串中的扩展名，不检查文件是否实际存在。
    auto extension = path.extension().string();
    // tolower 接收 unsigned char，避免负 char 引起未定义行为。
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if ( extension == ".mp3" || extension == ".ogg" || extension == ".wav" ||
         extension == ".flac" || extension == ".opus" || extension == ".aac" ||
         extension == ".m4a" )
        // 常见有损和无损音频统一作为主音频候选。
        return ProjectResourceType::Audio;
    if ( extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
         extension == ".bmp" )
        // 仅支持渲染管线已有解码能力的静态图片格式。
        return ProjectResourceType::Image;
    if ( extension == ".mp4" || extension == ".avi" || extension == ".mkv" ||
         extension == ".webm" || extension == ".mov" || extension == ".flv" ||
         extension == ".m4v" )
        // 常见视频容器统一交给后续媒体探针验证。
        return ProjectResourceType::Video;
    // 未知扩展名和无扩展名路径保持不支持。
    return ProjectResourceType::Unsupported;
}

/// @brief 将外部资源复制到项目，项目内文件直接复用，同名文件追加数字后缀。
/// @param projectRoot 目标项目根目录。
/// @param source 用户选择的源文件。
/// @return 成功时返回项目相对路径，失败时返回文件系统错误。
/// @warning 仅在用户确认导入的低频路径调用；执行文件系统检查和文件复制。
/// @details 项目根和源路径都先 canonical 化；源已位于项目内时不复制，
/// 外部源按原文件名复制，冲突时依次尝试 stem_1.ext、stem_2.ext。
inline std::expected<std::filesystem::path, std::error_code>
importProjectResource(const std::filesystem::path& projectRoot,
                      const std::filesystem::path& source)
{
    // 使用 error_code 版本保持无异常错误边界。
    std::error_code error;
    // canonical 同时验证项目根存在并生成稳定比较路径。
    const auto root = std::filesystem::canonical(projectRoot, error);
    if ( error ) return std::unexpected(error);
    // 源文件也解析符号链接后再判断是否已位于项目树内。
    const auto input = std::filesystem::canonical(source, error);
    if ( error ) return std::unexpected(error);
    if ( !std::filesystem::is_regular_file(input, error) ) {
        // 目录或特殊文件返回 invalid_argument，实际查询错误优先返回。
        return std::unexpected(
            error ? error : std::make_error_code(std::errc::invalid_argument));
    }
    // 项目内资源直接返回相对路径，避免无意义复制和同名改写。
    const auto relative = input.lexically_relative(root);
    if ( !relative.empty() && *relative.begin() != ".." &&
         !relative.is_absolute() ) {
        return relative;
    }
    auto filename = input.filename();
    for ( unsigned suffix = 1;; ++suffix ) {
        // copy_file 默认不覆盖，现有用户资源永远不会被替换。
        if ( std::filesystem::copy_file(input, root / filename, error) )
            return filename;
        // 只有名称冲突继续尝试，其他权限或 I/O 错误立即返回。
        if ( error != std::errc::file_exists ) return std::unexpected(error);
        error.clear();
        // 每次从原 stem 重建候选，避免累积 _1_2 后缀。
        filename = input.stem();
        filename += "_" + std::to_string(suffix);
        filename += input.extension();
    }
}
}  // namespace MMM::UI::Utils
