#include "logic/ProjectResourceService.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

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

/// @brief 谱面引用用途的紧凑位掩码。
using AudioReferenceKindMask = std::uint8_t;

/// @brief 歌曲文件提示引用位。
constexpr AudioReferenceKindMask SONG_FILE_HINT_MASK = 1U << 0U;

/// @brief 玩家物件采样绑定引用位。
constexpr AudioReferenceKindMask NOTE_SAMPLE_BINDING_MASK = 1U << 1U;

/// @brief 自动采样物件引用位。
constexpr AudioReferenceKindMask AUDIO_SAMPLE_EVENT_MASK = 1U << 2U;

/// @brief 将引用用途转换为索引位。
/// @param kind 引用用途。
/// @return 对应的单一索引位。
AudioReferenceKindMask audioReferenceKindMask(BeatmapAudioReferenceKind kind)
{
    switch ( kind ) {
    case BeatmapAudioReferenceKind::SongFileHint: return SONG_FILE_HINT_MASK;
    case BeatmapAudioReferenceKind::NoteSampleBinding:
        return NOTE_SAMPLE_BINDING_MASK;
    case BeatmapAudioReferenceKind::AudioSampleEvent:
        return AUDIO_SAMPLE_EVENT_MASK;
    }
    return 0U;
}

/// @brief 项目路径索引共享的根目录上下文。
struct ProjectPathLookupContext {
    /// @brief 一次性弱规范化后的项目根目录。
    std::filesystem::path m_projectRoot;
};

/// @brief 创建一次性项目路径查找上下文。
/// @param project 当前项目。
/// @return 缓存项目根目录后的上下文。
ProjectPathLookupContext makeProjectPathLookupContext(const Project& project)
{
    return ProjectPathLookupContext{ weaklyCanonicalAbsolutePath(
        project.m_projectRoot) };
}

/// @brief 将路径文本中的 Windows 分隔符转换为当前平台可解析形式。
/// @param pathText 待转换的 UTF-8 路径文本。
/// @return 分隔符统一后的文本。
std::string normalizePathSeparators(std::string pathText)
{
    std::replace(pathText.begin(), pathText.end(), '\\', '/');
    return pathText;
}

/// @brief 生成项目路径索引键。
/// @param context 已缓存的项目根目录。
/// @param pathText 项目相对路径、绝对路径或旧版带项目目录前缀路径。
/// @return 可用于哈希比较的 UTF-8 路径键。
/// @note 相对路径只做词法规范化；仅绝对旧路径需要弱规范化。
std::string makeProjectPathLookupKey(const ProjectPathLookupContext& context,
                                     const std::string&              pathText)
{
    if ( pathText.empty() ) return {};

    auto path = Config::utf8ToPath(normalizePathSeparators(pathText))
                    .lexically_normal();
    if ( path.is_absolute() ) {
        const auto absolutePath = weaklyCanonicalAbsolutePath(path);
        const auto relativePath =
            absolutePath.lexically_relative(context.m_projectRoot);
        if ( isRelativePathInsideRoot(relativePath) ) {
            return Config::pathToUtf8(relativePath.lexically_normal());
        }
        return Config::pathToUtf8(absolutePath.lexically_normal());
    }

    const auto stripped = stripProjectFolderPrefix(context.m_projectRoot, path);
    if ( isRelativePathInsideRoot(stripped) ) {
        path = stripped.lexically_normal();
    }
    return Config::pathToUtf8(path);
}

/// @brief 判断谱面相对候选是否越出项目根并需要旧 filename 回退。
/// @param mapRelativePath 尚未转换为项目键的谱面相对候选。
/// @param mapRelativePathKey 已转换后的项目路径键。
/// @return 旧 makeProjectRelativePath 会退回 filename 时返回 true。
bool requiresMapRelativeFilenameFallback(
    const std::filesystem::path& mapRelativePath,
    const std::string&           mapRelativePathKey)
{
    return (mapRelativePath.is_relative() &&
            !isRelativePathInsideRoot(mapRelativePath)) ||
           Config::utf8ToPath(mapRelativePathKey).is_absolute();
}

/// @brief 预先汇总全部谱面引用的不可变哈希索引。
///
/// 构建成本与引用数量线性相关。后续任意资源查询仅计算资源自身路径，
/// 不再执行 resource×reference 的路径存在性检查或弱规范化。
struct AudioReferenceLookupIndex {
    /// @brief 单个候选资源预计算后的 ID 与项目路径键。
    struct ResourceLookupKey {
        /// @brief 稳定资源 ID。
        std::string m_resourceId;

        /// @brief 规范化项目路径键。
        std::string m_projectPath;
    };

    /// @brief 仅创建共享路径上下文，不预先加入引用。
    /// @param project 引用所属项目。
    explicit AudioReferenceLookupIndex(const Project& project)
        : m_pathContext(makeProjectPathLookupContext(project))
    {
    }

    /// @brief 从完整谱面引用列表构建索引。
    /// @param project 引用所属项目。
    /// @param references 已汇总的谱面引用。
    AudioReferenceLookupIndex(
        const Project&                            project,
        const std::vector<BeatmapAudioReference>& references)
        : m_pathContext(makeProjectPathLookupContext(project))
    {
        m_resourceIdKinds.reserve(references.size() * 2U);
        m_projectPathKinds.reserve(references.size() * 2U);
        for ( const auto& reference : references ) {
            addReference(reference);
        }
    }

    /// @brief 查询资源匹配到的全部引用用途。
    /// @param resource 待查询项目资源。
    /// @return 匹配用途的位掩码；资源 ID 为空时返回零。
    [[nodiscard]] AudioReferenceKindMask matchingKinds(
        const AudioResource& resource) const
    {
        if ( resource.m_id.empty() ) return 0U;

        AudioReferenceKindMask result = 0U;
        const auto idIterator         = m_resourceIdKinds.find(resource.m_id);
        if ( idIterator != m_resourceIdKinds.end() ) {
            result |= idIterator->second;
        }

        const auto resourcePathKey = projectPathKey(resource.m_path);
        if ( !resourcePathKey.empty() ) {
            const auto pathIterator = m_projectPathKinds.find(resourcePathKey);
            if ( pathIterator != m_projectPathKinds.end() ) {
                result |= pathIterator->second;
            }
        }
        return result;
    }

    /// @brief 预计算单个资源的查找键。
    /// @param resource 待转换项目资源。
    /// @return 可跨多个引用复用的资源键。
    [[nodiscard]] ResourceLookupKey makeResourceLookupKey(
        const AudioResource& resource) const
    {
        return ResourceLookupKey{
            resource.m_id,
            projectPathKey(resource.m_path),
        };
    }

    /// @brief 判断一个引用是否匹配预计算资源键。
    /// @param reference 待判断谱面引用。
    /// @param resourceKey 候选项目资源的预计算键。
    /// @return 稳定 ID、项目路径、旧 basename 或谱面相对路径匹配时返回
    /// true。
    [[nodiscard]] bool referenceMatchesResource(
        const BeatmapAudioReference& reference,
        const ResourceLookupKey&     resourceKey) const
    {
        if ( reference.m_audioReference.empty() ||
             resourceKey.m_resourceId.empty() ) {
            return false;
        }
        if ( reference.m_audioReference == resourceKey.m_resourceId ) {
            return true;
        }

        const auto normalizedReferenceText =
            normalizePathSeparators(reference.m_audioReference);
        const auto referencePath = Config::utf8ToPath(normalizedReferenceText);
        if ( Config::pathToUtf8(referencePath.filename()) ==
             resourceKey.m_resourceId ) {
            return true;
        }

        if ( resourceKey.m_projectPath.empty() ) return false;
        if ( projectPathKey(normalizedReferenceText) ==
             resourceKey.m_projectPath ) {
            return true;
        }

        if ( reference.m_beatmapPath.empty() || referencePath.is_absolute() ) {
            return false;
        }

        const auto beatmapPathKey = projectPathKey(reference.m_beatmapPath);
        if ( beatmapPathKey.empty() ) return false;
        const auto mapRelativePath =
            (Config::utf8ToPath(beatmapPathKey).parent_path() / referencePath)
                .lexically_normal();
        const auto mapRelativePathKey =
            projectPathKey(Config::pathToUtf8(mapRelativePath));
        if ( mapRelativePathKey == resourceKey.m_projectPath ) return true;
        if ( !requiresMapRelativeFilenameFallback(mapRelativePath,
                                                  mapRelativePathKey) ) {
            return false;
        }
        return projectPathKey(Config::pathToUtf8(mapRelativePath.filename())) ==
               resourceKey.m_projectPath;
    }

    /// @brief 判断一个引用是否匹配指定资源。
    /// @param reference 待判断谱面引用。
    /// @param resource 候选项目资源。
    /// @return 任一兼容引用形式匹配时返回 true。
    [[nodiscard]] bool referenceMatchesResource(
        const BeatmapAudioReference& reference,
        const AudioResource&         resource) const
    {
        return referenceMatchesResource(reference,
                                        makeResourceLookupKey(resource));
    }

    /// @brief 生成与该索引一致的项目路径键。
    /// @param pathText 待规范化的项目路径文本。
    /// @return 可用于现有资源哈希表的 UTF-8 键。
    [[nodiscard]] std::string projectPathKey(const std::string& pathText) const
    {
        return makeProjectPathLookupKey(m_pathContext, pathText);
    }

private:
    /// @brief 将单个谱面引用加入 ID、项目路径和谱面相对路径索引。
    /// @param reference 待加入的谱面引用。
    void addReference(const BeatmapAudioReference& reference)
    {
        if ( reference.m_audioReference.empty() ) return;

        const auto kindMask = audioReferenceKindMask(reference.m_kind);
        m_resourceIdKinds[reference.m_audioReference] |= kindMask;

        const auto normalizedReferenceText =
            normalizePathSeparators(reference.m_audioReference);
        const auto referencePath = Config::utf8ToPath(normalizedReferenceText);
        const auto filename      = Config::pathToUtf8(referencePath.filename());
        if ( !filename.empty() ) {
            m_resourceIdKinds[filename] |= kindMask;
        }

        const auto directPathKey = projectPathKey(normalizedReferenceText);
        if ( !directPathKey.empty() ) {
            m_projectPathKinds[directPathKey] |= kindMask;
        }

        if ( reference.m_beatmapPath.empty() || referencePath.is_absolute() ) {
            return;
        }

        const auto beatmapPathKey = projectPathKey(reference.m_beatmapPath);
        if ( beatmapPathKey.empty() ) return;

        const auto mapRelativePath =
            (Config::utf8ToPath(beatmapPathKey).parent_path() / referencePath)
                .lexically_normal();
        const auto mapRelativePathKey =
            projectPathKey(Config::pathToUtf8(mapRelativePath));
        if ( !mapRelativePathKey.empty() ) {
            m_projectPathKinds[mapRelativePathKey] |= kindMask;
        }
        if ( requiresMapRelativeFilenameFallback(mapRelativePath,
                                                 mapRelativePathKey) ) {
            const auto filenamePathKey =
                projectPathKey(Config::pathToUtf8(mapRelativePath.filename()));
            if ( !filenamePathKey.empty() ) {
                m_projectPathKinds[filenamePathKey] |= kindMask;
            }
        }
    }

    /// @brief 缓存项目根目录，避免逐次资源查询访问文件系统。
    ProjectPathLookupContext m_pathContext;

    /// @brief 按稳定 ID、原始引用和旧版 basename 汇总的用途。
    std::unordered_map<std::string, AudioReferenceKindMask> m_resourceIdKinds;

    /// @brief 按项目相对路径和谱面相对路径汇总的用途。
    std::unordered_map<std::string, AudioReferenceKindMask> m_projectPathKinds;
};

/// @brief 项目资源按兼容引用键建立的首次匹配解析索引。
///
/// 每个候选保留资源在项目列表中的原始序号。单个引用同时命中 ID、路径或
/// basename 时选择序号最小者，保持旧版逐资源扫描的首次匹配语义。
struct AudioResourceResolutionIndex {
    /// @brief 一个哈希键对应的最早项目资源。
    struct Candidate {
        /// @brief 候选资源地址。
        const AudioResource* m_resource{ nullptr };

        /// @brief 资源在项目列表中的原始序号。
        std::size_t m_resourceIndex{ 0U };
    };

    /// @brief 一批引用共享的谱面目录解析状态。
    struct BeatmapDirectory {
        /// @brief 调用方是否提供了有效谱面路径。
        bool m_available{ false };

        /// @brief 规范化后的谱面目录；项目根目录谱面时允许为空。
        std::filesystem::path m_path;
    };

    /// @brief 从项目全部音频资源构建解析索引。
    /// @param project 待索引项目。
    explicit AudioResourceResolutionIndex(const Project& project)
        : m_pathContext(makeProjectPathLookupContext(project))
    {
        m_resourcesById.reserve(project.m_audioResources.size());
        m_resourcesByPath.reserve(project.m_audioResources.size());
        for ( std::size_t index = 0U; index < project.m_audioResources.size();
              ++index ) {
            const auto& resource = project.m_audioResources[index];
            if ( resource.m_id.empty() ) continue;

            const Candidate candidate{ &resource, index };
            m_resourcesById.try_emplace(resource.m_id, candidate);

            const auto pathKey =
                makeProjectPathLookupKey(m_pathContext, resource.m_path);
            if ( !pathKey.empty() ) {
                m_resourcesByPath.try_emplace(pathKey, candidate);
            }
        }
    }

    /// @brief 预计算同一批引用共享的谱面目录键。
    /// @param beatmapPath 谱面的项目相对或绝对路径。
    /// @return 可与相对音频引用拼接的目录状态。
    [[nodiscard]] BeatmapDirectory beatmapDirectoryKey(
        const std::filesystem::path& beatmapPath) const
    {
        if ( beatmapPath.empty() ) return {};
        const auto beatmapKey = makeProjectPathLookupKey(
            m_pathContext, Config::pathToUtf8(beatmapPath));
        return BeatmapDirectory{
            true,
            Config::utf8ToPath(beatmapKey).parent_path(),
        };
    }

    /// @brief 解析单个 ID 或旧路径引用。
    /// @param audioReference 谱面保存的引用文本。
    /// @param beatmapDirectory 已预计算的谱面目录键。
    /// @return 所有兼容匹配方式中项目序号最小的资源。
    [[nodiscard]] const AudioResource* resolve(
        const std::string&      audioReference,
        const BeatmapDirectory& beatmapDirectory) const
    {
        if ( audioReference.empty() ) return nullptr;

        const Candidate* bestCandidate = nullptr;
        /// @brief 合并一个候选并保留项目列表中最早的资源。
        const auto considerCandidate = [&](const Candidate* candidate) {
            if ( !candidate ) return;
            if ( !bestCandidate ||
                 candidate->m_resourceIndex < bestCandidate->m_resourceIndex ) {
                bestCandidate = candidate;
            }
        };
        /// @brief 按指定键查询候选哈希表。
        const auto considerByKey = [&](const auto&        candidates,
                                       const std::string& key) {
            if ( key.empty() ) return;
            const auto iterator = candidates.find(key);
            if ( iterator != candidates.end() ) {
                considerCandidate(&iterator->second);
            }
        };

        considerByKey(m_resourcesById, audioReference);

        const auto normalizedReferenceText =
            normalizePathSeparators(audioReference);
        const auto referencePath = Config::utf8ToPath(normalizedReferenceText);
        considerByKey(m_resourcesById,
                      Config::pathToUtf8(referencePath.filename()));
        considerByKey(
            m_resourcesByPath,
            makeProjectPathLookupKey(m_pathContext, normalizedReferenceText));

        if ( beatmapDirectory.m_available && !referencePath.is_absolute() ) {
            const auto mapRelativePath =
                (beatmapDirectory.m_path / referencePath).lexically_normal();
            const auto mapRelativePathKey = makeProjectPathLookupKey(
                m_pathContext, Config::pathToUtf8(mapRelativePath));
            considerByKey(m_resourcesByPath, mapRelativePathKey);

            if ( requiresMapRelativeFilenameFallback(mapRelativePath,
                                                     mapRelativePathKey) ) {
                considerByKey(m_resourcesByPath,
                              Config::pathToUtf8(mapRelativePath.filename()));
            }
        }
        return bestCandidate ? bestCandidate->m_resource : nullptr;
    }

private:
    /// @brief 本批资源共享的项目根目录。
    ProjectPathLookupContext m_pathContext;

    /// @brief 按稳定 ID 和旧 basename 建立的首次资源表。
    std::unordered_map<std::string, Candidate> m_resourcesById;

    /// @brief 按规范化项目路径建立的首次资源表。
    std::unordered_map<std::string, Candidate> m_resourcesByPath;
};

/// @brief 使用预构建索引推断音频资源类型。
/// @param referenceIndex 全部谱面引用的哈希索引。
/// @param resource 待推断资源。
/// @return Note 绑定优先的资源类型；没有类型线索时返回 Effect。
AudioTrackType inferIndexedAudioResourceType(
    const AudioReferenceLookupIndex& referenceIndex,
    const AudioResource&             resource)
{
    const auto matchingKinds   = referenceIndex.matchingKinds(resource);
    const bool hasSongFileHint = (matchingKinds & SONG_FILE_HINT_MASK) != 0U;
    const bool hasNoteBinding =
        (matchingKinds & NOTE_SAMPLE_BINDING_MASK) != 0U;

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

/// @brief 判断绝对路径是否等于移动源或位于移动源目录内。
/// @param candidate 待检查的规范化绝对路径。
/// @param oldPath 移动前的规范化绝对路径。
/// @param newPath 移动后的规范化绝对路径。
/// @return 候选路径移动后的绝对路径；不受移动影响时返回原路径。
std::filesystem::path remapAbsolutePathForMove(
    const std::filesystem::path& candidate,
    const std::filesystem::path& oldPath, const std::filesystem::path& newPath)
{
    if ( candidate == oldPath ) return newPath;

    std::error_code relativeError;
    const auto      suffix =
        std::filesystem::relative(candidate, oldPath, relativeError);
    if ( relativeError || !isRelativePathInsideRoot(suffix) ) {
        return candidate;
    }
    return (newPath / suffix).lexically_normal();
}

/// @brief 按移动前的项目路径解析谱面或资源绝对路径。
/// @param project 路径所属项目。
/// @param storedPath 项目保存的相对或绝对路径。
/// @return 弱规范化后的绝对路径。
std::filesystem::path resolveStoredProjectPath(
    const Project& project, const std::filesystem::path& storedPath)
{
    if ( storedPath.is_absolute() ) {
        return weaklyCanonicalAbsolutePath(storedPath);
    }
    return weaklyCanonicalAbsolutePath(project.m_projectRoot / storedPath);
}

/// @brief RM/IMD 能够隐式发现的音频扩展名，顺序与加载器一致。
static constexpr std::array<std::string_view, 7> RM_AUDIO_EXTENSIONS{
    ".mp3", ".wav", ".ogg", ".flac", ".opus", ".aac", ".m4a"
};

/// @brief 提取 RM/IMD 谱面文件名用于匹配音频的前缀。
/// @param mapPath RM/IMD 谱面路径。
/// @return 第一个下划线前的文件名前缀；无下划线时为空。
std::string imdAudioPrefix(const std::filesystem::path& mapPath)
{
    const auto filename  = Config::pathToUtf8(mapPath.filename());
    const auto separator = filename.find('_');
    return separator == std::string::npos ? std::string{}
                                          : filename.substr(0, separator);
}

/// @brief 判断路径是否等于指定根或位于其目录内。
/// @param path 待检查绝对路径。
/// @param root 候选根路径。
/// @return 相同或位于根内时返回 true。
bool pathIsInsideOrSame(const std::filesystem::path& path,
                        const std::filesystem::path& root)
{
    if ( path == root ) return true;
    std::error_code relativeError;
    const auto relative = std::filesystem::relative(path, root, relativeError);
    return !relativeError && isRelativePathInsideRoot(relative);
}

/// @brief 判断一个目标路径在文件移动完成后是否会存在。
/// @param candidateAfterMove 移动完成后的候选绝对路径。
/// @param absoluteOldPath 移动源绝对路径。
/// @param absoluteNewPath 移动目标绝对路径。
/// @return 按移动前文件系统投影后的存在性。
bool projectedPathExistsAfterMove(
    const std::filesystem::path& candidateAfterMove,
    const std::filesystem::path& absoluteOldPath,
    const std::filesystem::path& absoluteNewPath)
{
    std::filesystem::path sourceCandidate;
    if ( candidateAfterMove == absoluteNewPath ) {
        sourceCandidate = absoluteOldPath;
    } else {
        std::error_code relativeError;
        const auto      suffix = std::filesystem::relative(
            candidateAfterMove, absoluteNewPath, relativeError);
        if ( !relativeError && isRelativePathInsideRoot(suffix) ) {
            sourceCandidate = (absoluteOldPath / suffix).lexically_normal();
        }
    }

    std::error_code filesystemError;
    if ( !sourceCandidate.empty() ) {
        return std::filesystem::exists(sourceCandidate, filesystemError) &&
               !filesystemError;
    }
    if ( pathIsInsideOrSame(candidateAfterMove, absoluteOldPath) ) {
        return false;
    }
    return std::filesystem::exists(candidateAfterMove, filesystemError) &&
           !filesystemError;
}

/// @brief 按 RM/IMD 规则查找当前文件系统实际选中的音频路径。
/// @param mapPath 当前谱面绝对路径。
/// @return 被隐式选中的音频绝对路径；没有音频时为空。
std::optional<std::filesystem::path> resolveCurrentImdAudio(
    const std::filesystem::path& mapPath)
{
    const auto prefix = imdAudioPrefix(mapPath);
    if ( prefix.empty() ) return std::nullopt;

    for ( const auto extension : RM_AUDIO_EXTENSIONS ) {
        const auto candidate = weaklyCanonicalAbsolutePath(
            mapPath.parent_path() /
            Config::utf8ToPath(prefix + std::string(extension)));
        std::error_code filesystemError;
        if ( std::filesystem::exists(candidate, filesystemError) &&
             !filesystemError ) {
            return candidate;
        }
    }
    return std::nullopt;
}

/// @brief 按 RM/IMD 规则推演移动完成后会选中的音频路径。
/// @param mapPathAfterMove 移动后的谱面绝对路径。
/// @param absoluteOldPath 移动源绝对路径。
/// @param absoluteNewPath 移动目标绝对路径。
/// @return 推演后被隐式选中的音频绝对路径；没有音频时为空。
std::optional<std::filesystem::path> resolveProjectedImdAudioAfterMove(
    const std::filesystem::path& mapPathAfterMove,
    const std::filesystem::path& absoluteOldPath,
    const std::filesystem::path& absoluteNewPath)
{
    const auto prefix = imdAudioPrefix(mapPathAfterMove);
    if ( prefix.empty() ) return std::nullopt;

    for ( const auto extension : RM_AUDIO_EXTENSIONS ) {
        const auto candidate = weaklyCanonicalAbsolutePath(
            mapPathAfterMove.parent_path() /
            Config::utf8ToPath(prefix + std::string(extension)));
        if ( projectedPathExistsAfterMove(
                 candidate, absoluteOldPath, absoluteNewPath) ) {
            return candidate;
        }
    }
    return std::nullopt;
}

/// @brief 比较两个可选路径是否表示同一规范化位置。
/// @param lhs 左侧路径。
/// @param rhs 右侧路径。
/// @return 同为空或规范化路径相同时返回 true。
bool optionalPathsEqual(const std::optional<std::filesystem::path>& lhs,
                        const std::optional<std::filesystem::path>& rhs)
{
    if ( lhs.has_value() != rhs.has_value() ) return false;
    return !lhs || weaklyCanonicalAbsolutePath(*lhs) ==
                       weaklyCanonicalAbsolutePath(*rhs);
}

/// @brief 单个项目音频资源的路径移动投影。
struct ResourcePathRemap {
    /// @brief 移动前资源快照。
    AudioResource m_before;

    /// @brief 移动后的项目相对路径。
    std::string m_afterPath;
};

/// @brief 计算全部项目音频资源在文件移动后的路径投影。
/// @param project 待检查项目。
/// @param absoluteOldPath 移动源绝对路径。
/// @param absoluteNewPath 移动目标绝对路径。
/// @return 包含未移动资源的完整路径投影。
std::vector<ResourcePathRemap> collectResourcePathProjections(
    const Project& project, const std::filesystem::path& absoluteOldPath,
    const std::filesystem::path& absoluteNewPath)
{
    std::vector<ResourcePathRemap> result;
    for ( const auto& resource : project.m_audioResources ) {
        const auto absoluteResourcePath = resolveStoredProjectPath(
            project, Config::utf8ToPath(resource.m_path));
        const auto remappedAbsolutePath = remapAbsolutePathForMove(
            absoluteResourcePath, absoluteOldPath, absoluteNewPath);
        if ( remappedAbsolutePath == absoluteResourcePath ) {
            result.push_back(ResourcePathRemap{ resource, resource.m_path });
            continue;
        }

        const auto relativePath = makeRelativeToProjectRoot(
            project.m_projectRoot, remappedAbsolutePath);
        if ( relativePath.empty() ) continue;
        const auto remappedPath =
            Config::pathToUtf8(relativePath.lexically_normal());
        result.push_back(ResourcePathRemap{ resource, remappedPath });
    }
    return result;
}

/// @brief 收集一次文件移动会实际改变路径的项目音频资源。
/// @param project 待检查项目。
/// @param absoluteOldPath 移动源绝对路径。
/// @param absoluteNewPath 移动目标绝对路径。
/// @return 路径发生变化且保持稳定 ID 的资源投影。
std::vector<ResourcePathRemap> collectResourcePathRemaps(
    const Project& project, const std::filesystem::path& absoluteOldPath,
    const std::filesystem::path& absoluteNewPath)
{
    auto projections = collectResourcePathProjections(
        project, absoluteOldPath, absoluteNewPath);
    std::erase_if(projections, [](const ResourcePathRemap& projection) {
        return projection.m_before.m_path == projection.m_afterPath;
    });
    return projections;
}

/// @brief 将 osu! 相对引用中的反斜杠统一为可解析路径。
/// @param reference osu! 字段原始值。
/// @return 使用当前平台目录分隔符的路径。
std::filesystem::path osuReferencePath(std::string reference)
{
    std::replace(reference.begin(), reference.end(), '\\', '/');
    return Config::utf8ToPath(reference);
}

/// @brief 为移动后的 osu! 谱面计算一个资源的新相对引用。
/// @param project 资源所属项目。
/// @param mapPathAfterMove 移动后的谱面绝对路径。
/// @param remap 资源路径投影。
/// @return 可由 osu! 保存的相对引用；跨卷等无法表达时为空。
std::optional<std::string> makeOsuReferenceAfterMove(
    const Project& project, const std::filesystem::path& mapPathAfterMove,
    const ResourcePathRemap& remap)
{
    const auto audioPathAfterMove = resolveStoredProjectPath(
        project, Config::utf8ToPath(remap.m_afterPath));
    const auto relative =
        audioPathAfterMove.lexically_relative(mapPathAfterMove.parent_path());
    if ( relative.empty() || relative.is_absolute() ) return std::nullopt;
    auto reference = Config::pathToUtf8(relative);
    std::replace(reference.begin(), reference.end(), '\\', '/');
    return reference;
}

/// @brief 查找一个 osu! 音频引用在移动后的替换文本。
/// @param project 资源所属项目。
/// @param reference osu! 字段中的原始引用。
/// @param mapPathBeforeMove 移动前谱面绝对路径。
/// @param mapPathAfterMove 移动后谱面绝对路径。
/// @param remaps 受影响资源路径投影。
/// @return 未引用受影响资源时为空；无法表达时返回空字符串。
std::optional<std::string> remapOsuAudioReference(
    const Project& project, const std::string& reference,
    const std::filesystem::path&          mapPathBeforeMove,
    const std::filesystem::path&          mapPathAfterMove,
    const std::vector<ResourcePathRemap>& remaps)
{
    if ( reference.empty() ) return std::nullopt;

    const auto referencePath = osuReferencePath(reference);
    const auto absoluteReference =
        referencePath.is_absolute()
            ? weaklyCanonicalAbsolutePath(referencePath)
            : weaklyCanonicalAbsolutePath(mapPathBeforeMove.parent_path() /
                                          referencePath);
    for ( const auto& remap : remaps ) {
        const auto absoluteResourceBefore = resolveStoredProjectPath(
            project, Config::utf8ToPath(remap.m_before.m_path));
        if ( reference != remap.m_before.m_id &&
             absoluteReference != absoluteResourceBefore ) {
            continue;
        }
        const auto replacement =
            makeOsuReferenceAfterMove(project, mapPathAfterMove, remap);
        return replacement.value_or(std::string{});
    }
    return std::nullopt;
}

/// @brief 去除一行文本指定区间两端的 ASCII 空白。
/// @param line 待检查行。
/// @param begin 区间起点，调用后指向首个非空白字符。
/// @param end 区间终点，调用后位于最后一个非空白字符之后。
void trimAsciiRange(const std::string& line, std::size_t& begin,
                    std::size_t& end)
{
    while ( begin < end &&
            std::isspace(static_cast<unsigned char>(line[begin])) ) {
        ++begin;
    }
    while ( end > begin &&
            std::isspace(static_cast<unsigned char>(line[end - 1U])) ) {
        --end;
    }
}

/// @brief 在不重新序列化谱面的前提下重写 osu! 音频路径字段。
/// @param source 原始 osu! 文本。
/// @param project 资源所属项目。
/// @param mapPathBeforeMove 移动前谱面绝对路径。
/// @param mapPathAfterMove 移动后谱面绝对路径。
/// @param remaps 受影响资源路径投影。
/// @param output 改写后的完整文本。
/// @param changed 是否实际替换了至少一个引用。
/// @return 所有匹配引用都能无损改写时返回 true。
bool rewriteOsuAudioReferenceText(
    const std::string& source, const Project& project,
    const std::filesystem::path&          mapPathBeforeMove,
    const std::filesystem::path&          mapPathAfterMove,
    const std::vector<ResourcePathRemap>& remaps, std::string& output,
    bool& changed)
{
    output.clear();
    output.reserve(source.size());
    changed = false;
    std::string currentSection;
    std::size_t offset = 0U;
    while ( offset < source.size() ) {
        const auto newline = source.find('\n', offset);
        const auto lineEnd =
            newline == std::string::npos ? source.size() : newline;
        std::string line              = source.substr(offset, lineEnd - offset);
        const bool  hasCarriageReturn = !line.empty() && line.back() == '\r';
        if ( hasCarriageReturn ) line.pop_back();

        std::size_t contentBegin = 0U;
        std::size_t contentEnd   = line.size();
        trimAsciiRange(line, contentBegin, contentEnd);
        if ( contentEnd > contentBegin + 1U && line[contentBegin] == '[' &&
             line[contentEnd - 1U] == ']' ) {
            currentSection =
                line.substr(contentBegin + 1U, contentEnd - contentBegin - 2U);
        } else {
            std::size_t referenceBegin = std::string::npos;
            std::size_t referenceEnd   = std::string::npos;
            if ( currentSection == "General" ) {
                const auto separator = line.find(':');
                if ( separator != std::string::npos ) {
                    std::size_t keyBegin = 0U;
                    std::size_t keyEnd   = separator;
                    trimAsciiRange(line, keyBegin, keyEnd);
                    if ( line.substr(keyBegin, keyEnd - keyBegin) ==
                         "AudioFilename" ) {
                        referenceBegin = separator + 1U;
                        referenceEnd   = line.size();
                    }
                }
            } else if ( currentSection == "HitObjects" &&
                        contentBegin < contentEnd &&
                        line[contentBegin] != '/' ) {
                const auto                 finalComma = line.rfind(',');
                std::array<std::size_t, 4> leadingCommas{};
                std::size_t                commaCount = 0U;
                std::size_t                searchFrom = 0U;
                while ( commaCount < leadingCommas.size() ) {
                    const auto comma = line.find(',', searchFrom);
                    if ( comma == std::string::npos ) break;
                    leadingCommas[commaCount++] = comma;
                    searchFrom                  = comma + 1U;
                }

                if ( finalComma != std::string::npos &&
                     commaCount == leadingCommas.size() ) {
                    std::size_t typeBegin = leadingCommas[2] + 1U;
                    std::size_t typeEnd   = leadingCommas[3];
                    trimAsciiRange(line, typeBegin, typeEnd);
                    std::uint32_t objectType = 0U;
                    const auto [parseEnd, parseError] =
                        std::from_chars(line.data() + typeBegin,
                                        line.data() + typeEnd,
                                        objectType);
                    if ( parseError == std::errc{} &&
                         parseEnd == line.data() + typeEnd ) {
                        const bool        isHold = (objectType & 128U) != 0U;
                        const std::size_t requiredSeparators = isHold ? 5U : 4U;
                        std::size_t       cursor             = finalComma + 1U;
                        for ( std::size_t index = 0U;
                              index < requiredSeparators;
                              ++index ) {
                            const auto separator = line.find(':', cursor);
                            if ( separator == std::string::npos ) {
                                cursor = std::string::npos;
                                break;
                            }
                            cursor = separator + 1U;
                        }
                        if ( cursor != std::string::npos ) {
                            referenceBegin = cursor;
                            referenceEnd   = line.size();
                        }
                    }
                }
            }

            if ( referenceBegin != std::string::npos ) {
                trimAsciiRange(line, referenceBegin, referenceEnd);
                if ( referenceBegin < referenceEnd ) {
                    const auto reference = line.substr(
                        referenceBegin, referenceEnd - referenceBegin);
                    const auto replacement =
                        remapOsuAudioReference(project,
                                               reference,
                                               mapPathBeforeMove,
                                               mapPathAfterMove,
                                               remaps);
                    if ( replacement && replacement->empty() ) return false;
                    if ( replacement && *replacement != reference ) {
                        line.replace(referenceBegin,
                                     referenceEnd - referenceBegin,
                                     *replacement);
                        changed = true;
                    }
                }
            }
        }

        output += line;
        if ( hasCarriageReturn ) output.push_back('\r');
        if ( newline != std::string::npos ) output.push_back('\n');
        offset = newline == std::string::npos ? source.size() : newline + 1U;
    }
    return true;
}

/// @brief 读取完整文本文件且不改变换行符。
/// @param path 待读取路径。
/// @param output 文件内容。
/// @return 成功打开并完整读取时返回 true。
bool readBinaryTextFile(const std::filesystem::path& path, std::string& output)
{
    std::ifstream stream(path, std::ios::binary);
    if ( !stream.is_open() ) return false;
    output.assign(std::istreambuf_iterator<char>(stream),
                  std::istreambuf_iterator<char>());
    return stream.good() || stream.eof();
}

/// @brief 一个等待批量提交的 osu! 文本替换。
struct PendingTextFileReplacement {
    /// @brief 原谱面路径。
    std::filesystem::path m_targetPath;

    /// @brief 已写完并等待替换的临时文件。
    std::filesystem::path m_temporaryPath;

    /// @brief 提交期间保存原内容的备份文件。
    std::filesystem::path m_backupPath;
};

/// @brief 将 osu! 改写内容保存到同目录临时文件。
/// @param path 原谱面路径。
/// @param content 已改写文本。
/// @param replacement 成功时填入待提交路径。
/// @return 临时文件完整写出时返回 true。
bool stageTextFileReplacement(const std::filesystem::path& path,
                              const std::string&           content,
                              PendingTextFileReplacement&  replacement)
{
    replacement.m_targetPath    = path;
    replacement.m_temporaryPath = path;
    replacement.m_temporaryPath += ".mmm-audio-remap.tmp";
    replacement.m_backupPath = path;
    replacement.m_backupPath += ".mmm-audio-remap.bak";

    std::error_code filesystemError;
    const bool      temporaryExists =
        std::filesystem::exists(replacement.m_temporaryPath, filesystemError);
    if ( filesystemError || temporaryExists ) return false;
    filesystemError.clear();
    const bool backupExists =
        std::filesystem::exists(replacement.m_backupPath, filesystemError);
    if ( filesystemError || backupExists ) return false;

    std::ofstream stream(replacement.m_temporaryPath,
                         std::ios::binary | std::ios::trunc);
    if ( !stream.is_open() ) return false;
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.close();
    if ( stream.good() ) return true;

    filesystemError.clear();
    std::filesystem::remove(replacement.m_temporaryPath, filesystemError);
    return false;
}

/// @brief 清理一组尚未提交的 osu! 临时文件。
/// @param replacements 待清理替换列表。
void cleanupTextFileReplacements(
    const std::vector<PendingTextFileReplacement>& replacements)
{
    for ( const auto& replacement : replacements ) {
        std::error_code filesystemError;
        std::filesystem::remove(replacement.m_temporaryPath, filesystemError);
    }
}

/// @brief 将全部 osu! 临时文件作为一个事务替换原文件。
/// @param replacements 已完成临时写出的替换列表。
/// @return 全部替换成功时返回 true；失败时尽量恢复所有原文件。
bool commitTextFileReplacements(
    std::vector<PendingTextFileReplacement>& replacements)
{
    std::size_t committedCount = 0U;
    for ( auto& replacement : replacements ) {
        std::error_code filesystemError;
        std::filesystem::rename(replacement.m_targetPath,
                                replacement.m_backupPath,
                                filesystemError);
        if ( filesystemError ) break;

        std::filesystem::rename(replacement.m_temporaryPath,
                                replacement.m_targetPath,
                                filesystemError);
        if ( filesystemError ) {
            std::error_code restoreError;
            std::filesystem::rename(replacement.m_backupPath,
                                    replacement.m_targetPath,
                                    restoreError);
            break;
        }
        ++committedCount;
    }

    if ( committedCount != replacements.size() ) {
        while ( committedCount > 0U ) {
            --committedCount;
            auto&           replacement = replacements[committedCount];
            std::error_code filesystemError;
            std::filesystem::remove(replacement.m_targetPath, filesystemError);
            filesystemError.clear();
            std::filesystem::rename(replacement.m_backupPath,
                                    replacement.m_targetPath,
                                    filesystemError);
            if ( filesystemError ) {
                XERROR("Failed to restore osu! beatmap after move error: {}",
                       Config::pathToUtf8(replacement.m_targetPath));
            }
        }
        cleanupTextFileReplacements(replacements);
        return false;
    }

    for ( const auto& replacement : replacements ) {
        std::error_code filesystemError;
        std::filesystem::remove(replacement.m_backupPath, filesystemError);
        if ( filesystemError ) {
            XWARN("Failed to remove osu! move backup: {}",
                  Config::pathToUtf8(replacement.m_backupPath));
        }
    }
    return true;
}

/// @brief 将已经发生但未能持久化的移动恢复到原路径。
/// @param newPath 当前移动目标。
/// @param oldPath 需要恢复的原路径。
/// @return 完整恢复原路径并移除目标时返回 true。
bool rollbackFilesystemMove(const std::filesystem::path& newPath,
                            const std::filesystem::path& oldPath)
{
    std::error_code filesystemError;
    std::filesystem::rename(newPath, oldPath, filesystemError);
    if ( !filesystemError ) return true;

    filesystemError.clear();
    std::filesystem::create_directories(oldPath.parent_path(), filesystemError);
    if ( filesystemError ) return false;

    const bool isDirectory =
        std::filesystem::is_directory(newPath, filesystemError);
    if ( filesystemError ) return false;
    if ( isDirectory ) {
        std::filesystem::copy(newPath,
                              oldPath,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::copy_symlinks,
                              filesystemError);
    } else {
        std::filesystem::copy_file(newPath,
                                   oldPath,
                                   std::filesystem::copy_options::none,
                                   filesystemError);
    }
    if ( filesystemError ) return false;

    if ( isDirectory ) {
        std::filesystem::remove_all(newPath, filesystemError);
    } else {
        std::filesystem::remove(newPath, filesystemError);
    }
    return !filesystemError;
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

    /// @brief 一次性构建的谱面音频引用哈希索引。
    const AudioReferenceLookupIndex referenceIndex(project, audioReferences);
    for ( const auto& audioPath : scanResult.m_audioFiles ) {
        /// @brief 音频文件相对于项目根目录的 UTF-8 路径。
        const auto relativeAudioPath =
            makeProjectRelativeUtf8(project, audioPath);
        /// @brief 新建的项目音频资源条目。
        auto resource = createAudioResource(audioPath, relativeAudioPath);
        resource.m_type =
            inferIndexedAudioResourceType(referenceIndex, resource);

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
    /// @brief 按精确持久化路径建立的首次出现资源索引。
    std::unordered_map<std::string, const AudioResource*> persistedByPath;
    /// @brief 按稳定 ID 建立的首次出现资源索引。
    std::unordered_map<std::string, const AudioResource*> persistedById;
    persistedByPath.reserve(persistedProject.m_audioResources.size());
    persistedById.reserve(persistedProject.m_audioResources.size());
    for ( const auto& persistedResource : persistedProject.m_audioResources ) {
        if ( !persistedResource.m_path.empty() ) {
            persistedByPath.try_emplace(persistedResource.m_path,
                                        &persistedResource);
        }
        persistedById.try_emplace(persistedResource.m_id, &persistedResource);
    }

    for ( auto& resource : project.m_audioResources ) {
        const AudioResource* matchedPersistedResource = nullptr;
        if ( !resource.m_path.empty() ) {
            const auto pathIterator = persistedByPath.find(resource.m_path);
            if ( pathIterator != persistedByPath.end() ) {
                matchedPersistedResource = pathIterator->second;
            }
        }
        if ( !matchedPersistedResource ) {
            const auto idIterator = persistedById.find(resource.m_id);
            if ( idIterator != persistedById.end() ) {
                matchedPersistedResource = idIterator->second;
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
    /// @brief 合并校验共享的谱面音频引用哈希索引。
    const AudioReferenceLookupIndex referenceIndex(project, audioReferences);
    for ( auto& resource : project.m_audioResources ) {
        const bool boundToNote = (referenceIndex.matchingKinds(resource) &
                                  NOTE_SAMPLE_BINDING_MASK) != 0U;
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
    /// @brief 按精确项目路径建立的现有谱面索引。
    std::unordered_map<std::string, const Project::BeatmapEntry*>
        existingBeatmapsByPath;
    existingBeatmapsByPath.reserve(project.m_beatmaps.size());
    for ( const auto& entry : project.m_beatmaps ) {
        existingBeatmapsByPath.try_emplace(entry.m_filePath, &entry);
    }

    for ( const auto& mapPath : scanResult.m_beatmapFiles ) {
        /// @brief 谱面文件相对于项目根目录的 UTF-8 路径。
        auto relativeMapPath = makeProjectRelativeUtf8(project, mapPath);
        /// @brief 谱面文件名，用于显示和日志输出。
        auto filename = Config::pathToUtf8(mapPath.filename());

        if ( containsExcludedPath(project.m_excludedBeatmapPaths,
                                  relativeMapPath) ) {
            continue;
        }

        /// @brief 本次同步要写入的新谱面条目。
        Project::BeatmapEntry mapEntry;
        const auto existingEntry = existingBeatmapsByPath.find(relativeMapPath);
        if ( existingEntry != existingBeatmapsByPath.end() ) {
            mapEntry = *existingEntry->second;
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

    /// @brief 本轮同步共享的谱面音频引用哈希索引。
    const AudioReferenceLookupIndex referenceIndex(project, audioReferences);
    /// @brief 按规范化项目路径建立的现有音频资源索引。
    std::unordered_map<std::string, const AudioResource*>
        existingAudioResourcesByPath;
    existingAudioResourcesByPath.reserve(project.m_audioResources.size());
    for ( const auto& resource : project.m_audioResources ) {
        const auto pathKey = referenceIndex.projectPathKey(resource.m_path);
        if ( !pathKey.empty() ) {
            existingAudioResourcesByPath.try_emplace(pathKey, &resource);
        }
    }

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
        /// @brief 本次同步要写入的新音频资源。
        AudioResource resource;
        const auto    relativeAudioPathKey =
            referenceIndex.projectPathKey(relativeAudioPath);
        const auto existingResource =
            existingAudioResourcesByPath.find(relativeAudioPathKey);
        if ( existingResource != existingAudioResourcesByPath.end() ) {
            resource = *existingResource->second;
            const auto inferredType =
                inferIndexedAudioResourceType(referenceIndex, resource);
            const auto matchingKinds = referenceIndex.matchingKinds(resource);
            const bool hasTypeReference =
                (matchingKinds &
                 (SONG_FILE_HINT_MASK | NOTE_SAMPLE_BINDING_MASK)) != 0U;
            if ( hasTypeReference && resource.m_type != inferredType ) {
                resource.m_type  = inferredType;
                result.m_changed = true;
                if ( resource.m_type == AudioTrackType::Effect ) {
                    result.m_effectResourcesToRegister.push_back(resource);
                }
            }
        } else {
            resource = createAudioResource(audioPath, relativeAudioPath);
            resource.m_type =
                inferIndexedAudioResourceType(referenceIndex, resource);
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

/// @brief 从已经载入内存的谱面收集完整音频引用。
/// @param beatMap 待读取的内存谱面。
/// @param beatmapPath 用于诊断和相对路径解析的具体谱面路径。
/// @return 歌曲提示、玩家物件绑定和自动采样引用列表。
std::vector<BeatmapAudioReference>
ProjectResourceService::collectBeatmapAudioReferences(
    const BeatMap& beatMap, const std::string& beatmapPath)
{
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
    return collectBeatmapAudioReferences(beatMap, beatmapPath);
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
    const AudioReferenceLookupIndex referenceIndex(project);
    return referenceIndex.referenceMatchesResource(reference, resource);
}

/// @brief 将内存谱面中指向移动前资源的引用更新为稳定 ID 和新路径提示。
/// @param project 资源所属项目。
/// @param beatMap 需要原地更新的内存谱面。
/// @param beatmapPath 用于相对路径匹配的具体谱面路径。
/// @param previousResource 移动前的资源快照。
/// @param updatedResourcePath 移动后的项目相对路径。
/// @return 匹配和实际重写数量。
BeatmapAudioReferenceRemapResult
ProjectResourceService::remapBeatmapAudioReferencesAfterMove(
    const Project& project, BeatMap& beatMap, const std::string& beatmapPath,
    const AudioResource& previousResource,
    const std::string&   updatedResourcePath)
{
    BeatmapAudioReferenceRemapResult result;
    /// @brief 本次重映射共享的路径查找上下文。
    const AudioReferenceLookupIndex referenceIndex(project);
    /// @brief 移动前资源预计算后的查找键。
    const auto previousResourceKey =
        referenceIndex.makeResourceLookupKey(previousResource);

    /// @brief 判断字段是否仍指向移动前的资源。
    const auto matchesPreviousResource = [&](const std::string& audioReference,
                                             BeatmapAudioReferenceKind kind) {
        return referenceIndex.referenceMatchesResource(
            BeatmapAudioReference{
                beatmapPath,
                audioReference,
                kind,
            },
            previousResourceKey);
    };

    /// @brief 将歌曲路径提示改成移动后的项目相对路径。
    const auto remapMetadataPath = [&](std::filesystem::path& path) {
        if ( !matchesPreviousResource(
                 Config::pathToUtf8(path),
                 BeatmapAudioReferenceKind::SongFileHint) ) {
            return;
        }
        ++result.m_songFileHintReferenceCount;
        const auto updatedPath = Config::utf8ToPath(updatedResourcePath);
        if ( path == updatedPath ) return;
        path = updatedPath;
        ++result.m_changedReferenceCount;
    };
    remapMetadataPath(beatMap.m_baseMapMetadata.song_file_hint);
    remapMetadataPath(beatMap.m_baseMapMetadata.main_audio_path);

    /// @brief 将玩家物件的路径型绑定改成稳定资源 ID。
    const auto remapNoteBinding = [&](Note& note) {
        auto binding = note.getSampleBinding();
        if ( !binding || !matchesPreviousResource(
                             binding->m_audioResourceId,
                             BeatmapAudioReferenceKind::NoteSampleBinding) ) {
            return;
        }
        ++result.m_noteBindingReferenceCount;
        if ( binding->m_audioResourceId == previousResource.m_id ) return;
        binding->m_audioResourceId = previousResource.m_id;
        note.setSampleBinding(std::move(*binding));
        ++result.m_changedReferenceCount;
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
        if ( !matchesPreviousResource(
                 sample.m_audioResourceId,
                 BeatmapAudioReferenceKind::AudioSampleEvent) ) {
            continue;
        }
        ++result.m_audioSampleReferenceCount;
        if ( sample.m_audioResourceId == previousResource.m_id ) continue;
        sample.m_audioResourceId = previousResource.m_id;
        ++result.m_changedReferenceCount;
    }
    return result;
}

/// @brief 将内存谱面的玩家绑定和自动采样资源 ID 精确重命名。
/// @param beatMap 需要原地更新的谱面。
/// @param oldResourceId 旧资源 ID。
/// @param newResourceId 新资源 ID。
/// @return 实际改写的引用字段数量。
std::size_t ProjectResourceService::remapBeatmapAudioResourceId(
    BeatMap& beatMap, std::string_view oldResourceId,
    std::string_view newResourceId)
{
    if ( oldResourceId.empty() || newResourceId.empty() ||
         oldResourceId == newResourceId ) {
        return 0U;
    }

    std::size_t changedCount = 0U;
    /// @brief 精确改写一个玩家物件的采样绑定。
    const auto remapNoteBinding = [&](Note& note) {
        auto binding = note.getSampleBinding();
        if ( !binding || binding->m_audioResourceId != oldResourceId ) return;
        binding->m_audioResourceId = newResourceId;
        note.setSampleBinding(std::move(*binding));
        ++changedCount;
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
        if ( sample.m_audioResourceId != oldResourceId ) continue;
        sample.m_audioResourceId = newResourceId;
        ++changedCount;
    }
    return changedCount;
}

/// @brief 以全有或全无方式重写项目内 MMM/Malody 谱面的资源引用。
/// @param project 待扫描项目。
/// @param previousResource 重命名前的资源 ID 与路径快照。
/// @param updatedResourcePath 重命名后的项目相对路径。
/// @param newResourceId 新资源 ID。
/// @return 事务结果和实际变更谱面数量。
/// @warning 低频显式重命名路径：会读取并重新序列化相关谱面。
ProjectBeatmapAudioIdRemapResult
ProjectResourceService::remapProjectBeatmapAudioResourceId(
    const Project& project, const AudioResource& previousResource,
    std::string_view updatedResourcePath, std::string_view newResourceId)
{
    ProjectBeatmapAudioIdRemapResult result;
    if ( previousResource.m_id.empty() || newResourceId.empty() ) {
        result.m_errorMessage = "音频资源 ID 不能为空";
        return result;
    }
    if ( previousResource.m_id == newResourceId &&
         previousResource.m_path == updatedResourcePath ) {
        result.m_success = true;
        return result;
    }

    std::vector<PendingTextFileReplacement> pendingReplacements;
    for ( const auto& entry : project.m_beatmaps ) {
        const auto mapPath = resolveStoredProjectPath(
            project, Config::utf8ToPath(entry.m_filePath));
        auto extension = Config::pathToUtf8(mapPath.extension());
        std::transform(extension.begin(),
                       extension.end(),
                       extension.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });
        if ( extension != ".mmm" && extension != ".mc" ) continue;

        auto beatMap = BeatMap::loadFromFile(mapPath);
        if ( beatMap.m_baseMapMetadata.map_path.empty() ) {
            cleanupTextFileReplacements(pendingReplacements);
            result.m_errorMessage = "无法读取谱面 '" + entry.m_filePath +
                                    "'，已取消音频资源 ID 重命名";
            return result;
        }
        const auto pathRemap = remapBeatmapAudioReferencesAfterMove(
            project,
            beatMap,
            entry.m_filePath,
            previousResource,
            std::string(updatedResourcePath));
        const auto idRemap = remapBeatmapAudioResourceId(
            beatMap, previousResource.m_id, newResourceId);
        if ( !pathRemap.changed() && idRemap == 0U ) {
            continue;
        }

        auto serializedPath = mapPath;
        serializedPath += ".mmm-audio-id-remap";
        serializedPath += mapPath.extension();
        std::error_code filesystemError;
        if ( std::filesystem::exists(serializedPath, filesystemError) ||
             filesystemError ) {
            cleanupTextFileReplacements(pendingReplacements);
            result.m_errorMessage =
                "谱面 '" + entry.m_filePath + "' 的音频重命名临时文件已存在";
            return result;
        }
        if ( !beatMap.saveToFile(serializedPath) ) {
            filesystemError.clear();
            std::filesystem::remove(serializedPath, filesystemError);
            cleanupTextFileReplacements(pendingReplacements);
            result.m_errorMessage =
                "无法序列化谱面 '" + entry.m_filePath + "' 的音频资源 ID";
            return result;
        }

        std::string serializedContent;
        const bool  readSucceeded =
            readBinaryTextFile(serializedPath, serializedContent);
        filesystemError.clear();
        std::filesystem::remove(serializedPath, filesystemError);
        if ( !readSucceeded || filesystemError ) {
            cleanupTextFileReplacements(pendingReplacements);
            result.m_errorMessage = "无法读取或清理谱面 '" + entry.m_filePath +
                                    "' 的音频重命名临时文件";
            return result;
        }

        PendingTextFileReplacement replacement;
        if ( !stageTextFileReplacement(
                 mapPath, serializedContent, replacement) ) {
            cleanupTextFileReplacements(pendingReplacements);
            result.m_errorMessage =
                "无法暂存谱面 '" + entry.m_filePath + "' 的音频资源 ID";
            return result;
        }
        pendingReplacements.push_back(std::move(replacement));
        ++result.m_changedBeatmapCount;
    }

    if ( !commitTextFileReplacements(pendingReplacements) ) {
        result.m_changedBeatmapCount = 0U;
        result.m_errorMessage =
            "提交谱面音频资源 ID 事务失败，原谱面内容已恢复";
        return result;
    }
    result.m_success = true;
    return result;
}

/// @brief 保存前按 Malody 提示语义刷新 song_file_hint。
/// @param project 谱面所属项目。
/// @param beatMap 需要原地更新提示字段的谱面。
/// @param beatmapPath 当前谱面用于解析相对引用的路径。
/// @return 保留、回退或清空提示的具体结果。
/// @note 该函数只修改提示字段，不新增、删除或移动任何自动采样。
BeatmapSongFileHintUpdateResult
ProjectResourceService::refreshSongFileHintForSave(
    const Project& project, BeatMap& beatMap,
    const std::filesystem::path& beatmapPath)
{
    BeatmapSongFileHintUpdateResult result;
    auto&                           metadata = beatMap.m_baseMapMetadata;
    const auto                      referenceMapPath =
        beatmapPath.empty() ? metadata.map_path : beatmapPath;

    if ( !metadata.song_file_hint.empty() ) {
        const auto* existingResource = findAudioResourceForReference(
            project,
            referenceMapPath,
            Config::pathToUtf8(metadata.song_file_hint));
        if ( existingResource ) {
            result.m_source          = BeatmapSongFileHintSource::ExistingHint;
            result.m_audioResourceId = existingResource->m_id;
            if ( !metadata.main_audio_path.empty() ) {
                metadata.main_audio_path.clear();
                result.m_changed = true;
            }
            return result;
        }
    }

    const AudioResource* earliestMainResource = nullptr;
    double earliestEffectiveTimestamp = std::numeric_limits<double>::infinity();
    for ( const auto& sample : beatMap.m_audioSamples ) {
        const double effectiveTimestamp = sample.effectiveTimestamp();
        if ( !std::isfinite(effectiveTimestamp) ||
             effectiveTimestamp >= earliestEffectiveTimestamp ) {
            continue;
        }

        const auto* resource = findAudioResourceForReference(
            project, referenceMapPath, sample.m_audioResourceId);
        if ( !resource || resource->m_type != AudioTrackType::Main ) {
            continue;
        }
        earliestEffectiveTimestamp = effectiveTimestamp;
        earliestMainResource       = resource;
    }

    std::filesystem::path updatedHint;
    if ( earliestMainResource ) {
        updatedHint     = Config::utf8ToPath(earliestMainResource->m_path);
        result.m_source = BeatmapSongFileHintSource::EarliestMainSample;
        result.m_audioResourceId = earliestMainResource->m_id;
    }
    if ( metadata.song_file_hint != updatedHint ) {
        metadata.song_file_hint = std::move(updatedHint);
        result.m_changed        = true;
    }
    if ( !metadata.main_audio_path.empty() ) {
        metadata.main_audio_path.clear();
        result.m_changed = true;
    }
    return result;
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
    /// @brief 全部谱面引用匹配共享的路径查找上下文。
    const AudioReferenceLookupIndex referenceIndex(project);
    /// @brief 待查资源预计算后的查找键。
    const auto resourceKey = referenceIndex.makeResourceLookupKey(resource);
    for ( const auto& entry : project.m_beatmaps ) {
        const auto mapPath =
            resolveProjectPath(project, Config::utf8ToPath(entry.m_filePath));
        auto references = probeBeatmapAudioReferences(
            project, mapPath, entry.m_filePath, true);
        for ( auto& reference : references ) {
            if ( referenceIndex.referenceMatchesResource(reference,
                                                         resourceKey) ) {
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

    const AudioResourceResolutionIndex resolutionIndex(project);
    return resolutionIndex.resolve(
        audioReference, resolutionIndex.beatmapDirectoryKey(beatmapPath));
}

/// @brief 批量解析同一谱面的项目音频资源引用。
/// @param project 待查询项目。
/// @param beatmapPath 引用所在谱面的项目相对或绝对路径。
/// @param audioReferences 谱面保存的资源 ID 或旧路径视图。
/// @return 与输入顺序一一对应的资源地址；未解析项为空。
/// @warning 低频加载路径：一次构建资源解析索引并线性处理全部引用。
std::vector<const AudioResource*>
ProjectResourceService::resolveAudioResourceReferences(
    const Project& project, const std::filesystem::path& beatmapPath,
    const std::vector<std::string_view>& audioReferences)
{
    const AudioResourceResolutionIndex resolutionIndex(project);
    const auto                         beatmapDirectory =
        resolutionIndex.beatmapDirectoryKey(beatmapPath);

    std::vector<const AudioResource*> result;
    result.reserve(audioReferences.size());
    /// @brief 同一谱面内按引用内容缓存的首次解析结果。
    std::unordered_map<std::string, const AudioResource*> resolvedByReference;
    resolvedByReference.reserve(audioReferences.size());
    for ( const auto audioReference : audioReferences ) {
        const std::string referenceKey(audioReference);
        const auto cachedIterator = resolvedByReference.find(referenceKey);
        if ( cachedIterator != resolvedByReference.end() ) {
            result.push_back(cachedIterator->second);
            continue;
        }

        const auto* resource =
            resolutionIndex.resolve(referenceKey, beatmapDirectory);
        resolvedByReference.emplace(referenceKey, resource);
        result.push_back(resource);
    }
    return result;
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

/// @brief 在物理移动前验证外部谱面的音频引用仍可无损保持。
/// @param project 待检查项目。
/// @param oldPath 计划移动的文件或目录路径。
/// @param newPath 计划移动到的文件或目录路径。
/// @return 允许移动时为空；RM/IMD 隐式音频关联会改变或 osu!
/// 引用无法改写时返回面向用户的阻止原因。
std::string ProjectResourceService::validateAudioResourceMove(
    const Project& project, const std::filesystem::path& oldPath,
    const std::filesystem::path& newPath)
{
    if ( project.m_projectRoot.empty() || oldPath.empty() || newPath.empty() ) {
        return {};
    }

    const auto absoluteOldPath = weaklyCanonicalAbsolutePath(oldPath);
    const auto absoluteNewPath = weaklyCanonicalAbsolutePath(newPath);
    const auto resourceRemaps =
        collectResourcePathRemaps(project, absoluteOldPath, absoluteNewPath);
    const auto resourceProjections = collectResourcePathProjections(
        project, absoluteOldPath, absoluteNewPath);

    for ( const auto& entry : project.m_beatmaps ) {
        const auto mapPathBeforeMove = resolveStoredProjectPath(
            project, Config::utf8ToPath(entry.m_filePath));
        const auto mapPathAfterMove = remapAbsolutePathForMove(
            mapPathBeforeMove, absoluteOldPath, absoluteNewPath);
        auto extension = Config::pathToUtf8(mapPathBeforeMove.extension());
        std::transform(extension.begin(),
                       extension.end(),
                       extension.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });

        if ( extension == ".imd" ) {
            const auto audioBeforeMove =
                resolveCurrentImdAudio(mapPathBeforeMove);
            const auto expectedAudioAfterMove =
                audioBeforeMove
                    ? std::optional<
                          std::filesystem::path>{ remapAbsolutePathForMove(
                          *audioBeforeMove, absoluteOldPath, absoluteNewPath) }
                    : std::nullopt;
            const auto projectedAudioAfterMove =
                resolveProjectedImdAudioAfterMove(
                    mapPathAfterMove, absoluteOldPath, absoluteNewPath);
            if ( optionalPathsEqual(expectedAudioAfterMove,
                                    projectedAudioAfterMove) ) {
                continue;
            }

            const auto message =
                "无法移动：RM/IMD 谱面 '" + entry.m_filePath +
                "' 通过同目录同名前缀隐式选择音频，移动后会丢失或改为"
                "另一文件；请将谱面和音频作为保持相对关系的整目录移动";
            XWARN("{}", message);
            return message;
        }

        if ( extension != ".osu" || (resourceRemaps.empty() &&
                                     mapPathBeforeMove == mapPathAfterMove) ) {
            continue;
        }

        std::string source;
        if ( !readBinaryTextFile(mapPathBeforeMove, source) ) {
            const auto message = "无法移动：读取 osu! 谱面 '" +
                                 entry.m_filePath +
                                 "' 失败，不能安全检查并更新音频引用";
            XWARN("{}", message);
            return message;
        }

        std::string rewritten;
        bool        changed = false;
        if ( !rewriteOsuAudioReferenceText(source,
                                           project,
                                           mapPathBeforeMove,
                                           mapPathAfterMove,
                                           resourceProjections,
                                           rewritten,
                                           changed) ) {
            const auto message = "无法移动：osu! 谱面 '" + entry.m_filePath +
                                 "' 的音频引用在目标位置无法表示";
            XWARN("{}", message);
            return message;
        }
    }
    return {};
}

/// @brief 文件移动或重命名后重映射项目音频资源路径并保持资源 ID 稳定。
/// @param project 待更新项目。
/// @param oldPath 移动前的文件或目录路径。
/// @param newPath 移动后的文件或目录路径。
/// @param errorMessage 失败时接收面向用户的错误和回滚状态。
/// @return 路径发生变化的音频资源数量。
std::size_t ProjectResourceService::remapAudioResourcePathsAfterMove(
    Project& project, const std::filesystem::path& oldPath,
    const std::filesystem::path& newPath, std::string* errorMessage)
{
    if ( errorMessage ) errorMessage->clear();
    if ( project.m_projectRoot.empty() || oldPath.empty() || newPath.empty() ) {
        return 0;
    }

    const auto absoluteOldPath = weaklyCanonicalAbsolutePath(oldPath);
    const auto absoluteNewPath = weaklyCanonicalAbsolutePath(newPath);
    const auto remappedResources =
        collectResourcePathRemaps(project, absoluteOldPath, absoluteNewPath);
    const auto resourceProjections = collectResourcePathProjections(
        project, absoluteOldPath, absoluteNewPath);

    /// @brief 向 UI 报告失败并恢复已经发生的物理移动。
    const auto failAndRollbackMove = [&](std::string message) {
        const bool rolledBack =
            rollbackFilesystemMove(absoluteNewPath, absoluteOldPath);
        message += rolledBack ? "；文件移动已回滚"
                              : "；自动回滚失败，请立即检查源路径和目标路径";
        XERROR("{}", message);
        if ( errorMessage ) *errorMessage = std::move(message);
        return std::size_t{ 0U };
    };

    std::vector<PendingTextFileReplacement> pendingOsuReplacements;
    for ( const auto& entry : project.m_beatmaps ) {
        const auto mapPathBeforeMove = resolveStoredProjectPath(
            project, Config::utf8ToPath(entry.m_filePath));
        const auto mapPathAfterMove = remapAbsolutePathForMove(
            mapPathBeforeMove, absoluteOldPath, absoluteNewPath);
        auto extension = Config::pathToUtf8(mapPathBeforeMove.extension());
        std::transform(extension.begin(),
                       extension.end(),
                       extension.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });
        if ( extension != ".osu" || (remappedResources.empty() &&
                                     mapPathBeforeMove == mapPathAfterMove) ) {
            continue;
        }

        std::string source;
        if ( !readBinaryTextFile(mapPathAfterMove, source) ) {
            cleanupTextFileReplacements(pendingOsuReplacements);
            return failAndRollbackMove("移动后读取 osu! 谱面 '" +
                                       entry.m_filePath +
                                       "' 失败，未能更新音频引用");
        }

        std::string rewritten;
        bool        changed = false;
        if ( !rewriteOsuAudioReferenceText(source,
                                           project,
                                           mapPathBeforeMove,
                                           mapPathAfterMove,
                                           resourceProjections,
                                           rewritten,
                                           changed) ) {
            cleanupTextFileReplacements(pendingOsuReplacements);
            return failAndRollbackMove("移动后的音频路径无法由 osu! 谱面 '" +
                                       entry.m_filePath + "' 表达");
        }
        if ( !changed ) continue;

        PendingTextFileReplacement replacement;
        if ( !stageTextFileReplacement(
                 mapPathAfterMove, rewritten, replacement) ) {
            cleanupTextFileReplacements(pendingOsuReplacements);
            return failAndRollbackMove("写入 osu! 谱面 '" + entry.m_filePath +
                                       "' 的音频引用临时文件失败");
        }
        pendingOsuReplacements.push_back(std::move(replacement));
    }
    if ( !commitTextFileReplacements(pendingOsuReplacements) ) {
        return failAndRollbackMove(
            "提交 osu! 谱面音频引用事务失败，原谱面内容已恢复");
    }
    if ( remappedResources.empty() ) return 0U;

    for ( const auto& remap : remappedResources ) {
        const auto resource =
            std::find_if(project.m_audioResources.begin(),
                         project.m_audioResources.end(),
                         [&](const AudioResource& candidate) {
                             return candidate.m_id == remap.m_before.m_id &&
                                    candidate.m_path == remap.m_before.m_path;
                         });
        if ( resource != project.m_audioResources.end() ) {
            resource->m_path = remap.m_afterPath;
        }
    }

    for ( const auto& entry : project.m_beatmaps ) {
        const auto mapPathBeforeMove = resolveStoredProjectPath(
            project, Config::utf8ToPath(entry.m_filePath));
        const auto mapPathAfterMove = remapAbsolutePathForMove(
            mapPathBeforeMove, absoluteOldPath, absoluteNewPath);
        auto extension = Config::pathToUtf8(mapPathBeforeMove.extension());
        std::transform(extension.begin(),
                       extension.end(),
                       extension.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });
        if ( extension != ".mmm" ) continue;

        auto beatMap = BeatMap::loadFromFile(mapPathAfterMove);
        if ( beatMap.m_baseMapMetadata.map_path.empty() ) continue;
        bool changedBeatmap = false;

        for ( const auto& remap : remappedResources ) {
            const auto remapResult =
                remapBeatmapAudioReferencesAfterMove(project,
                                                     beatMap,
                                                     entry.m_filePath,
                                                     remap.m_before,
                                                     remap.m_afterPath);
            changedBeatmap |= remapResult.changed();
        }

        if ( changedBeatmap && !beatMap.saveToFile(mapPathAfterMove) ) {
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
/// @param audioPath 音频文件系统路径。
/// @param relativeAudioPath 已完成一次规范化的项目相对路径。
/// @return 填充默认配置后的音频资源条目。
AudioResource ProjectResourceService::createAudioResource(
    const std::filesystem::path& audioPath,
    const std::string&           relativeAudioPath)
{
    /// @brief 音频文件名，用作资源 ID。
    auto filename = Config::pathToUtf8(audioPath.filename());

    /// @brief 新建的项目音频资源条目。
    AudioResource resource;
    resource.m_id     = filename;
    resource.m_path   = relativeAudioPath;
    resource.m_type   = AudioTrackType::Effect;
    resource.m_config = makeDefaultAudioConfig();
    return resource;
}

}  // namespace MMM::Logic
