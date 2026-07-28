#pragma once

#include "logic/ProjectDirectoryScanner.h"
#include "mmm/project/Project.h"

#include <nlohmann/json.hpp>

#include <cstddef>
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

/// @brief 谱面中音频资源引用的用途。
enum class BeatmapAudioReferenceKind {
    SongFileHint,
    NoteSampleBinding,
    AudioSampleEvent,
};

/// @brief 一个可追溯到具体谱面和字段用途的音频资源引用。
struct BeatmapAudioReference {
    /// @brief 引用所在谱面的项目相对路径。
    std::string m_beatmapPath;

    /// @brief 谱面字段中保存的资源 ID 或路径。
    std::string m_audioReference;

    /// @brief 引用用途。
    BeatmapAudioReferenceKind m_kind{
        BeatmapAudioReferenceKind::AudioSampleEvent
    };
};

/// @brief 根据项目目录扫描结果构建并同步项目内的谱面和音频资源列表。
class ProjectResourceService
{
public:
    /// @brief 项目目录资源同步后的结果。
    struct DirectorySyncResult {
        /// @brief 本次同步是否改变了项目资源列表或资源类型。
        bool m_changed{ false };

        /// @brief 新发现且需要登记到音频引擎的按需加载音效资源列表。
        std::vector<AudioResource> m_effectResourcesToRegister;
    };

    /// @brief 旧项目单主音轨字段迁移结果。
    struct LegacyAudioMigrationResult {
        /// @brief 成功物化自动采样并写回的旧版 MMM 谱面数量。
        std::size_t m_migratedBeatmapCount{ 0 };

        /// @brief 无法定位资源、读取或写回的旧版 MMM 谱面路径。
        std::vector<std::string> m_failedBeatmapPaths;
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

    /// @brief 收集缺少当前 m_config 对象的旧版音频资源键。
    /// @param projectJson 项目描述 JSON。
    /// @return 优先使用资源路径、路径缺失时使用 ID 的旧版资源键集合。
    static std::unordered_set<std::string> collectLegacyAudioResourceKeys(
        const nlohmann::json& projectJson);

    /// @brief 将持久化音频配置合并到本次目录扫描得到的资源列表。
    /// @param project 以目录扫描结果为基础的项目实例。
    /// @param persistedProject 从项目描述文件读取的持久化项目。
    /// @param legacyAudioResourceKeys 需要保留扫描音轨类型的旧版资源键。
    void mergePersistedAudioResources(
        Project& project, const Project& persistedProject,
        const std::unordered_set<std::string>& legacyAudioResourceKeys) const;

    /// @brief 根据目录扫描结果同步已有项目的谱面和音频资源列表。
    /// @param project 需要同步资源列表的项目实例。
    /// @param scanResult 项目目录扫描结果。
    /// @return 同步是否改变项目，以及需要预加载的新增音效资源。
    DirectorySyncResult syncDirectoryResources(
        Project&                                   project,
        const ProjectDirectoryScanner::ScanResult& scanResult) const;

    /// @brief 查找指定项目音频资源在全部谱面中的引用。
    /// @param project 待扫描的项目。
    /// @param resource 待匹配的项目音频资源。
    /// @return 按谱面和用途记录的引用列表。
    static std::vector<BeatmapAudioReference> findAudioResourceReferences(
        const Project& project, const AudioResource& resource);

    /// @brief 按谱面引用解析项目音频资源。
    /// @param project 待查询项目。
    /// @param beatmapPath 引用所在谱面的项目相对或绝对路径。
    /// @param audioReference 谱面保存的资源 ID 或路径。
    /// @return 匹配到的项目资源；未找到时返回空。
    static const AudioResource* findAudioResourceForReference(
        const Project& project, const std::filesystem::path& beatmapPath,
        const std::string& audioReference);

    /// @brief 为谱面选择适合预览或 BPM 测量的默认音频资源。
    /// @param project 谱面所属项目。
    /// @param beatMap 待解析的谱面。
    /// @param beatmapPath 谱面的项目相对或绝对路径。
    /// @return 优先匹配歌曲提示和 Main 自动采样的项目资源。
    static const AudioResource* findDefaultBeatmapAudioResource(
        const Project& project, const BeatMap& beatMap,
        const std::filesystem::path& beatmapPath);

    /// @brief 将旧项目条目的单主音轨迁移为 MMM v2 自动采样。
    /// @param project 当前目录扫描和资源合并后的项目。
    /// @param persistedProject 从旧项目描述文件读取的项目。
    /// @return 成功迁移数量和失败谱面路径。
    LegacyAudioMigrationResult migrateLegacyBeatmapAudioTracks(
        Project& project, const Project& persistedProject) const;

    /// @brief 文件移动或重命名后重映射项目音频资源路径并保持资源 ID 稳定。
    /// @param project 待更新项目。
    /// @param oldPath 移动前的文件或目录路径。
    /// @param newPath 移动后的文件或目录路径。
    /// @return 路径发生变化的音频资源数量。
    static std::size_t remapAudioResourcePathsAfterMove(
        Project& project, const std::filesystem::path& oldPath,
        const std::filesystem::path& newPath);

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

    /// @brief 读取谱面并收集歌曲提示、玩家物件绑定和自动采样引用。
    /// @param project 谱面所属项目。
    /// @param mapPath 需要读取的谱面文件路径。
    /// @param beatmapPath 谱面用于诊断的项目相对路径。
    /// @param warnOnFailure 读取失败时是否输出警告日志。
    /// @return 谱面中的全部音频引用。
    static std::vector<BeatmapAudioReference> probeBeatmapAudioReferences(
        const Project& project, const std::filesystem::path& mapPath,
        const std::string& beatmapPath, bool warnOnFailure);

    /// @brief 判断谱面音频引用是否指向指定项目资源。
    /// @param project 资源所属项目。
    /// @param reference 待匹配的谱面引用。
    /// @param resource 候选项目音频资源。
    /// @return ID、项目相对路径或旧版文件名能够匹配时返回 true。
    static bool audioReferenceMatchesResource(
        const Project& project, const BeatmapAudioReference& reference,
        const AudioResource& resource);

    /// @brief 根据谱面引用推断新发现音频资源的类型。
    /// @param project 资源所属项目。
    /// @param references 已收集的全部谱面音频引用。
    /// @param resource 待推断的项目音频资源。
    /// @return Note 绑定优先的资源类型；没有类型线索时返回 Effect。
    static AudioTrackType inferAudioResourceType(
        const Project&                            project,
        const std::vector<BeatmapAudioReference>& references,
        const AudioResource&                      resource);

    /// @brief 创建默认音轨配置。
    /// @return 默认音轨配置。
    static AudioTrackConfig makeDefaultAudioConfig();

    /// @brief 创建项目音频资源条目。
    /// @param project 音频资源所属项目。
    /// @param audioPath 音频文件系统路径。
    /// @param references 已收集的全部谱面音频引用。
    /// @return 填充默认配置后的音频资源条目。
    static AudioResource createAudioResource(
        const Project& project, const std::filesystem::path& audioPath,
        const std::vector<BeatmapAudioReference>& references);

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
};

}  // namespace MMM::Logic
