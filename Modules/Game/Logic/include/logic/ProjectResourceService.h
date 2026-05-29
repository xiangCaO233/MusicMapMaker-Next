#pragma once

#include "logic/ProjectDirectoryScanner.h"
#include "mmm/project/Project.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace MMM
{
class BeatMap;
}

namespace MMM::Logic
{

/// @brief 根据项目目录扫描结果构建并同步项目内的谱面和音频资源列表。
class ProjectResourceService
{
public:
    /// @brief 项目目录资源同步后的结果。
    struct DirectorySyncResult {
        /// @brief 本次同步是否改变了项目资源列表或资源类型。
        bool m_changed{ false };

        /// @brief 新发现且需要由音频引擎预加载的音效资源列表。
        std::vector<AudioResource> m_effectResourcesToPreload;
    };

    /// @brief 根据初次目录扫描结果填充项目的谱面和音频资源列表。
    /// @param project 需要写入资源列表的项目实例。
    /// @param scanResult 项目目录扫描结果。
    void buildInitialResources(
        Project&                                   project,
        const ProjectDirectoryScanner::ScanResult& scanResult) const;

    /// @brief 根据项目排除列表过滤已经扫描出的谱面和音频资源。
    /// @param project 需要过滤资源列表的项目实例。
    void applyExcludedResources(Project& project) const;

    /// @brief 根据目录扫描结果同步已有项目的谱面和音频资源列表。
    /// @param project 需要同步资源列表的项目实例。
    /// @param scanResult 项目目录扫描结果。
    /// @return 同步是否改变项目，以及需要预加载的新增音效资源。
    DirectorySyncResult syncDirectoryResources(
        Project&                                   project,
        const ProjectDirectoryScanner::ScanResult& scanResult) const;

private:
    /// @brief 规范化项目相对路径，用于稳定比较排除列表。
    /// @param path UTF-8 编码的项目相对路径。
    /// @return 规范化后的 UTF-8 项目相对路径。
    static std::string normalizeProjectRelativePath(const std::string& path);

    /// @brief 判断路径是否存在于排除列表中。
    /// @param excludedPaths 项目排除列表。
    /// @param path 需要检查的 UTF-8 项目相对路径。
    /// @return 路径已被排除时返回 true。
    static bool containsExcludedPath(
        const std::vector<std::string>& excludedPaths, const std::string& path);

    /// @brief 将文件系统路径转换为 UTF-8 项目相对路径。
    /// @param project 路径所属项目。
    /// @param path 需要转换的文件系统路径。
    /// @return UTF-8 编码的项目相对路径。
    static std::string makeProjectRelativeUtf8(
        const Project& project, const std::filesystem::path& path);

    /// @brief 解析项目持久化路径为可访问的文件系统路径。
    /// @param project 路径所属项目。
    /// @param path 项目相对路径或绝对路径。
    /// @return 规范化后的文件系统路径。
    static std::filesystem::path resolveProjectPath(
        const Project& project, const std::filesystem::path& path);

    /// @brief 将文件系统路径转换为项目相对路径。
    /// @param project 路径所属项目。
    /// @param path 需要转换的文件系统路径。
    /// @return 项目相对路径；失败时退回文件名。
    static std::filesystem::path makeProjectRelativePath(
        const Project& project, const std::filesystem::path& path);

    /// @brief 在项目根目录和谱面目录之间解析元数据资源路径。
    /// @param project 路径所属项目。
    /// @param mapDirectory 谱面文件所在目录。
    /// @param path 元数据中记录的资源路径。
    /// @param preferProjectRoot 是否优先按项目根目录解析。
    /// @return 可访问优先的规范化资源路径。
    static std::filesystem::path resolveMetadataResourcePath(
        const Project& project, const std::filesystem::path& mapDirectory,
        const std::filesystem::path& path, bool preferProjectRoot);

    /// @brief 将谱面元数据中的长期资源路径规范化为项目相对路径。
    /// @param beatMap 需要规范化元数据路径的谱面。
    /// @param project 谱面所属项目。
    static void normalizeBeatmapMetadataPathsForProject(BeatMap&       beatMap,
                                                        const Project& project);

    /// @brief 尝试读取谱面主音轨并记录到主音轨路径集合。
    /// @param project 谱面所属项目。
    /// @param mapPath 需要读取的谱面文件路径。
    /// @param filename 谱面文件名，用于日志输出。
    /// @param mainAudioPaths 已识别的主音轨项目相对路径集合。
    /// @param warnOnFailure 读取失败时是否输出警告日志。
    /// @return 读取到主音轨时返回音轨 ID，否则返回空。
    static std::optional<std::string> probeMainAudioTrackId(
        const Project& project, const std::filesystem::path& mapPath,
        const std::string&               filename,
        std::unordered_set<std::string>& mainAudioPaths, bool warnOnFailure);

    /// @brief 创建默认音轨配置。
    /// @return 默认音轨配置。
    static AudioTrackConfig makeDefaultAudioConfig();

    /// @brief 创建项目音频资源条目。
    /// @param project 音频资源所属项目。
    /// @param audioPath 音频文件系统路径。
    /// @param mainAudioPaths 已识别的主音轨项目相对路径集合。
    /// @return 填充默认配置后的音频资源条目。
    static AudioResource createAudioResource(
        const Project& project, const std::filesystem::path& audioPath,
        const std::unordered_set<std::string>& mainAudioPaths);

    /// @brief 查找已有谱面条目。
    /// @param project 需要查询的项目。
    /// @param relativeMapPath UTF-8 编码的谱面项目相对路径。
    /// @return 找到时返回谱面条目副本，否则返回空。
    static std::optional<Project::BeatmapEntry> findExistingBeatmapEntry(
        const Project& project, const std::string& relativeMapPath);

    /// @brief 查找已有音频资源条目。
    /// @param project 需要查询的项目。
    /// @param relativeAudioPath UTF-8 编码的音频项目相对路径。
    /// @return 找到时返回音频资源副本，否则返回空。
    static std::optional<AudioResource> findExistingAudioResource(
        const Project& project, const std::string& relativeAudioPath);

    /// @brief 在没有谱面主音轨引用时，为项目资源设置兜底主音轨。
    /// @param project 需要设置兜底主音轨的项目。
    /// @param mainAudioPaths 已识别的主音轨项目相对路径集合。
    static void applyFallbackMainAudio(
        Project&                               project,
        const std::unordered_set<std::string>& mainAudioPaths);
};

}  // namespace MMM::Logic
