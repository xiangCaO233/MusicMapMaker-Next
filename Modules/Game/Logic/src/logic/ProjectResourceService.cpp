#include "logic/ProjectResourceService.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace MMM::Logic
{
namespace
{
/// @brief 判断相对路径是否位于项目根内。
/// @param path 待检查路径。
/// @return 路径没有越出根目录时返回 true。
bool isRelativePathInsideRoot(const std::filesystem::path& path)
{
    if ( path.empty() || path.is_absolute() ) return false;

    const auto normalized = path.lexically_normal();
    for ( const auto& part : normalized ) {
        if ( part == ".." ) return false;
        if ( part == "." ) continue;
        return true;
    }
    return false;
}

/// @brief 尽量取得路径的弱规范化绝对路径。
/// @param path 待规范化路径。
/// @return 成功时返回 weakly_canonical，失败时退回 absolute/原路径。
std::filesystem::path weaklyCanonicalAbsolutePath(
    const std::filesystem::path& path)
{
    std::error_code filesystemError;
    auto normalized = std::filesystem::weakly_canonical(path, filesystemError);
    if ( !filesystemError ) return normalized.lexically_normal();

    filesystemError.clear();
    normalized = std::filesystem::absolute(path, filesystemError);
    if ( !filesystemError ) return normalized.lexically_normal();

    return path.lexically_normal();
}

/// @brief 若路径以项目文件夹名开头，则剥掉该多余前缀。
/// @param projectRoot 项目根目录。
/// @param path 待修正的相对路径。
/// @return 可剥离时返回剥离后的路径，否则返回空。
std::filesystem::path stripProjectFolderPrefix(
    const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    if ( projectRoot.empty() || path.empty() || path.is_absolute() ) {
        return {};
    }

    auto iterator = path.begin();
    if ( iterator == path.end() || *iterator != projectRoot.filename() ) {
        return {};
    }

    std::filesystem::path stripped;
    ++iterator;
    for ( ; iterator != path.end(); ++iterator ) {
        stripped /= *iterator;
    }
    return stripped.lexically_normal();
}

/// @brief 尝试将文件系统路径转换为项目根相对路径。
/// @param projectRoot 项目根目录。
/// @param path 待转换路径。
/// @return 转换成功时返回相对路径，否则返回空。
std::filesystem::path makeRelativeToProjectRoot(
    const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    if ( projectRoot.empty() || path.empty() ) return {};

    const auto root = weaklyCanonicalAbsolutePath(projectRoot);
    if ( path.is_relative() ) {
        const auto stripped = stripProjectFolderPrefix(root, path);
        if ( isRelativePathInsideRoot(stripped) ) {
            return stripped.lexically_normal();
        }

        const auto direct = path.lexically_normal();
        if ( isRelativePathInsideRoot(direct) ) {
            const auto directCandidate = (root / direct).lexically_normal();
            std::error_code filesystemError;
            if ( std::filesystem::exists(directCandidate, filesystemError) &&
                 !filesystemError ) {
                return direct;
            }
        }
    }

    const auto      absolutePath = weaklyCanonicalAbsolutePath(path);
    std::error_code filesystemError;
    auto            relativePath =
        std::filesystem::relative(absolutePath, root, filesystemError);
    if ( !filesystemError && isRelativePathInsideRoot(relativePath) ) {
        return relativePath.lexically_normal();
    }
    return {};
}

/// @brief 将项目中已存储的 UTF-8 路径归一化为项目根相对路径键。
/// @param project 当前项目。
/// @param path 已存储的 UTF-8 路径。
/// @return 可用于稳定比较的项目根相对路径。
std::string normalizeStoredProjectPath(const Project&     project,
                                       const std::string& path)
{
    auto relativePath = makeRelativeToProjectRoot(project.m_projectRoot,
                                                  Config::utf8ToPath(path));
    if ( relativePath.empty() ) {
        relativePath = Config::utf8ToPath(path).lexically_normal();
    }
    return Config::pathToUtf8(relativePath.lexically_normal());
}
}  // namespace

/// @brief 根据初次目录扫描结果填充项目的谱面和音频资源列表。
/// @param project 需要写入资源列表的项目实例。
/// @param scanResult 项目目录扫描结果。
void ProjectResourceService::buildInitialResources(
    Project&                                   project,
    const ProjectDirectoryScanner::ScanResult& scanResult) const
{
    /// @brief 已识别为主音轨的音频项目相对路径集合。
    std::unordered_set<std::string> mainAudioPaths;

    project.m_beatmaps.clear();
    project.m_audioResources.clear();

    for ( const auto& mapPath : scanResult.m_beatmapFiles ) {
        /// @brief 谱面文件相对于项目根目录的 UTF-8 路径。
        auto relativeMapPath = makeProjectRelativeUtf8(project, mapPath);
        /// @brief 谱面文件名，用于显示和日志输出。
        auto filename = Config::pathToUtf8(mapPath.filename());

        /// @brief 新建的项目谱面条目。
        Project::BeatmapEntry mapEntry;
        mapEntry.m_name     = filename;
        mapEntry.m_filePath = relativeMapPath;

        /// @brief 谱面声明的主音轨 ID。
        auto audioTrackId = probeMainAudioTrackId(
            project, mapPath, filename, mainAudioPaths, true);
        if ( audioTrackId ) {
            mapEntry.m_audioTrackId = *audioTrackId;
        }

        project.m_beatmaps.push_back(mapEntry);
        XINFO("Found beatmap: {}", filename);
    }

    for ( const auto& audioPath : scanResult.m_audioFiles ) {
        /// @brief 新建的项目音频资源条目。
        auto resource = createAudioResource(project, audioPath, mainAudioPaths);

        project.m_audioResources.push_back(resource);
        XINFO("Found {} audio resource: {}",
              (resource.m_type == AudioTrackType::Main ? "Main" : "Effect"),
              resource.m_id);
    }

    applyFallbackMainAudio(project, mainAudioPaths);
}

/// @brief 根据项目排除列表过滤已经扫描出的谱面和音频资源。
/// @param project 需要过滤资源列表的项目实例。
void ProjectResourceService::applyExcludedResources(Project& project) const
{
    project.m_beatmaps.erase(
        std::remove_if(project.m_beatmaps.begin(),
                       project.m_beatmaps.end(),
                       [&](const Project::BeatmapEntry& entry) {
                           return containsExcludedPath(
                               project.m_excludedBeatmapPaths,
                               entry.m_filePath);
                       }),
        project.m_beatmaps.end());

    project.m_audioResources.erase(
        std::remove_if(project.m_audioResources.begin(),
                       project.m_audioResources.end(),
                       [&](const AudioResource& resource) {
                           return containsExcludedPath(
                               project.m_excludedAudioPaths, resource.m_path);
                       }),
        project.m_audioResources.end());
}

/// @brief 收集缺少当前 m_config 对象的旧版音频资源键。
/// @param projectJson 项目描述 JSON。
/// @return 优先使用资源路径、路径缺失时使用 ID 的旧版资源键集合。
std::unordered_set<std::string>
ProjectResourceService::collectLegacyAudioResourceKeys(
    const nlohmann::json& projectJson)
{
    std::unordered_set<std::string> result;
    const auto resourcesIterator = projectJson.find("m_audioResources");
    if ( resourcesIterator == projectJson.end() ||
         !resourcesIterator->is_array() ) {
        return result;
    }

    for ( const auto& resourceJson : *resourcesIterator ) {
        if ( !requiresLegacyAudioResourceMigration(resourceJson) ) continue;

        const auto pathIterator = resourceJson.find("m_path");
        if ( pathIterator != resourceJson.end() && pathIterator->is_string() ) {
            const auto path = pathIterator->get<std::string>();
            if ( !path.empty() ) {
                result.insert(path);
                continue;
            }
        }

        const auto idIterator = resourceJson.find("m_id");
        if ( idIterator != resourceJson.end() && idIterator->is_string() ) {
            result.insert(idIterator->get<std::string>());
        }
    }
    return result;
}

/// @brief 将持久化音频配置合并到本次目录扫描得到的资源列表。
/// @param project 以目录扫描结果为基础的项目实例。
/// @param persistedProject 从项目描述文件读取的持久化项目。
/// @param legacyAudioResourceKeys 需要保留扫描音轨类型的旧版资源键。
void ProjectResourceService::mergePersistedAudioResources(
    Project& project, const Project& persistedProject,
    const std::unordered_set<std::string>& legacyAudioResourceKeys) const
{
    for ( auto& resource : project.m_audioResources ) {
        const AudioResource* matchedPersistedResource = nullptr;
        for ( const auto& persistedResource :
              persistedProject.m_audioResources ) {
            if ( !resource.m_path.empty() &&
                 !persistedResource.m_path.empty() &&
                 resource.m_path == persistedResource.m_path ) {
                matchedPersistedResource = &persistedResource;
                break;
            }
        }
        if ( !matchedPersistedResource ) {
            for ( const auto& persistedResource :
                  persistedProject.m_audioResources ) {
                if ( resource.m_id == persistedResource.m_id ) {
                    matchedPersistedResource = &persistedResource;
                    break;
                }
            }
        }
        if ( !matchedPersistedResource ) continue;

        const auto& persistedResource = *matchedPersistedResource;
        const auto& persistedKey      = persistedResource.m_path.empty()
                                            ? persistedResource.m_id
                                            : persistedResource.m_path;

        if ( !legacyAudioResourceKeys.contains(persistedKey) ) {
            resource.m_type = persistedResource.m_type;
        }
        resource.m_config = persistedResource.m_config;
    }
}

/// @brief 根据目录扫描结果同步已有项目的谱面和音频资源列表。
/// @param project 需要同步资源列表的项目实例。
/// @param scanResult 项目目录扫描结果。
/// @return 同步是否改变项目，以及需要预加载的新增音效资源。
ProjectResourceService::DirectorySyncResult
ProjectResourceService::syncDirectoryResources(
    Project&                                   project,
    const ProjectDirectoryScanner::ScanResult& scanResult) const
{
    /// @brief 本次同步的输出结果。
    DirectorySyncResult result;
    /// @brief 同步后新的谱面条目列表。
    std::vector<Project::BeatmapEntry> newBeatmaps;
    /// @brief 已识别为主音轨的音频项目相对路径集合。
    std::unordered_set<std::string> mainAudioPaths;

    for ( const auto& mapPath : scanResult.m_beatmapFiles ) {
        /// @brief 谱面文件相对于项目根目录的 UTF-8 路径。
        auto relativeMapPath = makeProjectRelativeUtf8(project, mapPath);
        /// @brief 谱面文件名，用于显示和日志输出。
        auto filename = Config::pathToUtf8(mapPath.filename());

        if ( containsExcludedPath(project.m_excludedBeatmapPaths,
                                  relativeMapPath) ) {
            continue;
        }

        /// @brief 查找到的已有谱面条目。
        auto existingEntry = findExistingBeatmapEntry(project, relativeMapPath);
        /// @brief 本次同步要写入的新谱面条目。
        Project::BeatmapEntry mapEntry;
        if ( existingEntry ) {
            mapEntry = *existingEntry;
            if ( !mapEntry.m_audioTrackId.empty() ) {
                probeMainAudioTrackId(
                    project, mapPath, filename, mainAudioPaths, false);
            }
        } else {
            mapEntry.m_name     = filename;
            mapEntry.m_filePath = relativeMapPath;
            /// @brief 新发现谱面的主音轨 ID。
            auto audioTrackId = probeMainAudioTrackId(
                project, mapPath, filename, mainAudioPaths, false);
            if ( audioTrackId ) {
                mapEntry.m_audioTrackId = *audioTrackId;
            }
            result.m_changed = true;
            XINFO("Directory Listener: Discovered new beatmap: {}", filename);
        }

        newBeatmaps.push_back(mapEntry);
    }

    if ( newBeatmaps.size() != project.m_beatmaps.size() ) {
        result.m_changed = true;
        XINFO(
            "Directory Listener: Some beatmaps were removed from the "
            "directory.");
    }
    project.m_beatmaps = std::move(newBeatmaps);

    /// @brief 同步后新的音频资源列表。
    std::vector<AudioResource> newAudioResources;
    for ( const auto& audioPath : scanResult.m_audioFiles ) {
        /// @brief 音频文件相对于项目根目录的 UTF-8 路径。
        auto relativeAudioPath = makeProjectRelativeUtf8(project, audioPath);
        /// @brief 音频文件名，用于日志输出。
        auto filename = Config::pathToUtf8(audioPath.filename());

        if ( containsExcludedPath(project.m_excludedAudioPaths,
                                  relativeAudioPath) ) {
            continue;
        }

        /// @brief 查找到的已有音频资源。
        auto existingResource =
            findExistingAudioResource(project, relativeAudioPath);
        /// @brief 本次同步要写入的新音频资源。
        AudioResource resource;
        if ( existingResource ) {
            resource = *existingResource;
            if ( mainAudioPaths.count(relativeAudioPath) > 0 &&
                 resource.m_type != AudioTrackType::Main ) {
                resource.m_type  = AudioTrackType::Main;
                result.m_changed = true;
            }
        } else {
            resource = createAudioResource(project, audioPath, mainAudioPaths);
            if ( resource.m_type == AudioTrackType::Effect ) {
                result.m_effectResourcesToRegister.push_back(resource);
            }
            result.m_changed = true;
            XINFO("Directory Listener: Discovered new audio file: {}",
                  filename);
        }

        newAudioResources.push_back(resource);
    }

    if ( newAudioResources.size() != project.m_audioResources.size() ) {
        result.m_changed = true;
        XINFO(
            "Directory Listener: Some audio files were removed from the "
            "directory.");
    }
    project.m_audioResources = std::move(newAudioResources);

    return result;
}

/// @brief 规范化项目相对路径，用于稳定比较排除列表。
/// @param path UTF-8 编码的项目相对路径。
/// @return 规范化后的 UTF-8 项目相对路径。
std::string ProjectResourceService::normalizeProjectRelativePath(
    const std::string& path)
{
    if ( path.empty() ) return "";
    return Config::pathToUtf8(Config::utf8ToPath(path).lexically_normal());
}

/// @brief 判断路径是否存在于排除列表中。
/// @param excludedPaths 项目排除列表。
/// @param path 需要检查的 UTF-8 项目相对路径。
/// @return 路径已被排除时返回 true。
bool ProjectResourceService::containsExcludedPath(
    const std::vector<std::string>& excludedPaths, const std::string& path)
{
    /// @brief 规范化后的待检查项目相对路径。
    std::string normalized = normalizeProjectRelativePath(path);
    return std::any_of(excludedPaths.begin(),
                       excludedPaths.end(),
                       [&](const std::string& excludedPath) {
                           return normalizeProjectRelativePath(excludedPath) ==
                                  normalized;
                       });
}

/// @brief 将文件系统路径转换为 UTF-8 项目相对路径。
/// @param project 路径所属项目。
/// @param path 需要转换的文件系统路径。
/// @return UTF-8 编码的项目相对路径。
std::string ProjectResourceService::makeProjectRelativeUtf8(
    const Project& project, const std::filesystem::path& path)
{
    auto relativePath = makeRelativeToProjectRoot(project.m_projectRoot, path);
    if ( relativePath.empty() ) {
        relativePath = path.filename();
    }
    return Config::pathToUtf8(relativePath.lexically_normal());
}

/// @brief 解析项目持久化路径为可访问的文件系统路径。
/// @param project 路径所属项目。
/// @param path 项目相对路径或绝对路径。
/// @return 规范化后的文件系统路径。
std::filesystem::path ProjectResourceService::resolveProjectPath(
    const Project& project, const std::filesystem::path& path)
{
    if ( path.empty() || path.is_absolute() ) {
        return path.lexically_normal();
    }

    const auto      root = weaklyCanonicalAbsolutePath(project.m_projectRoot);
    const auto      directCandidate = (root / path).lexically_normal();
    std::error_code filesystemError;
    if ( std::filesystem::exists(directCandidate, filesystemError) &&
         !filesystemError ) {
        return directCandidate;
    }

    const auto stripped = stripProjectFolderPrefix(root, path);
    if ( !stripped.empty() ) {
        const auto strippedCandidate = (root / stripped).lexically_normal();
        filesystemError.clear();
        if ( std::filesystem::exists(strippedCandidate, filesystemError) &&
             !filesystemError ) {
            return strippedCandidate;
        }
    }

    return directCandidate;
}

/// @brief 将文件系统路径转换为项目相对路径。
/// @param project 路径所属项目。
/// @param path 需要转换的文件系统路径。
/// @return 项目相对路径；失败时退回文件名。
std::filesystem::path ProjectResourceService::makeProjectRelativePath(
    const Project& project, const std::filesystem::path& path)
{
    if ( path.empty() ) return {};

    auto relativePath = makeRelativeToProjectRoot(project.m_projectRoot, path);
    if ( !relativePath.empty() ) {
        return relativePath.lexically_normal();
    }

    if ( path.is_relative() ) return path.lexically_normal();
    return path.filename();
}

/// @brief 在项目根目录和谱面目录之间解析元数据资源路径。
/// @param project 路径所属项目。
/// @param mapDirectory 谱面文件所在目录。
/// @param path 元数据中记录的资源路径。
/// @param preferProjectRoot 是否优先按项目根目录解析。
/// @return 可访问优先的规范化资源路径。
std::filesystem::path ProjectResourceService::resolveMetadataResourcePath(
    const Project& project, const std::filesystem::path& mapDirectory,
    const std::filesystem::path& path, bool preferProjectRoot)
{
    if ( path.empty() || path.is_absolute() ) {
        return path.lexically_normal();
    }

    /// @brief 按项目根目录解析出的候选资源路径。
    auto projectPath = resolveProjectPath(project, path);
    /// @brief 按谱面目录解析出的候选资源路径。
    auto mapPath = (mapDirectory / path).lexically_normal();

    /// @brief 文件存在性检查错误码。
    std::error_code filesystemError;
    if ( preferProjectRoot ) {
        if ( std::filesystem::exists(projectPath, filesystemError) )
            return projectPath;
        filesystemError.clear();
        if ( std::filesystem::exists(mapPath, filesystemError) ) return mapPath;
        return projectPath;
    }

    if ( std::filesystem::exists(mapPath, filesystemError) ) return mapPath;
    filesystemError.clear();
    if ( std::filesystem::exists(projectPath, filesystemError) )
        return projectPath;
    return mapPath;
}

/// @brief 将谱面元数据中的长期资源路径规范化为项目相对路径。
/// @param beatMap 需要规范化元数据路径的谱面。
/// @param project 谱面所属项目。
void ProjectResourceService::normalizeBeatmapMetadataPathsForProject(
    BeatMap& beatMap, const Project& project)
{
    /// @brief 谱面的基础元数据引用。
    auto& meta = beatMap.m_baseMapMetadata;
    if ( meta.map_path.empty() ) return;

    /// @brief 谱面文件的绝对路径。
    auto absoluteMapPath = resolveProjectPath(project, meta.map_path);
    /// @brief 谱面文件所在目录。
    auto mapDirectory = absoluteMapPath.parent_path();
    /// @brief 谱面扩展名，用于判断资源路径解析优先级。
    auto mapExtension = Config::pathToUtf8(absoluteMapPath.extension());
    std::transform(mapExtension.begin(),
                   mapExtension.end(),
                   mapExtension.begin(),
                   ::tolower);
    /// @brief 是否优先按项目根目录解析资源路径。
    bool preferProjectRoot = (mapExtension == ".mmm");

    meta.map_path = makeProjectRelativePath(project, absoluteMapPath);

    /// @brief 规范化单个元数据资源路径的闭包。
    auto normalizeResourcePath = [&](std::filesystem::path& path) {
        if ( path.empty() ) return;
        /// @brief 解析后的资源路径。
        auto resolved = resolveMetadataResourcePath(
            project, mapDirectory, path, preferProjectRoot);
        path = makeProjectRelativePath(project, resolved);
    };

    normalizeResourcePath(meta.main_audio_path);
    normalizeResourcePath(meta.main_cover_path);
    normalizeResourcePath(meta.cover_path);
}

/// @brief 尝试读取谱面主音轨并记录到主音轨路径集合。
/// @param project 谱面所属项目。
/// @param mapPath 需要读取的谱面文件路径。
/// @param filename 谱面文件名，用于日志输出。
/// @param mainAudioPaths 已识别的主音轨项目相对路径集合。
/// @param warnOnFailure 读取失败时是否输出警告日志。
/// @return 读取到主音轨时返回音轨 ID，否则返回空。
std::optional<std::string> ProjectResourceService::probeMainAudioTrackId(
    const Project& project, const std::filesystem::path& mapPath,
    const std::string&               filename,
    std::unordered_set<std::string>& mainAudioPaths, bool warnOnFailure)
{
    /// @brief 临时加载的谱面，用于读取主音轨元数据。
    auto beatMap = BeatMap::loadFromFile(mapPath);
    if ( beatMap.m_baseMapMetadata.map_path.empty() ) {
        if ( warnOnFailure ) {
            XWARN("Failed to probe main audio for beatmap: {}", filename);
        }
        return std::nullopt;
    }

    normalizeBeatmapMetadataPathsForProject(beatMap, project);
    if ( beatMap.m_baseMapMetadata.main_audio_path.empty() ) {
        return std::nullopt;
    }

    /// @brief 主音轨在文件系统中的项目根目录拼接路径。
    auto absoluteAudioPath =
        resolveProjectPath(project, beatMap.m_baseMapMetadata.main_audio_path);
    /// @brief 主音轨相对于项目根目录的 UTF-8 路径。
    auto relativeAudioPath =
        Config::pathToUtf8(beatMap.m_baseMapMetadata.main_audio_path);

    mainAudioPaths.insert(relativeAudioPath);
    return Config::pathToUtf8(absoluteAudioPath.filename());
}

/// @brief 创建默认音轨配置。
/// @return 默认音轨配置。
AudioTrackConfig ProjectResourceService::makeDefaultAudioConfig()
{
    /// @brief 使用项目默认值初始化的音轨配置。
    AudioTrackConfig config;
    config.volume        = 0.5f;
    config.playbackSpeed = 1.0f;
    config.playbackPitch = 0.0f;
    config.muted         = false;
    config.eqEnabled     = false;
    config.eqPreset      = 0;
    return config;
}

/// @brief 创建项目音频资源条目。
/// @param project 音频资源所属项目。
/// @param audioPath 音频文件系统路径。
/// @param mainAudioPaths 已识别的主音轨项目相对路径集合。
/// @return 填充默认配置后的音频资源条目。
AudioResource ProjectResourceService::createAudioResource(
    const Project& project, const std::filesystem::path& audioPath,
    const std::unordered_set<std::string>& mainAudioPaths)
{
    /// @brief 音频文件相对于项目根目录的 UTF-8 路径。
    auto relativeAudioPath = makeProjectRelativeUtf8(project, audioPath);
    /// @brief 音频文件名，用作资源 ID。
    auto filename = Config::pathToUtf8(audioPath.filename());

    /// @brief 新建的项目音频资源条目。
    AudioResource resource;
    resource.m_id     = filename;
    resource.m_path   = relativeAudioPath;
    resource.m_type   = (mainAudioPaths.count(relativeAudioPath) > 0)
                            ? AudioTrackType::Main
                            : AudioTrackType::Effect;
    resource.m_config = makeDefaultAudioConfig();
    return resource;
}

/// @brief 查找已有谱面条目。
/// @param project 需要查询的项目。
/// @param relativeMapPath UTF-8 编码的谱面项目相对路径。
/// @return 找到时返回谱面条目副本，否则返回空。
std::optional<Project::BeatmapEntry>
ProjectResourceService::findExistingBeatmapEntry(
    const Project& project, const std::string& relativeMapPath)
{
    for ( const auto& entry : project.m_beatmaps ) {
        if ( entry.m_filePath == relativeMapPath ) {
            return entry;
        }
    }
    return std::nullopt;
}

/// @brief 查找已有音频资源条目。
/// @param project 需要查询的项目。
/// @param relativeAudioPath UTF-8 编码的音频项目相对路径。
/// @return 找到时返回音频资源副本，否则返回空。
std::optional<AudioResource> ProjectResourceService::findExistingAudioResource(
    const Project& project, const std::string& relativeAudioPath)
{
    const std::string normalizedRelativePath =
        normalizeStoredProjectPath(project, relativeAudioPath);
    for ( const auto& resource : project.m_audioResources ) {
        if ( normalizeStoredProjectPath(project, resource.m_path) ==
             normalizedRelativePath ) {
            return resource;
        }
    }
    return std::nullopt;
}

/// @brief 在没有谱面主音轨引用时，为项目资源设置兜底主音轨。
/// @param project 需要设置兜底主音轨的项目。
/// @param mainAudioPaths 已识别的主音轨项目相对路径集合。
void ProjectResourceService::applyFallbackMainAudio(
    Project& project, const std::unordered_set<std::string>& mainAudioPaths)
{
    if ( !mainAudioPaths.empty() || project.m_audioResources.empty() ) {
        return;
    }

    project.m_audioResources.front().m_type = AudioTrackType::Main;
    for ( auto& map : project.m_beatmaps ) {
        if ( map.m_audioTrackId.empty() ) {
            map.m_audioTrackId = project.m_audioResources.front().m_id;
        }
    }
}

}  // namespace MMM::Logic
