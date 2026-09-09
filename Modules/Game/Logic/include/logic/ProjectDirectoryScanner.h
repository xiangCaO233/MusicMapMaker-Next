#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace MMM::Logic
{

/// @brief 扫描项目目录并按资源类型归类项目文件。
/// 只收集扩展名匹配的路径；资源内容解析、业务排除列表与去重由上层处理。
class ProjectDirectoryScanner
{
public:
    /// @brief 单次项目目录扫描返回的结果。
    struct ScanResult {
        /// @brief 目录遍历是否在没有文件系统错误的情况下完成。
        /// 中途迭代失败时为 false，文件列表可能仍含已发现的部分结果。
        bool m_success{ false };

        /// @brief 在项目根目录下发现的谱面文件列表。
        std::vector<std::filesystem::path> m_beatmapFiles;

        /// @brief 在项目根目录下发现的音频文件列表。
        std::vector<std::filesystem::path> m_audioFiles;
    };

    /// @brief 从项目根目录递归收集已知格式的谱面和音频文件。
    /// @param projectRoot 作为递归扫描起点的项目根目录。
    /// @return 按资源类型分组的文件列表以及扫描是否成功。
    /// @warning 同步递归 IO，应由项目加载或低频资源重扫调用，不在每帧执行。
    ScanResult scan(const std::filesystem::path& projectRoot) const;

    /// @brief 判断路径是否带有受支持的谱面扩展名。
    /// @param path 需要检查扩展名的文件系统路径。
    /// @return 扩展名表示受支持谱面格式时返回 true。
    static bool isBeatmapFile(const std::filesystem::path& path);

    /// @brief 判断路径是否带有受支持的音频扩展名。
    /// @param path 需要检查扩展名的文件系统路径。
    /// @return 扩展名表示受支持音频格式时返回 true。
    static bool isAudioFile(const std::filesystem::path& path);

private:
    /// @brief 将路径扩展名规范化为小写 UTF-8 字符串，便于稳定比较。
    /// @param path 需要规范化扩展名的文件系统路径。
    /// @return 带前导点的小写 UTF-8 扩展名字符串。
    static std::string normalizedExtension(const std::filesystem::path& path);
};

}  // namespace MMM::Logic
