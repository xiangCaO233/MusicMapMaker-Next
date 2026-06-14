#include "logic/ProjectCommandService.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <system_error>
#include <utility>

namespace MMM::Logic
{
namespace
{
/// @brief 判断相对路径是否位于项目根内。
/// @param path 待检查的相对路径。
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

/// @brief 清空目标谱面的物件数据并复制非折线物件。
/// @param target 接收复制结果的新谱面。
/// @param source 模板谱面。
void copyStandaloneNotes(BeatMap& target, const BeatMap& source)
{
    target.m_noteData.notes.clear();
    target.m_noteData.holds.clear();
    target.m_noteData.flicks.clear();
    target.m_noteData.polylines.clear();

    for ( const auto& note : source.m_noteData.notes ) {
        if ( note.m_isSubNote ) continue;
        target.m_noteData.notes.push_back(note);
    }
    for ( const auto& hold : source.m_noteData.holds ) {
        if ( hold.m_isSubNote ) continue;
        target.m_noteData.holds.push_back(hold);
    }
    for ( const auto& flick : source.m_noteData.flicks ) {
        if ( flick.m_isSubNote ) continue;
        target.m_noteData.flicks.push_back(flick);
    }
}

/// @brief 将一个折线子物件复制到目标谱面并重新绑定到目标折线。
/// @param target 接收子物件的新谱面。
/// @param targetPolyline 正在构建的目标折线。
/// @param sourceSubNote 模板折线中的子物件。
void copyPolylineSubNote(BeatMap& target, Polyline& targetPolyline,
                         const Note& sourceSubNote)
{
    if ( sourceSubNote.m_type == ::MMM::NoteType::HOLD ) {
        const auto& sourceHold = static_cast<const Hold&>(sourceSubNote);
        Hold        copiedHold = sourceHold;
        copiedHold.m_isSubNote = true;
        target.m_noteData.holds.push_back(std::move(copiedHold));
        auto& copiedRef = target.m_noteData.holds.back();
        targetPolyline.m_subNotes.push_back(copiedRef);
        targetPolyline.m_subHolds.push_back(copiedRef);
        return;
    }

    if ( sourceSubNote.m_type == ::MMM::NoteType::FLICK ) {
        const auto& sourceFlick = static_cast<const Flick&>(sourceSubNote);
        Flick       copiedFlick = sourceFlick;
        copiedFlick.m_isSubNote = true;
        target.m_noteData.flicks.push_back(std::move(copiedFlick));
        auto& copiedRef = target.m_noteData.flicks.back();
        targetPolyline.m_subNotes.push_back(copiedRef);
        targetPolyline.m_subFlicks.push_back(copiedRef);
        return;
    }

    Note copiedNote        = sourceSubNote;
    copiedNote.m_isSubNote = true;
    target.m_noteData.notes.push_back(std::move(copiedNote));
    auto& copiedRef = target.m_noteData.notes.back();
    targetPolyline.m_subNotes.push_back(copiedRef);
}

/// @brief 复制模板谱面的全部物件，并修复折线对子物件的引用。
/// @param target 接收复制结果的新谱面。
/// @param source 模板谱面。
void copyTemplateNotes(BeatMap& target, const BeatMap& source)
{
    copyStandaloneNotes(target, source);

    for ( const auto& sourcePolyline : source.m_noteData.polylines ) {
        Polyline copiedPolyline = sourcePolyline;
        copiedPolyline.m_subNotes.clear();
        copiedPolyline.m_subHolds.clear();
        copiedPolyline.m_subFlicks.clear();

        for ( const auto& sourceSubNoteRef : sourcePolyline.m_subNotes ) {
            copyPolylineSubNote(target, copiedPolyline, sourceSubNoteRef.get());
        }

        target.m_noteData.polylines.push_back(std::move(copiedPolyline));
    }

    target.sync();
}

/// @brief 按用户选项将模板谱面内容应用到新谱面。
/// @param target 正在创建的新谱面。
/// @param source 模板谱面。
/// @param options 模板复制选项。
void applyTemplateBeatmap(BeatMap& target, const BeatMap& source,
                          const BeatmapTemplateCreateOptions& options)
{
    if ( options.copyMetadata ) {
        target.m_metadata = source.m_metadata;
    }
    if ( options.copyTimelines ) {
        target.m_timings = source.m_timings;
    }
    if ( options.copyObjects ) {
        copyTemplateNotes(target, source);
    } else {
        target.sync();
    }
}

/// @brief 按时间和类型稳定排序谱面 Timing。
/// @param timings 待排序的 Timing 列表。
void sortBeatmapTimings(std::vector<::MMM::Timing>& timings)
{
    std::stable_sort(
        timings.begin(), timings.end(), [](const auto& lhs, const auto& rhs) {
            if ( lhs.m_timestamp != rhs.m_timestamp ) {
                return lhs.m_timestamp < rhs.m_timestamp;
            }
            return static_cast<int>(lhs.m_timingEffect) <
                   static_cast<int>(rhs.m_timingEffect);
        });
}

/// @brief 将新建命令携带的初始 Timing 写入新谱面。
/// @param target 接收初始 Timing 的新谱面。
/// @param initialTimings 新建向导或其他创建流程提供的 Timing 列表。
/// @param keepNonBpmTimings 是否保留模板中的非 BPM 流速/特效 Timing。
void applyInitialBeatmapTimings(
    BeatMap& target, const std::vector<::MMM::Timing>& initialTimings,
    bool keepNonBpmTimings)
{
    if ( initialTimings.empty() ) {
        return;
    }

    if ( keepNonBpmTimings ) {
        std::erase_if(target.m_timings, [](const auto& timing) {
            return timing.m_timingEffect == ::MMM::TimingEffect::BPM;
        });
    } else {
        target.m_timings.clear();
    }

    target.m_timings.insert(
        target.m_timings.end(), initialTimings.begin(), initialTimings.end());
    sortBeatmapTimings(target.m_timings);
}
}  // namespace

/// @brief 创建谱面文件并登记到项目资源列表。
/// @param project 当前打开的项目。
/// @param cmd 新建谱面命令。
/// @return 新建谱面的处理结果。
ProjectCommandService::CreateBeatmapResult ProjectCommandService::createBeatmap(
    Project& project, const CmdCreateBeatmap& cmd) const
{
    /// @brief 本次新建谱面的返回结果。
    CreateBeatmapResult result;

    /// @brief 新谱面的基础元数据副本，后续会规范化路径后写入文件。
    auto meta = cmd.baseMeta;
    XINFO("Creating new beatmap: {} (Title: {})", meta.name, meta.title);

    /// @brief 经过非法文件名字符替换后的谱面文件名主体。
    std::string safeFilename = meta.name;
    std::replace_if(
        safeFilename.begin(),
        safeFilename.end(),
        [](char c) {
            return c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                   c == '"' || c == '<' || c == '>' || c == '|';
        },
        '_');

    /// @brief 新谱面文件的候选保存路径。
    std::filesystem::path mapPath =
        project.m_projectRoot / Config::utf8ToPath(safeFilename + ".mmm");

    /// @brief 文件名冲突时递增追加的数字后缀。
    int suffix = 1;
    while ( std::filesystem::exists(mapPath) ) {
        mapPath = project.m_projectRoot /
                  Config::utf8ToPath(safeFilename + "_" +
                                     std::to_string(suffix++) + ".mmm");
    }

    meta.map_path = makeProjectRelativePath(project, mapPath);

    meta.main_audio_path =
        makeProjectRelativePath(project, meta.main_audio_path);
    meta.main_cover_path =
        makeProjectRelativePath(project, meta.main_cover_path);
    meta.cover_path = makeProjectRelativePath(project, meta.cover_path);

    /// @brief 新建并即将保存到磁盘的谱面实例。
    auto newBeatmap               = std::make_shared<MMM::BeatMap>();
    newBeatmap->m_baseMapMetadata = meta;
    if ( cmd.templateBeatmap ) {
        applyTemplateBeatmap(
            *newBeatmap, *cmd.templateBeatmap, cmd.templateOptions);
    }
    applyInitialBeatmapTimings(
        *newBeatmap,
        cmd.initialTimings,
        cmd.templateBeatmap && cmd.templateOptions.copyTimelines);

    try {
        newBeatmap->saveToFile(mapPath);
        XINFO("Beatmap saved to: {}", Config::pathToUtf8(mapPath));
    } catch ( const std::exception& e ) {
        XERROR("Failed to save new beatmap: {}", e.what());
        return result;
    }

    /// @brief 新谱面在项目列表中的入口。
    Project::BeatmapEntry entry;
    entry.m_name = meta.name;
    entry.m_filePath =
        Config::pathToUtf8(makeProjectRelativePath(project, mapPath));
    entry.m_audioTrackId = Config::pathToUtf8(meta.main_audio_path.filename());
    removeExcludedPath(project.m_excludedBeatmapPaths, entry.m_filePath);
    project.m_beatmaps.push_back(entry);

    /// @brief 当前项目是否已经登记了新谱面声明的主音轨。
    bool audioExists = false;
    /// @brief 新谱面主音轨的规范化项目相对路径。
    const std::string normalizedMainAudioPath = normalizeStoredProjectPath(
        project, Config::pathToUtf8(meta.main_audio_path));
    for ( const auto& resource : project.m_audioResources ) {
        if ( normalizeStoredProjectPath(project, resource.m_path) ==
             normalizedMainAudioPath ) {
            audioExists = true;
            break;
        }
    }

    if ( !audioExists && !meta.main_audio_path.empty() ) {
        /// @brief 根据新谱面主音轨自动补充的项目音频资源。
        AudioResource resource;
        resource.m_id     = Config::pathToUtf8(meta.main_audio_path.filename());
        resource.m_path   = Config::pathToUtf8(meta.main_audio_path);
        resource.m_type   = AudioTrackType::Main;
        resource.m_config = makeDefaultAudioConfig();
        removeExcludedPath(project.m_excludedAudioPaths, resource.m_path);
        project.m_audioResources.push_back(resource);
    }

    result.m_created     = true;
    result.m_beatmap     = std::move(newBeatmap);
    result.m_displayName = meta.name;
    return result;
}

/// @brief 导入音频文件并登记到项目资源列表。
/// @param project 当前打开的项目。
/// @param cmd 导入音频命令。
/// @return 导入音频的处理结果。
ProjectCommandService::ImportAudioResult ProjectCommandService::importAudio(
    Project& project, const CmdImportAudio& cmd) const
{
    /// @brief 本次导入音频的返回结果。
    ImportAudioResult result;

    /// @brief 用户指定的源音频路径。
    std::filesystem::path audioPath = Config::utf8ToPath(cmd.path);
    std::error_code       filesystemError;
    const auto            sourcePath = weaklyCanonicalAbsolutePath(audioPath);
    if ( !std::filesystem::is_regular_file(sourcePath, filesystemError) ||
         filesystemError ) {
        XERROR("Cannot import audio: File does not exist: {}", cmd.path);
        return result;
    }

    XINFO("Importing audio: {}", cmd.path);

    /// @brief 项目根目录的绝对规范化路径。
    const auto projectRoot = weaklyCanonicalAbsolutePath(project.m_projectRoot);
    /// @brief 音频资源最终写入项目配置的项目相对路径。
    std::filesystem::path finalRelativePath =
        makeRelativeToProjectRoot(projectRoot, sourcePath);
    /// @brief 音频资源最终落盘或已存在的绝对路径。
    std::filesystem::path finalAbsolutePath =
        finalRelativePath.empty()
            ? std::filesystem::path{}
            : (projectRoot / finalRelativePath).lexically_normal();

    if ( finalRelativePath.empty() ) {
        finalAbsolutePath = projectRoot / sourcePath.filename();

        /// @brief 复制目标文件名冲突时递增追加的数字后缀。
        int suffix = 1;
        while ( std::filesystem::exists(finalAbsolutePath, filesystemError) &&
                !filesystemError ) {
            finalAbsolutePath =
                projectRoot /
                Config::utf8ToPath(Config::pathToUtf8(sourcePath.stem()) + "_" +
                                   std::to_string(suffix++) +
                                   Config::pathToUtf8(sourcePath.extension()));
        }
        if ( filesystemError ) {
            XERROR("Failed to inspect audio import target: {}",
                   Config::pathToUtf8(finalAbsolutePath));
            return result;
        }

        std::filesystem::copy_file(
            sourcePath, finalAbsolutePath, filesystemError);
        if ( filesystemError ) {
            XERROR("Failed to copy audio file: {}", filesystemError.message());
            return result;
        }
        XINFO("Copied external audio to project: {}",
              Config::pathToUtf8(finalAbsolutePath));

        finalRelativePath =
            makeRelativeToProjectRoot(projectRoot, finalAbsolutePath);
        if ( finalRelativePath.empty() ) {
            finalRelativePath = finalAbsolutePath.filename();
        }
    }

    /// @brief 音频资源最终写入项目配置的 UTF-8 相对路径。
    std::string relPathUtf8 =
        Config::pathToUtf8(finalRelativePath.lexically_normal());
    removeExcludedPath(project.m_excludedAudioPaths, relPathUtf8);
    for ( const auto& resource : project.m_audioResources ) {
        if ( normalizeStoredProjectPath(project, resource.m_path) ==
             normalizeStoredProjectPath(project, relPathUtf8) ) {
            XWARN("Audio already exists in project: {}", relPathUtf8);
            return result;
        }
    }

    /// @brief 新导入的项目音频资源。
    AudioResource resource;
    resource.m_id            = Config::pathToUtf8(finalRelativePath.filename());
    resource.m_path          = relPathUtf8;
    resource.m_type          = cmd.trackType;
    resource.m_config.volume = 0.5f;
    resource.m_config.playbackSpeed = 1.0f;
    resource.m_config.playbackPitch = 0.0f;
    resource.m_config.muted         = false;

    project.m_audioResources.push_back(resource);

    if ( resource.m_type == AudioTrackType::Effect ) {
        /// @brief 新导入音效的预加载请求。
        AudioPreloadRequest preloadRequest;
        preloadRequest.m_resource     = resource;
        preloadRequest.m_absolutePath = finalAbsolutePath;
        result.m_effectPreload        = preloadRequest;
    }

    result.m_imported = true;
    XINFO("Successfully imported audio: {} as ID: {}",
          relPathUtf8,
          resource.m_id);
    return result;
}

/// @brief 将单个谱面文件同步到项目谱面列表。
/// @param project 当前打开的项目。
/// @param mapPath 需要同步的谱面文件路径。
/// @return 项目谱面列表是否发生变化。
ProjectCommandService::ProjectMutationResult
ProjectCommandService::syncProjectWithFile(
    Project& project, const std::filesystem::path& mapPath) const
{
    /// @brief 本次项目同步的返回结果。
    ProjectMutationResult result;

    /// @brief 需要同步的谱面绝对路径。
    auto absMapPath = std::filesystem::absolute(mapPath);
    /// @brief 当前项目根目录绝对路径。
    auto absRoot = std::filesystem::absolute(project.m_projectRoot);

    /// @brief 项目根路径和谱面路径的公共前缀比较迭代器。
    auto [rootIt, pathIt] = std::mismatch(
        absRoot.begin(), absRoot.end(), absMapPath.begin(), absMapPath.end());
    (void)pathIt;

    if ( rootIt != absRoot.end() ) {
        return result;
    }

    /// @brief 谱面文件相对于项目根目录的 UTF-8 路径。
    std::string relMapPath =
        Config::pathToUtf8(std::filesystem::relative(absMapPath, absRoot));
    if ( removeExcludedPath(project.m_excludedBeatmapPaths, relMapPath) ) {
        result.m_changed = true;
    }

    for ( const auto& entry : project.m_beatmaps ) {
        /// @brief 已登记谱面入口的绝对路径。
        auto entryPath = absRoot / Config::utf8ToPath(entry.m_filePath);
        if ( std::filesystem::exists(entryPath) &&
             std::filesystem::equivalent(entryPath, absMapPath) ) {
            return result;
        }
    }

    try {
        /// @brief 临时加载的新谱面，用于读取显示名和主音轨。
        auto map = BeatMap::loadFromFile(absMapPath);
        normalizeBeatmapMetadataPathsForProject(map, project);

        /// @brief 新发现谱面在项目列表中的入口。
        Project::BeatmapEntry entry;
        entry.m_name = map.m_baseMapMetadata.version;
        if ( entry.m_name.empty() ) {
            entry.m_name = Config::pathToUtf8(absMapPath.filename());
        }

        entry.m_filePath = relMapPath;

        if ( !map.m_baseMapMetadata.main_audio_path.empty() ) {
            entry.m_audioTrackId = Config::pathToUtf8(
                map.m_baseMapMetadata.main_audio_path.filename());
        }

        project.m_beatmaps.push_back(entry);
        result.m_changed = true;
        XINFO("EditorEngine: Discovered new beatmap for project: {}",
              entry.m_name);
    } catch ( const std::exception& e ) {
        XWARN("EditorEngine: Failed to sync new beatmap {}: {}",
              Config::pathToUtf8(absMapPath),
              e.what());
    }

    return result;
}

/// @brief 更新项目内谱面条目的文件路径关联。
/// @param project 当前打开的项目。
/// @param oldPath 旧谱面路径。
/// @param newPath 新谱面路径。
/// @return 项目谱面列表是否发生变化。
ProjectCommandService::ProjectMutationResult
ProjectCommandService::updateBeatmapFilePath(
    Project& project, const std::filesystem::path& oldPath,
    const std::filesystem::path& newPath) const
{
    /// @brief 当前项目根目录绝对路径。
    auto absRoot = std::filesystem::absolute(project.m_projectRoot);
    /// @brief 旧谱面路径解析后的绝对路径。
    auto absOld = resolveProjectPath(project, oldPath);
    /// @brief 新谱面路径解析后的绝对路径。
    auto absNew = resolveProjectPath(project, newPath);

    /// @brief 旧谱面相对路径计算错误码。
    std::error_code oldEc;
    /// @brief 新谱面相对路径计算错误码。
    std::error_code newEc;
    /// @brief 旧谱面相对于项目根目录的路径。
    auto relOldPath = std::filesystem::relative(absOld, absRoot, oldEc);
    /// @brief 新谱面相对于项目根目录的路径。
    auto relNewPath = std::filesystem::relative(absNew, absRoot, newEc);

    /// @brief 旧谱面项目相对路径的 UTF-8 字符串。
    std::string relOld =
        (oldEc || relOldPath.empty()) ? "" : Config::pathToUtf8(relOldPath);
    /// @brief 新谱面项目相对路径的 UTF-8 字符串。
    std::string relNew =
        (newEc || relNewPath.empty()) ? "" : Config::pathToUtf8(relNewPath);

    for ( auto& entry : project.m_beatmaps ) {
        if ( entry.m_filePath != relOld ) {
            continue;
        }

        entry.m_filePath = relNew;
        try {
            /// @brief 临时加载的新谱面，用于刷新项目入口元数据。
            auto map = BeatMap::loadFromFile(absNew);
            normalizeBeatmapMetadataPathsForProject(map, project);
            entry.m_name = map.m_baseMapMetadata.version;
            if ( entry.m_name.empty() ) {
                entry.m_name = Config::pathToUtf8(absNew.filename());
            }
            if ( !map.m_baseMapMetadata.main_audio_path.empty() ) {
                entry.m_audioTrackId = Config::pathToUtf8(
                    map.m_baseMapMetadata.main_audio_path.filename());
            }
        } catch ( ... ) {
        }

        return ProjectMutationResult{ true };
    }

    return syncProjectWithFile(project, newPath);
}

/// @brief 更新音频资源类型。
/// @param project 当前打开的项目。
/// @param cmd 更新音频资源命令。
/// @return 更新音频资源的处理结果。
ProjectCommandService::UpdateAudioResourceResult
ProjectCommandService::updateAudioResource(
    Project& project, const CmdUpdateAudioResource& cmd) const
{
    /// @brief 本次更新音频资源的返回结果。
    UpdateAudioResourceResult result;

    XINFO("Updating audio resource type: {} -> {}",
          cmd.id,
          (cmd.newType == AudioTrackType::Main ? "Main" : "Effect"));

    for ( auto& resource : project.m_audioResources ) {
        if ( resource.m_id != cmd.id ) {
            continue;
        }

        resource.m_type  = cmd.newType;
        result.m_updated = true;
        if ( resource.m_type == AudioTrackType::Effect ) {
            /// @brief 音频资源在项目目录中的绝对路径。
            auto absPath = resolveProjectPath(
                project, Config::utf8ToPath(resource.m_path));
            if ( std::filesystem::exists(absPath) ) {
                /// @brief 更新后音效的预加载请求。
                AudioPreloadRequest preloadRequest;
                preloadRequest.m_resource     = resource;
                preloadRequest.m_absolutePath = absPath;
                result.m_effectPreload        = preloadRequest;
            }
        }
        break;
    }

    return result;
}

/// @brief 从项目中删除音频资源并清理谱面引用。
/// @param project 当前打开的项目。
/// @param cmd 删除音频资源命令。
/// @return 删除音频资源的处理结果。
ProjectCommandService::RemoveAudioResourceResult
ProjectCommandService::removeAudioResource(
    Project& project, const CmdRemoveAudioResource& cmd) const
{
    /// @brief 本次删除音频资源的返回结果。
    RemoveAudioResourceResult result;

    XINFO("Removing audio resource from project: {}", cmd.id);

    /// @brief 被删除音频资源的项目相对路径。
    std::string removedPath;
    /// @brief 当前项目音频资源列表引用。
    auto& resources = project.m_audioResources;
    /// @brief 删除前的音频资源数量。
    auto oldSize = resources.size();

    resources.erase(
        std::remove_if(resources.begin(),
                       resources.end(),
                       [&](const AudioResource& resource) {
                           if ( resource.m_id != cmd.id ) {
                               return false;
                           }
                           removedPath = resource.m_path;
                           if ( resource.m_type == AudioTrackType::Effect ) {
                               result.m_effectResourceIdToUnload =
                                   resource.m_id;
                           }
                           return true;
                       }),
        resources.end());

    result.m_removed = resources.size() != oldSize;
    if ( !result.m_removed ) {
        return result;
    }

    addExcludedPath(project.m_excludedAudioPaths, removedPath);

    for ( auto& map : project.m_beatmaps ) {
        if ( map.m_audioTrackId == cmd.id ) {
            map.m_audioTrackId = "";
        }
    }

    return result;
}

/// @brief 从项目谱面列表中删除谱面。
/// @param project 当前打开的项目。
/// @param cmd 删除谱面命令。
/// @return 项目谱面列表是否发生变化。
ProjectCommandService::ProjectMutationResult
ProjectCommandService::removeBeatmap(Project&                project,
                                     const CmdRemoveBeatmap& cmd) const
{
    /// @brief 本次删除谱面的返回结果。
    ProjectMutationResult result;

    XINFO("Removing beatmap from project list: {}", cmd.filePath);

    if ( addExcludedPath(project.m_excludedBeatmapPaths, cmd.filePath) ) {
        result.m_changed = true;
    }

    /// @brief 当前项目谱面列表引用。
    auto& maps = project.m_beatmaps;
    /// @brief 删除前的谱面条目数量。
    auto oldSize = maps.size();
    maps.erase(std::remove_if(maps.begin(),
                              maps.end(),
                              [&](const Project::BeatmapEntry& entry) {
                                  return entry.m_filePath == cmd.filePath;
                              }),
               maps.end());

    if ( maps.size() != oldSize ) {
        result.m_changed = true;
    }

    return result;
}

/// @brief 规范化项目相对路径，用于稳定比较排除列表。
/// @param path UTF-8 编码的项目相对路径。
/// @return 规范化后的 UTF-8 项目相对路径。
std::string ProjectCommandService::normalizeProjectRelativePath(
    const std::string& path)
{
    if ( path.empty() ) return "";
    return Config::pathToUtf8(Config::utf8ToPath(path).lexically_normal());
}

/// @brief 判断路径是否存在于排除列表中。
/// @param excludedPaths 项目排除列表。
/// @param path 需要检查的 UTF-8 项目相对路径。
/// @return 路径已被排除时返回 true。
bool ProjectCommandService::containsExcludedPath(
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

/// @brief 将路径加入排除列表。
/// @param excludedPaths 项目排除列表。
/// @param path 需要加入的 UTF-8 项目相对路径。
/// @return 排除列表发生变化时返回 true。
bool ProjectCommandService::addExcludedPath(
    std::vector<std::string>& excludedPaths, const std::string& path)
{
    /// @brief 规范化后的待加入项目相对路径。
    std::string normalized = normalizeProjectRelativePath(path);
    if ( normalized.empty() ||
         containsExcludedPath(excludedPaths, normalized) ) {
        return false;
    }

    excludedPaths.push_back(normalized);
    return true;
}

/// @brief 从排除列表移除路径。
/// @param excludedPaths 项目排除列表。
/// @param path 需要移除的 UTF-8 项目相对路径。
/// @return 排除列表发生变化时返回 true。
bool ProjectCommandService::removeExcludedPath(
    std::vector<std::string>& excludedPaths, const std::string& path)
{
    /// @brief 规范化后的待移除项目相对路径。
    std::string normalized = normalizeProjectRelativePath(path);
    /// @brief 移除前的排除列表长度。
    auto oldSize = excludedPaths.size();
    excludedPaths.erase(
        std::remove_if(excludedPaths.begin(),
                       excludedPaths.end(),
                       [&](const std::string& excludedPath) {
                           return normalizeProjectRelativePath(excludedPath) ==
                                  normalized;
                       }),
        excludedPaths.end());
    return excludedPaths.size() != oldSize;
}

/// @brief 解析项目持久化路径为可访问的文件系统路径。
/// @param project 路径所属项目。
/// @param path 项目相对路径或绝对路径。
/// @return 规范化后的文件系统路径。
std::filesystem::path ProjectCommandService::resolveProjectPath(
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
std::filesystem::path ProjectCommandService::makeProjectRelativePath(
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
std::filesystem::path ProjectCommandService::resolveMetadataResourcePath(
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
void ProjectCommandService::normalizeBeatmapMetadataPathsForProject(
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

/// @brief 创建默认音轨配置。
/// @return 默认音轨配置。
AudioTrackConfig ProjectCommandService::makeDefaultAudioConfig()
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

}  // namespace MMM::Logic
