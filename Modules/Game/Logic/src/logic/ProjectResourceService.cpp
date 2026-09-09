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
#include <nlohmann/json.hpp>
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
/// @note 只检查词法相对关系，不查询符号链接；空路径和单独的点不作为资源路径。
/// 例如 a/../b 可折叠为 b，而 ../b 仍越界；不能只在原文本中搜索两个点。
bool isRelativePathInsideRoot(const std::filesystem::path& path)
{
    if ( path.empty() || path.is_absolute() ) return false;

    // 先折叠内部的目录抵消；归一化相对路径若仍有越界上级，必然出现在开头。
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
/// @note 回退结果仍可用于词法比较，但返回值不证明目标存在或可读取。
/// 最终回退可能仍为相对路径，调用方不能仅根据函数名假定已解析所有文件系统关系。
std::filesystem::path weaklyCanonicalAbsolutePath(
    const std::filesystem::path& path)
{
    std::error_code filesystemError;
    auto normalized = std::filesystem::weakly_canonical(path, filesystemError);
    // 弱规范化允许路径尾部尚不存在，适合移动目标和待创建文件。
    if ( !filesystemError ) return normalized.lexically_normal();

    // 不因部分路径尚不存在就丢弃引用，至少尝试固定其工作目录基准。
    filesystemError.clear();
    normalized = std::filesystem::absolute(path, filesystemError);
    if ( !filesystemError ) return normalized.lexically_normal();

    return path.lexically_normal();
}

/// @brief 若路径以项目文件夹名开头，则剥掉该多余前缀。
/// @param projectRoot 项目根目录。
/// @param path 待修正的相对路径。
/// @return 可剥离时返回剥离后的路径，否则返回空。
/// @note 兼容旧数据重复保存项目文件夹名的写法，不按任意字符串前缀截断。
std::filesystem::path stripProjectFolderPrefix(
    const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    if ( projectRoot.empty() || path.empty() || path.is_absolute() ) {
        return {};
    }

    // 按完整路径片段比较，防止项目名恰好是另一目录名的前缀时被误删。
    auto iterator = path.begin();
    if ( iterator == path.end() || *iterator != projectRoot.filename() ) {
        return {};
    }

    std::filesystem::path stripped;
    // 仅剥掉一次匹配的首段，其后同名目录仍属于实际资源层级。
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
/// @details
/// 先兼容多余项目名前缀，再尝试已存在的根内相对路径，最后解释文件系统路径。
/// @note 旧前缀分支不要求文件存在；返回根内词法路径不等于通过访问权限检查。
std::filesystem::path makeRelativeToProjectRoot(
    const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    if ( projectRoot.empty() || path.empty() ) return {};

    const auto root = weaklyCanonicalAbsolutePath(projectRoot);
    if ( path.is_relative() ) {
        const auto stripped = stripProjectFolderPrefix(root, path);
        if ( isRelativePathInsideRoot(stripped) ) {
            // 历史存储表示优先恢复，不让暂时缺失的资源失去原有相对路径。
            return stripped.lexically_normal();
        }

        // 相对输入可能是项目相对，也可能是工作目录相对；存在的项目内候选优先。
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

    // 前两种解释无结果时，按文件系统基准求相对路径；越界结果不作为项目资源键。
    const auto      absolutePath = weaklyCanonicalAbsolutePath(path);
    std::error_code filesystemError;
    auto            relativePath =
        std::filesystem::relative(absolutePath, root, filesystemError);
    if ( !filesystemError && isRelativePathInsideRoot(relativePath) ) {
        // relative 计算成功仍可能得到 ..，因此需要额外限制其词法范围。
        return relativePath.lexically_normal();
    }
    return {};
}

/// @brief 将项目中已存储的 UTF-8 路径归一化为项目根相对路径键。
/// @param project 当前项目。
/// @param path 已存储的 UTF-8 路径。
/// @return 可用于稳定比较的项目根相对路径。
/// @note 无法定位时保留词法输入，结果可能仍为绝对路径，不保证它对应项目内文件。
std::string normalizeStoredProjectPath(const Project&     project,
                                       const std::string& path)
{
    auto relativePath = makeRelativeToProjectRoot(project.m_projectRoot,
                                                  Config::utf8ToPath(path));
    if ( relativePath.empty() ) {
        // 保留无法定位的旧文本用于后续兼容匹配，不把尚未找到的资源变成空引用。
        relativePath = Config::utf8ToPath(path).lexically_normal();
    }
    return Config::pathToUtf8(relativePath.lexically_normal());
}

/// @brief 谱面引用用途的紧凑位掩码。
/// @note 同一资源可被多种字段引用，合并使用按位或而非覆盖单个用途。
/// 此位图只在进程内推断用途，不是写入谱面或网络协议的枚举编码。
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
/// @note 未识别枚举值不贡献用途位，不能默认解释成玩家绑定或歌曲提示。
AudioReferenceKindMask audioReferenceKindMask(BeatmapAudioReferenceKind kind)
{
    // 显式映射使索引位布局不依赖枚举整数顺序，新增用途时应在此补充分支。
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
/// @note 项目整体搬迁后需重建上下文，不能只更新被查询资源的相对路径。
ProjectPathLookupContext makeProjectPathLookupContext(const Project& project)
{
    // 根目录规范化集中在索引建立时，相对引用匹配无需逐条重复查询文件系统。
    return ProjectPathLookupContext{ weaklyCanonicalAbsolutePath(
        project.m_projectRoot) };
}

/// @brief 将路径文本中的 Windows 分隔符转换为当前平台可解析形式。
/// @param pathText 待转换的 UTF-8 路径文本。
/// @return 分隔符统一后的文本。
/// @note 不转换编码、大小写或引号；返回文本仍需按调用场景解释为 ID 或路径。
std::string normalizePathSeparators(std::string pathText)
{
    // Linux 不把反斜杠视为分隔符，先转换才能解析 Windows 谱面内保存的路径。
    std::replace(pathText.begin(), pathText.end(), '\\', '/');
    return pathText;
}

/// @brief 生成项目路径索引键。
/// @param context 已缓存的项目根目录。
/// @param pathText 项目相对路径、绝对路径或旧版带项目目录前缀路径。
/// @return 可用于哈希比较的 UTF-8 路径键。
/// @note 相对路径只做词法规范化；仅绝对旧路径需要弱规范化。
/// 键用于兼容查找，不作为文件删除的根内范围校验或存在性证明。
std::string makeProjectPathLookupKey(const ProjectPathLookupContext& context,
                                     const std::string&              pathText)
{
    if ( pathText.empty() ) return {};

    auto path = Config::utf8ToPath(normalizePathSeparators(pathText))
                    .lexically_normal();
    // 分隔符先统一再折叠点路径，保证 Windows 写法在当前平台参与同一种比较。
    if ( path.is_absolute() ) {
        // 根内绝对路径收敛到项目相对键；根外路径保留绝对形式，不能与根内同名文件混同。
        const auto absolutePath = weaklyCanonicalAbsolutePath(path);
        const auto relativePath =
            absolutePath.lexically_relative(context.m_projectRoot);
        if ( isRelativePathInsideRoot(relativePath) ) {
            return Config::pathToUtf8(relativePath.lexically_normal());
        }
        return Config::pathToUtf8(absolutePath.lexically_normal());
    }

    // 相对路径不做存在性验证，资源移动或暂时缺失时仍需得到稳定的比较键。
    const auto stripped = stripProjectFolderPrefix(context.m_projectRoot, path);
    // 例如旧存储 Project/audio/song.ogg 可归为 audio/song.ogg，文件名本身不变。
    if ( isRelativePathInsideRoot(stripped) ) {
        path = stripped.lexically_normal();
    }
    return Config::pathToUtf8(path);
}

/// @brief 判断谱面相对候选是否越出项目根并需要旧 filename 回退。
/// @param mapRelativePath 尚未转换为项目键的谱面相对候选。
/// @param mapRelativePathKey 已转换后的项目路径键。
/// @return 旧 makeProjectRelativePath 会退回 filename 时返回 true。
/// @pre 两个参数描述同一个候选，第二个必须通过当前项目上下文生成。
bool requiresMapRelativeFilenameFallback(
    const std::filesystem::path& mapRelativePath,
    const std::string&           mapRelativePathKey)
{
    // 只在旧逻辑无法得到根内路径时启用 basename 回退，不放宽正常子目录匹配。
    return (mapRelativePath.is_relative() &&
            !isRelativePathInsideRoot(mapRelativePath)) ||
           Config::utf8ToPath(mapRelativePathKey).is_absolute();
}

/// @brief 预先汇总全部谱面引用的不可变哈希索引。
///
/// 构建成本与引用数量线性相关。后续任意资源查询仅计算资源自身路径，
/// 不再执行 resource×reference 的路径存在性检查或弱规范化。
/// @note 保存引用文本与用途，不持有谱面对象；内容变化后需重新建立索引。
/// @warning
/// 索引建立会分配容器，旧绝对路径也可能查询文件系统，仅用于低频资源处理。
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
        // 空引用表仍可调用逐项匹配，但 matchingKinds 没有预装用途可供返回。
    }

    /// @brief 从完整谱面引用列表构建索引。
    /// @param project 引用所属项目。
    /// @param references 已汇总的谱面引用。
    AudioReferenceLookupIndex(
        const Project&                            project,
        const std::vector<BeatmapAudioReference>& references)
        : m_pathContext(makeProjectPathLookupContext(project))
    {
        // 一个引用会加入原始 ID、旧文件名等多个键，预留空间减少构建期重哈希。
        m_resourceIdKinds.reserve(references.size() * 2U);
        m_projectPathKinds.reserve(references.size() * 2U);
        for ( const auto& reference : references ) {
            addReference(reference);
        }
    }

    /// @brief 查询资源匹配到的全部引用用途。
    /// @param resource 待查询项目资源。
    /// @return 匹配用途的位掩码；资源 ID 为空时返回零。
    /// @note 未命中为零，不区分没有引用与输入引用探测失败；索引不保存探测状态。
    [[nodiscard]] AudioReferenceKindMask matchingKinds(
        const AudioResource& resource) const
    {
        if ( resource.m_id.empty() ) return 0U;

        // ID 与路径可能各匹配不同用途，必须取并集，不能在首次命中后提前结束。
        AudioReferenceKindMask result = 0U;
        const auto idIterator         = m_resourceIdKinds.find(resource.m_id);
        if ( idIterator != m_resourceIdKinds.end() ) {
            result |= idIterator->second;
        }

        const auto resourcePathKey = projectPathKey(resource.m_path);
        // 即使 ID
        // 已命中歌曲提示，也还可能从路径键找到玩家绑定，进而改变类型推断。
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
    /// @note 同一资源批量比对时复用此值，移动资源或切换项目后必须重新计算。
    [[nodiscard]] ResourceLookupKey makeResourceLookupKey(
        const AudioResource& resource) const
    {
        // 返回独立字符串值，后续修改资源对象不会反向改变这份移动前的匹配快照。
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
    /// @note 仅判断单个候选是否可匹配，不在多个资源间选择唯一最佳对象。
    [[nodiscard]] bool referenceMatchesResource(
        const BeatmapAudioReference& reference,
        const ResourceLookupKey&     resourceKey) const
    {
        if ( reference.m_audioReference.empty() ||
             resourceKey.m_resourceId.empty() ) {
            return false;
        }
        // 稳定 ID 无需路径解析，先处理当前格式的直接引用。
        if ( reference.m_audioReference == resourceKey.m_resourceId ) {
            return true;
        }

        const auto normalizedReferenceText =
            normalizePathSeparators(reference.m_audioReference);
        // 原始 ID 的精确比较在前，不能先替换斜杠而改变 ID 文本本身的语义。
        const auto referencePath = Config::utf8ToPath(normalizedReferenceText);
        // 旧格式可能把带目录的引用和文件名式资源 ID 混用，保留此兼容匹配。
        if ( Config::pathToUtf8(referencePath.filename()) ==
             resourceKey.m_resourceId ) {
            return true;
        }

        // 后续都是路径匹配；资源没有路径时不能凭谱面目录猜测它的落盘位置。
        if ( resourceKey.m_projectPath.empty() ) return false;
        if ( projectPathKey(normalizedReferenceText) ==
             resourceKey.m_projectPath ) {
            return true;
        }

        if ( reference.m_beatmapPath.empty() || referencePath.is_absolute() ) {
            // 缺少谱面位置时不猜测当前工作目录，绝对引用也无需第二种相对基准。
            return false;
        }

        // 根相对解释未命中后，再以谱面所在目录解释；绝对引用不参与此分支。
        const auto beatmapPathKey = projectPathKey(reference.m_beatmapPath);
        if ( beatmapPathKey.empty() ) return false;
        const auto mapRelativePath =
            (Config::utf8ToPath(beatmapPathKey).parent_path() / referencePath)
                .lexically_normal();
        const auto mapRelativePathKey =
            projectPathKey(Config::pathToUtf8(mapRelativePath));
        if ( mapRelativePathKey == resourceKey.m_projectPath ) return true;
        // 常规根内路径未命中时就保留未匹配结果，不总是去掉目录扩大匹配范围。
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
    /// @note 此便利重载每次计算资源键；一对多检查应使用预计算键重载。
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
    /// @note 键不能跨不同项目根的索引直接比较，同一相对文本可能指向不同文件。
    [[nodiscard]] std::string projectPathKey(const std::string& pathText) const
    {
        return makeProjectPathLookupKey(m_pathContext, pathText);
    }

private:
    /// @brief 将单个谱面引用加入 ID、项目路径和谱面相对路径索引。
    /// @param reference 待加入的谱面引用。
    void addReference(const BeatmapAudioReference& reference)
    {
        // 本构建流程须与 referenceMatchesResource 的兼容分支对应，
        // 否则批量用途推断与逐项引用诊断会对同一资源给出不同结论。
        if ( reference.m_audioReference.empty() ) return;

        const auto kindMask = audioReferenceKindMask(reference.m_kind);
        // 所有兼容键复用同一个用途位，路径解释变化不能把绑定变成歌曲提示。
        // 重复引用合并用途而非增加计数，本索引回答是否引用，不负责统计物件数量。
        m_resourceIdKinds[reference.m_audioReference] |= kindMask;
        // 用途位只表达存在性；重复十次绑定与一次绑定产生相同的位集合。

        const auto normalizedReferenceText =
            normalizePathSeparators(reference.m_audioReference);
        const auto referencePath = Config::utf8ToPath(normalizedReferenceText);
        const auto filename      = Config::pathToUtf8(referencePath.filename());
        if ( !filename.empty() ) {
            // 文件名兼容键放在 ID 表中，不能与完整项目路径表混成一种匹配规则。
            m_resourceIdKinds[filename] |= kindMask;
        }

        const auto directPathKey = projectPathKey(normalizedReferenceText);
        if ( !directPathKey.empty() ) {
            // 一个字段可以同时保留 ID 与路径解释，查询时再合并命中的用途。
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
            // 谱面目录参与生成键，来自不同子目录的同一相对文本可得到不同位置。
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

    /// @brief 缓存项目根目录，避免相对资源查询反复规范化根目录。
    /// @note 旧绝对路径仍会经过弱规范化，不能将整个索引视作纯内存路径处理。
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
/// @pre 被索引的项目资源列表在使用期间不得增删、重排或销毁。
/// 索引期间也不能改写 ID 或路径，否则保存的键与观察对象的字段不再一致。
/// @note Candidate 持有非拥有指针，索引仅用于一次资源处理批次。
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
    /// @note 两张表分别保留各自最早候选，最终还需跨表比较资源序号。
    explicit AudioResourceResolutionIndex(const Project& project)
        : m_pathContext(makeProjectPathLookupContext(project))
    {
        m_resourcesById.reserve(project.m_audioResources.size());
        m_resourcesByPath.reserve(project.m_audioResources.size());
        for ( std::size_t index = 0U; index < project.m_audioResources.size();
              ++index ) {
            const auto& resource = project.m_audioResources[index];
            if ( resource.m_id.empty() ) continue;
            // 空 ID
            // 资源整体不参与解析，包括它可能存在的路径，保持逐项匹配约束。

            // 重复键不覆盖已有候选，保证每张表中都留下项目列表里的最早资源。
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
        // 未提供谱面与“谱面就在项目根”不同，后者仍允许按空父目录解释引用。
        const auto beatmapKey = makeProjectPathLookupKey(
            m_pathContext, Config::pathToUtf8(beatmapPath));
        // 根目录谱面的 parent_path 可以为空；available
        // 独立表示是否提供了谱面上下文。
        return BeatmapDirectory{
            true,
            Config::utf8ToPath(beatmapKey).parent_path(),
        };
    }

    /// @brief 解析单个 ID 或旧路径引用。
    /// @param audioReference 谱面保存的引用文本。
    /// @param beatmapDirectory 已预计算的谱面目录键。
    /// @return 所有兼容匹配方式中项目序号最小的资源。
    /// @note 解析不要求调用方已收集引用用途，适用于单条提示或采样选择。
    [[nodiscard]] const AudioResource* resolve(
        const std::string&      audioReference,
        const BeatmapDirectory& beatmapDirectory) const
    {
        if ( audioReference.empty() ) return nullptr;

        // 即使 ID 命中也继续考察路径：这里复现旧版按资源顺序查找，
        // 不是按匹配形式设置 ID 高于路径的优先级。
        const Candidate* bestCandidate = nullptr;
        /// @brief 合并一个候选并保留项目列表中最早的资源。
        const auto considerCandidate = [&](const Candidate* candidate) {
            // Candidate 属于本索引的只读表；解析期间不插入元素，因此地址稳定。
            if ( !candidate ) return;
            if ( !bestCandidate ||
                 candidate->m_resourceIndex < bestCandidate->m_resourceIndex ) {
                bestCandidate = candidate;
            }
        };
        /// @brief 按指定键查询候选哈希表。
        const auto considerByKey = [&](const auto&        candidates,
                                       const std::string& key) {
            // 查询不使用 operator[]，失败不能改变索引或构造一个伪候选。
            if ( key.empty() ) return;
            const auto iterator = candidates.find(key);
            if ( iterator != candidates.end() ) {
                considerCandidate(&iterator->second);
            }
        };

        considerByKey(m_resourcesById, audioReference);
        // 此时只得到暂定候选；其他匹配形式仍可能命中排列更靠前的资源。

        const auto normalizedReferenceText =
            normalizePathSeparators(audioReference);
        const auto referencePath = Config::utf8ToPath(normalizedReferenceText);
        considerByKey(m_resourcesById,
                      Config::pathToUtf8(referencePath.filename()));
        considerByKey(
            m_resourcesByPath,
            makeProjectPathLookupKey(m_pathContext, normalizedReferenceText));

        // 只有相对引用才借助谱面目录；否则再次拼接会破坏绝对路径语义。
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
        // 输出借用项目资源，而不是 Candidate 本身；索引销毁不销毁资源对象。
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
/// @note 这是用途推断，不修改音轨配置；持久化恢复是否覆盖类型由调用流程决定。
/// 同一资源兼作歌曲和玩家音效时优先 Effect，不通过拆成两个资源消除冲突。
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
    // 物件绑定优先，确保演奏时能够按音效触发；自动采样本身不推定为主音轨。
    if ( hasNoteBinding ) return AudioTrackType::Effect;
    if ( hasSongFileHint ) return AudioTrackType::Main;
    return AudioTrackType::Effect;
}

/// @brief 将移动源及其后代路径映射到目标位置。
/// @param candidate 待检查的规范化绝对路径。
/// @param oldPath 移动前的规范化绝对路径。
/// @param newPath 移动后的规范化绝对路径。
/// @return 候选路径移动后的绝对路径；不受移动影响时返回原路径。
/// @note 不执行 rename；返回路径只表达计划位置，不证明移动能成功。
/// @pre 三个输入使用相同文件系统基准，否则相等与相对层级判断没有可比性。
std::filesystem::path remapAbsolutePathForMove(
    const std::filesystem::path& candidate,
    const std::filesystem::path& oldPath, const std::filesystem::path& newPath)
{
    // 源本身没有有效的子路径后缀，先处理精确命中，再处理目录内部的后代。
    if ( candidate == oldPath ) return newPath;

    std::error_code relativeError;
    const auto      suffix =
        std::filesystem::relative(candidate, oldPath, relativeError);
    if ( relativeError || !isRelativePathInsideRoot(suffix) ) {
        // 无法得到安全的子路径时不改写候选，避免把相邻目录误拼进目标树。
        return candidate;
    }
    // 只替换移动根，保留内部层级，避免目录移动后嵌套谱面及资源引用变平。
    return (newPath / suffix).lexically_normal();
}

/// @brief 按移动前的项目路径解析谱面或资源绝对路径。
/// @param project 路径所属项目。
/// @param storedPath 项目保存的相对或绝对路径。
/// @return 弱规范化后的绝对路径。
/// @note 直接按存储基准拼接，不用“哪个文件当前存在”猜测移动前位置。
std::filesystem::path resolveStoredProjectPath(
    const Project& project, const std::filesystem::path& storedPath)
{
    if ( storedPath.is_absolute() ) {
        return weaklyCanonicalAbsolutePath(storedPath);
    }
    return weaklyCanonicalAbsolutePath(project.m_projectRoot / storedPath);
}

/// @brief RM/IMD 能够隐式发现的音频扩展名，顺序与加载器一致。
/// 同前缀有多种音频时，数组顺序决定选择；不能为展示排序而重排本表。
static constexpr std::array<std::string_view, 7> RM_AUDIO_EXTENSIONS{
    ".mp3", ".wav", ".ogg", ".flac", ".opus", ".aac", ".m4a"
};

/// @brief 提取 RM/IMD 谱面文件名用于匹配音频的前缀。
/// @param mapPath RM/IMD 谱面路径。
/// @return 第一个下划线前的文件名前缀；无下划线时为空。
/// @note 下划线位于首字符时同样没有可用前缀，后续下划线不参与截取。
std::string imdAudioPrefix(const std::filesystem::path& mapPath)
{
    // IMD 以文件名首个下划线前的部分寻找歌曲，无此前缀时不能猜测音频名。
    const auto filename  = Config::pathToUtf8(mapPath.filename());
    const auto separator = filename.find('_');
    return separator == std::string::npos ? std::string{}
                                          : filename.substr(0, separator);
}

/// @brief 判断路径是否等于指定根或位于其目录内。
/// @param path 待检查绝对路径。
/// @param root 候选根路径。
/// @return 相同或位于根内时返回 true。
/// @note 查询相对关系失败按不在范围内处理，函数不会扩大到共同父目录。
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
/// @pre 物理移动尚未发生，目标冲突检查由更上层负责；这里模拟单次移动布局。
bool projectedPathExistsAfterMove(
    const std::filesystem::path& candidateAfterMove,
    const std::filesystem::path& absoluteOldPath,
    const std::filesystem::path& absoluteNewPath)
{
    // 文件尚未移动，目标树中的候选要反向映射到源树检查，不能直接查目标是否存在。
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
        // 目标树命中时只看对应源文件；目标当前位置的同名文件不属于本次投影来源。
        return std::filesystem::exists(sourceCandidate, filesystemError) &&
               !filesystemError;
    }
    // 源树在移动后将消失；不在目标树的旧位置不能因目前仍存在就判为可用。
    if ( pathIsInsideOrSame(candidateAfterMove, absoluteOldPath) ) {
        return false;
    }
    return std::filesystem::exists(candidateAfterMove, filesystemError) &&
           !filesystemError;
}

/// @brief 按 RM/IMD 规则查找当前文件系统实际选中的音频路径。
/// @param mapPath 当前谱面绝对路径。
/// @return 被隐式选中的音频绝对路径；没有音频时为空。
/// @note 按存在性模拟加载器选名，不尝试解码，也不依赖项目音频资源清单。
std::optional<std::filesystem::path> resolveCurrentImdAudio(
    const std::filesystem::path& mapPath)
{
    const auto prefix = imdAudioPrefix(mapPath);
    if ( prefix.empty() ) return std::nullopt;

    // 隐式歌曲选择依赖扩展名优先级，必须与加载器一致，不能按目录枚举顺序选择。
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
/// @note 谱面重命名可能改变歌曲前缀，因此必须使用移动后的谱面名生成候选。
std::optional<std::filesystem::path> resolveProjectedImdAudioAfterMove(
    const std::filesystem::path& mapPathAfterMove,
    const std::filesystem::path& absoluteOldPath,
    const std::filesystem::path& absoluteNewPath)
{
    const auto prefix = imdAudioPrefix(mapPathAfterMove);
    if ( prefix.empty() ) return std::nullopt;

    // 保持同一候选顺序，仅把存在性检查替换为移动投影，才能比较移动是否改变歌曲选择。
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
/// @note 比较规范化位置，不通过文件内容或资源 ID 判断是否为同一首歌。
/// 不调用 equivalent 比较文件身份，因为推演中的目标可能尚不存在。
bool optionalPathsEqual(const std::optional<std::filesystem::path>& lhs,
                        const std::optional<std::filesystem::path>& rhs)
{
    // 两边均无歌曲也视为关联不变；只有一边缺失才构成关联变化。
    if ( lhs.has_value() != rhs.has_value() ) return false;
    return !lhs || weaklyCanonicalAbsolutePath(*lhs) ==
                       weaklyCanonicalAbsolutePath(*rhs);
}

/// @brief 单个项目音频资源的路径移动投影。
/// @note m_afterPath 与 m_before.m_path 相等的项也可用于表达未移动资源。
struct ResourcePathRemap {
    /// @brief 移动前资源快照。
    AudioResource m_before;
    // 值快照保留稳定 ID 与旧路径，项目列表更新后仍可用它匹配旧谱面引用。

    /// @brief 移动后的项目相对路径。
    std::string m_afterPath;
};

/// @brief
/// 目录移动时按存储文本重写子路径，避免文件名中的冒号被路径库解释为卷名。
/// @param project 资源所属项目。
/// @param resource 待投影资源。
/// @param absoluteResourcePath 移动前资源绝对路径。
/// @param absoluteOldPath 移动源绝对路径。
/// @param absoluteNewPath 移动目标绝对路径。
/// @return 能按项目相对目录前缀重写时返回保留原始后缀的路径，否则为空。
/// @note 空结果意味着改用普通路径投影，不直接表示整次移动应失败。
std::optional<std::string> remapStoredChildPathText(
    const Project& project, const AudioResource& resource,
    const std::filesystem::path& absoluteResourcePath,
    const std::filesystem::path& absoluteOldPath,
    const std::filesystem::path& absoluteNewPath)
{
    // 精确移动单个文件没有可保留的子路径后缀，交给普通路径投影处理。
    if ( absoluteResourcePath == absoluteOldPath ) return std::nullopt;

    const auto oldRelative =
        makeRelativeToProjectRoot(project.m_projectRoot, absoluteOldPath);
    const auto newRelative =
        makeRelativeToProjectRoot(project.m_projectRoot, absoluteNewPath);
    if ( oldRelative.empty() || newRelative.empty() ) return std::nullopt;
    // 文本替换依赖两个根内相对前缀；任一无法表示就不能安全拼接保存字符串。

    const auto storedPath = normalizePathSeparators(resource.m_path);
    const auto oldPrefix =
        Config::pathToUtf8Generic(oldRelative.lexically_normal());
    // 前缀末尾必须是目录边界，例如移动 audio 不能连带改写 audio_backup。
    if ( storedPath.size() <= oldPrefix.size() ||
         storedPath.compare(0U, oldPrefix.size(), oldPrefix) != 0 ||
         storedPath[oldPrefix.size()] != '/' ) {
        return std::nullopt;
    }

    const auto newPrefix =
        Config::pathToUtf8Generic(newRelative.lexically_normal());
    // 原样保留后缀文本，避免冒号等名字片段被宿主路径库重新解释。
    return newPrefix + storedPath.substr(oldPrefix.size());
}

/// @brief 计算全部项目音频资源在文件移动后的路径投影。
/// @param project 待检查项目。
/// @param absoluteOldPath 移动源绝对路径。
/// @param absoluteNewPath 移动目标绝对路径。
/// @return 包含未移动资源的路径投影；无法表示为项目相对路径的移动项跳过。
/// @note 输出保留项目资源顺序，不会实际更新路径或重新分配资源 ID。
/// @warning 投影包含规范化与相对路径查询，限用于移动命令的准备或收尾阶段。
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
        // 未移动资源仍加入投影，调用方可用完整可解析集合比较新旧关联。
        if ( remappedAbsolutePath == absoluteResourcePath ) {
            result.push_back(ResourcePathRemap{ resource, resource.m_path });
            continue;
        }

        const auto relativePath = makeRelativeToProjectRoot(
            project.m_projectRoot, remappedAbsolutePath);
        if ( relativePath.empty() ) continue;
        // 无根内相对表示的移动项不生成空路径条目；输出数量不一定等于项目资源数。
        // 优先保留子路径存储文本，不适用时才采用文件系统计算出的相对路径。
        const auto preservedPath =
            remapStoredChildPathText(project,
                                     resource,
                                     absoluteResourcePath,
                                     absoluteOldPath,
                                     absoluteNewPath);
        const auto remappedPath = preservedPath.value_or(
            Config::pathToUtf8Generic(relativePath.lexically_normal()));
        result.push_back(ResourcePathRemap{ resource, remappedPath });
    }
    return result;
}

/// @brief 收集一次文件移动会实际改变路径的项目音频资源。
/// @param project 待检查项目。
/// @param absoluteOldPath 移动源绝对路径。
/// @param absoluteNewPath 移动目标绝对路径。
/// @return 路径发生变化且保持稳定 ID 的资源投影。
/// @note 变化按存储路径文本判断，过滤后仍保持原有资源相对顺序。
std::vector<ResourcePathRemap> collectResourcePathRemaps(
    const Project& project, const std::filesystem::path& absoluteOldPath,
    const std::filesystem::path& absoluteNewPath)
{
    auto projections = collectResourcePathProjections(
        project, absoluteOldPath, absoluteNewPath);
    // 重写任务只关心变化项；保持原始快照中的稳定 ID，不在移动时重建资源身份。
    std::erase_if(projections, [](const ResourcePathRemap& projection) {
        return projection.m_before.m_path == projection.m_afterPath;
    });
    return projections;
}

/// @brief 将 osu! 相对引用中的反斜杠统一为可解析路径。
/// @param reference osu! 字段原始值。
/// @return 使用当前平台目录分隔符的路径。
/// @note 此入口处理字段值，不移除周围引号或空白；字段边界由文本重写器处理。
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
/// @note 相对谱面目录允许 ..，并不要求音频位于该谱面子目录内。
std::optional<std::string> makeOsuReferenceAfterMove(
    const Project& project, const std::filesystem::path& mapPathAfterMove,
    const ResourcePathRemap& remap)
{
    // 将文件名文本与父目录路径分开，避免文件名里的特殊字符干扰相对目录计算。
    const auto storedPath = normalizePathSeparators(remap.m_afterPath);
    const auto separator  = storedPath.rfind('/');
    const auto filename   = separator == std::string::npos
                                ? storedPath
                                : storedPath.substr(separator + 1U);
    if ( filename.empty() ) return std::nullopt;
    // 文件名必须非空，不能把一个目录路径写成音频字段。

    const auto storedParent =
        separator == std::string::npos
            ? std::filesystem::path{}
            : Config::utf8ToPath(storedPath.substr(0U, separator));
    const auto audioParentAfterMove =
        resolveStoredProjectPath(project, storedParent);
    // osu! 引用相对谱面目录而非项目根；谱面和音频一起移动时也要用双方的新位置。
    const auto relativeParent =
        audioParentAfterMove.lexically_relative(mapPathAfterMove.parent_path());
    if ( relativeParent.empty() || relativeParent.is_absolute() ) {
        // 无法得到目录间相对表示就明确失败，不退回可能指向其他文件的 basename。
        return std::nullopt;
    }

    auto reference = Config::pathToUtf8Generic(relativeParent);
    // 同目录直接存文件名，避免生成无必要的 ./ 前缀改变原有引用写法。
    if ( reference == "." ) return filename;
    return reference + '/' + filename;
}

/// @brief 查找一个 osu! 音频引用在移动后的替换文本。
/// @param project 资源所属项目。
/// @param reference osu! 字段中的原始引用。
/// @param mapPathBeforeMove 移动前谱面绝对路径。
/// @param mapPathAfterMove 移动后谱面绝对路径。
/// @param remaps 受影响资源路径投影。
/// @return 未引用受影响资源时为空；无法表达时返回空字符串。
/// @note 首个 ID 或旧绝对位置命中即决定替换，调用方需保持投影顺序稳定。
/// 未登记且无法匹配投影的引用保持原文，不在此自动导入项目外资源。
std::optional<std::string> remapOsuAudioReference(
    const Project& project, const std::string& reference,
    const std::filesystem::path&          mapPathBeforeMove,
    const std::filesystem::path&          mapPathAfterMove,
    const std::vector<ResourcePathRemap>& remaps)
{
    // nullopt 表示不涉及本次移动；有值但为空表示命中资源却无法表达新引用。
    // 调用方必须区分两者，后者应使整次重写失败，而不是保留一个将失效的旧路径。
    if ( reference.empty() ) return std::nullopt;

    const auto referencePath = osuReferencePath(reference);
    // 相对引用使用旧谱面目录，不能因物理文件已移动而改用新目录解释原文本。
    const auto absoluteReference =
        referencePath.is_absolute()
            ? weaklyCanonicalAbsolutePath(referencePath)
            : weaklyCanonicalAbsolutePath(mapPathBeforeMove.parent_path() /
                                          referencePath);
    for ( const auto& remap : remaps ) {
        // 对比移动前位置，不能拿已更新的资源路径去解析原始 osu! 文本。
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
/// @pre begin <= end 且 end 不超过 line.size()；区间采用左闭右开表示。
void trimAsciiRange(const std::string& line, std::size_t& begin,
                    std::size_t& end)
{
    // 只移动区间边界，不修改原行；替换路径时可原样保留字段周围的空白。
    while ( begin < end &&
            std::isspace(static_cast<unsigned char>(line[begin])) ) {
        ++begin;
    }
    // 转成 unsigned char 再调用字符分类，避免 UTF-8 高位字节作为负值传入。
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
/// @note 失败时 output 可能只包含已处理前缀，调用方不能提交这份不完整文本。
/// @pre source 与 output
/// 不得别名，入口会先清空输出；资源投影在处理期间保持稳定。
/// @details 仅识别 General/AudioFilename 和 HitObjects 尾部显式采样文件名。
/// 这不是完整 osu! 格式校验器，成功不表示所有未知字段都已验证。
/// @warning 全文副本和局部字符串替换会分配内存，只用于低频移动准备与提交。
bool rewriteOsuAudioReferenceText(
    const std::string& source, const Project& project,
    const std::filesystem::path&          mapPathBeforeMove,
    const std::filesystem::path&          mapPathAfterMove,
    const std::vector<ResourcePathRemap>& remaps, std::string& output,
    bool& changed)
{
    output.clear();
    output.reserve(source.size());
    // 原长度只是容量估计，替换路径可能更长；不按原长度截断输出。
    changed = false;
    // 空输入正常返回空输出且未变化；调用方负责区分空文件与读取失败。
    std::string currentSection;
    std::size_t offset = 0U;
    while ( offset < source.size() ) {
        // 每行独立保留 CRLF/LF
        // 与末行是否有换行，避免局部路径替换改变整份文件格式。
        const auto newline = source.find('\n', offset);
        const auto lineEnd =
            newline == std::string::npos ? source.size() : newline;
        std::string line              = source.substr(offset, lineEnd - offset);
        const bool  hasCarriageReturn = !line.empty() && line.back() == '\r';
        if ( hasCarriageReturn ) line.pop_back();

        std::size_t contentBegin = 0U;
        std::size_t contentEnd   = line.size();
        trimAsciiRange(line, contentBegin, contentEnd);
        // 去空白只用于识别，原行仍保留缩进；节名按原文精确比较。
        if ( contentEnd > contentBegin + 1U && line[contentBegin] == '[' &&
             line[contentEnd - 1U] == ']' ) {
            currentSection =
                line.substr(contentBegin + 1U, contentEnd - contentBegin - 2U);
            // 每次遇到节头都切换上下文，未知节不能沿用上一节的音频字段解释。
        } else {
            std::size_t referenceBegin = std::string::npos;
            std::size_t referenceEnd   = std::string::npos;
            // 只识别明确支持的音频字段；其他节及无法识别的物件行原样传递。
            if ( currentSection == "General" ) {
                const auto separator = line.find(':');
                if ( separator != std::string::npos ) {
                    std::size_t keyBegin = 0U;
                    std::size_t keyEnd   = separator;
                    trimAsciiRange(line, keyBegin, keyEnd);
                    if ( line.substr(keyBegin, keyEnd - keyBegin) ==
                         "AudioFilename" ) {
                        // 只把首个冒号视为键值分隔，路径文本里的后续冒号不再次拆字段。
                        referenceBegin = separator + 1U;
                        referenceEnd   = line.size();
                    }
                }
            } else if ( currentSection == "HitObjects" &&
                        contentBegin < contentEnd &&
                        line[contentBegin] != '/' ) {
                // 前四个逗号用于定位 type，最后一个逗号用于定位采样字段。
                // 物件类型决定后者是否还包含长条结束时间前缀。
                const auto                 finalComma = line.rfind(',');
                std::array<std::size_t, 4> leadingCommas{};
                std::size_t                commaCount = 0U;
                std::size_t                searchFrom = 0U;
                while ( commaCount < leadingCommas.size() ) {
                    // 只读取定位 type
                    // 必需的分隔符，数值和其他物件参数不重新格式化。
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
                    // 必须完整解析
                    // type，不能把带额外尾部字符的损坏字段当成有效类型。
                    if ( parseError == std::errc{} &&
                         parseEnd == line.data() + typeEnd ) {
                        const bool isHold = (objectType & 128U) != 0U;
                        // type 是位集合，不能用等于 128
                        // 判断含有附加标记的长条。 长条的 endTime 比普通
                        // hitSample 多一个冒号分隔部分。
                        const std::size_t requiredSeparators = isHold ? 5U : 4U;
                        std::size_t       cursor             = finalComma + 1U;
                        for ( std::size_t index = 0U;
                              index < requiredSeparators;
                              ++index ) {
                            const auto separator = line.find(':', cursor);
                            if ( separator == std::string::npos ) {
                                // 不完整采样字段保持原文，不能猜一个偏移后误替换数值。
                                cursor = std::string::npos;
                                break;
                            }
                            cursor = separator + 1U;
                        }
                        if ( cursor != std::string::npos ) {
                            // 只跳过固定数量前缀，文件名自身的冒号保留在待替换文本中。
                            referenceBegin = cursor;
                            referenceEnd   = line.size();
                        }
                    }
                }
            }

            if ( referenceBegin != std::string::npos ) {
                trimAsciiRange(line, referenceBegin, referenceEnd);
                if ( referenceBegin < referenceEnd ) {
                    // 空文件名表示没有显式文件引用，不为它查找默认项目音效。
                    const auto reference = line.substr(
                        referenceBegin, referenceEnd - referenceBegin);
                    const auto replacement =
                        remapOsuAudioReference(project,
                                               reference,
                                               mapPathBeforeMove,
                                               mapPathAfterMove,
                                               remaps);
                    // 命中却不能构造替换值时停止，不交付“部分引用已改、部分失效”的谱面。
                    if ( replacement && replacement->empty() ) return false;
                    if ( replacement && *replacement != reference ) {
                        // 两侧空白不属于替换范围，同值替换也不将 changed 置为
                        // true。
                        line.replace(referenceBegin,
                                     referenceEnd - referenceBegin,
                                     *replacement);
                        changed = true;
                    }
                }
            }
        }

        // 仅路径片段经过 replace，数值字段、注释和未知行均沿用原始文本。
        output += line;
        if ( hasCarriageReturn ) output.push_back('\r');
        if ( newline != std::string::npos ) output.push_back('\n');
        offset = newline == std::string::npos ? source.size() : newline + 1U;
        // 末行没有换行时仍完整处理一次，不额外补写换行或产生空尾行。
    }
    return true;
}

/// @brief 读取完整文本文件且不改变换行符。
/// @param path 待读取路径。
/// @param output 文件内容。
/// @return 成功打开并完整读取时返回 true。
/// @note 打开失败时不改 output；读取失败时可能已有部分内容，必须检查返回值。
/// @warning 同步读取整个文件，不适用于每帧预览或实时播放路径。
bool readBinaryTextFile(const std::filesystem::path& path, std::string& output)
{
    // 二进制模式绕过平台文本换行转换，供无损局部替换使用。
    std::ifstream stream(path, std::ios::binary);
    if ( !stream.is_open() ) return false;
    output.assign(std::istreambuf_iterator<char>(stream),
                  std::istreambuf_iterator<char>());
    // 空文件也是合法读取结果；正常 EOF 与底层读取错误不能混为一谈。
    return stream.good() || stream.eof();
}

/// @brief 一个等待批量提交的 osu! 文本替换。
/// @note 同样用于 MMM/Malody 重序列化字节的提交，路径后缀不决定内容格式。
struct PendingTextFileReplacement {
    // 三条路径描述替换过程，不自动执行清理；提交和回滚函数统一控制其生命周期。
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
/// @note 只写旁路文件，不触碰原谱面；replacement 在失败时也可能已有路径值。
/// @pre 同一目标的替换任务串行执行，存在性检查不承担跨进程互斥。
/// @warning 同步完整写入并关闭文件，只用于低频保存或资源移动操作。
bool stageTextFileReplacement(const std::filesystem::path& path,
                              const std::string&           content,
                              PendingTextFileReplacement&  replacement)
{
    replacement.m_targetPath    = path;
    replacement.m_temporaryPath = path;
    replacement.m_temporaryPath += ".mmm-audio-remap.tmp";
    replacement.m_backupPath = path;
    replacement.m_backupPath += ".mmm-audio-remap.bak";
    // 临时文件和备份都与目标同目录，提交阶段使用 rename 而非跨目录复制。

    // 固定后缀若已存在就拒绝本次操作，不能覆盖上次失败留下的临时文件或恢复备份。
    std::error_code filesystemError;
    const bool      temporaryExists =
        std::filesystem::exists(replacement.m_temporaryPath, filesystemError);
    if ( filesystemError || temporaryExists ) return false;
    filesystemError.clear();
    const bool backupExists =
        std::filesystem::exists(replacement.m_backupPath, filesystemError);
    if ( filesystemError || backupExists ) return false;

    // 二进制写入对应完整字节缓冲，避免 Windows 文本模式再次转换换行。
    std::ofstream stream(replacement.m_temporaryPath,
                         std::ios::binary | std::ios::trunc);
    if ( !stream.is_open() ) return false;
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    // 按显式字节长度写出，内容中的零字节不会被当作 C 字符串结束标记。
    // 关闭后检查状态，覆盖缓冲区最终写出时才发生的错误。
    stream.close();
    if ( stream.good() ) return true;

    filesystemError.clear();
    std::filesystem::remove(replacement.m_temporaryPath, filesystemError);
    // 清理失败仍返回写入失败，不能把残留旁路文件当成已经成功暂存。
    return false;
}

/// @brief 清理一组尚未提交的 osu! 临时文件。
/// @param replacements 待清理替换列表。
/// @note 尽力清理且不返回删除状态，调用方不得据此断言磁盘没有临时文件残留。
void cleanupTextFileReplacements(
    const std::vector<PendingTextFileReplacement>& replacements)
{
    // 此入口仅处理尚未提交的输出，不删除备份；备份可能是回滚失败后的唯一原内容。
    for ( const auto& replacement : replacements ) {
        std::error_code filesystemError;
        std::filesystem::remove(replacement.m_temporaryPath, filesystemError);
    }
}

/// @brief 将全部 osu! 临时文件作为一个事务替换原文件。
/// @param replacements 已完成临时写出的替换列表。
/// @return 全部替换成功时返回 true；失败时尽量恢复所有原文件。
/// @note 这是进程内的批量补偿流程，不承诺断电或进程终止时的跨文件原子性。
/// @pre 列表中的目标互不重复，且每个暂存文件都已完成写入与关闭。
/// 空列表直接成功，表示没有待提交文本，而非已有文件经过重写验证。
/// @warning 同步 rename、删除及失败恢复可能阻塞，仅用于显式低频命令。
bool commitTextFileReplacements(
    std::vector<PendingTextFileReplacement>& replacements)
{
    // 计数只涵盖已完成两次 rename 的前缀；当前失败项在自身分支中恢复。
    std::size_t committedCount = 0U;
    // 替换顺序沿用暂存列表；回滚只覆盖本批已经安装的前缀，不重写未提交目标。
    for ( auto& replacement : replacements ) {
        std::error_code filesystemError;
        // 原内容先让位到备份，再把暂存文件放入正式路径，为后续失败保留恢复来源。
        std::filesystem::rename(replacement.m_targetPath,
                                replacement.m_backupPath,
                                filesystemError);
        if ( filesystemError ) break;

        // 到这里原文只在备份位置，若安装新文本失败须先恢复当前项再撤销前缀。
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
        // 两次 rename 都成功才把该项纳入已提交前缀，不把半完成项计入数量。
    }

    // 逆序撤销已提交前缀，尚未提交的临时文件随后统一清理。
    if ( committedCount != replacements.size() ) {
        while ( committedCount > 0U ) {
            --committedCount;
            // 先回退索引再取元素，保证最后成功安装的文件最先被恢复。
            auto&           replacement = replacements[committedCount];
            std::error_code filesystemError;
            std::filesystem::remove(replacement.m_targetPath, filesystemError);
            // 删除的是本批刚安装的新内容，原内容仍由备份路径持有。
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
        // 失败结果覆盖整个批次，即使部分恢复成功也不能向上层报告提交完成。
        return false;
    }

    // 全部目标替换成功后才移除备份；清理失败只警告，不否定已完成的内容提交。
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
/// @note 复制回退不是事务：失败时可能同时留下部分原路径和完整目标，需人工核对。
/// @pre newPath 与 oldPath
/// 必须来自本次已完成移动，不能作为任意目录恢复接口使用。
bool rollbackFilesystemMove(const std::filesystem::path& newPath,
                            const std::filesystem::path& oldPath)
{
    // 优先重命名回原位置；不可行时才复制恢复，以支持无法直接 rename 的场景。
    std::error_code filesystemError;
    std::filesystem::rename(newPath, oldPath, filesystemError);
    if ( !filesystemError ) return true;

    filesystemError.clear();
    std::filesystem::create_directories(oldPath.parent_path(), filesystemError);
    // 恢复父目录失败时不触碰当前位置，避免复制到不完整的目的层级。
    if ( filesystemError ) return false;

    const bool isDirectory =
        std::filesystem::is_directory(newPath, filesystemError);
    // 按当前位置的实际类型选择恢复方式，不依赖扩展名猜测目录或文件。
    if ( filesystemError ) return false;
    // 目录恢复保留符号链接本身，不跟随链接复制项目外的目标内容。
    if ( isDirectory ) {
        std::filesystem::copy(newPath,
                              oldPath,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::copy_symlinks,
                              filesystemError);
    } else {
        // 单文件复制不覆盖现有原路径，避免回滚覆盖移动后由其他流程创建的内容。
        std::filesystem::copy_file(newPath,
                                   oldPath,
                                   std::filesystem::copy_options::none,
                                   filesystemError);
    }
    if ( filesystemError ) return false;

    // 复制完全成功后才清理当前位置；复制失败时保留它作为恢复来源。
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
/// @pre 调用方已确认扫描可用；此首次构建入口不检查 m_success。
/// @warning 包含全量谱面探测和文件大小查询，只用于低频项目加载。
/// @note 只构建资源描述，不加载音频引擎，也不保存项目配置。
/// @pre project 根目录已设定，scanResult 路径属于同一次项目扫描。
void ProjectResourceService::buildInitialResources(
    Project&                                   project,
    const ProjectDirectoryScanner::ScanResult& scanResult) const
{
    /// @brief 扫描谱面后汇总的全部音频引用。
    std::vector<BeatmapAudioReference> audioReferences;

    // 首次构建以扫描为基线；旧配置的用户设置随后再合并，不沿用过时文件清单。
    project.m_beatmaps.clear();
    project.m_audioResources.clear();

    // 此阶段保留扫描到的条目，排除项过滤通过 applyExcludedResources 单独执行。
    // 不把谱面引用探测失败当成从扫描列表删除该谱面的理由。
    for ( const auto& mapPath : scanResult.m_beatmapFiles ) {
        /// @brief 谱面文件相对于项目根目录的 UTF-8 路径。
        auto relativeMapPath = makeProjectRelativeUtf8(project, mapPath);
        /// @brief 谱面文件名，用于显示和日志输出。
        auto filename = Config::pathToUtf8(mapPath.filename());

        /// @brief 新建的项目谱面条目。
        Project::BeatmapEntry mapEntry;
        mapEntry.m_name     = filename;
        mapEntry.m_filePath = relativeMapPath;

        // 初次发现采用文件名展示，探测只负责音频引用，不重写入口名称。
        auto references = probeBeatmapAudioReferences(
            project, mapPath, relativeMapPath, true);
        audioReferences.insert(audioReferences.end(),
                               std::make_move_iterator(references.begin()),
                               std::make_move_iterator(references.end()));

        project.m_beatmaps.push_back(mapEntry);
        XINFO("Found beatmap: {}", filename);
    }

    // 先收齐所有谱面引用，再分类音频，避免资源遍历顺序影响主音轨识别。
    /// @brief 一次性构建的谱面音频引用哈希索引。
    const AudioReferenceLookupIndex referenceIndex(project, audioReferences);
    /// @brief 是否已经通过谱面歌曲提示识别出主音轨。
    bool hasMainAudio = false;
    // 该标志只控制最终回退，不跳过后续资源的正常用途推断。
    /// @brief 没有歌曲提示时按文件大小选出的主音轨候选索引。
    std::optional<std::size_t> fallbackMainAudioIndex;
    // 候选保存索引而非元素地址，后续 push_back 扩容不会让候选定位失效。
    /// @brief 当前主音轨候选的文件大小。
    std::uintmax_t fallbackMainAudioSize = 0U;
    /// @brief 文件大小相同时用于稳定选择的项目相对路径。
    std::string fallbackMainAudioPath;
    for ( const auto& audioPath : scanResult.m_audioFiles ) {
        /// @brief 音频文件相对于项目根目录的 UTF-8 路径。
        const auto relativeAudioPath =
            makeProjectRelativeUtf8(project, audioPath);
        /// @brief 新建的项目音频资源条目。
        auto resource = createAudioResource(audioPath, relativeAudioPath);
        resource.m_type =
            inferIndexedAudioResourceType(referenceIndex, resource);

        project.m_audioResources.push_back(resource);
        // 已识别的 Main 可以不止一个；回退逻辑只处理一个也没有识别到的情形。
        if ( resource.m_type == AudioTrackType::Main ) {
            hasMainAudio = true;
            continue;
        }

        const bool boundToNote = (referenceIndex.matchingKinds(resource) &
                                  NOTE_SAMPLE_BINDING_MASK) != 0U;
        // 玩家物件绑定的音效不能仅因文件较大就升级为主音轨。
        if ( boundToNote ) continue;

        std::error_code fileSizeError;
        const auto      fileSize =
            std::filesystem::file_size(audioPath, fileSizeError);
        const auto candidatePath = relativeAudioPath;
        // 比较的是文件字节数，不是解码时长；不为寻找默认主音轨解码全部音频。
        // 无可靠大小时按零处理；同大小再按路径排序，避免扫描顺序改变默认主音轨。
        if ( !fallbackMainAudioIndex ||
             (!fileSizeError && fileSize > fallbackMainAudioSize) ||
             ((!fileSizeError ? fileSize : 0U) == fallbackMainAudioSize &&
              candidatePath < fallbackMainAudioPath) ) {
            fallbackMainAudioIndex = project.m_audioResources.size() - 1U;
            fallbackMainAudioSize  = fileSizeError ? 0U : fileSize;
            fallbackMainAudioPath  = candidatePath;
        }
    }

    // 回退候选只补缺，不覆盖已由谱面歌曲提示确定的主音轨。
    if ( !hasMainAudio && fallbackMainAudioIndex ) {
        // 所有候选都被玩家绑定时保持没有 Main，不牺牲物件音效语义强行选一个。
        auto& fallbackMainAudio =
            project.m_audioResources[*fallbackMainAudioIndex];
        fallbackMainAudio.m_type = AudioTrackType::Main;
        XINFO(
            "No song_file_hint matched; selected largest unbound audio '{}' "
            "as Main ({} bytes)",
            fallbackMainAudio.m_id,
            fallbackMainAudioSize);
    }

    for ( const auto& resource : project.m_audioResources ) {
        XINFO("Found {} audio resource: {}",
              (resource.m_type == AudioTrackType::Main ? "Main" : "Effect"),
              resource.m_id);
    }
}

/// @brief 根据项目排除列表过滤已经扫描出的谱面和音频资源。
/// @param project 需要过滤资源列表的项目实例。
/// @note 不重新推断剩余音频类型，也不执行会话关闭或音效卸载。
/// @pre 过滤时没有其他线程借用待擦除的项目资源元素。
/// 过滤保持未排除项的相对顺序，不重新生成显示名或资源 ID。
void ProjectResourceService::applyExcludedResources(Project& project) const
{
    // 从项目列表移除，不删除磁盘文件；排除项需保留以防下一次扫描重新收录。
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
/// @note 键保留 JSON 原文，不做文件系统规范化，须与持久化条目自身键配对。
/// 不修改原 JSON，识别结果仅影响随后类型合并时是否信任旧值。
std::unordered_set<std::string>
ProjectResourceService::collectLegacyAudioResourceKeys(
    const nlohmann::json& projectJson)
{
    std::unordered_set<std::string> result;
    // 读取原始 JSON
    // 而非反序列化默认值，才能区分旧格式缺字段与当前格式显式配置。
    const auto resourcesIterator = projectJson.find("m_audioResources");
    if ( resourcesIterator == projectJson.end() ||
         !resourcesIterator->is_array() ) {
        return result;
    }

    for ( const auto& resourceJson : *resourcesIterator ) {
        if ( !requiresLegacyAudioResourceMigration(resourceJson) ) continue;

        // 迁移资格以每条资源为单位，允许旧格式和已有 m_config 的条目共存。
        // 键的选择须与合并时相同：有路径用路径，缺路径才退回资源 ID。
        const auto pathIterator = resourceJson.find("m_path");
        if ( pathIterator != resourceJson.end() && pathIterator->is_string() ) {
            const auto path = pathIterator->get<std::string>();
            if ( !path.empty() ) {
                result.insert(path);
                continue;
            }
        }

        const auto idIterator = resourceJson.find("m_id");
        // 路径缺失、空或不是字符串时才尝试 ID，不把两种键都加入而扩大匹配范围。
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
/// @details 扫描列表决定资源是否存在，持久化列表只提供匹配条目的配置。
/// 不恢复磁盘扫描中已经消失的资源，也不覆盖扫描得到的 ID 和路径。
/// @warning 合并后还会探测谱面引用，不能作为每帧配置刷新函数调用。
/// @pre persistedProject 保持只读，不能与被修改的 project 别名。
void ProjectResourceService::mergePersistedAudioResources(
    Project& project, const Project& persistedProject,
    const std::unordered_set<std::string>& legacyAudioResourceKeys) const
{
    /// @brief 按精确持久化路径建立的首次出现资源索引。
    std::unordered_map<std::string, const AudioResource*> persistedByPath;
    /// @brief 按稳定 ID 建立的首次出现资源索引。
    std::unordered_map<std::string, const AudioResource*> persistedById;
    // 索引键由哈希表持有，观察指针只用于读取旧资源配置。
    // 索引观察持久化列表，在整个合并期间不修改该列表，指针因此保持有效。
    // 重复键保留首次出现项，避免哈希插入顺序改变恢复选择。
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
        // 精确字符串索引用于恢复旧配置身份，不调用路径兼容解析去猜测匹配。
        if ( !resource.m_path.empty() ) {
            const auto pathIterator = persistedByPath.find(resource.m_path);
            if ( pathIterator != persistedByPath.end() ) {
                matchedPersistedResource = pathIterator->second;
            }
        }
        // 同 ID 可能出现在不同路径；精确路径匹配优先，只有缺失时才尝试 ID。
        if ( !matchedPersistedResource ) {
            const auto idIterator = persistedById.find(resource.m_id);
            if ( idIterator != persistedById.end() ) {
                matchedPersistedResource = idIterator->second;
            }
        }
        // 新发现的资源维持扫描默认配置；没有旧条目并不是合并失败。
        if ( !matchedPersistedResource ) continue;

        const auto& persistedResource = *matchedPersistedResource;
        const auto& persistedKey      = persistedResource.m_path.empty()
                                            ? persistedResource.m_id
                                            : persistedResource.m_path;

        // 旧格式类型不可靠，保留扫描推断；音量等配置仍然迁移，不能整条跳过。
        if ( !legacyAudioResourceKeys.contains(persistedKey) ) {
            resource.m_type = persistedResource.m_type;
        }
        resource.m_config = persistedResource.m_config;
        // 整体复制音轨配置，音量之外的静音、播放参数及均衡器设置一并保留。
    }

    /// @brief 合并后重新收集 Note 绑定，防止持久化类型恢复出非法 Main。
    std::vector<BeatmapAudioReference> audioReferences;
    for ( const auto& entry : project.m_beatmaps ) {
        // 从本次项目实际收录的谱面读取引用，不以旧项目列表决定绑定约束。
        auto references = probeBeatmapAudioReferences(
            project,
            resolveProjectPath(project, Config::utf8ToPath(entry.m_filePath)),
            entry.m_filePath,
            false);
        audioReferences.insert(audioReferences.end(),
                               std::make_move_iterator(references.begin()),
                               std::make_move_iterator(references.end()));
    }
    // 配置恢复之后再执行绑定约束，保证持久化 Main 不绕过 Note 音效类型要求。
    /// @brief 合并校验共享的谱面音频引用哈希索引。
    const AudioReferenceLookupIndex referenceIndex(project, audioReferences);
    for ( auto& resource : project.m_audioResources ) {
        const bool boundToNote = (referenceIndex.matchingKinds(resource) &
                                  NOTE_SAMPLE_BINDING_MASK) != 0U;
        if ( !boundToNote || resource.m_type == AudioTrackType::Effect ) {
            continue;
        }
        // 绑定约束仅纠正类型，刚恢复的音量和效果参数不因降为 Effect 而重置。
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
/// @details 返回的登记列表也包括由 Main 转成 Effect 的已有资源。
/// m_changed 描述列表成员或音轨类型变化，不代表完成音频加载或项目落盘。
/// @warning 同步会读取谱面并重建索引，仅用于合并后的低频目录事件。
/// @pre 调用方串行更新项目，两次列表替换不是可供并发读者观察的原子事务。
ProjectResourceService::DirectorySyncResult
ProjectResourceService::syncDirectoryResources(
    Project&                                   project,
    const ProjectDirectoryScanner::ScanResult& scanResult) const
{
    /// @brief 本次同步的输出结果。
    DirectorySyncResult result;
    // 扫描失败不等于目录为空，不能据此删除已有资源或登记一次成功同步。
    if ( !scanResult.m_success ) {
        return result;
    }
    result.m_scanSucceeded = true;
    // 成功的空扫描与失败扫描不同：前者会清空不再存在的资源条目。

    /// @brief 同步后新的谱面条目列表。
    std::vector<Project::BeatmapEntry> newBeatmaps;
    // scanResult
    // 是完整清单，不是事件增量；只传变化项会让未列出的旧资源退出项目。
    /// @brief 扫描谱面后汇总的全部音频引用。
    std::vector<BeatmapAudioReference> audioReferences;
    /// @brief 按精确项目路径建立的现有谱面索引。
    std::unordered_map<std::string, const Project::BeatmapEntry*>
        existingBeatmapsByPath;
    // 建立只读索引直到新列表组装完成，沿用已有谱面条目的用户设置和显示信息。
    existingBeatmapsByPath.reserve(project.m_beatmaps.size());
    for ( const auto& entry : project.m_beatmaps ) {
        existingBeatmapsByPath.try_emplace(entry.m_filePath, &entry);
        // 同一路径重复出现时保留首个旧入口，不让后者覆盖已经索引的显示信息。
    }

    for ( const auto& mapPath : scanResult.m_beatmapFiles ) {
        /// @brief 谱面文件相对于项目根目录的 UTF-8 路径。
        auto relativeMapPath = makeProjectRelativeUtf8(project, mapPath);
        /// @brief 谱面文件名，用于显示和日志输出。
        auto filename = Config::pathToUtf8(mapPath.filename());

        // 排除项不进入新列表，也不参与这一轮音频用途推断。
        if ( containsExcludedPath(project.m_excludedBeatmapPaths,
                                  relativeMapPath) ) {
            continue;
        }

        /// @brief 本次同步要写入的新谱面条目。
        Project::BeatmapEntry mapEntry;
        const auto existingEntry = existingBeatmapsByPath.find(relativeMapPath);
        if ( existingEntry != existingBeatmapsByPath.end() ) {
            // 文件仍在原位置时复制已有入口，不仅恢复文件名这一项。
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
        // 新列表顺序跟随扫描结果，哈希表仅用于查找而不参与输出遍历。
    }

    // 新增已在发现时置脏；数量差异补充捕获删除，二者结合覆盖等量替换。
    if ( newBeatmaps.size() != project.m_beatmaps.size() ) {
        result.m_changed = true;
        XINFO(
            "Directory Listener: Some beatmaps were removed from the "
            "directory.");
    }
    project.m_beatmaps = std::move(newBeatmaps);
    // 仅顺序变化不会在此额外置脏，changed 不是新旧列表逐字段比较的结果。
    // 旧入口观察指针从这里起不可解引用，后续音频处理只使用已收集的引用值。

    /// @brief 本轮同步共享的谱面音频引用哈希索引。
    const AudioReferenceLookupIndex referenceIndex(project, audioReferences);
    /// @brief 按规范化项目路径建立的现有音频资源索引。
    std::unordered_map<std::string, const AudioResource*>
        existingAudioResourcesByPath;
    // 音频使用与引用解析相同的路径键，避免斜杠等写法差异重复创建资源。
    existingAudioResourcesByPath.reserve(project.m_audioResources.size());
    for ( const auto& resource : project.m_audioResources ) {
        const auto pathKey = referenceIndex.projectPathKey(resource.m_path);
        if ( !pathKey.empty() ) {
            existingAudioResourcesByPath.try_emplace(pathKey, &resource);
            // 无法生成路径键的旧条目不参与路径恢复，不使用空键匹配任意扫描文件。
        }
    }

    /// @brief 同步后新的音频资源列表。
    std::vector<AudioResource> newAudioResources;
    // 全部候选准备完成前保留旧列表，匹配索引中的观察地址不会在循环中失效。
    for ( const auto& audioPath : scanResult.m_audioFiles ) {
        /// @brief 音频文件相对于项目根目录的 UTF-8 路径。
        auto relativeAudioPath = makeProjectRelativeUtf8(project, audioPath);
        /// @brief 音频文件名，用于日志输出。
        auto filename = Config::pathToUtf8(audioPath.filename());

        if ( containsExcludedPath(project.m_excludedAudioPaths,
                                  relativeAudioPath) ) {
            continue;
        }

        /// @brief 本次同步要写入的新音频资源。
        AudioResource resource;
        const auto    relativeAudioPathKey =
            referenceIndex.projectPathKey(relativeAudioPath);
        const auto existingResource =
            existingAudioResourcesByPath.find(relativeAudioPathKey);
        if ( existingResource != existingAudioResourcesByPath.end() ) {
            // 路径命中后保留稳定资源 ID
            // 和用户配置，类型按当前引用约束单独调整。
            resource = *existingResource->second;
            const auto inferredType =
                inferIndexedAudioResourceType(referenceIndex, resource);
            const auto matchingKinds = referenceIndex.matchingKinds(resource);
            // 没有歌曲提示或物件绑定时保留已有类型，不因一般自动采样引用改写用户选择。
            const bool hasTypeReference =
                (matchingKinds &
                 (SONG_FILE_HINT_MASK | NOTE_SAMPLE_BINDING_MASK)) != 0U;
            if ( hasTypeReference && resource.m_type != inferredType ) {
                resource.m_type  = inferredType;
                result.m_changed = true;
                // 由主音轨转为音效时也需要登记，不仅新发现的文件需要音效接入。
                if ( resource.m_type == AudioTrackType::Effect ) {
                    // 返回值持有资源副本，后续替换项目列表不会使登记数据悬空。
                    result.m_effectResourcesToRegister.push_back(resource);
                }
            }
        } else {
            // 真正新增的文件使用默认音轨配置，不从其他同文件名资源复制参数。
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
        // 扫描中消失或被排除的资源不会进入新列表，不需要再次按旧索引逐项擦除。
    }

    if ( newAudioResources.size() != project.m_audioResources.size() ) {
        result.m_changed = true;
        XINFO(
            "Directory Listener: Some audio files were removed from the "
            "directory.");
    }
    project.m_audioResources = std::move(newAudioResources);
    // 替换后不再访问旧资源索引；需要交付上层的数据已复制进结果结构。

    // 此处只交付需登记的音效；具体加载、旧资源卸载和项目保存由上层协调。
    return result;
}

/// @brief 规范化项目相对路径，用于稳定比较排除列表。
/// @param path UTF-8 编码的项目相对路径。
/// @return 规范化后的 UTF-8 项目相对路径。
/// @note 仅词法处理，不查询文件系统；排除项对应文件已删除时仍能稳定比较。
/// 不主动转换历史 Windows 分隔符，区别于音频引用索引键生成。
std::string ProjectResourceService::normalizeProjectRelativePath(
    const std::string& path)
{
    if ( path.empty() ) return "";
    // 不把空路径转换成点目录，避免无资源的输入匹配到项目根。
    return Config::pathToUtf8(Config::utf8ToPath(path).lexically_normal());
}

/// @brief 判断路径是否存在于排除列表中。
/// @param excludedPaths 项目排除列表。
/// @param path 需要检查的 UTF-8 项目相对路径。
/// @return 路径已被排除时返回 true。
/// @note 不修改排除列表，也不按文件系统等价性合并大小写或符号链接别名。
bool ProjectResourceService::containsExcludedPath(
    const std::vector<std::string>& excludedPaths, const std::string& path)
{
    /// @brief 规范化后的待检查项目相对路径。
    std::string normalized = normalizeProjectRelativePath(path);
    // 旧条目也在比较时规范化，不要求先批量重写已有用户配置。
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
/// @note 转换失败时返回文件名；该回退不同于保留相对输入的
/// makeProjectRelativePath。
std::string ProjectResourceService::makeProjectRelativeUtf8(
    const Project& project, const std::filesystem::path& path)
{
    auto relativePath = makeRelativeToProjectRoot(project.m_projectRoot, path);
    if ( relativePath.empty() ) {
        // 兼容旧引用键的 basename 回退，不能据此认定原路径真的在项目根内。
        relativePath = path.filename();
    }
    return Config::pathToUtf8(relativePath.lexically_normal());
}

/// @brief 解析项目持久化路径为可访问的文件系统路径。
/// @param project 路径所属项目。
/// @param path 项目相对路径或绝对路径。
/// @return 规范化后的文件系统路径。
/// @note 允许绝对输入及缺失文件，敏感文件操作仍需独立校验范围和权限。
std::filesystem::path ProjectResourceService::resolveProjectPath(
    const Project& project, const std::filesystem::path& path)
{
    if ( path.empty() || path.is_absolute() ) {
        // 空值保持未选择状态，绝对值不再次拼接项目根目录。
        return path.lexically_normal();
    }

    const auto      root = weaklyCanonicalAbsolutePath(project.m_projectRoot);
    const auto      directCandidate = (root / path).lexically_normal();
    std::error_code filesystemError;
    if ( std::filesystem::exists(directCandidate, filesystemError) &&
         !filesystemError ) {
        // 实际存在的同名子目录优先，不能只因首段像项目名就直接删除该层级。
        return directCandidate;
    }

    // 直接根相对解释优先，只有找不到文件时才尝试旧版重复项目名前缀。
    const auto stripped = stripProjectFolderPrefix(root, path);
    if ( !stripped.empty() ) {
        // 兼容候选必须存在才采用，否则仍返回直接解释的位置供缺失资源诊断。
        const auto strippedCandidate = (root / stripped).lexically_normal();
        filesystemError.clear();
        if ( std::filesystem::exists(strippedCandidate, filesystemError) &&
             !filesystemError ) {
            return strippedCandidate;
        }
    }

    // 两种候选都不存在时返回首选位置，保持缺失资源的解析结果可预测。
    return directCandidate;
}

/// @brief 将文件系统路径转换为项目相对路径。
/// @param project 路径所属项目。
/// @param path 需要转换的文件系统路径。
/// @return 项目相对路径；无法转换时保留相对输入，绝对输入退回文件名。
/// @note 只转换表示，不导入文件；basename 回退并不意味着文件已经复制到根目录。
std::filesystem::path ProjectResourceService::makeProjectRelativePath(
    const Project& project, const std::filesystem::path& path)
{
    if ( path.empty() ) return {};

    auto relativePath = makeRelativeToProjectRoot(project.m_projectRoot, path);
    if ( !relativePath.empty() ) {
        return relativePath.lexically_normal();
    }

    if ( path.is_relative() ) return path.lexically_normal();
    // 绝对输入无法相对化时只保留资源名，不将宿主机目录写入这一回退表示。
    return path.filename();
}

/// @brief 在项目根目录和谱面目录之间解析元数据资源路径。
/// @param project 路径所属项目。
/// @param mapDirectory 谱面文件所在目录。
/// @param path 元数据中记录的资源路径。
/// @param preferProjectRoot 是否优先按项目根目录解析。
/// @return 可访问优先的规范化资源路径。
/// @note 只检查存在性，不判断文件类型或解码能力；两个候选均失败仍返回首选路径。
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
    // 两个基准分别解释同一输入，不能用根目录解析结果再拼接谱面目录。

    /// @brief 文件存在性检查错误码。
    std::error_code filesystemError;
    // 优先级决定同名文件冲突时选择哪一个；两者均缺失时也保留首选解释。
    if ( preferProjectRoot ) {
        if ( std::filesystem::exists(projectPath, filesystemError) )
            return projectPath;
        filesystemError.clear();
        if ( std::filesystem::exists(mapPath, filesystemError) ) return mapPath;
        return projectPath;
    }

    if ( std::filesystem::exists(mapPath, filesystemError) ) return mapPath;
    // 上一候选失败不会阻止尝试另一位置，错误码不作为跨候选累积状态。
    filesystemError.clear();
    if ( std::filesystem::exists(projectPath, filesystemError) )
        return projectPath;
    return mapPath;
}

/// @brief 将谱面元数据中的长期资源路径规范化为项目相对路径。
/// @param beatMap 需要规范化元数据路径的谱面。
/// @param project 谱面所属项目。
/// @note 仅原地规范化基础元数据；不保存文件，也不改玩家绑定和自动采样的资源
/// ID。
void ProjectResourceService::normalizeBeatmapMetadataPathsForProject(
    BeatMap& beatMap, const Project& project)
{
    /// @brief 谱面的基础元数据引用。
    auto& meta = beatMap.m_baseMapMetadata;
    // 谱面自身路径缺失时无法确定格式和相对基准，不猜测资源所属目录。
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
    // MMM
    // 内部格式按项目根解释，其余导入格式先按谱面目录解释，兼容各自引用习惯。
    bool preferProjectRoot = (mapExtension == ".mmm");

    meta.map_path = makeProjectRelativePath(project, absoluteMapPath);
    // mapDirectory 已保存原位置，随后改写其他字段不受 map_path 表示变化影响。

    /// @brief 规范化单个元数据资源路径的闭包。
    auto normalizeResourcePath = [&](std::filesystem::path& path) {
        // 未填写字段保持空，不因路径回退而自动选择目录或默认图片。
        if ( path.empty() ) return;
        /// @brief 解析后的资源路径。
        auto resolved = resolveMetadataResourcePath(
            project, mapDirectory, path, preferProjectRoot);
        path = makeProjectRelativePath(project, resolved);
    };

    normalizeResourcePath(meta.main_audio_path);
    // 新旧音频字段各自规范化，此处不执行单主音轨迁移或删除旧字段。
    normalizeResourcePath(meta.song_file_hint);
    normalizeResourcePath(meta.main_cover_path);
    // 封面和背景分别保存，不能假定二者总是引用同一个文件。
    normalizeResourcePath(meta.cover_path);
}

/// @brief 从已经载入内存的谱面收集完整音频引用。
/// @param beatMap 待读取的内存谱面。
/// @param beatmapPath 用于诊断和相对路径解析的具体谱面路径。
/// @return 歌曲提示、玩家物件绑定和自动采样引用列表。
/// @note 返回值拥有引用文本，不借用谱面字段；同一资源的重复用途不会合并。
/// 音量为零的绑定仍是引用，不因当前不可听见就允许删除其资源。
/// @pre 折线子物件仍由 notes/holds/flicks 持有，父对象中的连接引用有效。
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

    // 新歌曲提示优先，旧 main_audio_path 仅补缺，避免同一歌曲重复计入两个提示。
    const auto& meta         = beatMap.m_baseMapMetadata;
    const auto& songFileHint = meta.song_file_hint.empty()
                                   ? meta.main_audio_path
                                   : meta.song_file_hint;
    appendReference(Config::pathToUtf8(songFileHint),
                    BeatmapAudioReferenceKind::SongFileHint);

    /// @brief 收集一个玩家物件的命中采样绑定。
    auto appendNoteBinding = [&](const Note& note) {
        // 只读取物件显式绑定；父折线的回退音效由父对象自身的绑定另外覆盖。
        const auto binding = note.getSampleBinding();
        if ( !binding ) return;
        appendReference(binding->m_audioResourceId,
                        BeatmapAudioReferenceKind::NoteSampleBinding);
    };
    for ( const auto& note : beatMap.m_noteData.notes ) {
        // 不跳过子物件，因为子节点也可以拥有独立于折线头的采样绑定。
        appendNoteBinding(note);
    }
    for ( const auto& hold : beatMap.m_noteData.holds ) {
        appendNoteBinding(hold);
    }
    for ( const auto& flick : beatMap.m_noteData.flicks ) {
        appendNoteBinding(flick);
    }
    for ( const auto& polyline : beatMap.m_noteData.polylines ) {
        // 子物件已由持有容器遍历，不再沿 m_subNotes 重复收集。
        appendNoteBinding(polyline);
    }

    // 自动采样和玩家绑定分开标记，用途推断与删除前检查需要区分它们。
    for ( const auto& sample : beatMap.m_audioSamples ) {
        // 不按时间或轨道可见性过滤，离屏和未来物件同样依赖资源。
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
/// @warning 磁盘加载入口，只用于低频项目扫描，不应加入播放更新调用链。
/// @note 加载失败和没有音频引用均返回空列表，调用方不能据空列表证明读取成功。
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

    // 先统一导入格式的元数据路径基准，再收集引用；不修改磁盘谱面。
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
    // 单次查询只建根目录上下文，不预装引用表，直接走相同的兼容匹配规则。
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
/// @note 原地修改模型，不保存磁盘文件，也不更新打开会话的 ECS 数据。
/// 返回的各类匹配数按字段计数，不是去重后的资源数或谱面数。
/// @pre previousResource 是移动前的独立快照，updatedResourcePath
/// 使用项目相对表示。
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
    // 资源路径已更新的调用方仍可用旧快照匹配字段，不借用当前资源表的路径值。

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
        // 匹配计数先于是否改写判断，已是目标路径也仍属于该资源的引用。
        ++result.m_songFileHintReferenceCount;
        const auto updatedPath = Config::utf8ToPath(updatedResourcePath);
        if ( path == updatedPath ) return;
        path = updatedPath;
        ++result.m_changedReferenceCount;
    };
    remapMetadataPath(beatMap.m_baseMapMetadata.song_file_hint);
    // 这里处理两个实际字段，与收集时的新字段优先不同；旧字段不能遗留失效位置。
    remapMetadataPath(beatMap.m_baseMapMetadata.main_audio_path);
    // 两个提示均命中时分别计数，调用方不能用提示数推算歌曲资源的唯一数量。

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
        // 已使用稳定 ID 时跳过 setSampleBinding，避免无变化写回绑定元数据。
        // 移动只改变位置，不改变身份；把路径型绑定收敛到原有稳定 ID。
        binding->m_audioResourceId = previousResource.m_id;
        // 写回完整绑定以保留原音量，仅资源身份从路径收敛为稳定 ID。
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
        // 匹配数统计使用关系，改写数统计字段变化；已使用稳定 ID 时二者会不同。
        if ( sample.m_audioResourceId == previousResource.m_id ) continue;
        sample.m_audioResourceId = previousResource.m_id;
        // 文件位置变化不移动采样时间、偏移或轨道，也不重新生成采样物件。
        ++result.m_changedReferenceCount;
    }
    return result;
}

/// @brief 将内存谱面的玩家绑定和自动采样资源 ID 精确重命名。
/// @param beatMap 需要原地更新的谱面。
/// @param oldResourceId 旧资源 ID。
/// @param newResourceId 新资源 ID。
/// @return 实际改写的引用字段数量。
/// @note 不检查新 ID 是否已属于其他项目资源，身份冲突由资源管理命令处理。
/// 不解析外部音频文件，字段替换成功也不代表新资源可以播放。
/// @note 这里只替换精确 ID，不兼容匹配旧路径；路径型引用需先经过移动重映射。
/// 歌曲提示是路径字段，不随此 ID 重命名步骤修改。
/// @pre 传入的 ID 视图在整个调用中稳定，不能观察即将被改写的绑定字符串。
std::size_t ProjectResourceService::remapBeatmapAudioResourceId(
    BeatMap& beatMap, std::string_view oldResourceId,
    std::string_view newResourceId)
{
    if ( oldResourceId.empty() || newResourceId.empty() ||
         oldResourceId == newResourceId ) {
        // 空输入不是清除绑定命令，同 ID 也不计为发生了修改。
        return 0U;
    }

    std::size_t changedCount = 0U;
    // 不更改物件自身身份，撤销、选择等机制引用的 Note ID 不受影响。
    // 统计目标字段数，即使同一个 ID 被多次引用，也须逐物件写回并分别累计。
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

/// @brief 暂存项目内 MMM/Malody 资源引用改写，并批量提交或尝试补偿恢复。
/// @param project 待扫描项目。
/// @param previousResource 重命名前的资源 ID 与路径快照。
/// @param updatedResourcePath 重命名后的项目相对路径。
/// @param newResourceId 新资源 ID。
/// @return 事务结果和实际变更谱面数量。
/// @note m_success 为完成判据；暂存过程中失败可能保留此前的候选计数。
/// 成功且数量为零表示无需写回，不能据此认定资源未被任何谱面引用。
/// @warning 低频显式重命名路径：会读取并重新序列化相关谱面。
/// @note 文件提交失败会尝试回滚，但文件系统再次失败时不能保证恢复全部原内容。
/// 此入口不修改项目资源表；打开会话的未保存编辑需由上层另行协调。
/// @pre 同一项目的资源重命名串行执行，临时文件后缀不用于并发任务隔离。
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
        // 身份与位置均未变时直接成功，路径单独变化仍需刷新谱面歌曲提示。
        result.m_success = true;
        return result;
    }

    // 先完成所有候选谱面的读取、转换和暂存，避免后一谱面失败时前一谱面已经落盘。
    std::vector<PendingTextFileReplacement> pendingReplacements;
    // 每个谱面至多产生一个替换项，内部引用数量不影响提交列表长度。
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
        // 本入口重写支持资源 ID 的格式；其他格式的路径引用不经此序列化流程。
        if ( extension != ".mmm" && extension != ".mc" ) continue;

        auto beatMap = BeatMap::loadFromFile(mapPath);
        // 不能可靠读取的候选也不能判断是否无引用，因此取消整批而非静默跳过。
        if ( beatMap.m_baseMapMetadata.map_path.empty() ) {
            cleanupTextFileReplacements(pendingReplacements);
            result.m_errorMessage = "无法读取谱面 '" + entry.m_filePath +
                                    "'，已取消音频资源 ID 重命名";
            return result;
        }
        // 先把旧路径式绑定收敛为旧稳定 ID，再精确替换 ID，避免遗漏兼容引用。
        const auto pathRemap = remapBeatmapAudioReferencesAfterMove(
            project,
            beatMap,
            entry.m_filePath,
            previousResource,
            std::string(updatedResourcePath));
        const auto idRemap = remapBeatmapAudioResourceId(
            beatMap, previousResource.m_id, newResourceId);
        // 两步可能作用于同一字段，不能把两步计数相加当成不同物件数量。
        if ( !pathRemap.changed() && idRemap == 0U ) {
            // 无实际字段变化不重新序列化，避免改变与本次资源操作无关的文件。
            continue;
        }

        // 临时序列化路径仍以原扩展名结尾，让保存器选择与源谱面相同的格式。
        auto serializedPath = mapPath;
        serializedPath += ".mmm-audio-id-remap";
        serializedPath += mapPath.extension();
        // 序列化旁路和统一提交的 .tmp 是两类文件，前者读取后即清理。
        std::error_code filesystemError;
        if ( std::filesystem::exists(serializedPath, filesystemError) ||
             filesystemError ) {
            // 已存在的旁路文件可能是上次操作的证据，本次不覆盖或清理它。
            cleanupTextFileReplacements(pendingReplacements);
            result.m_errorMessage =
                "谱面 '" + entry.m_filePath + "' 的音频重命名临时文件已存在";
            return result;
        }
        if ( !beatMap.saveToFile(serializedPath) ) {
            // 清理本次序列化产物及此前暂存项，正式目标尚未进入替换阶段。
            filesystemError.clear();
            std::filesystem::remove(serializedPath, filesystemError);
            cleanupTextFileReplacements(pendingReplacements);
            result.m_errorMessage =
                "无法序列化谱面 '" + entry.m_filePath + "' 的音频资源 ID";
            return result;
        }

        // 保存器输出转为完整字节后交给统一暂存提交，不直接覆盖源谱面。
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
            // 只在暂存成功后纳入统一清理列表，失败项由暂存函数处理其写入残留。
            cleanupTextFileReplacements(pendingReplacements);
            result.m_errorMessage =
                "无法暂存谱面 '" + entry.m_filePath + "' 的音频资源 ID";
            return result;
        }
        pendingReplacements.push_back(std::move(replacement));
        // 暂存成功才累计候选；较后候选失败时 m_success 仍为
        // false，不能只看计数。
        ++result.m_changedBeatmapCount;
    }

    // 暂存阶段计数不等于提交成功；提交失败把数量清零，避免上层据此认为已完成重写。
    if ( !commitTextFileReplacements(pendingReplacements) ) {
        // 返回失败是权威状态；恢复步骤也可能失败，应保留日志及备份用于诊断。
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
/// @details 现有可解析提示不要求资源是 Main；仅自动采样回退限定 Main 类型。
/// @note m_source 表示选取依据，m_changed 表示字段改变，两者不能互相替代。
/// 返回的资源 ID 是值副本，不借用参与选择的项目列表元素。
BeatmapSongFileHintUpdateResult
ProjectResourceService::refreshSongFileHintForSave(
    const Project& project, BeatMap& beatMap,
    const std::filesystem::path& beatmapPath)
{
    BeatmapSongFileHintUpdateResult result;
    auto&                           metadata = beatMap.m_baseMapMetadata;
    const auto                      referenceMapPath =
        beatmapPath.empty() ? metadata.map_path : beatmapPath;
    // 调用方传入的保存位置优先，另存为时不能继续用旧文件目录解释相对引用。

    // 有效的现有提示优先保留，不因最早采样变化而每次保存都换歌。
    if ( !metadata.song_file_hint.empty() ) {
        const auto* existingResource = findAudioResourceForReference(
            project,
            referenceMapPath,
            Config::pathToUtf8(metadata.song_file_hint));
        if ( existingResource ) {
            // 保留现有提示原文，不因解析成功而改成另一种路径写法。
            result.m_source = BeatmapSongFileHintSource::ExistingHint;
            // 返回资源身份方便上层展示，不在这里将提示字符串改成资源 ID。
            result.m_audioResourceId = existingResource->m_id;
            if ( !metadata.main_audio_path.empty() ) {
                metadata.main_audio_path.clear();
                result.m_changed = true;
            }
            return result;
        }
    }

    // 提示失效才回退到主音轨采样；比较有效时间戳，不依赖容器当前排列。
    const AudioResource* earliestMainResource = nullptr;
    double earliestEffectiveTimestamp = std::numeric_limits<double>::infinity();
    // 从正无穷开始允许负的有效时间成为候选，不把谱面零点当作下界。
    for ( const auto& sample : beatMap.m_audioSamples ) {
        const double effectiveTimestamp = sample.effectiveTimestamp();
        // 有效时间包含采样偏移，不能仅按物件锚点确定歌曲起始顺序。
        // 非有限时间不可作为提示依据；相同时间保留首次遇到的可用主音轨。
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
        // 只有可解析 Main 才更新最早时间，未解析的早期采样不能阻挡后续候选。
        earliestMainResource = resource;
    }

    // 无可用主音轨时保持空目标，以清除已失效的旧提示，不保留悬空歌曲路径。
    std::filesystem::path updatedHint;
    if ( earliestMainResource ) {
        // 提示写资源路径而非资源 ID，维持需要歌曲文件提示的格式语义。
        updatedHint     = Config::utf8ToPath(earliestMainResource->m_path);
        result.m_source = BeatmapSongFileHintSource::EarliestMainSample;
        result.m_audioResourceId = earliestMainResource->m_id;
    }
    // 只在字段值实际变化时置脏，查到资源或选到相同提示本身不构成修改。
    if ( metadata.song_file_hint != updatedHint ) {
        metadata.song_file_hint = std::move(updatedHint);
        result.m_changed        = true;
    }
    if ( !metadata.main_audio_path.empty() ) {
        // 未找到可用新提示时也清掉旧字段，不能让已废弃字段重新参与选歌回退。
        metadata.main_audio_path.clear();
        result.m_changed = true;
    }
    return result;
}

/// @brief 查找指定项目音频资源在全部谱面中的引用。
/// @param project 待扫描的项目。
/// @param resource 待匹配的项目音频资源。
/// @return 按谱面和用途记录的引用列表。
/// @note 扫描磁盘谱面，不包含未保存的内存编辑；调用方需另行合并打开会话引用。
/// @warning 全项目文件读取，只用于资源变更前检查等低频命令。
/// @note 读取失败会由探测器告警，但本接口不返回独立的扫描成功标志。
std::vector<BeatmapAudioReference>
ProjectResourceService::findAudioResourceReferences(
    const Project& project, const AudioResource& resource)
{
    std::vector<BeatmapAudioReference> result;
    /// @brief 全部谱面引用匹配共享的路径查找上下文。
    const AudioReferenceLookupIndex referenceIndex(project);
    /// @brief 待查资源预计算后的查找键。
    const auto resourceKey = referenceIndex.makeResourceLookupKey(resource);
    // 资源键一次计算后复用于所有谱面，不随遍历到的谱面目录改变资源自身位置。
    for ( const auto& entry : project.m_beatmaps ) {
        const auto mapPath =
            resolveProjectPath(project, Config::utf8ToPath(entry.m_filePath));
        auto references = probeBeatmapAudioReferences(
            project, mapPath, entry.m_filePath, true);
        for ( auto& reference : references ) {
            if ( referenceIndex.referenceMatchesResource(reference,
                                                         resourceKey) ) {
                result.push_back(std::move(reference));
                // 保留每项用途和来源，不在此按谱面去重丢掉绑定与自动采样的区别。
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
/// @note 返回值观察项目列表元素，不持有资源；列表增删或项目切换后不可沿用。
/// @warning 单次调用也会建立资源索引，不应放入每帧物件遍历中逐项调用。
const AudioResource* ProjectResourceService::findAudioResourceForReference(
    const Project& project, const std::filesystem::path& beatmapPath,
    const std::string& audioReference)
{
    if ( audioReference.empty() ) return nullptr;

    const AudioResourceResolutionIndex resolutionIndex(project);
    // 单次调用构建临时索引，多引用调用方应使用批量入口复用解析工作。
    return resolutionIndex.resolve(
        audioReference, resolutionIndex.beatmapDirectoryKey(beatmapPath));
}

/// @brief 批量解析同一谱面的项目音频资源引用。
/// @param project 待查询项目。
/// @param beatmapPath 引用所在谱面的项目相对或绝对路径。
/// @param audioReferences 谱面保存的资源 ID 或旧路径视图。
/// @return 与输入顺序一一对应的资源地址；未解析项为空。
/// @note 输出不去重、不省略失败项；指针寿命由项目资源列表决定。
/// @warning 低频加载路径：一次构建资源解析索引并线性处理全部引用。
/// @pre 所有引用来自同一谱面目录，混入另一谱面的相对路径会使用错误基准。
std::vector<const AudioResource*>
ProjectResourceService::resolveAudioResourceReferences(
    const Project& project, const std::filesystem::path& beatmapPath,
    const std::vector<std::string_view>& audioReferences)
{
    // 索引与缓存均局限于本次调用，不跨项目资源列表变化继续使用观察指针。
    const AudioResourceResolutionIndex resolutionIndex(project);
    const auto                         beatmapDirectory =
        resolutionIndex.beatmapDirectoryKey(beatmapPath);

    std::vector<const AudioResource*> result;
    result.reserve(audioReferences.size());
    // 容量按输入数量预留，重复引用仍保留输出槽，方便调用方按原索引回填。
    /// @brief 同一谱面内按引用内容缓存的首次解析结果。
    std::unordered_map<std::string, const AudioResource*> resolvedByReference;
    resolvedByReference.reserve(audioReferences.size());
    for ( const auto audioReference : audioReferences ) {
        const std::string referenceKey(audioReference);
        // 缓存键持有文本，不将调用方 string_view 的寿命传播到内部容器之外。
        // 相同文本也缓存解析失败，既减少重复路径处理，又保持输出和输入逐项对应。
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
/// @note 返回非拥有指针，仅用于选择候选；不验证解码能力或开始音频播放。
/// @details 优先级为提示、最早 Main 采样、最早任意采样、项目 Main、项目首资源。
/// 不写回歌曲提示，也不修改所选音轨的类型或配置。
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
        // 已解析歌曲提示优先于采样类型推断，不要求提示资源本身标记为 Main。
        return hintedResource;
    }

    // 同时记录最早 Main 和最早任意采样，优先级不受两者实际时间先后影响。
    const AudioResource* firstMainSampleResource = nullptr;
    const AudioResource* firstSampleResource     = nullptr;
    double firstMainTimestamp = std::numeric_limits<double>::infinity();
    // Main 和任意资源各有独立时间基准，不能共用一个最早时刻压掉 Main 候选。
    double firstTimestamp = std::numeric_limits<double>::infinity();
    // 严格小于使相同有效时刻保留容器首次出现项，不另按资源名打破平局。
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
    // 只要存在 Main 候选，就不让更早的 Effect 抢占预览的默认歌曲。
    if ( firstSampleResource ) return firstSampleResource;
    // 两种采样候选均为空后才使用项目资源顺序，不修改谱面本身的歌曲提示。

    // 谱面没有可解析引用时才回退到项目级默认资源，供预览或测量仍可选到音频。
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
/// @warning 低频兼容迁移会逐谱面加载、保存并重新扫描资源引用。
/// @note 各谱面独立处理，不因后一谱面失败撤销前面已经保存的迁移。
/// @pre 当前项目已完成扫描与配置合并，持久化项目保留旧 m_audioTrackId 信息。
/// 不删除旧项目中的字段；描述文件升级与最终项目保存由调用方负责。
/// 已有自动采样及非 MMM 条目被跳过，不计入成功数或失败列表。
ProjectResourceService::LegacyAudioMigrationResult
ProjectResourceService::migrateLegacyBeatmapAudioTracks(
    Project& project, const Project& persistedProject) const
{
    LegacyAudioMigrationResult result;
    // 以旧入口识别迁移需求，以当前入口确认仍被项目收录，不复活已排除的谱面。
    for ( const auto& persistedEntry : persistedProject.m_beatmaps ) {
        if ( persistedEntry.m_audioTrackId.empty() ) continue;

        // 项目入口按规范化路径匹配，不按可能重复的显示名选择迁移对象。
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
            // 外部格式不在本迁移的写回范围，跳过不计作失败谱面。
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
        // 已有自动采样说明谱面具备当前表示，不再把旧单主音轨重复物化。
        if ( !beatMap.m_audioSamples.empty() ) continue;
        // 这里按是否存在任何采样判断，无需再验证它是否对应旧单主音轨。

        // 旧字段可能是 ID
        // 或历史路径，用统一解析器定位当前资源，不直接构造文件名。
        const auto* resource =
            findAudioResourceForReference(project,
                                          currentEntryIterator->m_filePath,
                                          persistedEntry.m_audioTrackId);
        if ( !resource ) {
            // 缺失资源时不生成悬空采样；失败列表保留谱面路径供上层提示。
            XWARN(
                "Cannot migrate legacy m_audioTrackId '{}' for beatmap '{}': "
                "audio resource was not found",
                persistedEntry.m_audioTrackId,
                currentEntryIterator->m_filePath);
            result.m_failedBeatmapPaths.push_back(
                currentEntryIterator->m_filePath);
            continue;
        }

        // 旧单主音轨从谱面起点播放，将其物化为零时刻、无额外偏移的自动采样。
        // 自动采样放在玩家轨道之后，至少保留一条 BGM 轨承载这个事件。
        AudioSampleEvent sample;
        sample.m_timestamp = 0.0;
        sample.m_offsetMs  = 0;
        sample.m_track     = static_cast<std::uint32_t>(
            std::max(0, beatMap.m_baseMapMetadata.track_count));
        sample.m_audioResourceId = resource->m_id;
        // 事件使用稳定 ID，旧入口存储的路径写法不继续传播到新自动采样。
        beatMap.m_audioSamples.push_back(std::move(sample));
        beatMap.m_baseMapMetadata.bgm_track_count =
            std::max(1, beatMap.m_baseMapMetadata.bgm_track_count);
        // 现有歌曲提示可能包含用户选择，迁移只补空值，不强行替换。
        if ( beatMap.m_baseMapMetadata.song_file_hint.empty() ) {
            beatMap.m_baseMapMetadata.song_file_hint =
                Config::utf8ToPath(resource->m_path);
        }

        if ( !beatMap.saveToFile(mapPath) ) {
            // 临时模型没有发布给会话，保存失败不会把这次采样插入会话注册表。
            result.m_failedBeatmapPaths.push_back(
                currentEntryIterator->m_filePath);
            continue;
        }

        // 写回成功后再检查全项目引用，只有没有玩家物件绑定时才可升级资源为
        // Main。
        const auto references = findAudioResourceReferences(project, *resource);
        // 使用磁盘引用决定类型，不在迁移中读取未同步的编辑会话。
        const bool boundToNote =
            std::any_of(references.begin(),
                        references.end(),
                        [](const BeatmapAudioReference& reference) {
                            return reference.m_kind ==
                                   BeatmapAudioReferenceKind::NoteSampleBinding;
                        });
        if ( !boundToNote ) {
            // 保持既有音量等配置，只纠正资源的用途类型。
            // 查回同一资源对象再改类型，不在迁移中重排或追加资源列表。
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

        // 成功数量以落盘为界，不能把仅在临时 BeatMap 中创建的事件算作已迁移。
        result.m_migratedBeatmapCount++;
        // 类型是否已为 Main 不影响迁移计数，持久化采样才是此入口的完成单位。
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
/// @note
/// 这里只验证外部格式的音频关联，不替代目标冲突、权限或移动操作本身的检查。
/// 校验期间不写谱面，不能将通过校验视为已经更新磁盘引用。
/// @warning 校验会探测文件并读取 osu! 文本，仅用于显式文件移动命令。
/// @pre 项目路径描述移动前的布局；调用方在物理移动之前执行此入口。
/// @note 只返回首个阻止原因，不收集整项目的格式问题清单。
std::string ProjectResourceService::validateAudioResourceMove(
    const Project& project, const std::filesystem::path& oldPath,
    const std::filesystem::path& newPath)
{
    if ( project.m_projectRoot.empty() || oldPath.empty() || newPath.empty() ) {
        // 缺少上下文时此层不出具阻止原因，空返回值不能代替调用方的参数检查。
        return {};
    }

    const auto absoluteOldPath = weaklyCanonicalAbsolutePath(oldPath);
    const auto absoluteNewPath = weaklyCanonicalAbsolutePath(newPath);
    // 变化项用于跳过无关谱面，完整投影则覆盖“谱面移动但音频不动”的相对路径变化。
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
            // 不尝试重写隐式选歌字段：该格式靠文件布局决定歌曲，并无路径字段可改。
            // 隐式选歌还可能受未登记文件影响，因此不以资源变更列表为空提前跳过。
            const auto audioBeforeMove =
                resolveCurrentImdAudio(mapPathBeforeMove);
            // 期望仍选中原歌曲随移动后的对应文件；只比较新位置存在某首歌并不够。
            const auto expectedAudioAfterMove =
                audioBeforeMove
                    ? std::optional<
                          std::filesystem::path>{ remapAbsolutePathForMove(
                          *audioBeforeMove, absoluteOldPath, absoluteNewPath) }
                    : std::nullopt;
            // 原先没有隐式音频时，移动后意外找到一首也属于关联改变。
            // 按加载器命名规则重新选择一次，检测扩展名优先级或同名前缀引发的换歌。
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
            // osu! 的相对引用只有音频或谱面位置变化才需要重写预演。
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

        // 预演与提交复用同一重写器；这里只关心能否表达，不提交生成的文本。
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
        // changed 仅表示预演文本不同；未变化与可改写都允许继续检查下一谱面。
    }
    return {};
}

/// @brief 文件移动或重命名后重映射项目音频资源路径并保持资源 ID 稳定。
/// @param project 待更新项目。
/// @param oldPath 移动前的文件或目录路径。
/// @param newPath 移动后的文件或目录路径。
/// @param errorMessage 失败时接收面向用户的错误和回滚状态。
/// @return 路径发生变化的音频资源数量。
/// @pre 物理移动已完成，但 project 仍保存移动前的资源路径和谱面路径。
/// @note 零返回值也可能表示只移动谱面而未移动音频，需结合错误信息判断失败。
/// @details osu! 引用先暂存并批量提交，再更新资源路径；MMM 引用随后逐谱面写回。
/// MMM 写回失败只告警，不回滚已完成的资源路径更新。
/// @warning 低频移动收尾入口执行同步文件读写，不能在逐帧目录状态查询中调用。
/// @note 返回数量来自移动投影，不是成功写回谱面或已加载音频的数量。
/// errorMessage 可空；省略时失败及恢复状态只能通过日志观察。
/// 此入口不会更新项目谱面入口路径，调用方须协调其与物理移动的先后关系。
std::size_t ProjectResourceService::remapAudioResourcePathsAfterMove(
    Project& project, const std::filesystem::path& oldPath,
    const std::filesystem::path& newPath, std::string* errorMessage)
{
    if ( errorMessage ) errorMessage->clear();
    // 清除调用方上次操作的错误，零变化成功不应携带旧失败信息。
    if ( project.m_projectRoot.empty() || oldPath.empty() || newPath.empty() ) {
        return 0;
    }

    const auto absoluteOldPath = weaklyCanonicalAbsolutePath(oldPath);
    const auto absoluteNewPath = weaklyCanonicalAbsolutePath(newPath);
    const auto remappedResources =
        collectResourcePathRemaps(project, absoluteOldPath, absoluteNewPath);
    const auto resourceProjections = collectResourcePathProjections(
        project, absoluteOldPath, absoluteNewPath);

    // 全量投影仍保留未移动资源，谱面本身改目录时它们的相对引用也可能改变。
    // 到资源列表更新前，失败仍可按旧项目信息尝试把物理文件移回原处。
    /// @brief 向 UI 报告失败并恢复已经发生的物理移动。
    const auto failAndRollbackMove = [&](std::string message) {
        // 文件移动恢复与谱面文本恢复是两个步骤，此闭包只负责前者。
        const bool rolledBack =
            rollbackFilesystemMove(absoluteNewPath, absoluteOldPath);
        message += rolledBack ? "；文件移动已回滚"
                              : "；自动回滚失败，请立即检查源路径和目标路径";
        XERROR("{}", message);
        if ( errorMessage ) *errorMessage = std::move(message);
        // 失败返回不表示“没有移动资源”，调用方必须同时处理错误信息。
        return std::size_t{ 0U };
    };

    // 先准备所有 osu!
    // 改写，源文本此时位于移动后的路径，引用解析仍按移动前目录。
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
            // 先清理本批未提交文本，再尝试移动回退，避免遗留暂存文件被一起搬回。
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

        // 只有实际改写才创建旁路文件，未改动的谱面无需参与批量替换。
        PendingTextFileReplacement replacement;
        if ( !stageTextFileReplacement(
                 mapPathAfterMove, rewritten, replacement) ) {
            cleanupTextFileReplacements(pendingOsuReplacements);
            return failAndRollbackMove("写入 osu! 谱面 '" + entry.m_filePath +
                                       "' 的音频引用临时文件失败");
        }
        pendingOsuReplacements.push_back(std::move(replacement));
    }
    // 先落盘依赖路径的外部格式，成功之后才交接内存资源路径。
    if ( !commitTextFileReplacements(pendingOsuReplacements) ) {
        // 文本事务已尝试恢复其前缀，随后再尝试恢复物理位置；都依赖文件系统可用。
        return failAndRollbackMove(
            "提交 osu! 谱面音频引用事务失败，原谱面内容已恢复");
    }
    // 即使没有音频资源移动，前面的谱面相对路径更新仍可能已完成，不能提前跳过。
    if ( remappedResources.empty() ) return 0U;
    // 后续 MMM 兼容引用写回仅在音频资源发生移动时执行，不处理单独的谱面移动。

    for ( const auto& remap : remappedResources ) {
        // 投影持有旧资源值，不因这里覆盖项目路径而丢失后续引用匹配的旧位置。
        const auto resource =
            std::find_if(project.m_audioResources.begin(),
                         project.m_audioResources.end(),
                         [&](const AudioResource& candidate) {
                             // 用旧 ID
                             // 和旧路径共同定位，不只凭可能重名的文件名身份匹配。
                             return candidate.m_id == remap.m_before.m_id &&
                                    candidate.m_path == remap.m_before.m_path;
                         });
        if ( resource != project.m_audioResources.end() ) {
            // 只换位置，ID、类型及音量等用户配置全部沿用。
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
        // MMM 绑定优先使用稳定
        // ID；此轮补写仍以路径形式保存的兼容引用和歌曲提示。
        if ( extension != ".mmm" ) continue;

        // 正式文件已经位于新位置；匹配旧引用仍使用旧项目入口与旧资源快照。
        auto beatMap = BeatMap::loadFromFile(mapPathAfterMove);
        if ( beatMap.m_baseMapMetadata.map_path.empty() ) continue;
        // 一份谱面可能引用多条移动资源，全部重映射后只保存一次。
        bool changedBeatmap = false;

        // 按资源逐项累计变化，后面未匹配到引用不能清掉前面已经产生的修改标记。
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
            // 此时资源表和 osu!
            // 已提交；这里只告警，不能再调用前阶段的移动回退。
            XWARN("Failed to update moved audio references in beatmap: {}",
                  entry.m_filePath);
        }
    }
    // 计数来自预先收集的投影，不能作为 MMM 文件全部写回成功的证明。
    return remappedResources.size();
}

/// @brief 创建默认音轨配置。
/// @return 默认音轨配置。
AudioTrackConfig ProjectResourceService::makeDefaultAudioConfig()
{
    // 这是新资源的项目级起点，后续持久化合并可以覆盖，不直接读取播放器当前状态。
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
/// @note 只构造值对象，不复制源文件、不检查解码能力，也不登记播放器资源。
AudioResource ProjectResourceService::createAudioResource(
    const std::filesystem::path& audioPath,
    const std::string&           relativeAudioPath)
{
    /// @brief 音频文件名，用作资源 ID。
    auto filename = Config::pathToUtf8(audioPath.filename());
    // ID 沿用完整文件名含扩展名，所在子目录通过独立路径字段保存。

    /// @brief 新建的项目音频资源条目。
    AudioResource resource;
    resource.m_id   = filename;
    resource.m_path = relativeAudioPath;
    // 未解析谱面引用前先设为音效，主音轨身份由引用推断或持久化恢复决定。
    resource.m_type   = AudioTrackType::Effect;
    resource.m_config = makeDefaultAudioConfig();
    return resource;
}

}  // namespace MMM::Logic
