#pragma once

#include "common/LogicCommands.h"
#include "logic/ProjectResourceService.h"
#include "logic/ProjectTypes.h"
#include "mmm/project/Project.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace MMM
{
class BeatMap;
}

namespace MMM::Logic
{

/// @brief 处理项目级资源命令的低频服务。
class ProjectCommandService
{
public:
    /// @brief 保留原嵌套类型名，兼容现有项目命令调用方。
    using AudioRegistrationRequest = MMM::Logic::AudioRegistrationRequest;

    /// @brief 新建谱面命令结果。
    struct CreateBeatmapResult {
        /// @brief 是否成功创建并登记谱面。
        bool m_created{ false };

        /// @brief 新建的谱面实例，用于交给会话立即打开。
        std::shared_ptr<BeatMap> m_beatmap;

        /// @brief 新建谱面打开到画布时使用的显示名称。
        std::string m_displayName;
    };

    /// @brief 导入音频命令结果。
    struct ImportAudioResult {
        /// @brief 是否成功导入音频并登记资源。
        bool m_imported{ false };

        /// @brief 新导入资源若为音效，则需要由引擎登记。
        std::optional<AudioRegistrationRequest> m_effectRegistration;
    };

    /// @brief 更新音频资源命令结果。
    struct UpdateAudioResourceResult {
        /// @brief 是否成功找到并更新音频资源。
        bool m_updated{ false };

        /// @brief 阻止 Effect 转 Main 的 Note 绑定所在谱面列表。
        std::vector<std::string> m_blockingBeatmapPaths;

        /// @brief 更新后若资源变成音效，则需要由引擎登记。
        std::optional<AudioRegistrationRequest> m_effectRegistration;

        /// @brief 更新前若资源是音效且现已变成主音轨，需要卸载的音效 ID。
        std::optional<std::string> m_effectResourceIdToUnload;
    };

    /// @brief 删除音频资源命令结果。
    struct RemoveAudioResourceResult {
        /// @brief 是否从项目资源列表中删除了音频资源。
        bool m_removed{ false };

        /// @brief 阻止删除的 Note 或自动采样引用所在谱面列表。
        std::vector<std::string> m_blockingBeatmapPaths;

        /// @brief 被删除资源若为音效，则需要由引擎卸载音效缓存。
        std::optional<std::string> m_effectResourceIdToUnload;

        /// @brief 删除失败时供调用方显示的具体原因。
        std::string m_errorMessage;
    };

    /// @brief 项目资源列表变更结果。
    struct ProjectMutationResult {
        /// @brief 本次操作是否改变了项目配置。
        bool m_changed{ false };
    };

    /// @brief 创建谱面文件并登记到项目资源列表。
    /// @param project 当前打开的项目。
    /// @param cmd 新建谱面命令。
    /// @return 新建谱面的处理结果。
    CreateBeatmapResult createBeatmap(Project&                project,
                                      const CmdCreateBeatmap& cmd) const;

    /// @brief 导入音频文件并登记到项目资源列表。
    /// @param project 当前打开的项目。
    /// @param cmd 导入音频命令。
    /// @return 导入音频的处理结果。
    ImportAudioResult importAudio(Project&              project,
                                  const CmdImportAudio& cmd) const;

    /// @brief 将单个谱面文件同步到项目谱面列表。
    /// @param project 当前打开的项目。
    /// @param mapPath 需要同步的谱面文件路径。
    /// @return 项目谱面列表是否发生变化。
    ProjectMutationResult syncProjectWithFile(
        Project& project, const std::filesystem::path& mapPath) const;

    /// @brief 更新项目内谱面条目的文件路径关联。
    /// @param project 当前打开的项目。
    /// @param oldPath 旧谱面路径。
    /// @param newPath 新谱面路径。
    /// @return 项目谱面列表是否发生变化。
    ProjectMutationResult updateBeatmapFilePath(
        Project& project, const std::filesystem::path& oldPath,
        const std::filesystem::path& newPath) const;

    /// @brief 更新音频资源类型。
    /// @param project 当前打开的项目。
    /// @param cmd 更新音频资源命令。
    /// @param openBeatmapReferences 已同步的打开会话内存谱面引用。
    /// @return 更新音频资源的处理结果。
    UpdateAudioResourceResult updateAudioResource(
        Project& project, const CmdUpdateAudioResource& cmd,
        const std::vector<BeatmapAudioReference>& openBeatmapReferences = {})
        const;

    /// @brief 从项目中删除音频资源并清理谱面引用。
    /// @param project 当前打开的项目。
    /// @param cmd 删除音频资源命令。
    /// @param openBeatmapReferences 已同步的打开会话内存谱面引用。
    /// @return 删除音频资源的处理结果。
    RemoveAudioResourceResult removeAudioResource(
        Project& project, const CmdRemoveAudioResource& cmd,
        const std::vector<BeatmapAudioReference>& openBeatmapReferences = {})
        const;

    /// @brief 从项目谱面列表中删除谱面。
    /// @param project 当前打开的项目。
    /// @param cmd 删除谱面命令。
    /// @return 项目谱面列表是否发生变化。
    ProjectMutationResult removeBeatmap(Project&                project,
                                        const CmdRemoveBeatmap& cmd) const;

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

    /// @brief 将路径加入排除列表。
    /// @param excludedPaths 项目排除列表。
    /// @param path 需要加入的 UTF-8 项目相对路径。
    /// @return 排除列表发生变化时返回 true。
    static bool addExcludedPath(std::vector<std::string>& excludedPaths,
                                const std::string&        path);

    /// @brief 从排除列表移除路径。
    /// @param excludedPaths 项目排除列表。
    /// @param path 需要移除的 UTF-8 项目相对路径。
    /// @return 排除列表发生变化时返回 true。
    static bool removeExcludedPath(std::vector<std::string>& excludedPaths,
                                   const std::string&        path);

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

    /// @brief 创建默认音轨配置。
    /// @return 默认音轨配置。
    static AudioTrackConfig makeDefaultAudioConfig();
};

}  // namespace MMM::Logic
