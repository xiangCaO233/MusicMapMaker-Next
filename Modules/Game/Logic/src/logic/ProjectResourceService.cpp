#include "logic/ProjectResourceService.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <limits>
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
    /// @brief 扫描谱面后汇总的全部音频引用。
    std::vector<BeatmapAudioReference> audioReferences;

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

        auto references = probeBeatmapAudioReferences(
            project, mapPath, relativeMapPath, true);
        audioReferences.insert(audioReferences.end(),
                               std::make_move_iterator(references.begin()),
                               std::make_move_iterator(references.end()));

        project.m_beatmaps.push_back(mapEntry);
        XINFO("Found beatmap: {}", filename);
    }

    for ( const auto& audioPath : scanResult.m_audioFiles ) {
        /// @brief 新建的项目音频资源条目。
        auto resource =
            createAudioResource(project, audioPath, audioReferences);

        project.m_audioResources.push_back(resource);
        XINFO("Found {} audio resource: {}",
              (resource.m_type == AudioTrackType::Main ? "Main" : "Effect"),
              resource.m_id);
    }
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

    /// @brief 合并后重新收集 Note 绑定，防止持久化类型恢复出非法 Main。
    std::vector<BeatmapAudioReference> audioReferences;
    for ( const auto& entry : project.m_beatmaps ) {
        auto references = probeBeatmapAudioReferences(
            project,
            resolveProjectPath(project, Config::utf8ToPath(entry.m_filePath)),
            entry.m_filePath,
            false);
        audioReferences.insert(audioReferences.end(),
                               std::make_move_iterator(references.begin()),
                               std::make_move_iterator(references.end()));
    }
    for ( auto& resource : project.m_audioResources ) {
        const bool boundToNote = std::any_of(
            audioReferences.begin(),
            audioReferences.end(),
            [&](const BeatmapAudioReference& reference) {
                return reference.m_kind ==
                           BeatmapAudioReferenceKind::NoteSampleBinding &&
                       audioReferenceMatchesResource(
                           project, reference, resource);
            });
        if ( !boundToNote || resource.m_type == AudioTrackType::Effect ) {
            continue;
        }
        resource.m_type = AudioTrackType::Effect;
        XWARN(
            "Persisted Main resource '{}' is bound to a Note; keeping it as "
            "Effect",
            resource.m_id);
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
    /// @brief 扫描谱面后汇总的全部音频引用。
    std::vector<BeatmapAudioReference> audioReferences;

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
        } else {
            mapEntry.m_name     = filename;
            mapEntry.m_filePath = relativeMapPath;
            result.m_changed    = true;
            XINFO("Directory Listener: Discovered new beatmap: {}", filename);
        }

        auto references = probeBeatmapAudioReferences(
            project, mapPath, relativeMapPath, false);
        audioReferences.insert(audioReferences.end(),
                               std::make_move_iterator(references.begin()),
                               std::make_move_iterator(references.end()));
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
            const auto inferredType =
                inferAudioResourceType(project, audioReferences, resource);
            const bool hasTypeReference = std::any_of(
                audioReferences.begin(),
                audioReferences.end(),
                [&](const BeatmapAudioReference& reference) {
                    return reference.m_kind !=
                               BeatmapAudioReferenceKind::AudioSampleEvent &&
                           audioReferenceMatchesResource(
                               project, reference, resource);
                });
            if ( hasTypeReference && resource.m_type != inferredType ) {
                resource.m_type  = inferredType;
                result.m_changed = true;
                if ( resource.m_type == AudioTrackType::Effect ) {
                    result.m_effectResourcesToRegister.push_back(resource);
                }
            }
        } else {
            resource = createAudioResource(project, audioPath, audioReferences);
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
    normalizeResourcePath(meta.song_file_hint);
    normalizeResourcePath(meta.main_cover_path);
    normalizeResourcePath(meta.cover_path);
}

/// @brief 读取谱面并收集歌曲提示、玩家物件绑定和自动采样引用。
/// @param project 谱面所属项目。
/// @param mapPath 需要读取的谱面文件路径。
/// @param beatmapPath 谱面用于诊断的项目相对路径。
/// @param warnOnFailure 读取失败时是否输出警告日志。
/// @return 谱面中的全部音频引用。
std::vector<BeatmapAudioReference>
ProjectResourceService::probeBeatmapAudioReferences(
    const Project& project, const std::filesystem::path& mapPath,
    const std::string& beatmapPath, bool warnOnFailure)
{
    /// @brief 临时加载的谱面，用于读取完整音频引用。
    auto beatMap = BeatMap::loadFromFile(mapPath);
    if ( beatMap.m_baseMapMetadata.map_path.empty() ) {
        if ( warnOnFailure ) {
            XWARN("Failed to probe audio references for beatmap: {}",
                  beatmapPath);
        }
        return {};
    }

    normalizeBeatmapMetadataPathsForProject(beatMap, project);

    /// @brief 当前谱面的音频引用结果。
    std::vector<BeatmapAudioReference> result;
    /// @brief 追加非空音频引用的闭包。
    auto appendReference = [&](const std::string&        audioReference,
                               BeatmapAudioReferenceKind kind) {
        if ( audioReference.empty() ) return;
        result.push_back(BeatmapAudioReference{
            beatmapPath,
            audioReference,
            kind,
        });
    };

    const auto& meta         = beatMap.m_baseMapMetadata;
    const auto& songFileHint = meta.song_file_hint.empty()
                                   ? meta.main_audio_path
                                   : meta.song_file_hint;
    appendReference(Config::pathToUtf8(songFileHint),
                    BeatmapAudioReferenceKind::SongFileHint);

    /// @brief 收集一个玩家物件的命中采样绑定。
    auto appendNoteBinding = [&](const Note& note) {
        const auto binding = note.getSampleBinding();
        if ( !binding ) return;
        appendReference(binding->m_audioResourceId,
                        BeatmapAudioReferenceKind::NoteSampleBinding);
    };
    for ( const auto& note : beatMap.m_noteData.notes ) {
        appendNoteBinding(note);
    }
    for ( const auto& hold : beatMap.m_noteData.holds ) {
        appendNoteBinding(hold);
    }
    for ( const auto& flick : beatMap.m_noteData.flicks ) {
        appendNoteBinding(flick);
    }
    for ( const auto& polyline : beatMap.m_noteData.polylines ) {
        appendNoteBinding(polyline);
    }

    for ( const auto& sample : beatMap.m_audioSamples ) {
        appendReference(sample.m_audioResourceId,
                        BeatmapAudioReferenceKind::AudioSampleEvent);
    }
    return result;
}

/// @brief 判断谱面音频引用是否指向指定项目资源。
/// @param project 资源所属项目。
/// @param reference 待匹配的谱面引用。
/// @param resource 候选项目音频资源。
/// @return ID、项目相对路径或旧版文件名能够匹配时返回 true。
bool ProjectResourceService::audioReferenceMatchesResource(
    const Project& project, const BeatmapAudioReference& reference,
    const AudioResource& resource)
{
    if ( reference.m_audioReference.empty() || resource.m_id.empty() ) {
        return false;
    }
    if ( reference.m_audioReference == resource.m_id ) return true;

    /// @brief 将旧版反斜杠路径统一为当前平台可比较的形式。
    auto normalizedReferenceText = reference.m_audioReference;
    std::replace(normalizedReferenceText.begin(),
                 normalizedReferenceText.end(),
                 '\\',
                 '/');

    const auto normalizedReference =
        normalizeStoredProjectPath(project, normalizedReferenceText);
    const auto normalizedResourcePath =
        normalizeStoredProjectPath(project, resource.m_path);
    if ( normalizedReference == normalizedResourcePath ) return true;

    const auto referencePath = Config::utf8ToPath(normalizedReferenceText);
    if ( Config::pathToUtf8(referencePath.filename()) == resource.m_id ) {
        return true;
    }

    if ( reference.m_beatmapPath.empty() || referencePath.is_absolute() ) {
        return false;
    }

    const auto absoluteMapPath = resolveProjectPath(
        project, Config::utf8ToPath(reference.m_beatmapPath));
    const auto mapRelativeCandidate = makeProjectRelativePath(
        project, absoluteMapPath.parent_path() / referencePath);
    return normalizeStoredProjectPath(
               project, Config::pathToUtf8(mapRelativeCandidate)) ==
           normalizedResourcePath;
}

/// @brief 根据谱面引用推断新发现音频资源的类型。
/// @param project 资源所属项目。
/// @param references 已收集的全部谱面音频引用。
/// @param resource 待推断的项目音频资源。
/// @return Note 绑定优先的资源类型；没有类型线索时返回 Effect。
AudioTrackType ProjectResourceService::inferAudioResourceType(
    const Project&                            project,
    const std::vector<BeatmapAudioReference>& references,
    const AudioResource&                      resource)
{
    bool hasSongFileHint = false;
    bool hasNoteBinding  = false;
    for ( const auto& reference : references ) {
        if ( !audioReferenceMatchesResource(project, reference, resource) ) {
            continue;
        }
        hasSongFileHint |=
            reference.m_kind == BeatmapAudioReferenceKind::SongFileHint;
        hasNoteBinding |=
            reference.m_kind == BeatmapAudioReferenceKind::NoteSampleBinding;
    }

    if ( hasSongFileHint && hasNoteBinding ) {
        XWARN(
            "Audio resource '{}' is both song_file_hint and Note sample; "
            "classifying it as Effect to preserve Note playback",
            resource.m_id);
    }
    if ( hasNoteBinding ) return AudioTrackType::Effect;
    if ( hasSongFileHint ) return AudioTrackType::Main;
    return AudioTrackType::Effect;
}

/// @brief 查找指定项目音频资源在全部谱面中的引用。
/// @param project 待扫描的项目。
/// @param resource 待匹配的项目音频资源。
/// @return 按谱面和用途记录的引用列表。
std::vector<BeatmapAudioReference>
ProjectResourceService::findAudioResourceReferences(
    const Project& project, const AudioResource& resource)
{
    std::vector<BeatmapAudioReference> result;
    for ( const auto& entry : project.m_beatmaps ) {
        const auto mapPath =
            resolveProjectPath(project, Config::utf8ToPath(entry.m_filePath));
        auto references = probeBeatmapAudioReferences(
            project, mapPath, entry.m_filePath, true);
        for ( auto& reference : references ) {
            if ( audioReferenceMatchesResource(project, reference, resource) ) {
                result.push_back(std::move(reference));
            }
        }
    }
    return result;
}

/// @brief 按谱面引用解析项目音频资源。
/// @param project 待查询项目。
/// @param beatmapPath 引用所在谱面的项目相对或绝对路径。
/// @param audioReference 谱面保存的资源 ID 或路径。
/// @return 匹配到的项目资源；未找到时返回空。
const AudioResource* ProjectResourceService::findAudioResourceForReference(
    const Project& project, const std::filesystem::path& beatmapPath,
    const std::string& audioReference)
{
    if ( audioReference.empty() ) return nullptr;

    const BeatmapAudioReference reference{
        Config::pathToUtf8(beatmapPath),
        audioReference,
        BeatmapAudioReferenceKind::AudioSampleEvent,
    };
    for ( const auto& resource : project.m_audioResources ) {
        if ( audioReferenceMatchesResource(project, reference, resource) ) {
            return &resource;
        }
    }
    return nullptr;
}

/// @brief 为谱面选择适合预览或 BPM 测量的默认音频资源。
/// @param project 谱面所属项目。
/// @param beatMap 待解析的谱面。
/// @param beatmapPath 谱面的项目相对或绝对路径。
/// @return 优先匹配歌曲提示和 Main 自动采样的项目资源。
const AudioResource* ProjectResourceService::findDefaultBeatmapAudioResource(
    const Project& project, const BeatMap& beatMap,
    const std::filesystem::path& beatmapPath)
{
    const auto& meta         = beatMap.m_baseMapMetadata;
    const auto& songFileHint = meta.song_file_hint.empty()
                                   ? meta.main_audio_path
                                   : meta.song_file_hint;
    if ( const auto* hintedResource = findAudioResourceForReference(
             project, beatmapPath, Config::pathToUtf8(songFileHint)) ) {
        return hintedResource;
    }

    const AudioResource* firstMainSampleResource = nullptr;
    const AudioResource* firstSampleResource     = nullptr;
    double firstMainTimestamp = std::numeric_limits<double>::infinity();
    double firstTimestamp     = std::numeric_limits<double>::infinity();
    for ( const auto& sample : beatMap.m_audioSamples ) {
        const auto* resource = findAudioResourceForReference(
            project, beatmapPath, sample.m_audioResourceId);
        if ( !resource ) continue;

        const double timestamp = sample.effectiveTimestamp();
        if ( timestamp < firstTimestamp ) {
            firstTimestamp      = timestamp;
            firstSampleResource = resource;
        }
        if ( resource->m_type == AudioTrackType::Main &&
             timestamp < firstMainTimestamp ) {
            firstMainTimestamp      = timestamp;
            firstMainSampleResource = resource;
        }
    }
    if ( firstMainSampleResource ) return firstMainSampleResource;
    if ( firstSampleResource ) return firstSampleResource;

    const auto mainIterator =
        std::find_if(project.m_audioResources.begin(),
                     project.m_audioResources.end(),
                     [](const AudioResource& resource) {
                         return resource.m_type == AudioTrackType::Main;
                     });
    if ( mainIterator != project.m_audioResources.end() ) {
        return &*mainIterator;
    }
    return project.m_audioResources.empty() ? nullptr
                                            : &project.m_audioResources.front();
}

/// @brief 将旧项目条目的单主音轨迁移为 MMM v2 自动采样。
/// @param project 当前目录扫描和资源合并后的项目。
/// @param persistedProject 从旧项目描述文件读取的项目。
/// @return 成功迁移数量和失败谱面路径。
ProjectResourceService::LegacyAudioMigrationResult
ProjectResourceService::migrateLegacyBeatmapAudioTracks(
    Project& project, const Project& persistedProject) const
{
    LegacyAudioMigrationResult result;
    for ( const auto& persistedEntry : persistedProject.m_beatmaps ) {
        if ( persistedEntry.m_audioTrackId.empty() ) continue;

        const auto currentEntryIterator = std::find_if(
            project.m_beatmaps.begin(),
            project.m_beatmaps.end(),
            [&](const Project::BeatmapEntry& entry) {
                return normalizeStoredProjectPath(project, entry.m_filePath) ==
                       normalizeStoredProjectPath(project,
                                                  persistedEntry.m_filePath);
            });
        if ( currentEntryIterator == project.m_beatmaps.end() ) continue;

        const auto mapPath = resolveProjectPath(
            project, Config::utf8ToPath(currentEntryIterator->m_filePath));
        auto extension = Config::pathToUtf8(mapPath.extension());
        std::transform(extension.begin(),
                       extension.end(),
                       extension.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });
        if ( extension != ".mmm" ) {
            XINFO(
                "Skipping legacy m_audioTrackId migration for non-MMM "
                "beatmap: {}",
                currentEntryIterator->m_filePath);
            continue;
        }

        auto beatMap = BeatMap::loadFromFile(mapPath);
        if ( beatMap.m_baseMapMetadata.map_path.empty() ) {
            result.m_failedBeatmapPaths.push_back(
                currentEntryIterator->m_filePath);
            continue;
        }
        if ( !beatMap.m_audioSamples.empty() ) continue;

        const auto* resource =
            findAudioResourceForReference(project,
                                          currentEntryIterator->m_filePath,
                                          persistedEntry.m_audioTrackId);
        if ( !resource ) {
            XWARN(
                "Cannot migrate legacy m_audioTrackId '{}' for beatmap '{}': "
                "audio resource was not found",
                persistedEntry.m_audioTrackId,
                currentEntryIterator->m_filePath);
            result.m_failedBeatmapPaths.push_back(
                currentEntryIterator->m_filePath);
            continue;
        }

        AudioSampleEvent sample;
        sample.m_timestamp = 0.0;
        sample.m_offsetMs  = 0;
        sample.m_track     = static_cast<std::uint32_t>(
            std::max(0, beatMap.m_baseMapMetadata.track_count));
        sample.m_audioResourceId = resource->m_id;
        beatMap.m_audioSamples.push_back(std::move(sample));
        beatMap.m_baseMapMetadata.bgm_track_count =
            std::max(1, beatMap.m_baseMapMetadata.bgm_track_count);
        if ( beatMap.m_baseMapMetadata.song_file_hint.empty() ) {
            beatMap.m_baseMapMetadata.song_file_hint =
                Config::utf8ToPath(resource->m_path);
        }

        if ( !beatMap.saveToFile(mapPath) ) {
            result.m_failedBeatmapPaths.push_back(
                currentEntryIterator->m_filePath);
            continue;
        }

        const auto references = findAudioResourceReferences(project, *resource);
        const bool boundToNote =
            std::any_of(references.begin(),
                        references.end(),
                        [](const BeatmapAudioReference& reference) {
                            return reference.m_kind ==
                                   BeatmapAudioReferenceKind::NoteSampleBinding;
                        });
        if ( !boundToNote ) {
            const auto mutableResource =
                std::find_if(project.m_audioResources.begin(),
                             project.m_audioResources.end(),
                             [&](const AudioResource& candidate) {
                                 return &candidate == resource;
                             });
            if ( mutableResource != project.m_audioResources.end() ) {
                mutableResource->m_type = AudioTrackType::Main;
            }
        }

        result.m_migratedBeatmapCount++;
        XINFO(
            "Migrated legacy m_audioTrackId '{}' to a beat-0 BGM sample in "
            "'{}'",
            resource->m_id,
            currentEntryIterator->m_filePath);
    }
    return result;
}

/// @brief 文件移动或重命名后重映射项目音频资源路径并保持资源 ID 稳定。
/// @param project 待更新项目。
/// @param oldPath 移动前的文件或目录路径。
/// @param newPath 移动后的文件或目录路径。
/// @return 路径发生变化的音频资源数量。
std::size_t ProjectResourceService::remapAudioResourcePathsAfterMove(
    Project& project, const std::filesystem::path& oldPath,
    const std::filesystem::path& newPath)
{
    if ( project.m_projectRoot.empty() || oldPath.empty() || newPath.empty() ) {
        return 0;
    }

    const auto absoluteOldPath = weaklyCanonicalAbsolutePath(oldPath);
    const auto absoluteNewPath = weaklyCanonicalAbsolutePath(newPath);
    /// @brief 单个资源路径变化前后的快照，用于同步 MMM 内的路径型引用。
    struct ResourcePathRemap {
        /// @brief 移动前资源。
        AudioResource m_before;

        /// @brief 移动后项目相对路径。
        std::string m_afterPath;
    };
    std::vector<ResourcePathRemap> remappedResources;
    for ( auto& resource : project.m_audioResources ) {
        const auto absoluteResourcePath = weaklyCanonicalAbsolutePath(
            resolveProjectPath(project, Config::utf8ToPath(resource.m_path)));

        std::filesystem::path suffix;
        if ( absoluteResourcePath != absoluteOldPath ) {
            std::error_code relativeError;
            suffix = std::filesystem::relative(
                absoluteResourcePath, absoluteOldPath, relativeError);
            if ( relativeError || !isRelativePathInsideRoot(suffix) ) {
                continue;
            }
        }

        const auto remappedAbsolutePath =
            suffix.empty() ? absoluteNewPath
                           : (absoluteNewPath / suffix).lexically_normal();
        const auto remappedRelativePath = makeRelativeToProjectRoot(
            project.m_projectRoot, remappedAbsolutePath);
        if ( remappedRelativePath.empty() ) continue;

        const auto remappedPath =
            Config::pathToUtf8(remappedRelativePath.lexically_normal());
        if ( resource.m_path == remappedPath ) continue;
        remappedResources.push_back(ResourcePathRemap{
            resource,
            remappedPath,
        });
        resource.m_path = remappedPath;
    }
    if ( remappedResources.empty() ) return 0;

    for ( const auto& entry : project.m_beatmaps ) {
        const auto mapPath =
            resolveProjectPath(project, Config::utf8ToPath(entry.m_filePath));
        auto extension = Config::pathToUtf8(mapPath.extension());
        std::transform(extension.begin(),
                       extension.end(),
                       extension.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });
        if ( extension != ".mmm" ) continue;

        auto beatMap = BeatMap::loadFromFile(mapPath);
        if ( beatMap.m_baseMapMetadata.map_path.empty() ) continue;
        bool changedBeatmap = false;

        for ( const auto& remap : remappedResources ) {
            /// @brief 判断一个字符串引用是否指向本次移动前资源。
            auto matchesBefore = [&](const std::string& audioReference) {
                return audioReferenceMatchesResource(
                    project,
                    BeatmapAudioReference{
                        entry.m_filePath,
                        audioReference,
                        BeatmapAudioReferenceKind::AudioSampleEvent,
                    },
                    remap.m_before);
            };
            /// @brief 将 metadata 路径提示更新为移动后的项目相对路径。
            auto remapMetadataPath = [&](std::filesystem::path& path) {
                if ( !matchesBefore(Config::pathToUtf8(path)) ) return;
                path           = Config::utf8ToPath(remap.m_afterPath);
                changedBeatmap = true;
            };
            remapMetadataPath(beatMap.m_baseMapMetadata.song_file_hint);
            remapMetadataPath(beatMap.m_baseMapMetadata.main_audio_path);

            /// @brief 将玩家物件路径型引用规范成稳定资源 ID。
            auto remapNoteBinding = [&](Note& note) {
                auto binding = note.getSampleBinding();
                if ( !binding || !matchesBefore(binding->m_audioResourceId) ) {
                    return;
                }
                binding->m_audioResourceId = remap.m_before.m_id;
                note.setSampleBinding(std::move(*binding));
                changedBeatmap = true;
            };
            for ( auto& note : beatMap.m_noteData.notes ) {
                remapNoteBinding(note);
            }
            for ( auto& hold : beatMap.m_noteData.holds ) {
                remapNoteBinding(hold);
            }
            for ( auto& flick : beatMap.m_noteData.flicks ) {
                remapNoteBinding(flick);
            }
            for ( auto& polyline : beatMap.m_noteData.polylines ) {
                remapNoteBinding(polyline);
            }

            for ( auto& sample : beatMap.m_audioSamples ) {
                if ( !matchesBefore(sample.m_audioResourceId) ) continue;
                sample.m_audioResourceId = remap.m_before.m_id;
                changedBeatmap           = true;
            }
        }

        if ( changedBeatmap && !beatMap.saveToFile(mapPath) ) {
            XWARN("Failed to update moved audio references in beatmap: {}",
                  entry.m_filePath);
        }
    }
    return remappedResources.size();
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
/// @param references 已收集的全部谱面音频引用。
/// @return 填充默认配置后的音频资源条目。
AudioResource ProjectResourceService::createAudioResource(
    const Project& project, const std::filesystem::path& audioPath,
    const std::vector<BeatmapAudioReference>& references)
{
    /// @brief 音频文件相对于项目根目录的 UTF-8 路径。
    auto relativeAudioPath = makeProjectRelativeUtf8(project, audioPath);
    /// @brief 音频文件名，用作资源 ID。
    auto filename = Config::pathToUtf8(audioPath.filename());

    /// @brief 新建的项目音频资源条目。
    AudioResource resource;
    resource.m_id     = filename;
    resource.m_path   = relativeAudioPath;
    resource.m_type   = inferAudioResourceType(project, references, resource);
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

}  // namespace MMM::Logic
