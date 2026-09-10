#include "logic/ProjectCommandService.h"
#include "config/AppConfig.h"
#include "config/CreatorIdentity.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "logic/ProjectResourceService.h"
#include "mmm/beatmap/BeatMap.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <limits>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace MMM::Logic
{
namespace
{
/// @brief 判断相对路径是否位于项目根内。
/// @param path 待检查的相对路径。
/// @return 路径没有越出根目录时返回 true。
/// @note 这里只判断词法层级，不查询符号链接；磁盘删除前仍需解析实际位置。
bool isRelativePathInsideRoot(const std::filesystem::path& path)
{
    if ( path.empty() || path.is_absolute() ) return false;

    // 先折叠 a/../b 这种合法根内写法，再拒绝仍以 .. 开头的结果。
    // 纯根路径没有资源名，不能作为可登记文件的相对路径返回。
    const auto normalized = path.lexically_normal();
    // 折叠内部的目录抵消后，只需检查首个有效片段是否仍为上级目录。
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
/// @note 回退结果不保证文件存在，也不保证已经消除了符号链接。
std::filesystem::path weaklyCanonicalAbsolutePath(
    const std::filesystem::path& path)
{
    std::error_code filesystemError;
    // weakly_canonical 允许末尾尚不存在，适合新建谱面的候选文件路径。
    auto normalized = std::filesystem::weakly_canonical(path, filesystemError);
    if ( !filesystemError ) return normalized.lexically_normal();

    // 无法查询完整文件系统关系时至少固定工作目录基准，不直接丢弃输入路径。
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

    // 兼容重复项目目录名前缀；按片段比较，不误删名称相似的其他目录。
    auto iterator = path.begin();
    if ( iterator == path.end() || *iterator != projectRoot.filename() ) {
        return {};
    }

    std::filesystem::path stripped;
    // 仅消耗一个项目名片段；后续同名目录仍是资源实际层级的一部分。
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
/// @details 相对输入兼容项目名前缀、根相对路径和工作目录相对路径。
/// 空返回值表达无法得到根内路径，不等同于文件不存在。
std::filesystem::path makeRelativeToProjectRoot(
    const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    if ( projectRoot.empty() || path.empty() ) return {};

    const auto root = weaklyCanonicalAbsolutePath(projectRoot);
    if ( path.is_relative() ) {
        const auto stripped = stripProjectFolderPrefix(root, path);
        // 旧配置的重复前缀按结构恢复，不要求资源此刻已经落盘。
        // 该分支优先于直接根相对解释，与访问路径解析器的存在性优先策略不同。
        if ( isRelativePathInsideRoot(stripped) ) {
            return stripped.lexically_normal();
        }

        // 根内已有的相对候选优先，找不到才尝试按进程工作目录解释输入。
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

    const auto absolutePath = weaklyCanonicalAbsolutePath(path);
    // 文件系统相对化可以产生上级目录片段；转换成功本身不代表仍在项目内。
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
        // 未能定位的旧配置路径仍保留词法形式，以便排除列表及资源去重继续比较。
        relativePath = Config::utf8ToPath(path).lexically_normal();
    }
    return Config::pathToUtf8(relativePath.lexically_normal());
}

/// @brief 规范化草稿组使用的跨平台谱面路径键。
/// @param pathText 项目相对谱面路径。
/// @return 固定使用正斜杠且折叠点目录的 UTF-8 路径。
/// @note 草稿键已经是项目相对路径，不再次访问文件系统解析项目根。
/// @pre 调用方负责先把绝对路径转换为项目相对路径。
/// @details Windows 历史反斜杠与当前平台分隔符最终得到同一个持久键。
std::string normalizeDraftBeatmapPath(std::string pathText)
{
    if ( pathText.empty() ) return {};
    // 项目可能在 Windows 与类 Unix 平台间移动，比较前统一历史分隔符。
    std::replace(pathText.begin(), pathText.end(), '\\', '/');
    return Config::pathToUtf8Generic(
        Config::utf8ToPath(pathText).lexically_normal());
}

/// @brief 按资源 ID、项目相对路径或旧版文件名查找项目音频资源。
/// @param project 当前项目。
/// @param audioReference 谱面中保存的资源 ID 或路径。
/// @return 匹配到的项目音频资源；未匹配时返回空。
/// @note 返回项目列表中的非拥有指针；列表修改后不可继续使用。
/// @details 三种匹配方式在同一次遍历中并列判断，不是先全局查 ID 再查路径。
/// 同名资源的歧义由资源登记约束处理，此处不检查音频内容或解码能力。
const AudioResource* findAudioResourceForReference(
    const Project& project, const std::filesystem::path& audioReference)
{
    if ( audioReference.empty() ) return nullptr;

    const auto referenceText = Config::pathToUtf8(audioReference);
    // 保留原文作为 ID 候选，不能只比较规范化路径而破坏旧资源 ID 的匹配。
    const auto normalizedReference =
        normalizeStoredProjectPath(project, referenceText);
    const auto filename = Config::pathToUtf8(audioReference.filename());
    // 按项目列表首次命中选择，兼容稳定 ID、旧 basename 和路径三种写法。
    for ( const auto& resource : project.m_audioResources ) {
        if ( resource.m_id == referenceText || resource.m_id == filename ||
             normalizeStoredProjectPath(project, resource.m_path) ==
                 normalizedReference ) {
            return &resource;
        }
    }
    return nullptr;
}

/// @brief 从详细引用中提取去重后的谱面路径。
/// @param references 待汇总的音频引用。
/// @param acceptedKind 需要保留的引用类型判断器。
/// @return 保持首次出现顺序的谱面路径列表。
/// @note 路径按字符串相等去重；资源引用类型仍由调用方选择，不能统一阻止。
template<typename Predicate>
std::vector<std::string> collectBlockingBeatmapPaths(
    const std::vector<BeatmapAudioReference>& references,
    Predicate                                 acceptedKind)
{
    // 面向阻止原因展示按谱面去重，不把同谱面的每一个绑定都重复列给用户。
    std::vector<std::string> result;
    for ( const auto& reference : references ) {
        // 先应用用途过滤，再做谱面去重，忽略的歌曲提示不能抢占有效绑定条目。
        if ( !acceptedKind(reference.m_kind) ||
             std::find(result.begin(), result.end(), reference.m_beatmapPath) !=
                 result.end() ) {
            continue;
        }
        result.push_back(reference.m_beatmapPath);
    }
    return result;
}

/// @brief 将打开会话中的匹配引用合并进磁盘扫描结果。
/// @param project 当前项目。
/// @param resource 待检查的音频资源。
/// @param openBeatmapReferences 已同步的打开会话内存谱面引用。
/// @param matchingReferences 接收匹配引用的结果列表。
/// @pre 调用方已将会话编辑同步到引用快照，本函数不主动读取会话注册表。
void appendMatchingOpenBeatmapReferences(
    const Project& project, const AudioResource& resource,
    const std::vector<BeatmapAudioReference>& openBeatmapReferences,
    std::vector<BeatmapAudioReference>&       matchingReferences)
{
    // 内存引用补充未保存编辑；此层不删除磁盘引用，仍保留两份状态中的匹配项。
    for ( const auto& reference : openBeatmapReferences ) {
        if ( ProjectResourceService::audioReferenceMatchesResource(
                 project, reference, resource) ) {
            matchingReferences.push_back(reference);
        }
    }
}

/// @brief 收集模板谱面中被 Polyline 引用的全部子物件。
/// @param source 模板谱面。
/// @return 子物件的稳定地址集合。
/// @note 地址只用于本次模板读取期的身份判定，不作为目标谱面的持久引用。
/// @pre 模板折线中的引用有效，收集期间所属物件容器不会被清空或替换。
std::unordered_set<const Note*> collectPolylineSubNotes(const BeatMap& source)
{
    std::unordered_set<const Note*> subNotes;
    // 地址集合仅表示成员身份，不决定目标顺序；复制阶段仍遍历模板连接列表。
    for ( const auto& polyline : source.m_noteData.polylines ) {
        for ( const auto& subNoteReference : polyline.m_subNotes ) {
            subNotes.insert(&subNoteReference.get());
        }
    }
    return subNotes;
}

/// @brief 清空目标谱面的物件数据并复制真正的非折线物件。
/// @param target 接收复制结果的新谱面。
/// @param source 模板谱面。
/// @pre 源和目标是不同谱面；清空目标容器会使其中原有对象引用失效。
void copyStandaloneNotes(BeatMap& target, const BeatMap& source)
{
    // 独立物件与折线子物件分两阶段复制，避免子物件被当成普通物件重复创建。
    target.m_noteData.notes.clear();
    target.m_noteData.holds.clear();
    target.m_noteData.flicks.clear();
    target.m_noteData.polylines.clear();

    /// @brief 某些旧格式加载器没有写入 m_isSubNote，实际引用关系才是权威来源。
    const auto polylineSubNotes = collectPolylineSubNotes(source);
    for ( const auto& note : source.m_noteData.notes ) {
        if ( note.m_isSubNote || polylineSubNotes.contains(&note) ) continue;
        target.m_noteData.notes.push_back(note);
    }
    for ( const auto& hold : source.m_noteData.holds ) {
        if ( hold.m_isSubNote || polylineSubNotes.contains(&hold) ) continue;
        target.m_noteData.holds.push_back(hold);
    }
    for ( const auto& flick : source.m_noteData.flicks ) {
        if ( flick.m_isSubNote || polylineSubNotes.contains(&flick) ) continue;
        target.m_noteData.flicks.push_back(flick);
    }
}

/// @brief 将一个折线子物件复制到目标谱面并重新绑定到目标折线。
/// @param target 接收子物件的新谱面。
/// @param targetPolyline 正在构建的目标折线。
/// @param sourceSubNote 模板折线中的子物件。
/// @pre sourceSubNote 的类型标记与实际动态类型一致；target
/// 的容器负责持有复制品。
/// @note 子物件按值保留 ID、绑定及元数据；这里只修复所有权和子物件标志。
void copyPolylineSubNote(BeatMap& target, Polyline& targetPolyline,
                         const Note& sourceSubNote)
{
    // 拷贝到目标所有者容器后再取得引用，不能把模板中的 reference_wrapper
    // 原样带过去。
    if ( sourceSubNote.m_type == ::MMM::NoteType::HOLD ) {
        const auto& sourceHold = static_cast<const Hold&>(sourceSubNote);
        Hold        copiedHold = sourceHold;
        copiedHold.m_isSubNote = true;
        target.m_noteData.holds.push_back(std::move(copiedHold));
        auto& copiedRef = target.m_noteData.holds.back();
        // 持有容器负责对象生命周期，折线内两份引用不拥有也不再复制该对象。
        targetPolyline.m_subNotes.push_back(copiedRef);
        // 通用子物件顺序和类型专用列表都必须指向同一个目标对象。
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
        // 专用列表只收录 Flick，通用列表则保留它与 Hold/Note 的相对连接次序。
        return;
    }

    // 非 Hold/Flick 分支只复制基类 Note；模板节点的类型集合由加载流程约束。
    // 不把未知派生对象的额外状态假定为可完整复制。
    Note copiedNote        = sourceSubNote;
    copiedNote.m_isSubNote = true;
    target.m_noteData.notes.push_back(std::move(copiedNote));
    auto& copiedRef = target.m_noteData.notes.back();
    targetPolyline.m_subNotes.push_back(copiedRef);
}

/// @brief 复制模板谱面的全部物件，并修复折线对子物件的引用。
/// @param target 接收复制结果的新谱面。
/// @param source 模板谱面。
/// @pre source 在复制期间保持稳定，且不与 target 别名。
/// @note 折线节点按连接顺序重建，不能按时间排序后再恢复连接。
/// @warning 全量复制会分配容器并同步派生索引，仅用于低频新建流程。
void copyTemplateNotes(BeatMap& target, const BeatMap& source)
{
    copyStandaloneNotes(target, source);

    for ( const auto& sourcePolyline : source.m_noteData.polylines ) {
        Polyline copiedPolyline = sourcePolyline;
        // 保留折线自身属性，但清空浅拷贝带来的源对象引用，再按源顺序重建关联。
        copiedPolyline.m_subNotes.clear();
        copiedPolyline.m_subHolds.clear();
        copiedPolyline.m_subFlicks.clear();

        for ( const auto& sourceSubNoteRef : sourcePolyline.m_subNotes ) {
            copyPolylineSubNote(target, copiedPolyline, sourceSubNoteRef.get());
        }

        target.m_noteData.polylines.push_back(std::move(copiedPolyline));
        // 移动父对象不会把子物件挪出目标容器，刚建立的引用仍指向目标所有者。
    }

    // 所有对象及关联完整后统一刷新派生状态，不在逐个子物件复制时同步。
    target.sync();
}

/// @brief 判断玩家物件的起始列是否位于目标主轨道区。
/// @param note 待验证物件。
/// @param targetPlayerTrackCount 目标玩家轨道数。
/// @return 起始列可原样复制时返回 true。
/// @note 本检查不覆盖 Flick 跨列终点；调用方必须使用其专用校验。
bool isTemplateNoteTrackValid(const Note&  note,
                              std::int32_t targetPlayerTrackCount)
{
    // 比较零基起始列与轨道数量，上界必须严格小于，不能把首条 BGM 列当玩家列。
    return targetPlayerTrackCount > 0 &&
           static_cast<std::uint64_t>(note.m_track) <
               static_cast<std::uint64_t>(targetPlayerTrackCount);
}

/// @brief 判断 Flick 的起始列和终止列是否均位于目标主轨道区。
/// @param flick 待验证 Flick。
/// @param targetPlayerTrackCount 目标玩家轨道数。
/// @return Flick 可保持列和方向原样复制时返回 true。
bool isTemplateFlickTrackValid(const Flick& flick,
                               std::int32_t targetPlayerTrackCount)
{
    if ( !isTemplateNoteTrackValid(flick, targetPlayerTrackCount) ) {
        return false;
    }

    // 方向偏移可为负，用宽有符号整数计算终点，避免无符号相加回绕。
    const auto endTrack = static_cast<std::int64_t>(flick.m_track) +
                          static_cast<std::int64_t>(flick.m_dtrack);
    return endTrack >= 0 && endTrack < targetPlayerTrackCount;
}

/// @brief 验证模板全部玩家物件均可保持原列复制到目标主轨道区。
/// @param source 模板谱面。
/// @param targetPlayerTrackCount 目标玩家轨道数。
/// @param errorMessage 接收失败原因。
/// @return 普通 Note、Hold、Flick、Polyline 及全部子物件均合法时返回 true。
/// @note 首个失败类型即写入错误信息；这里只验证列范围，不修正时间和时长。
bool validateTemplatePlayerObjectTracks(const BeatMap& source,
                                        std::int32_t   targetPlayerTrackCount,
                                        std::string&   errorMessage)
{
    // 缩减轨道不自动裁剪或压缩物件列；任一物件无法原样容纳即拒绝整次模板应用。
    for ( const auto& note : source.m_noteData.notes ) {
        if ( isTemplateNoteTrackValid(note, targetPlayerTrackCount) ) {
            continue;
        }
        errorMessage = "模板 Note 超出新谱面的玩家轨道范围";
        return false;
    }
    for ( const auto& hold : source.m_noteData.holds ) {
        if ( isTemplateNoteTrackValid(hold, targetPlayerTrackCount) ) {
            continue;
        }
        errorMessage = "模板 Hold 超出新谱面的玩家轨道范围";
        return false;
    }
    for ( const auto& flick : source.m_noteData.flicks ) {
        if ( isTemplateFlickTrackValid(flick, targetPlayerTrackCount) ) {
            continue;
        }
        errorMessage = "模板 Flick 起止列超出新谱面的玩家轨道范围";
        return false;
    }
    // 折线头合法并不代表全部节点合法，子 Flick 还需验证终止列。
    for ( const auto& polyline : source.m_noteData.polylines ) {
        if ( isTemplateNoteTrackValid(polyline, targetPlayerTrackCount) ) {
            for ( const auto& subNoteReference : polyline.m_subNotes ) {
                const auto& subNote = subNoteReference.get();
                const bool  valid   = subNote.m_type == NoteType::FLICK
                                          ? isTemplateFlickTrackValid(
                                                static_cast<const Flick&>(subNote),
                                                targetPlayerTrackCount)
                                          : isTemplateNoteTrackValid(
                                                subNote, targetPlayerTrackCount);
                if ( valid ) continue;

                errorMessage = "模板 Polyline 子物件超出新谱面的玩家轨道范围";
                return false;
            }
            continue;
        }
        errorMessage = "模板 Polyline 超出新谱面的玩家轨道范围";
        return false;
    }
    errorMessage.clear();
    return true;
}

/// @brief 原子构造模板自动采样在新玩家轨道数下的绝对轨道。
/// @param source 模板谱面。
/// @param targetPlayerTrackCount 新谱面的玩家轨道数。
/// @param targetBgmTrackCount 新谱面原有的 BGM 轨道数。
/// @param remappedSamples 接收完整验证后的自动采样列表。
/// @param remappedBgmTrackCount 接收保留并扩展后的 BGM 轨道数。
/// @param errorMessage 接收失败原因。
/// @return 全部自动采样均可保持 BGM 相对轨道时返回 true。
/// @note
/// 失败不改输出采样列表和轨道数；这里的原子性指结果一次性交付，而非原子指令。
/// @details 轨道重映射不改变资源 ID、采样时间、偏移和音量。
/// 空采样列表仍保留显式 BGM 轨道数，不能从物件数量反推轨道布局。
bool remapTemplateAudioSamples(const BeatMap& source,
                               std::int32_t   targetPlayerTrackCount,
                               std::int32_t   targetBgmTrackCount,
                               std::deque<AudioSampleEvent>& remappedSamples,
                               std::int32_t& remappedBgmTrackCount,
                               std::string&  errorMessage)
{
    /// @brief 模板声明的玩家轨道数。
    const auto sourcePlayerTrackCount = source.m_baseMapMetadata.track_count;
    // 只有实际采样需要计算玩家区边界；空列表不会进入后续无符号转换。
    if ( !source.m_audioSamples.empty() &&
         (sourcePlayerTrackCount < 0 || targetPlayerTrackCount < 0) ) {
        errorMessage = "模板或新谱面的玩家轨道数无效，无法重映射自动采样";
        return false;
    }

    /// @brief 完整验证成功前不暴露给调用方的候选采样列表。
    std::deque<AudioSampleEvent> candidateSamples;

    /// @brief 同时保留模板和目标显式创建的空 BGM 轨道。
    std::int32_t candidateBgmTrackCount = std::max(
        { 0, targetBgmTrackCount, source.m_baseMapMetadata.bgm_track_count });
    for ( const auto& sourceSample : source.m_audioSamples ) {
        const auto sourcePlayerTrackCountUnsigned =
            static_cast<std::uint32_t>(sourcePlayerTrackCount);
        if ( sourceSample.m_track < sourcePlayerTrackCountUnsigned ) {
            // 无法得到非负的 BGM 相对列，不将非法采样自动钳到首条 BGM 轨。
            errorMessage = "模板自动采样落入玩家轨道区，无法复制到新谱面";
            return false;
        }

        /// @brief 自动采样相对于模板首条 BGM 轨道的零基索引。
        // 保留相对首条 BGM 轨的偏移，而不是直接复制受玩家轨道数影响的绝对列。
        const auto relativeBgmTrack =
            static_cast<std::uint64_t>(sourceSample.m_track) -
            sourcePlayerTrackCountUnsigned;
        /// @brief 新谱面中的绝对轨道，使用宽整数防止无符号回绕。
        const auto mappedTrack =
            static_cast<std::uint64_t>(targetPlayerTrackCount) +
            relativeBgmTrack;
        /// @brief 容纳当前相对轨道所需的最小 BGM 轨道数。
        const auto requiredBgmTrackCount = relativeBgmTrack + 1U;
        // 轨道数是数量而非末尾索引，因此最右采样列对应的需求需要加一。
        // 物件列和 BGM 轨道数使用不同整数类型，两种容量都要验证后才能收窄。
        if ( mappedTrack > std::numeric_limits<std::uint32_t>::max() ||
             requiredBgmTrackCount >
                 static_cast<std::uint64_t>(
                     std::numeric_limits<std::int32_t>::max()) ) {
            errorMessage = "模板自动采样轨道重映射溢出，无法复制到新谱面";
            return false;
        }

        // 以整项复制保留非轨道字段，玩家轨道数变化不应挪动音频的播放时刻。
        auto remappedSample    = sourceSample;
        remappedSample.m_track = static_cast<std::uint32_t>(mappedTrack);
        candidateSamples.push_back(std::move(remappedSample));
        // 只扩展布局，不因当前采样集中在左侧而缩掉显式保留的空轨道。
        candidateBgmTrackCount =
            std::max(candidateBgmTrackCount,
                     static_cast<std::int32_t>(requiredBgmTrackCount));
    }

    // 所有采样通过后再交付，后部非法采样不会留下前部已经重映射的半成品。
    remappedSamples       = std::move(candidateSamples);
    remappedBgmTrackCount = candidateBgmTrackCount;
    errorMessage.clear();
    return true;
}

/// @brief 按用户选项将模板谱面内容应用到新谱面。
/// @param target 正在创建的新谱面。
/// @param source 模板谱面。
/// @param options 模板复制选项。
/// @param errorMessage 接收模板内容无法无损复制时的失败原因。
/// @return 所选模板内容完整应用时返回 true。
/// @details copyObjects 同时控制玩家物件和自动采样；Timing 有独立复制开关。
/// @pre 目标是独立的新谱面，基础元数据已经由创建命令初始化。
bool applyTemplateBeatmap(BeatMap& target, const BeatMap& source,
                          const BeatmapTemplateCreateOptions& options,
                          std::string&                        errorMessage)
{
    /// @brief 在修改目标谱面前完成整批自动采样重映射验证。
    std::deque<AudioSampleEvent> remappedSamples;
    /// @brief 自动采样重映射后需要持久化的 BGM 轨道数。
    std::int32_t remappedBgmTrackCount =
        target.m_baseMapMetadata.bgm_track_count;
    if ( options.copyObjects ) {
        // 验证阶段放在任何目标元数据写入之前，失败时目标仍是向导创建的初始状态。
        if ( !validateTemplatePlayerObjectTracks(
                 source, target.m_baseMapMetadata.track_count, errorMessage) ) {
            return false;
        }
        if ( !remapTemplateAudioSamples(
                 source,
                 target.m_baseMapMetadata.track_count,
                 target.m_baseMapMetadata.bgm_track_count,
                 remappedSamples,
                 remappedBgmTrackCount,
                 errorMessage) ) {
            return false;
        }
    }

    if ( options.copyMetadata ) {
        // 复制扩展元数据，不覆盖向导已选的基础标题、轨道数和资源路径。
        target.m_metadata = source.m_metadata;
    }
    if ( options.copyTimelines ) {
        // 模板时间点在这里完整复制，向导初始 BPM 的覆盖由后续独立步骤完成。
        target.m_timings = source.m_timings;
    }
    if ( options.copyObjects ) {
        copyTemplateNotes(target, source);
        // 自动采样直接使用预校验输出，不在玩家物件已复制后再次计算可能失败的映射。
        target.m_audioSamples                    = std::move(remappedSamples);
        target.m_baseMapMetadata.bgm_track_count = remappedBgmTrackCount;
    } else {
        // 未复制物件也要同步目标的派生数据，不能依赖物件复制分支代为完成。
        target.sync();
    }
    errorMessage.clear();
    return true;
}

/// @brief 按时间和类型稳定排序谱面 Timing。
/// @param timings 待排序的 Timing 列表。
/// @pre 时间戳满足排序比较要求；此步骤不承担非有限值清理。
void sortBeatmapTimings(std::vector<::MMM::Timing>& timings)
{
    // 同时刻按效果类型排列；相同时刻同类型保留输入顺序，不额外改变模板优先关系。
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
/// @note 初始列表按原值追加，不在此再次归一化 BPM 或过滤效果类型。
/// @pre initialTimings 不与目标 Timing 容器别名，清空目标不能影响输入列表。
void applyInitialBeatmapTimings(
    BeatMap& target, const std::vector<::MMM::Timing>& initialTimings,
    bool keepNonBpmTimings)
{
    if ( initialTimings.empty() ) {
        // 未提供向导时间点时保留模板，不能把空输入当成清空全部 Timing 的命令。
        return;
    }

    if ( keepNonBpmTimings ) {
        // 向导 BPM 替换模板 BPM，但仍保留用户选中的流速及特效时间点。
        std::erase_if(target.m_timings, [](const auto& timing) {
            return timing.m_timingEffect == ::MMM::TimingEffect::BPM;
        });
    } else {
        target.m_timings.clear();
    }

    // keepNonBpmTimings 只约束旧模板条目，初始列表中的非 BPM 项同样会追加。
    target.m_timings.insert(
        target.m_timings.end(), initialTimings.begin(), initialTimings.end());
    sortBeatmapTimings(target.m_timings);
}
}  // namespace

/// @brief 创建谱面文件并登记到项目资源列表。
/// @param project 当前打开的项目。
/// @param cmd 新建谱面命令。
/// @return 新建谱面的处理结果。
/// @details 先在内存组装并保存谱面，成功后才登记项目条目及缺失主音轨资源。
/// @note 返回谱面供调用方打开会话；此函数不负责保存整个项目配置。
/// @warning 低频创建入口包含磁盘查询、模板全量复制和同步保存，不可逐帧调用。
/// @pre 调用方负责项目可写性、命令参数及项目列表的串行访问。
ProjectCommandService::CreateBeatmapResult ProjectCommandService::createBeatmap(
    Project& project, const CmdCreateBeatmap& cmd) const
{
    /// @brief 本次新建谱面的返回结果。
    CreateBeatmapResult result;

    /// @brief 新谱面的基础元数据副本，后续会规范化路径后写入文件。
    auto meta = cmd.baseMeta;
    // 仅缺省作者使用全局身份，显式填写的作者不因个人配置改变而被重写。
    if ( meta.author.empty() ) {
        meta.author = Config::normalizeCreatorIdentity(
            Config::AppConfig::instance().getEditorSettings().defaultCreator);
    }
    XINFO("Creating new beatmap: {} (Title: {})", meta.name, meta.title);

    /// @brief 经过非法文件名字符替换后的谱面文件名主体。
    // 文件名与显示名称分离，净化只影响磁盘名称，不改谱面难度名的展示内容。
    std::string safeFilename = meta.name;
    std::replace_if(
        safeFilename.begin(),
        safeFilename.end(),
        [](char c) {
            return c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                   c == '"' || c == '<' || c == '>' || c == '|';
        },
        '_');
    // 不自动裁剪空白或截断 UTF-8 名称；这里的替换集合仅覆盖列出的保留字符。

    /// @brief 新谱面文件的候选保存路径。
    std::filesystem::path mapPath =
        project.m_projectRoot / Config::utf8ToPath(safeFilename + ".mmm");

    /// @brief 文件名冲突时递增追加的数字后缀。
    int             suffix = 1;
    std::error_code mapPathError;
    // 后缀只用于避让已存在文件，不占用目标路径；实际写入成败由保存结果决定。
    // 查询过程也不创建项目目录，目录准备属于项目创建流程。
    while ( std::filesystem::exists(mapPath, mapPathError) && !mapPathError ) {
        mapPath = project.m_projectRoot /
                  Config::utf8ToPath(safeFilename + "_" +
                                     std::to_string(suffix++) + ".mmm");
        mapPathError.clear();
    }

    meta.map_path = makeProjectRelativePath(project, mapPath);
    // 实际保存仍使用绝对候选 mapPath；元数据中的相对路径便于项目整体搬迁。

    if ( meta.song_file_hint.empty() ) {
        // 新字段优先；旧字段只在新字段未给出时参与迁移。
        meta.song_file_hint = meta.main_audio_path;
    }
    meta.song_file_hint = makeProjectRelativePath(project, meta.song_file_hint);
    // 新谱面使用当前歌曲提示字段，旧 main_audio_path 只作输入兼容，不能双写。
    meta.main_audio_path.clear();
    meta.main_cover_path =
        makeProjectRelativePath(project, meta.main_cover_path);
    // 封面与背景是独立字段，不能因二者当前引用相同文件而合并存储。
    meta.cover_path = makeProjectRelativePath(project, meta.cover_path);
    // 图片和视频的实际资源导入不在新建命令中重复执行。

    /// @brief 新谱面歌曲提示解析出的项目主音频资源 ID。
    std::string selectedMainResourceId;
    /// @brief 保存成功后才登记的缺失主音频资源。
    std::optional<AudioResource> pendingMainResource;
    // 已有资源只接受 Main 作为自动歌曲采样来源，不将 Note 使用的 Effect
    // 静默升级。
    if ( !meta.song_file_hint.empty() ) {
        if ( const auto* resource =
                 findAudioResourceForReference(project, meta.song_file_hint) ) {
            if ( resource->m_type == AudioTrackType::Main ) {
                selectedMainResourceId = resource->m_id;
            } else {
                XWARN(
                    "New beatmap song_file_hint '{}' resolves to Effect '{}'; "
                    "no automatic Main sample was created",
                    Config::pathToUtf8(meta.song_file_hint),
                    resource->m_id);
            }
        } else {
            // 未登记的提示先形成待提交资源，模板校验或谱面保存失败时不污染项目列表。
            // 这里只建立逻辑引用，不复制文件或验证音频可解码性。
            // 资源 ID 沿用文件名，路径则保留项目相对层级。
            AudioResource pendingResource;
            pendingResource.m_id =
                Config::pathToUtf8(meta.song_file_hint.filename());
            pendingResource.m_path   = Config::pathToUtf8(meta.song_file_hint);
            pendingResource.m_type   = AudioTrackType::Main;
            pendingResource.m_config = makeDefaultAudioConfig();
            selectedMainResourceId   = pendingResource.m_id;
            pendingMainResource      = std::move(pendingResource);
        }
    }

    /// @brief 新建并即将保存到磁盘的谱面实例。
    auto newBeatmap               = std::make_shared<MMM::BeatMap>();
    newBeatmap->m_baseMapMetadata = meta;
    if ( cmd.templateBeatmap ) {
        // 模板作为只读来源，不把新谱面的容器或引用反向写回模板会话。
        /// @brief 模板复制失败时用于日志说明且不会产生任何持久化副作用。
        std::string templateError;
        if ( !applyTemplateBeatmap(*newBeatmap,
                                   *cmd.templateBeatmap,
                                   cmd.templateOptions,
                                   templateError) ) {
            XERROR("Cannot create beatmap from template: {}", templateError);
            return result;
        }
    }
    // 向导初始 Timing 在模板复制后应用，保证用户本次设定的 BPM 生效。
    applyInitialBeatmapTimings(
        *newBeatmap,
        cmd.initialTimings,
        cmd.templateBeatmap && cmd.templateOptions.copyTimelines);

    if ( !selectedMainResourceId.empty() ) {
        // 默认歌曲落在首条 BGM 轨；至少声明一条轨道，即使起点采样已存在。
        auto& baseMeta           = newBeatmap->m_baseMapMetadata;
        baseMeta.bgm_track_count = std::max(1, baseMeta.bgm_track_count);
        const auto sampleTrack =
            static_cast<uint32_t>(std::max(0, baseMeta.track_count));
        // 自动采样使用玩家区之后的绝对列，不使用 BGM 局部索引零代替。
        // 模板可能已有完全相同的起点采样，只对资源、时间、偏移和轨道均一致的项去重。
        const bool alreadyMaterialized = std::any_of(
            newBeatmap->m_audioSamples.begin(),
            newBeatmap->m_audioSamples.end(),
            [&](const AudioSampleEvent& sample) {
                return sample.m_audioResourceId == selectedMainResourceId &&
                       sample.m_timestamp == 0.0 && sample.m_offsetMs == 0 &&
                       sample.m_track == sampleTrack;
            });
        if ( !alreadyMaterialized ) {
            // 精确匹配的模板采样保留原音量；新增项才使用采样结构的默认属性。
            AudioSampleEvent sample;
            sample.m_timestamp       = 0.0;
            sample.m_offsetMs        = 0;
            sample.m_track           = sampleTrack;
            sample.m_audioResourceId = selectedMainResourceId;
            newBeatmap->m_audioSamples.push_back(std::move(sample));
        }
    }

    // 保存前的所有模板与 Timing 修改都局限于新对象，项目资源表尚未提交。
    // 失败返回默认结果，调用方不能据此打开一个已创建的谱面入口。
    if ( newBeatmap->saveToFile(mapPath) ) {
        XINFO("Beatmap saved to: {}", Config::pathToUtf8(mapPath));
    } else {
        XERROR("Failed to save new beatmap: {}", Config::pathToUtf8(mapPath));
        return result;
    }

    // 落盘成功是登记边界；显式创建也撤销同路径的历史排除项。
    /// @brief 新谱面在项目列表中的入口。
    Project::BeatmapEntry entry;
    entry.m_name = meta.name;
    entry.m_filePath =
        Config::pathToUtf8(makeProjectRelativePath(project, mapPath));
    removeExcludedPath(project.m_excludedBeatmapPaths, entry.m_filePath);
    project.m_beatmaps.push_back(entry);

    if ( pendingMainResource ) {
        // 只撤销本次新登记资源的排除项；已有音轨不会在这里重复加入列表。
        removeExcludedPath(project.m_excludedAudioPaths,
                           pendingMainResource->m_path);
        project.m_audioResources.push_back(std::move(*pendingMainResource));
    }

    result.m_created = true;
    // 将同一个已保存对象交给会话，调用方无需再从磁盘加载一份模板复制结果。
    result.m_beatmap     = std::move(newBeatmap);
    result.m_displayName = meta.name;
    return result;
}

/// @brief 导入音频文件并登记到项目资源列表。
/// @param project 当前打开的项目。
/// @param cmd 导入音频命令。
/// @return 导入音频的处理结果。
/// @note 项目内文件原地登记，项目外文件先复制；返回音效注册请求由调用方执行。
/// @warning 导入执行同步文件复制，仅用于用户命令等低频路径。
/// @details m_imported 表示新增资源条目，不代表音频已解码或整个项目已保存。
/// @pre 调用方负责项目写权限及后续配置保存，本服务不持有项目级互斥锁。
ProjectCommandService::ImportAudioResult ProjectCommandService::importAudio(
    Project& project, const CmdImportAudio& cmd) const
{
    /// @brief 本次导入音频的返回结果。
    ImportAudioResult result;

    /// @brief 用户指定的源音频路径。
    std::filesystem::path audioPath = Config::utf8ToPath(cmd.path);
    std::error_code       filesystemError;
    const auto            sourcePath = weaklyCanonicalAbsolutePath(audioPath);
    // 路径有效性在复制前确认；这里只接受普通文件，不展开目录或谱包。
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
    // 根内源文件使用已有位置，根外输入在选出复制目标前没有最终路径。
    std::filesystem::path finalAbsolutePath =
        finalRelativePath.empty()
            ? std::filesystem::path{}
            : (projectRoot / finalRelativePath).lexically_normal();

    if ( finalRelativePath.empty() ) {
        // 只有外部资源需要复制到根目录；已有项目内子目录资源保持原相对层级。
        finalAbsolutePath = projectRoot / sourcePath.filename();

        /// @brief 复制目标文件名冲突时递增追加的数字后缀。
        int suffix = 1;
        // 保留扩展名以便后续按格式识别；重名只改变目标名称，不覆盖已有资源。
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

        // 先验证冲突检查没有失败再复制，不把查询错误当成目标不存在。
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
        // 目标已明确放在项目根下，相对化失败时以文件名维持登记路径。
        if ( finalRelativePath.empty() ) {
            finalRelativePath = finalAbsolutePath.filename();
        }
    }

    /// @brief 音频资源最终写入项目配置的 UTF-8 相对路径。
    std::string relPathUtf8 =
        Config::pathToUtf8(finalRelativePath.lexically_normal());
    // 显式导入撤销排除；随后按规范化路径去重，不仅按可能相同的文件名 ID 判断。
    removeExcludedPath(project.m_excludedAudioPaths, relPathUtf8);
    // 重复导入仍会撤销排除项；返回未新增不保证项目配置完全没有变化。
    // 外部文件在到达这里之前已复制，去重不是跨文件系统操作的回滚事务。
    for ( const auto& resource : project.m_audioResources ) {
        if ( normalizeStoredProjectPath(project, resource.m_path) ==
             normalizeStoredProjectPath(project, relPathUtf8) ) {
            XWARN("Audio already exists in project: {}", relPathUtf8);
            return result;
        }
    }

    /// @brief 新导入的项目音频资源。
    AudioResource resource;
    resource.m_id = Config::pathToUtf8(finalRelativePath.filename());
    // 选取音轨类型来自命令，不通过扩展名或所在目录猜测 Main/Effect。
    resource.m_path                 = relPathUtf8;
    resource.m_type                 = cmd.trackType;
    resource.m_config.volume        = 0.5f;
    resource.m_config.playbackSpeed = 1.0f;
    resource.m_config.playbackPitch = 0.0f;
    resource.m_config.muted         = false;

    // 资源配置先登记，再生成音效加载请求；加载器的后续失败不由此函数回滚。
    project.m_audioResources.push_back(resource);

    // 主音轨由会话播放流程接入，只有音效需要返回按需加载登记请求。
    if ( resource.m_type == AudioTrackType::Effect ) {
        /// @brief 新导入音效的按需加载登记请求。
        AudioRegistrationRequest registrationRequest;
        registrationRequest.m_resource     = resource;
        registrationRequest.m_absolutePath = finalAbsolutePath;
        result.m_effectRegistration        = registrationRequest;
        // 请求持有资源值副本，不借用可能因后续导入而重分配的项目列表元素。
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
/// @warning 此同步入口查询文件系统并加载谱面，仅用于低频文件发现或保存回调。
/// @note 不复制源文件，也不在这里打开编辑会话或注册谱面引用的音频。
/// @pre 调用方串行处理项目目录事件，避免重复发现并发插入同一入口。
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

    // 比较目录片段而非字符串前缀，避免把相似名称的相邻项目误当作子目录。
    // 此处使用 absolute 结果，并非弱规范化路径或符号链接安全边界。
    /// @brief 项目根路径和谱面路径的公共前缀比较迭代器。
    auto [rootIt, pathIt] = std::mismatch(
        absRoot.begin(), absRoot.end(), absMapPath.begin(), absMapPath.end());
    (void)pathIt;

    if ( rootIt != absRoot.end() ) {
        // 根路径尚有未匹配片段，当前文件不属于本次项目发现范围。
        return result;
    }

    /// @brief 谱面文件相对于项目根目录的 UTF-8 路径。
    std::error_code relativeError;
    auto            relativeMapPath =
        std::filesystem::relative(absMapPath, absRoot, relativeError);
    if ( relativeError || relativeMapPath.empty() ) {
        // 无法建立可存储的关联时不加入列表，避免留下不可定位的空路径条目。
        return result;
    }
    std::string relMapPath = Config::pathToUtf8(relativeMapPath);
    // 显式同步恢复已排除的路径；即使后续发现已有入口，也需保留 changed。
    if ( removeExcludedPath(project.m_excludedBeatmapPaths, relMapPath) ) {
        result.m_changed = true;
    }

    for ( const auto& entry : project.m_beatmaps ) {
        /// @brief 已登记谱面入口的绝对路径。
        auto entryPath = absRoot / Config::utf8ToPath(entry.m_filePath);
        std::error_code equivalentError;
        // 已存在路径按文件系统等价性去重，避免不同路径写法重复登记同一谱面。
        const bool alreadyTracked =
            std::filesystem::exists(entryPath, equivalentError) &&
            !equivalentError &&
            std::filesystem::equivalent(
                entryPath, absMapPath, equivalentError) &&
            !equivalentError;
        if ( alreadyTracked ) {
            // 保持已有显示名和顺序；同步发现不是刷新全部谱面元数据的请求。
            return result;
        }
    }

    /// @brief 临时加载的新谱面，用于读取显示名和主音轨。
    auto map = BeatMap::loadFromFile(absMapPath);
    if ( map.m_baseMapMetadata.map_path.empty() ) {
        // 加载失败不登记空入口；此前撤销排除项产生的变化仍通过结果返回。
        XWARN("EditorEngine: Failed to sync new beatmap {}",
              Config::pathToUtf8(absMapPath));
        return result;
    }
    normalizeBeatmapMetadataPathsForProject(map, project);

    // 展示名来自文件内容的 version；空版本才回退文件名，而非目录名。
    /// @brief 新发现谱面在项目列表中的入口。
    Project::BeatmapEntry entry;
    entry.m_name = map.m_baseMapMetadata.version;
    if ( entry.m_name.empty() ) {
        entry.m_name = Config::pathToUtf8(absMapPath.filename());
    }

    entry.m_filePath = relMapPath;

    project.m_beatmaps.push_back(entry);
    // 只新增项目入口，临时加载对象随函数结束释放，不向会话层转移所有权。
    result.m_changed = true;
    XINFO("EditorEngine: Discovered new beatmap for project: {}", entry.m_name);

    return result;
}

/// @brief 更新项目内谱面条目的文件路径关联。
/// @note 只更新列表元数据，不负责物理移动或会话路径切换。
/// @param project 当前打开的项目。
/// @param oldPath 旧谱面路径。
/// @param newPath 新谱面路径。
/// @return 项目谱面列表是否发生变化。
/// @pre 文件移动已由调用方完成；这里接收的是关联更新，而非重命名请求。
/// @note 旧条目命中时不重写排除列表；未命中才走新增文件的排除项撤销流程。
/// @details 谱面入口与独占草稿组使用同一对相对路径完成事务式重映射。
/// 草稿组即使暂时没有对应入口，也会保留这次路径变化供后续项目扫描恢复。
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

    // 谱面另存或移动后，独占草稿必须跟随项目相对路径一起重命名。
    const auto oldDraftPath = normalizeDraftBeatmapPath(relOld);
    const auto newDraftPath = normalizeDraftBeatmapPath(relNew);
    // 独立记录变化，旧条目缺失但草稿组命中时仍要通知上层保存项目。
    bool draftPathChanged = false;
    if ( !oldDraftPath.empty() && !newDraftPath.empty() ) {
        for ( auto& group : project.m_draftLaneGroups ) {
            if ( group.m_beatmapFilePath.empty() ||
                 normalizeDraftBeatmapPath(group.m_beatmapFilePath) !=
                     oldDraftPath ) {
                continue;
            }
            group.m_beatmapFilePath = newDraftPath;
            // 同谱面已打开画布通过修订号在下一安全更新点重新绑定状态。
            ++group.m_runtimeRevision;
            draftPathChanged = true;
        }
    }

    for ( auto& entry : project.m_beatmaps ) {
        // 此入口按存储的相对字符串匹配旧条目，不逐项查询文件等价性。
        if ( entry.m_filePath != relOld ) {
            continue;
        }

        // 路径先跟随移动结果；新文件暂时不能加载时仍保留旧显示名，不撤销路径关联。
        entry.m_filePath = relNew;
        /// @brief 临时加载的新谱面，用于刷新项目入口元数据。
        auto map = BeatMap::loadFromFile(absNew);
        if ( !map.m_baseMapMetadata.map_path.empty() ) {
            normalizeBeatmapMetadataPathsForProject(map, project);
            entry.m_name = map.m_baseMapMetadata.version;
            if ( entry.m_name.empty() ) {
                entry.m_name = Config::pathToUtf8(absNew.filename());
            }
        }

        // 首个匹配条目处理完即结束；本入口不作为历史重复条目的批量清理工具。
        return ProjectMutationResult{ true };
    }

    // 旧条目不存在时按新增文件处理，复用根目录限制、排除项撤销和文件去重。
    auto result = syncProjectWithFile(project, newPath);
    // 草稿路径自身也是项目数据变化，不能被谱面扫描的无变化结果覆盖。
    result.m_changed = result.m_changed || draftPathChanged;
    return result;
}

/// @brief 更新音频资源类型。
/// @param project 当前打开的项目。
/// @param cmd 更新音频资源命令。
/// @param openBeatmapReferences 已同步的打开会话内存谱面引用。
/// @return 更新音频资源的处理结果。
/// @details 修改项目配置并返回加载/卸载请求，不直接操作音频引擎。
/// @note 被玩家物件绑定的音效不能转成主音轨；自动采样引用本身不阻止此转换。
/// @warning 引用检查可能加载磁盘谱面，必须位于低频命令处理路径。
/// @pre 项目资源列表及传入会话快照在本次处理期间保持稳定。
/// @note 阻止结果只汇总谱面路径，不携带可供后续编辑的物件引用。
ProjectCommandService::UpdateAudioResourceResult
ProjectCommandService::updateAudioResource(
    Project& project, const CmdUpdateAudioResource& cmd,
    const std::vector<BeatmapAudioReference>& openBeatmapReferences) const
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

        const AudioTrackType previousType = resource.m_type;
        // 只有 Effect 到 Main
        // 需要绑定校验，避免其失去玩家物件触发所需的音效语义。
        if ( previousType == AudioTrackType::Effect &&
             cmd.newType == AudioTrackType::Main ) {
            auto references =
                ProjectResourceService::findAudioResourceReferences(project,
                                                                    resource);
            // 同时检查磁盘和未保存会话，不能仅因文件里暂时没有绑定就允许转换。
            appendMatchingOpenBeatmapReferences(
                project, resource, openBeatmapReferences, references);
            // 转换只禁止玩家触发绑定；自动采样仍可以引用 Main 资源。
            result.m_blockingBeatmapPaths = collectBlockingBeatmapPaths(
                references, [](BeatmapAudioReferenceKind kind) {
                    return kind == BeatmapAudioReferenceKind::NoteSampleBinding;
                });
            if ( !result.m_blockingBeatmapPaths.empty() ) {
                XWARN(
                    "Cannot change Effect '{}' to Main because it is bound by "
                    "Notes in {} beatmap(s)",
                    resource.m_id,
                    result.m_blockingBeatmapPaths.size());
                for ( const auto& beatmapPath :
                      result.m_blockingBeatmapPaths ) {
                    XWARN("  Note sample reference: {}", beatmapPath);
                }
                return result;
            }
        }

        // 阻止条件全部通过后才改类型；请求卸载只对应真正离开 Effect
        // 的状态转换。
        resource.m_type = cmd.newType;
        // 匹配到资源即报告更新，包括类型未变的请求；Effect 可借此重新登记。
        result.m_updated = true;
        if ( previousType == AudioTrackType::Effect &&
             resource.m_type == AudioTrackType::Main ) {
            result.m_effectResourceIdToUnload = resource.m_id;
            // 只返回 ID，不在列表修改中同步卸载音频，避免在此耦合引擎生命周期。
        }
        if ( resource.m_type == AudioTrackType::Effect ) {
            /// @brief 音频资源在项目目录中的绝对路径。
            auto absPath = resolveProjectPath(
                project, Config::utf8ToPath(resource.m_path));
            std::error_code resourcePathError;
            // 类型配置可先更新，缺失源文件不生成无法执行的音效登记请求。
            if ( std::filesystem::exists(absPath, resourcePathError) &&
                 !resourcePathError ) {
                /// @brief 更新后音效的按需加载登记请求。
                AudioRegistrationRequest registrationRequest;
                registrationRequest.m_resource     = resource;
                registrationRequest.m_absolutePath = absPath;
                result.m_effectRegistration        = registrationRequest;
            }
        }
        break;
    }

    // 未找到 ID 时保持默认未更新结果，不新建资源来满足类型变更请求。
    return result;
}

/// @brief 检查物件引用后从项目中删除音频资源，可选删除源文件。
/// @param project 当前打开的项目。
/// @param cmd 删除音频资源命令。
/// @param openBeatmapReferences 已同步的打开会话内存谱面引用。
/// @return 删除音频资源的处理结果。
/// @details 存在玩家绑定或自动采样时拒绝删除，不主动删除这些谱面物件。
/// 歌曲提示不作为阻止条件，本函数也不重写磁盘上的提示字段。
/// @note 源文件删除失败时保持资源列表不变；加载器卸载交由调用方处理返回请求。
/// @warning 显式删除源文件不可由本函数撤销；调用方应在发命令前完成用户确认。
/// @pre 资源 ID 由项目登记流程维持唯一，且检查与删除期间没有并发列表修改。
/// @details 引用阻止通过 m_blockingBeatmapPaths 表达，文件失败则提供错误消息。
/// 调用方不能只检查错误字符串来判断是否成功移除。
ProjectCommandService::RemoveAudioResourceResult
ProjectCommandService::removeAudioResource(
    Project& project, const CmdRemoveAudioResource& cmd,
    const std::vector<BeatmapAudioReference>& openBeatmapReferences) const
{
    /// @brief 本次删除音频资源的返回结果。
    RemoveAudioResourceResult result;

    XINFO("Removing audio resource from project: {}", cmd.id);

    const auto resourceIterator = std::find_if(
        project.m_audioResources.begin(),
        project.m_audioResources.end(),
        [&](const AudioResource& resource) { return resource.m_id == cmd.id; });
    if ( resourceIterator == project.m_audioResources.end() ) {
        // 未登记的 ID 不构成文件删除授权，即使命令中开启了源文件删除。
        return result;
    }

    // 先验证引用，再触碰磁盘，避免先删文件后才发现打开会话还在使用该资源。
    auto references = ProjectResourceService::findAudioResourceReferences(
        project, *resourceIterator);
    appendMatchingOpenBeatmapReferences(
        project, *resourceIterator, openBeatmapReferences, references);
    // 移除资源比改变类型更严格：自动采样同样依赖资源继续存在。
    result.m_blockingBeatmapPaths = collectBlockingBeatmapPaths(
        references, [](BeatmapAudioReferenceKind kind) {
            return kind == BeatmapAudioReferenceKind::NoteSampleBinding ||
                   kind == BeatmapAudioReferenceKind::AudioSampleEvent;
        });
    if ( !result.m_blockingBeatmapPaths.empty() ) {
        XWARN(
            "Cannot remove audio resource '{}' because it is referenced by {} "
            "beatmap(s)",
            cmd.id,
            result.m_blockingBeatmapPaths.size());
        for ( const auto& beatmapPath : result.m_blockingBeatmapPaths ) {
            XWARN("  Audio object reference: {}", beatmapPath);
        }
        return result;
    }

    // 默认只移除项目登记；只有命令显式要求时才进入物理删除流程。
    if ( cmd.deleteSourceFile ) {
        /// @brief 项目根目录的规范化绝对路径。
        const auto projectRoot =
            weaklyCanonicalAbsolutePath(project.m_projectRoot);
        /// @brief 待删除源音频的解析路径。
        const auto sourcePath = resolveProjectPath(
            project, Config::utf8ToPath(resourceIterator->m_path));
        /// @brief 解析符号链接后的源音频规范化路径。
        const auto comparableSourcePath =
            weaklyCanonicalAbsolutePath(sourcePath);
        // 规范化路径只用于范围判断，实际 remove 仍使用原解析路径。
        // 不递归删除目录，也不按通配符扩展待删除目标。
        /// @brief 源音频相对项目根目录的安全路径。
        const auto relativeSourcePath =
            makeRelativeToProjectRoot(projectRoot, comparableSourcePath);
        // 用解析后的路径确认删除范围，项目条目不能授权删除根目录外的文件。
        if ( relativeSourcePath.empty() ) {
            result.m_errorMessage = "拒绝删除项目目录之外的音频源文件";
            XWARN(
                "Refusing to delete audio resource '{}' outside project root: "
                "{}",
                cmd.id,
                Config::pathToUtf8(sourcePath));
            return result;
        }

        std::error_code filesystemError;
        const bool      sourceIsRegularFile =
            std::filesystem::is_regular_file(sourcePath, filesystemError);
        // 拒绝目录及访问失败，不能用删除配置条目的成功掩盖源文件操作失败。
        if ( filesystemError || !sourceIsRegularFile ) {
            result.m_errorMessage =
                filesystemError
                    ? "无法访问待删除的音频源文件：" + filesystemError.message()
                    : "待删除的音频源文件不存在或不是普通文件";
            XWARN("Cannot access audio source file '{}': {}",
                  Config::pathToUtf8(sourcePath),
                  result.m_errorMessage);
            return result;
        }

        filesystemError.clear();
        // remove 返回 false 也视为失败，不把目标已消失混同于本次删除成功。
        const bool sourceRemoved =
            std::filesystem::remove(sourcePath, filesystemError);
        if ( filesystemError || !sourceRemoved ) {
            result.m_errorMessage =
                filesystemError
                    ? "删除音频源文件失败：" + filesystemError.message()
                    : "删除音频源文件失败";
            XWARN("Failed to delete audio source file '{}': {}",
                  Config::pathToUtf8(sourcePath),
                  result.m_errorMessage);
            return result;
        }
    }

    /// @brief 被删除音频资源的项目相对路径。
    std::string removedPath;
    /// @brief 当前项目音频资源列表引用。
    auto& resources = project.m_audioResources;
    /// @brief 删除前的音频资源数量。
    auto oldSize = resources.size();

    // 源文件处理完成后再删除内存条目，并提取卸载所需的 ID，避免保留失效迭代器。
    // 擦除可能移动容器元素；其后只使用结果中复制的路径和 ID。
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
    // 用实际列表变化决定返回值，不把通过引用检查等同于已经删除资源。
    if ( !result.m_removed ) {
        return result;
    }

    // 只移除登记时文件仍在磁盘，排除项防止目录监听下次扫描又把它加回来。
    addExcludedPath(project.m_excludedAudioPaths, removedPath);
    // 即使源文件已删除也保留排除项，后来重新出现的同路径文件不会被自动收录。

    return result;
}

/// @brief 从项目谱面列表中删除谱面。
/// @note
/// 不删除谱面文件或关闭其会话；通过排除列表维持“仍在磁盘但不再收录”的状态。
/// 不清理音频和图片资源，共享资源的生命周期不能由单个谱面入口决定。
/// @param project 当前打开的项目。
/// @param cmd 删除谱面命令。
/// @return 项目谱面列表是否发生变化。
/// @details 删除谱面入口时只清理该路径的新格式草稿组；尚未迁移的旧音频组保留。
/// 这样无法归属的兼容数据不会因删除任一共用音频的谱面而提前丢失。
ProjectCommandService::ProjectMutationResult
ProjectCommandService::removeBeatmap(Project&                project,
                                     const CmdRemoveBeatmap& cmd) const
{
    /// @brief 本次删除谱面的返回结果。
    ProjectMutationResult result;

    XINFO("Removing beatmap from project list: {}", cmd.filePath);

    // 即使条目已不在列表，新增排除项仍是项目配置变化，需要报告给调用方保存。
    if ( addExcludedPath(project.m_excludedBeatmapPaths, cmd.filePath) ) {
        result.m_changed = true;
    }

    /// @brief 当前项目谱面列表引用。
    auto& maps = project.m_beatmaps;
    /// @brief 删除前的谱面条目数量。
    auto oldSize = maps.size();
    // 入口按命令中的存储路径精确匹配，不依赖文件此刻存在。
    maps.erase(std::remove_if(maps.begin(),
                              maps.end(),
                              [&](const Project::BeatmapEntry& entry) {
                                  return entry.m_filePath == cmd.filePath;
                              }),
               maps.end());

    if ( maps.size() != oldSize ) {
        // 保留前面新增排除项产生的 true，不能用本次擦除数量直接覆盖结果。
        result.m_changed = true;
    }

    // 草稿已按谱面独占，移除项目入口时同步清理其侧车数据，不能留下孤立分组。
    const auto removedDraftPath = normalizeDraftBeatmapPath(cmd.filePath);
    const auto oldDraftCount    = project.m_draftLaneGroups.size();
    // 旧版音频键组尚未确定属于哪张谱面，不能因删除任一共用音频谱面而误删。
    std::erase_if(
        project.m_draftLaneGroups, [&](const ProjectDraftLaneGroup& group) {
            return !group.m_beatmapFilePath.empty() &&
                   normalizeDraftBeatmapPath(group.m_beatmapFilePath) ==
                       removedDraftPath;
        });
    if ( project.m_draftLaneGroups.size() != oldDraftCount ) {
        result.m_changed = true;
    }

    return result;
}

/// @brief 规范化项目相对路径，用于稳定比较排除列表。
/// @param path UTF-8 编码的项目相对路径。
/// @return 规范化后的 UTF-8 项目相对路径。
/// @note 仅作词法折叠，不检查文件存在性；排除项在源文件被删除后仍需可比较。
/// 这不是访问权限校验，不拒绝绝对路径或上级目录；调用方须提供项目相对路径。
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
/// @note 同时规范化查询值和历史项，旧配置无需先批量改写即可参与比较。
/// 路径大小写沿用存储值，不模拟具体文件系统的大小写等价规则。
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
        // 空路径没有可排除的资源；等价项已经存在时维持原列表顺序。
        return false;
    }

    // 保存规范形式，避免同一相对位置因点路径写法不同反复加入。
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
    // 即使待移除文件已经不存在也按配置键移除，不调用 equivalent 查询磁盘。
    /// @brief 移除前的排除列表长度。
    auto oldSize = excludedPaths.size();
    // 一次移除全部等价写法，兼容旧配置已经存在重复排除项的情况。
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
/// @note 绝对输入原样词法规范化，不限制在项目根内；敏感文件操作需另做范围检查。
std::filesystem::path ProjectCommandService::resolveProjectPath(
    const Project& project, const std::filesystem::path& path)
{
    if ( path.empty() || path.is_absolute() ) {
        // 空输入保持空，不意外解析成项目根；绝对输入也不重复拼接根目录。
        return path.lexically_normal();
    }

    const auto      root = weaklyCanonicalAbsolutePath(project.m_projectRoot);
    const auto      directCandidate = (root / path).lexically_normal();
    std::error_code filesystemError;
    if ( std::filesystem::exists(directCandidate, filesystemError) &&
         !filesystemError ) {
        // 存在真实的同名子目录时优先尊重它，不盲目剥掉看似重复的项目名。
        return directCandidate;
    }

    // 首选直接根相对路径，只有该候选缺失才尝试旧版重复项目名前缀。
    const auto stripped = stripProjectFolderPrefix(root, path);
    if ( !stripped.empty() ) {
        const auto strippedCandidate = (root / stripped).lexically_normal();
        filesystemError.clear();
        if ( std::filesystem::exists(strippedCandidate, filesystemError) &&
             !filesystemError ) {
            return strippedCandidate;
        }
    }

    // 未找到文件也保留首选解释供错误提示使用，返回路径不是存在性证明。
    return directCandidate;
}

/// @brief 将文件系统路径转换为项目相对路径。
/// @param project 路径所属项目。
/// @param path 需要转换的文件系统路径。
/// @return 项目相对路径；无法转换时保留相对输入，绝对输入退回文件名。
/// @details 仅绝对输入在转换失败时退回文件名，相对输入保留词法规范形式。
/// 返回结果不意味着文件已经复制到项目；文件导入与路径表示是两个步骤。
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
/// @note 两处都不存在时保留首选解释，便于缺失资源诊断，而不是返回空路径。
/// 这里以存在性选择候选，不验证候选是图片、视频或可解码音频。
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
    // 候选路径都按同一原始相对输入构造，不能将项目根解析结果再拼到谱面目录。

    /// @brief 文件存在性检查错误码。
    std::error_code filesystemError;
    if ( preferProjectRoot ) {
        // 两处都有同名文件时遵循格式约定，不通过文件内容猜测引用意图。
        if ( std::filesystem::exists(projectPath, filesystemError) )
            return projectPath;
        filesystemError.clear();
        if ( std::filesystem::exists(mapPath, filesystemError) ) return mapPath;
        return projectPath;
    }

    if ( std::filesystem::exists(mapPath, filesystemError) ) return mapPath;
    // 清除上一候选的错误状态，再独立尝试另一基准下的资源。
    filesystemError.clear();
    if ( std::filesystem::exists(projectPath, filesystemError) )
        return projectPath;
    return mapPath;
}

/// @brief 将谱面元数据中的长期资源路径规范化为项目相对路径。
/// @param beatMap 需要规范化元数据路径的谱面。
/// @param project 谱面所属项目。
/// @note 仅改内存元数据，不移动资源，也不写回谱面文件。
void ProjectCommandService::normalizeBeatmapMetadataPathsForProject(
    BeatMap& beatMap, const Project& project)
{
    /// @brief 谱面的基础元数据引用。
    auto& meta = beatMap.m_baseMapMetadata;
    // 没有谱面自身路径就缺少外部格式的相对基准，此时不猜测资源位置。
    if ( meta.map_path.empty() ) return;

    /// @brief 谱面文件的绝对路径。
    auto absoluteMapPath = resolveProjectPath(project, meta.map_path);
    /// @brief 谱面文件所在目录。
    auto mapDirectory = absoluteMapPath.parent_path();
    /// @brief 谱面扩展名，用于判断资源路径解析优先级。
    auto mapExtension = Config::pathToUtf8(absoluteMapPath.extension());
    // 扩展名比较不区分大小写，避免 .MMM 使用外部格式的解析优先级。
    std::transform(mapExtension.begin(),
                   mapExtension.end(),
                   mapExtension.begin(),
                   ::tolower);
    /// @brief 是否优先按项目根目录解析资源路径。
    bool preferProjectRoot = (mapExtension == ".mmm");

    // 先保存原谱面目录再改写 map_path，外部格式的相对资源仍按原位置解析。
    // 本地 MMM 格式优先项目根，其他格式优先谱面目录；缺失时再尝试另一候选。
    meta.map_path = makeProjectRelativePath(project, absoluteMapPath);

    /// @brief 规范化单个元数据资源路径的闭包。
    auto normalizeResourcePath = [&](std::filesystem::path& path) {
        // 空资源字段表示未选择，不将其填成目录名或默认资源。
        if ( path.empty() ) return;
        /// @brief 解析后的资源路径。
        auto resolved = resolveMetadataResourcePath(
            project, mapDirectory, path, preferProjectRoot);
        path = makeProjectRelativePath(project, resolved);
    };

    normalizeResourcePath(meta.main_audio_path);
    // 旧主音频字段与歌曲提示分别保留，只规范化表示，不在此做字段迁移。
    normalizeResourcePath(meta.song_file_hint);
    normalizeResourcePath(meta.main_cover_path);
    normalizeResourcePath(meta.cover_path);
}

/// @brief 创建默认音轨配置。
/// @return 默认音轨配置。
/// @note 用于新建谱面补登记音轨，不覆盖项目中已经保存的用户音轨配置。
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
    // 其他成员继续使用结构自身默认值，不为未开启的均衡器生成额外配置数据。
    return config;
}

}  // namespace MMM::Logic
