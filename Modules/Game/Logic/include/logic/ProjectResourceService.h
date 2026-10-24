#pragma once

#include "logic/ProjectDirectoryScanner.h"
#include "logic/ProjectResourceTypes.h"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace MMM
{
class BeatMap;
class Project;
}  // namespace MMM

namespace MMM::Logic
{

/// @brief 根据项目目录扫描结果构建并同步项目内的谱面和音频资源列表。
class ProjectResourceService
{
public:
    /// @brief 保留资源服务结果的嵌套名称，沿用独立 DTO 的值语义。
    using DirectorySyncResult = MMM::Logic::DirectorySyncResult;

    /// @brief 保留资源服务结果的嵌套名称，沿用独立 DTO 的值语义。
    using LegacyAudioMigrationResult = MMM::Logic::LegacyAudioMigrationResult;

    /// @brief 根据初次目录扫描结果填充项目的谱面和音频资源列表。
    /// @param project 需要写入资源列表的项目实例。
    /// @param scanResult 项目目录扫描结果。
    /// @note 重建列表而非增量合并；持久化配置和排除项由后续步骤恢复。
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
    /// @note 路径优先于 ID 匹配；玩家物件绑定仍强制资源保持音效类型。
    void mergePersistedAudioResources(
        Project& project, const Project& persistedProject,
        const std::unordered_set<std::string>& legacyAudioResourceKeys) const;

    /// @brief 根据目录扫描结果同步已有项目的谱面和音频资源列表。
    /// @param project 需要同步资源列表的项目实例。
    /// @param scanResult 项目目录扫描结果。
    /// @return 同步是否改变项目，以及需要预加载的新增音效资源。
    /// @warning 目录变更后的低频同步，包含谱面读取；不得直接逐帧执行。
    DirectorySyncResult syncDirectoryResources(
        Project&                                   project,
        const ProjectDirectoryScanner::ScanResult& scanResult) const;

    /// @brief 查找指定项目音频资源在全部谱面中的引用。
    /// @param project 待扫描的项目。
    /// @param resource 待匹配的项目音频资源。
    /// @return 按谱面和用途记录的引用列表。
    static std::vector<BeatmapAudioReference> findAudioResourceReferences(
        const Project& project, const AudioResource& resource);

    /// @brief 从已经载入内存的谱面收集完整音频引用。
    /// @param beatMap 待读取的内存谱面。
    /// @param beatmapPath 用于诊断和相对路径解析的具体谱面路径。
    /// @return 歌曲提示、玩家物件绑定和自动采样引用列表。
    static std::vector<BeatmapAudioReference> collectBeatmapAudioReferences(
        const BeatMap& beatMap, const std::string& beatmapPath);

    /// @brief 判断谱面音频引用是否指向指定项目资源。
    /// @param project 资源所属项目。
    /// @param reference 待匹配的谱面引用。
    /// @param resource 候选项目音频资源。
    /// @return ID、项目相对路径或旧版文件名能够匹配时返回 true。
    static bool audioReferenceMatchesResource(
        const Project& project, const BeatmapAudioReference& reference,
        const AudioResource& resource);

    /// @brief 将内存谱面中指向移动前资源的引用更新为稳定 ID 和新路径提示。
    /// @param project 资源所属项目。
    /// @param beatMap 需要原地更新的内存谱面。
    /// @param beatmapPath 用于相对路径匹配的具体谱面路径。
    /// @param previousResource 移动前的资源快照。
    /// @param updatedResourcePath 移动后的项目相对路径。
    /// @return 匹配和实际重写数量。
    static BeatmapAudioReferenceRemapResult
    remapBeatmapAudioReferencesAfterMove(
        const Project& project, BeatMap& beatMap,
        const std::string& beatmapPath, const AudioResource& previousResource,
        const std::string& updatedResourcePath);

    /// @brief 将内存谱面的玩家绑定和自动采样资源 ID 精确重命名。
    /// @param beatMap 需要原地更新的谱面。
    /// @param oldResourceId 旧资源 ID。
    /// @param newResourceId 新资源 ID。
    /// @return 实际改写的引用字段数量。
    static std::size_t remapBeatmapAudioResourceId(
        BeatMap& beatMap, std::string_view oldResourceId,
        std::string_view newResourceId);

    /// @brief 以全有或全无方式重写项目内 MMM/Malody 谱面的资源引用。
    /// @param project 待扫描项目。
    /// @param previousResource 重命名前的资源 ID 与路径快照。
    /// @param updatedResourcePath 重命名后的项目相对路径。
    /// @param newResourceId 新资源 ID。
    /// @return 事务结果和实际变更谱面数量。
    /// @warning 低频显式重命名路径：会读取并重新序列化相关谱面。
    static ProjectBeatmapAudioIdRemapResult remapProjectBeatmapAudioResourceId(
        const Project& project, const AudioResource& previousResource,
        std::string_view updatedResourcePath, std::string_view newResourceId);

    /// @brief 保存前按 Malody 提示语义刷新 song_file_hint。
    /// @param project 谱面所属项目。
    /// @param beatMap 需要原地更新提示字段的谱面。
    /// @param beatmapPath 当前谱面用于解析相对引用的路径。
    /// @return 保留、回退或清空提示的具体结果。
    /// @note 该函数只修改提示字段，不新增、删除或移动任何自动采样。
    static BeatmapSongFileHintUpdateResult refreshSongFileHintForSave(
        const Project& project, BeatMap& beatMap,
        const std::filesystem::path& beatmapPath);

    /// @brief 按谱面引用解析项目音频资源。
    /// @param project 待查询项目。
    /// @param beatmapPath 引用所在谱面的项目相对或绝对路径。
    /// @param audioReference 谱面保存的资源 ID 或路径。
    /// @return 匹配到的项目资源；未找到时返回空。
    static const AudioResource* findAudioResourceForReference(
        const Project& project, const std::filesystem::path& beatmapPath,
        const std::string& audioReference);

    /// @brief 批量解析同一谱面的项目音频资源引用。
    /// @param project 待查询项目。
    /// @param beatmapPath 引用所在谱面的项目相对或绝对路径。
    /// @param audioReferences 谱面保存的资源 ID 或旧路径视图。
    /// @return 与输入顺序一一对应的资源地址；未解析项为空。
    /// @warning 低频加载路径：一次构建资源解析索引并线性处理全部引用。
    static std::vector<const AudioResource*> resolveAudioResourceReferences(
        const Project& project, const std::filesystem::path& beatmapPath,
        const std::vector<std::string_view>& audioReferences);

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

    /// @brief 在物理移动前验证外部谱面的音频引用仍可无损保持。
    /// @param project 待检查项目。
    /// @param oldPath 计划移动的文件或目录路径。
    /// @param newPath 计划移动到的文件或目录路径。
    /// @return 允许移动时为空；RM/IMD 隐式音频关联会改变或 osu!
    /// 引用无法改写时返回面向用户的阻止原因。
    static std::string validateAudioResourceMove(
        const Project& project, const std::filesystem::path& oldPath,
        const std::filesystem::path& newPath);

    /// @brief 文件移动或重命名后重映射项目音频资源路径并保持资源 ID 稳定。
    /// @param project 待更新项目。
    /// @param oldPath 移动前的文件或目录路径。
    /// @param newPath 移动后的文件或目录路径。
    /// @param errorMessage 失败时接收面向用户的错误和回滚状态。
    /// @return 路径发生变化的音频资源数量。
    static std::size_t remapAudioResourcePathsAfterMove(
        Project& project, const std::filesystem::path& oldPath,
        const std::filesystem::path& newPath,
        std::string*                 errorMessage = nullptr);

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

    /// @brief 创建默认音轨配置。
    /// @return 默认音轨配置。
    static AudioTrackConfig makeDefaultAudioConfig();

    /// @brief 创建项目音频资源条目。
    /// @param audioPath 音频文件系统路径。
    /// @param relativeAudioPath 已完成一次规范化的项目相对路径。
    /// @return 填充默认配置后的音频资源条目。
    static AudioResource createAudioResource(
        const std::filesystem::path& audioPath,
        const std::string&           relativeAudioPath);
};

}  // namespace MMM::Logic
