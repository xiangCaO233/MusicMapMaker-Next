#include "logic/SharedResourceRenameService.h"

#include "config/Utf8Path.h"
#include "logic/ProjectResourceService.h"
#include "logic/VisualResourceRenameService.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/AudioResource.h"
#include "mmm/project/Project.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <system_error>

/// @file SharedResourceRenameService.cpp
/// @brief 为单谱面改名选择无破坏的直接移动或复制路径。
/// @details 复制授权与执行由 UI 确认边界管理，本服务从不持久化或更改模型。
namespace MMM::Logic
{
namespace
{
/// @brief 统一项目路径的文件系统身份，解析错误由上层作为失败处理。
/// @param root 已解析的项目根目录。
/// @param path 项目相对路径或绝对路径。
/// @param error 接收查询错误，不使用异常机制。
/// @return 真实路径；只在显式改名预检中调用。
std::filesystem::path resolve(const std::filesystem::path& root,
                              const std::filesystem::path& path,
                              std::error_code&             error)
{
    // canonical 阻止通过符号链接越出工程，分量比较避免同名前缀目录误匹配。
    return std::filesystem::canonical(path.is_absolute() ? path : root / path,
                                      error);
}

/// @brief 判断谱面当前是否直接或隐式引用源文件，不使用项目默认音频兜底。
/// @param map 已从真实磁盘模型或打开会话提取的引用值。
/// @note 打开的实体注册表可能继续编辑，但该证据只属于确认时的不可变快照。
/// @param project 提供音频 ID 与路径解析规则的项目。
/// @param audio 音频资源；空值表示检查视觉字段。
/// @param source 已解析的源文件绝对路径。
/// @param destination 仅供视觉路径重映射预演，不产生实际写入。
/// @return 存在引用时为真，不能据此修改来源谱面。
bool references(const OpenResourceReferenceSnapshot& map,
                const Project& project, const AudioResource* audio,
                const std::filesystem::path& source,
                const std::filesystem::path& destination)
{
    if ( audio ) {
        // 打开会话的引用早已冻结，后台只解析其路径身份。
        return std::any_of(
            map.m_audioReferences.begin(),
            map.m_audioReferences.end(),
            [&](const auto& ref) {
                return ProjectResourceService::audioReferenceMatchesResource(
                    project, ref, *audio);
            });
    }
    // 预演只复制轻量元数据，绝不为了检查共享而保存或规范化原谱面。
    // 当前 IMD 的封面和背景由加载器填入实际找到的文件，空推测路径不会误命中。
    // 复用原有视觉匹配器，保持项目相对、谱面相对和历史目录前缀兼容一致。
    auto metadata = map.m_metadata;
    return VisualResourceRenameService::remapMetadata(
        metadata, project.m_projectRoot, source, destination);
}
}  // namespace

/// @brief 校验文件身份及目标名称，再检查其他谱面的资源依赖。
/// @details 所有分支都只读，用户取消前不建立文件副本或项目资源记录。
/// 打开会话与磁盘两份来源都检查，避免未保存删除引用掩盖磁盘仍有的依赖。
/// @warning 仅用户确认时读取磁盘；无操作帧不得调用。
SharedResourceRenamePlan SharedResourceRenameService::inspect(
    const Project& project, const std::filesystem::path& currentMap,
    const std::filesystem::path& source, const std::string& newName,
    const AudioResource* audio, std::span<const BeatMap* const> openMaps)
{
    // 同步调用方仍可提供模型观察指针；先提取完全独立的值，
    // 使后续的事务与使用文件池的设置页面共享同一口径。
    std::vector<OpenResourceReferenceSnapshot> snapshots;
    snapshots.reserve(openMaps.size());
    for ( const auto* map : openMaps )
        if ( map ) snapshots.push_back(capture(*map, audio != nullptr));
    return inspectCaptured(
        project, currentMap, source, newName, audio, snapshots);
}

/// @brief 在绑定仍稳定时提取音频引用或视觉元数据。
/// @param map 由调用方稳定的谱面，捕获完成后可关闭或继续编辑。
/// @param audio 音频需要探测全部显式和兼容引用；视觉只复制元数据。
/// @details 捕获只读取领域值，不调用解析器、不跟随符号链接。返回值不保存
/// ECS 实体或折线子节点指针，也不延长打开谱面的生命周期。
/// 音频引用需在复制前采集，避免并发未保存修改只出现在运行时而漏过冲突。
// 视觉检查所需路径已在基础元数据中，捕获时无需建立物件引用索引。
OpenResourceReferenceSnapshot SharedResourceRenameService::capture(
    const BeatMap& map, bool audio)
{
    OpenResourceReferenceSnapshot result{ .m_metadata = map.m_baseMapMetadata };
    if ( audio )
        result.m_audioReferences =
            ProjectResourceService::collectBeatmapAudioReferences(
                map, Config::pathToUtf8(result.m_metadata.map_path));
    return result;
}

/// @brief 在后台检查冻结的工程清单及打开谱面引用。
/// @param openMaps 从 UI 确认时的项目会话中已捕获的引用和文件身份。
/// @details 磁盘证据保留尚未被保存覆盖的旧引用，值快照保留尚未落盘的新引用。
/// 用户同意建立副本前应重新确认共享情况，源与目标路径可能已发生变化。
SharedResourceRenamePlan SharedResourceRenameService::inspectCaptured(
    const Project& project, const std::filesystem::path& currentMap,
    const std::filesystem::path& source, const std::string& newName,
    const AudioResource*                           audio,
    std::span<const OpenResourceReferenceSnapshot> openMaps)
{
    // 每次确认独立建立结果，不能把之前预检成功视为文件仍然存在的保证。
    // 结果只保存路径与标签，谱面或项目切换后不会留下悬挂的来源指针。
    SharedResourceRenamePlan result;
    std::error_code          error;
    const auto root = std::filesystem::canonical(project.m_projectRoot, error);
    if ( !error ) result.m_source = resolve(root, source, error);
    // 路径分量比较比字符串 starts_with 更严格，工程旁的同名前缀目录不算内部。
    // canonical 的查询错误在任何名称处理或目录扫描之前终止。
    const auto relative = result.m_source.lexically_relative(root);
    // 源必须是工程内普通文件；临时只读工程在 UI 入口和这里分别把关。
    if ( project.m_isTemporaryProject || error || relative.empty() ||
         relative.is_absolute() || *relative.begin() == ".." ||
         !std::filesystem::is_regular_file(result.m_source, error) || error ) {
        result.m_error = "源文件不存在或不属于当前正式项目";
        return result;
    }
    // 两种平台都可能打开项目，因此统一拒绝 Windows 保留符号及末尾点、空格。
    if ( newName.empty() || newName == "." || newName == ".." ||
         newName.find_first_of("/\\<>:\"|?*") != std::string::npos ||
         newName.back() == '.' || newName.back() == ' ' ||
         std::any_of(newName.begin(), newName.end(), [](unsigned char c) {
             return c < 32;
         }) ) {
        result.m_error = "文件名无效，不能包含路径或保留字符";
        return result;
    }
    // 输入是单个最终文件名，固定在源目录下，不能把复制变成移动目录入口。
    // UTF-8 转换复用项目路径工具，Unicode 名称不参与日志或 UI 格式串解析。
    auto name = Config::utf8ToPath(newName);
    if ( name.extension().empty() ) name += result.m_source.extension();
    // 不把扩展名变更当作转码，源内容始终逐字保留。
    if ( name.extension() != result.m_source.extension() ) {
        result.m_error = "重命名不能改变资源文件扩展名";
        return result;
    }
    result.m_destination = result.m_source.parent_path() / name;
    // 同名是幂等操作，不制造副本、提示或谱面写回。
    if ( result.m_source == result.m_destination ) return result;
    // 目标文件冲突在共享扫描之前拒绝，原文件和其他谱面都保持只读。
    // 后续执行仍用不覆盖复制，避免只依赖这一次存在性检查。
    if ( std::filesystem::exists(result.m_destination, error) || error ) {
        result.m_error = "目标文件已存在或不可访问";
        return result;
    }
    if ( audio && std::any_of(project.m_audioResources.begin(),
                              project.m_audioResources.end(),
                              [&](const auto& item) {
                                  return item.m_id == Config::pathToUtf8(name);
                              }) ) {
        // 即使位于不同目录，项目音频主键也不能重复。
        result.m_error = "项目中已存在同名音频资源";
        return result;
    }
    // 预检不自动登记新音轨，也不建立文件复制旁路，提示前始终保持纯查询。
    // 音频主键冲突单独于磁盘路径冲突，因为不同目录也不能拥有歧义 ID。
    // 当前谱面允许还没落盘，词法身份用于区分它与项目中的其他来源。
    // 绝对和相对写法采用同一词法基准；不以标题、难度名或标签顺序识别来源。
    // 这里不要求 currentMap 已存在，因此新建但未保存的会话也能发起操作。
    const auto current =
        (currentMap.is_absolute() ? currentMap : root / currentMap)
            .lexically_normal();
    /// @brief 合并一份谱面的引用证据，同时检查复制目标是否会影响 IMD 选名。
    /// @details 不修改 map，重复的磁盘和内存证据按规范路径合并展示。
    const auto checkMap = [&](const OpenResourceReferenceSnapshot& map,
                              const std::filesystem::path&         path) {
        auto extension = Config::pathToUtf8(path.extension());
        std::transform(
            extension.begin(),
            extension.end(),
            extension.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if ( extension == ".imd" && path != current &&
             path.parent_path() == result.m_destination.parent_path() ) {
            // 新副本也不能抢占另一份 IMD 的隐式歌曲或图片名称。
            // 只检查共享源文件不足以发现这种“新增文件改变选名”的反向影响。
            const auto fileName  = Config::pathToUtf8(path.filename());
            const auto separator = fileName.find('_');
            if ( separator != std::string::npos &&
                 fileName.substr(0, separator) ==
                     Config::pathToUtf8(result.m_destination.stem()) ) {
                result.m_error = "新文件名可能改变 IMD 谱面 '" +
                                 Config::pathToUtf8(path.filename()) +
                                 "' 的隐式资源关联，请使用其他名称";
                return;
            }
        }
        // 普通当前谱面本来就会改绑，不能把自身引用误当成其他谱面共享。
        // IMD 是例外：即使只有它自身使用，隐式命名仍禁止直接搬走源文件。
        if ( path == current && extension != ".imd" ) return;
        if ( !references(
                 map, project, audio, result.m_source, result.m_destination) )
            return;
        // 检查到一份来源即不能直接移动原文件；其它已知来源继续收集用于提示。
        // 复制模式无需重写外部格式，其他谱面原始字节因此能够完整保留。
        result.m_requiresCopy = true;
        const auto label = Config::pathToUtf8(path.lexically_relative(root));
        // 打开的谱面也在磁盘目录内时仅展示一次，但两份引用都必须检查。
        if ( std::find(result.m_references.begin(),
                       result.m_references.end(),
                       label) == result.m_references.end() )
            result.m_references.push_back(label);
    };
    // 只读取项目已登记的谱面，不扫描目录里的备份、导出物或任意未知文件。
    // 原始磁盘仍可能被下次打开使用，即使某个未保存会话已移除引用也不能忽略。
    for ( const auto& entry : project.m_beatmaps ) {
        const auto path =
            resolve(root, Config::utf8ToPath(entry.m_filePath), error);
        if ( error ) {
            // 无法检查的谱面可能仍有引用，拒绝猜测“无人使用”。
            result.m_error = "无法检查项目谱面：" + entry.m_filePath;
            return result;
        }
        // 真实解析器负责格式差异，IMD 隐式音频和图片也转换为可匹配的模型引用。
        // 加载失败不能被当成“没有关联”，否则移动源文件会破坏未检查到的谱面。
        auto map = BeatMap::loadFromFile(path);
        if ( map.m_baseMapMetadata.map_path.empty() ) {
            result.m_error = "无法读取项目谱面：" + entry.m_filePath;
            return result;
        }
        // 磁盘谱面也使用同一提取器，避免与打开会话对采样的判断产生两套定义。
        checkMap(capture(map, audio != nullptr), path);
        if ( !result.m_error.empty() ) return result;
    }
    // 额外检查尚未保存或尚未登记的新谱面，覆盖只存在内存中的资源关联。
    // 文件任务只读取捕获时的引用，不在后台访问打开会话的对象。
    // 磁盘保留旧引用与内存新增引用各自独立保护，不能只采用其中一个来源。
    for ( const auto& map : openMaps ) {
        const auto path = (map.m_metadata.map_path.is_absolute()
                               ? map.m_metadata.map_path
                               : root / map.m_metadata.map_path)
                              .lexically_normal();
        checkMap(map, path);
        if ( !result.m_error.empty() ) return result;
    }
    return result;
}
}  // namespace MMM::Logic
