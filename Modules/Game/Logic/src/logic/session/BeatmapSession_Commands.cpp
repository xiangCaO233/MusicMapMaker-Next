#include "logic/BeatmapSession.h"

#include "audio/AudioManager.h"
#include "audio/AudioOriginAlignmentService.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/BeatmapSaveConflictEvent.h"
#include "event/logic/BeatmapSaveProgressEvent.h"
#include "event/logic/BeatmapSaveResultEvent.h"
#include "log/colorful-log.h"
#include "logic/BeatmapLoadDiagnosticPublisher.h"
#include "logic/EditorEngine.h"
#include "logic/ImdPackageExportService.h"
#include "logic/MalodyPackageCompatibility.h"
#include "logic/MczAudioOriginAlignment.h"
#include "logic/ProjectDraftLaneService.h"
#include "logic/ProjectResourceService.h"
#include "logic/audio/AudioTimelineDescriptor.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/ActionController.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/InteractionController.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/PackageFileTypes.h"
#include "mmm/project/Project.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <limits>
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * @file BeatmapSession_Commands.cpp
 * @brief 实现会话命令消费、谱面载入保存及导出打包事务。
 *
 * 本文件是 BeatmapSession 的命令边界：UI 与 EditorEngine 只提交值语义命令，
 * 会话在逻辑线程 update 中串行消费并修改 SessionContext。编辑动作委托给
 * ActionController，播放与相机命令委托给对应控制器，文件操作只在显式保存、
 * 导出或载图分支执行，不能混入普通每帧更新。
 *
 * 保存路径同时维护磁盘文件、会话哈希、项目谱面入口和 UI 结果事件。外部修改
 * 检测必须在覆盖前完成，成功写盘后才更新基线哈希。导出路径使用当前 ECS 同步
 * 后的 BeatMap 副本，不应把格式转换副作用写回正在编辑的原谱面。
 *
 * 项目资源路径持久化为项目相对形式，跨入解码器或文件系统前再解析为实际路径。
 * Malody song.file、Main BGM 自动采样和项目资源 ID 之间存在兼容映射，修改元数据
 * 时必须同步维护这些表示，避免下次保存或导出重新引用旧音轨。
 *
 * 命令消费遵循以下状态所有权约定：
 *
 * - SessionContext 是活动编辑状态的唯一写入目标；命令对象只携带输入快照，
 *   处理器不得把指向命令临时存储的引用留到本轮之后。
 * - Note、自动采样与 Timing 分属不同 Registry 或缓存，任何需要持久化的路径
 *   都必须先显式同步相应脏域，不能假定上一帧已经更新 BeatMap。
 * - ActionController 负责可撤销领域动作，InteractionController 负责手势状态，
 *   PlaybackController 负责时间位置；会话层只协调跨控制器的事务边界。
 * - mutationFlags 表示一批命令完成后的领域变化，不表示命令是否被读取。
 *   高频鼠标状态可以被消费而不让 Unlimited 更新策略误判仍有业务工作。
 * - 协作权威替换不得越过尚未被服务端包含的本地对象序号，也不得破坏正在
 *   提交的本地手势；必要时保存最新替换并等待后续 update 再处理。
 * - 文件命令通过全局文件操作门闩串行化。门闩获取只能使用 try_lock，逻辑
 *   线程不能等待正在绘制保存进度的 UI 线程释放资源。
 *
 * 路径处理分为三种表示，维护时不能混用：
 *
 * - 元数据路径优先保存为项目相对路径，保证项目目录整体移动后仍可解析。
 * - 文件系统操作使用由项目根解析出的规范路径，不能依赖进程工作目录。
 * - zip 条目使用通用 UTF-8 分隔符的纯相对路径，并拒绝盘符、根目录和 `..`。
 *
 * 保存事务的提交点是目标文件完整写出。只有越过该提交点，才允许更新当前
 * 谱面路径、外部修改哈希、ActionStack 保存点和项目清单。保存失败必须保留
 * 原有脏状态及尾随元数据任务，避免一次 IO 故障被误认为内容已经持久化。
 *
 * 导出事务与保存事务刻意分离。另存为和打包可以读取当前会话的最新内容，
 * 但不能接管 currentBeatmap 的编辑路径，也不能推进撤销栈保存点。格式私有
 * 覆盖只作用于待写出的 BeatMap 副本，完成后恢复或销毁临时元数据。
 *
 * 包写入采用“预检、准备、归档、提交”四阶段：
 *
 * 1. 预检验证选择路径、格式支持和 MCZ 音频原点对齐依赖。
 * 2. 准备阶段按需转换谱面、生成兼容 IMD 或转码非 OGG Main 音频。
 * 3. 归档阶段把完整字节写入内存 zip，并用规范路径集合处理重复条目。
 * 4. 提交阶段只在全部条目成功后生成最终缓冲并一次性覆盖目标文件。
 *
 * 这一顺序保证共享 Main 音频相位冲突在写出任何目标文件前被发现，也保证
 * 中途失败不会留下可被用户误认为完整资源包的半成品。系统临时文件均由创建
 * 它们的分支负责回收，清理失败只记录为非致命环境问题，不覆盖原始业务错误。
 */

namespace
{
/// @brief 将逻辑保存来源映射为 UI 反馈策略。
/// @param kind 谱面保存请求来源。
/// @return 保存成功时应采用的界面反馈形式。
/// @note Internal 保存保持静默，定时与事件触发自动保存使用不同状态栏反馈。
[[nodiscard]] MMM::Event::BeatmapSavePresentation savePresentationFor(
    MMM::Logic::BeatmapSaveKind kind)
{
    switch ( kind ) {
    case MMM::Logic::BeatmapSaveKind::TimedAutoSave:
        return MMM::Event::BeatmapSavePresentation::TimedAutoSaveStatus;
    case MMM::Logic::BeatmapSaveKind::TriggeredAutoSave:
        return MMM::Event::BeatmapSavePresentation::TriggeredAutoSaveStatus;
    case MMM::Logic::BeatmapSaveKind::Internal:
        return MMM::Event::BeatmapSavePresentation::Silent;
    case MMM::Logic::BeatmapSaveKind::Manual:
        // 未知值按用户主动保存处理，确保成功结果至少得到一次可见反馈。
    default: return MMM::Event::BeatmapSavePresentation::Transient;
    }
}

/// @brief 在存在当前项目时，将元数据路径解析为项目内路径。
/// @param path 元数据中保存的绝对或相对路径。
/// @return 以当前项目根解释并词法规范化的路径；无项目时保持相对语义。
/// @note 不检查目标存在性，允许尚未写出的另存目标继续参与路径规划。
/// @details
/// 该助手只负责表示转换，不承担安全校验或 IO。相对路径在项目打开时绑定项目根，
/// 绝对路径保持调用方选择；无项目时不能擅自使用进程工作目录补全。
std::filesystem::path resolveCurrentProjectPath(
    const std::filesystem::path& path)
{
    if ( path.empty() || path.is_absolute() ) {
        // 空路径保持未配置语义，绝对路径只整理点目录而不重新挂到项目根。
        return path.lexically_normal();
    }

    auto* project = MMM::Logic::EditorEngine::instance().getCurrentProject();
    if ( project ) {
        // 项目相对元数据必须以项目根解释，不能依赖进程当前工作目录。
        return (project->m_projectRoot / path).lexically_normal();
    }
    // 无项目的独立谱面保留相对路径，后续格式加载器可按谱面目录解释。
    return path.lexically_normal();
}

/// @brief 尽量将文件系统路径保存为当前项目相对元数据路径。
/// @param path 已解析的文件系统路径。
/// @return 项目内相对路径；无法相对化时回退文件名或原规范路径。
/// @details
/// 跨卷、权限或项目根解析失败时不能形成可靠相对路径。回退文件名避免把本机
/// 绝对目录写入可搬迁项目，同时保留足够的资源身份供用户重新定位。
/// @warning 保存低频路径：absolute 与 relative 可能访问文件系统。
std::filesystem::path makeCurrentProjectRelativePath(
    const std::filesystem::path& path)
{
    // 空字段不生成点路径，已经相对的字段也不重复拼接项目根。
    if ( path.empty() ) return {};
    if ( path.is_relative() ) return path.lexically_normal();

    auto* project = MMM::Logic::EditorEngine::instance().getCurrentProject();
    // 独立谱面没有可靠项目基准，只能保留调用方给出的绝对路径。
    if ( !project ) return path.lexically_normal();

    std::error_code ec;
    auto root = std::filesystem::absolute(project->m_projectRoot, ec);
    // 项目根无法解析时回退文件名，避免把机器绝对路径写入可搬迁项目。
    if ( ec ) return path.filename();

    auto relativePath = std::filesystem::relative(path, root, ec);
    if ( !ec && !relativePath.empty() ) {
        return relativePath.lexically_normal();
    }
    // 跨卷或权限失败时无法形成可靠相对路径，保留最小可展示文件身份。
    return path.filename();
}

/// @brief 将项目音频或图片资源路径解析为可直接读取的实际路径。
/// @param project 当前项目。
/// @param beatmapPath 当前谱面路径，用作项目根缺失时的回退基准。
/// @param storedPath 资源中保存的路径。
/// @return 词法规范化后的实际路径；原路径为空时返回空。
std::filesystem::path resolveProjectResourcePath(
    const MMM::Project& project, const std::filesystem::path& beatmapPath,
    const std::filesystem::path& storedPath)
{
    // 空资源字段表示未绑定，不把它解析为项目根目录。
    if ( storedPath.empty() ) return {};
    auto resolvedPath = storedPath;
    if ( resolvedPath.is_relative() ) {
        if ( !project.m_projectRoot.empty() ) {
            // 正常项目资源统一相对项目根保存，优先采用这一权威基准。
            resolvedPath = project.m_projectRoot / resolvedPath;
        } else if ( !beatmapPath.empty() ) {
            // 临时构造的项目可能没有根目录，外部格式退回谱面所在目录解释。
            resolvedPath = beatmapPath.parent_path() / resolvedPath;
        }
    }
    std::error_code filesystemError;
    const auto      canonicalPath =
        std::filesystem::weakly_canonical(resolvedPath, filesystemError);
    // 弱规范化失败时仍返回词法路径，调用方再通过实际读取报告具体错误。
    return filesystemError ? resolvedPath.lexically_normal()
                           : canonicalPath.lexically_normal();
}

/// @brief 解析当前谱面可用于 IMD 资源包的背景图片。
/// @param project 当前项目。
/// @param beatMap 当前谱面。
/// @return 第一张存在的主背景或封面图片；均不存在时返回空。
std::filesystem::path resolveImdPackageCoverPath(const MMM::Project& project,
                                                 const MMM::BeatMap& beatMap)
{
    const auto&                                meta = beatMap.m_baseMapMetadata;
    const std::array<std::filesystem::path, 2> candidates{ meta.main_cover_path,
                                                           meta.cover_path };
    for ( const auto& candidate : candidates ) {
        // 主封面优先于普通封面，保持项目显式背景选择的导出语义。
        const auto path =
            resolveProjectResourcePath(project, meta.map_path, candidate);
        std::error_code filesystemError;
        if ( std::filesystem::is_regular_file(path, filesystemError) &&
             !filesystemError ) {
            // 只接受普通文件，目录或不可访问路径继续尝试下一候选。
            return path;
        }
    }
    // IMD 包允许无封面导出，空路径交给导出服务省略图片条目。
    return {};
}

/// @brief 把玩家物件绑定音效追加到待导出的复合音频时间线。
/// @param project 当前项目。
/// @param beatMap 当前谱面。
/// @param hitEvents 已按当前 ECS 同步的全部打击事件。
/// @param events 已包含自动采样的目标事件列表。
/// @param errorMessage 接收资源解析失败原因。
/// @return 全部绑定均成功解析时返回 true。
/// @details
/// 绑定事件先批量解析资源，保持输入索引和结果一一对应。生成的事件 ID 必须避开
/// 描述符已有的自动采样 ID；任一引用失败都会中止，避免资源包静默漏失击键音。
bool appendBoundSampleTimelineEvents(
    const MMM::Project& project, const MMM::BeatMap& beatMap,
    const std::vector<MMM::Logic::System::HitFXSystem::HitEvent>& hitEvents,
    std::vector<MMM::Audio::AudioTimelineLoadEvent>&              events,
    std::string&                                                  errorMessage)
{
    using HitEvent = MMM::Logic::System::HitFXSystem::HitEvent;
    // 先收集绑定事件与引用字符串，资源服务可一次批量解析并保持索引对应。
    std::vector<const HitEvent*>  boundEvents;
    std::vector<std::string_view> references;
    boundEvents.reserve(hitEvents.size());
    references.reserve(hitEvents.size());
    for ( const auto& hitEvent : hitEvents ) {
        if ( !hitEvent.sampleBinding ||
             hitEvent.sampleBinding->m_audioResourceId.empty() ) {
            // 未绑定或空 ID 的打击事件只保留视觉效果，不进入导出音频时间线。
            continue;
        }
        boundEvents.push_back(&hitEvent);
        references.emplace_back(hitEvent.sampleBinding->m_audioResourceId);
    }
    // 没有物件音效是正常成功路径，不需要访问项目资源表。
    if ( boundEvents.empty() ) return true;

    // 批量解析结果与 references 保持相同顺序，空项表示对应资源不可解析。
    const auto resources =
        MMM::Logic::ProjectResourceService::resolveAudioResourceReferences(
            project, beatMap.m_baseMapMetadata.map_path, references);
    std::unordered_set<std::uint64_t> usedEventIds;
    // 自动采样事件已占用部分 ID，物件音效必须避开全部现有标识。
    usedEventIds.reserve(events.size() + boundEvents.size());
    for ( const auto& event : events ) usedEventIds.insert(event.eventId);
    std::uint64_t nextEventId = 1U;

    for ( std::size_t index = 0U; index < boundEvents.size(); ++index ) {
        const auto* resource = resources[index];
        const auto& binding  = *boundEvents[index]->sampleBinding;
        if ( !resource ) {
            // 任一绑定无法解析都会使复合音频不完整，终止导出而不是静默丢音。
            errorMessage = "无法解析物件音效资源：" + binding.m_audioResourceId;
            return false;
        }
        while ( nextEventId == 0U || usedEventIds.contains(nextEventId) ) {
            // 0 保留为无效标识，线性寻找下一未使用值保证导出事件唯一。
            ++nextEventId;
        }
        usedEventIds.insert(nextEventId);

        const auto resourcePath = resolveProjectResourcePath(
            project,
            beatMap.m_baseMapMetadata.map_path,
            MMM::Config::utf8ToPath(resource->m_path));
        events.push_back(MMM::Audio::AudioTimelineLoadEvent{
            // 物件绑定按打击时间进入 0 号 BGM 轨道，音量和资源配置原样保留。
            .eventId               = nextEventId,
            .resourceKey           = resource->m_id,
            .filePath              = MMM::Config::pathToUtf8(resourcePath),
            .effectiveStartSeconds = boundEvents[index]->timestamp,
            .bgmTrackIndex         = 0U,
            .eventVolume           = binding.m_volume,
            .resourceConfig        = resource->m_config,
        });
        ++nextEventId;
    }
    // 所有绑定均追加后才报告成功，调用方可直接交给复合时间线导出器。
    return true;
}

/// @brief 按当前项目资源刷新 Malody song.file 提示并清除旧单音轨字段。
/// @param beatMap 保存或导出前需要更新的谱面。
/// @note 只更新提示字段，不创建或移动任何自动采样。
/// @details
/// 当前项目存在时由 ProjectResourceService 在资源 ID、相对路径和 Malody 提示间
/// 统一映射；独立谱面无法可靠解析项目资源，因此清除可能陈旧的旧单音轨字段。
void refreshCurrentProjectSongFileHint(MMM::BeatMap& beatMap)
{
    auto* project = MMM::Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) {
        // 无项目时无法把资源 ID 解析为路径，清除旧单音轨字段避免输出陈旧引用。
        beatMap.m_baseMapMetadata.main_audio_path.clear();
        return;
    }
    // 资源服务按当前项目 Main 轨道刷新 song.file，并处理格式兼容字段。
    (void)MMM::Logic::ProjectResourceService::refreshSongFileHintForSave(
        *project, beatMap, beatMap.m_baseMapMetadata.map_path);
}

/// @brief 将长期保存的谱面元数据路径规范化为项目存储路径。
/// @param meta 需要原地规范化的谱面基础元数据。
/// @details
/// 每个路径先按项目根解析为文件系统身份，再尽量转回项目相对表示。独立谱面没有
/// 稳定项目基准，保持其原有路径语义，不把当前进程目录写入元数据。
/// @warning 保存低频路径：逐字段解析和相对化可能访问文件系统。
void normalizeCurrentProjectMetadataPaths(MMM::BaseMapMeta& meta)
{
    auto* project = MMM::Logic::EditorEngine::instance().getCurrentProject();
    // 独立谱面不强制采用项目相对路径，保持其格式自身的路径语义。
    if ( !project ) return;

    // 谱面自身和所有长期媒体字段分别规范化，空字段由助手保持为空。
    meta.map_path = makeCurrentProjectRelativePath(
        resolveCurrentProjectPath(meta.map_path));
    meta.main_audio_path = makeCurrentProjectRelativePath(
        resolveCurrentProjectPath(meta.main_audio_path));
    meta.song_file_hint = makeCurrentProjectRelativePath(
        resolveCurrentProjectPath(meta.song_file_hint));
    meta.main_cover_path = makeCurrentProjectRelativePath(
        resolveCurrentProjectPath(meta.main_cover_path));
    meta.cover_path = makeCurrentProjectRelativePath(
        resolveCurrentProjectPath(meta.cover_path));
}

/// @brief 判断两份基础谱面元数据是否完全一致。
/// @param lhs 左侧元数据。
/// @param rhs 右侧元数据。
/// @return 所有基础字段都一致时返回 true。
bool baseMapMetadataEqual(const MMM::BaseMapMeta& lhs,
                          const MMM::BaseMapMeta& rhs)
{
    // 显式比较全部持久化基础字段，避免结构填充或未来非持久化成员参与判等。
    return lhs.name == rhs.name && lhs.title == rhs.title &&
           lhs.title_unicode == rhs.title_unicode && lhs.artist == rhs.artist &&
           lhs.artist_unicode == rhs.artist_unicode &&
           lhs.map_path == rhs.map_path &&
           lhs.main_audio_path == rhs.main_audio_path &&
           lhs.song_file_hint == rhs.song_file_hint &&
           lhs.main_cover_path == rhs.main_cover_path &&
           lhs.cover_path == rhs.cover_path &&
           lhs.cover_type == rhs.cover_type &&
           lhs.video_starttime == rhs.video_starttime &&
           lhs.bgxoffset == rhs.bgxoffset && lhs.bgyoffset == rhs.bgyoffset &&
           lhs.version == rhs.version && lhs.author == rhs.author &&
           lhs.preference_bpm == rhs.preference_bpm &&
           lhs.track_count == rhs.track_count &&
           lhs.bgm_track_count == rhs.bgm_track_count &&
           lhs.map_length == rhs.map_length;
}

/// @brief 在提交玩家轨道数变化前验证全部自动采样的绝对轨道迁移。
/// @param ctx 当前会话上下文。
/// @param oldTrackCount 当前玩家轨道数。
/// @param newTrackCount 目标玩家轨道数。
/// @param error 验证失败时写入的用户可读原因。
/// @return 全部采样都能保持 BGM 相对轨道且目标索引可表示时返回 true。
/// @details
/// 自动采样的 m_track 保存玩家轨道区之后的绝对索引，而用户编辑的是玩家轨道
/// 数量。迁移必须先减去旧玩家轨道数得到 BGM 相对索引，再加上新数量。验证使用
/// 64 位中间值检查范围且不修改 Registry；全部通过后才能原子提交迁移动作。
bool validateSampleTrackCountMigration(const MMM::Logic::SessionContext& ctx,
                                       std::int32_t oldTrackCount,
                                       std::int32_t newTrackCount,
                                       std::string& error)
{
    if ( oldTrackCount <= 0 || newTrackCount <= 0 ) {
        // 玩家轨道数是 BGM 绝对轨道到相对索引转换的基准，非正值没有定义。
        error =
            fmt::format("无法将玩家轨道数从 {} 调整为 {}：玩家轨道数必须为正数",
                        oldTrackCount,
                        newTrackCount);
        return false;
    }

    const auto oldTrackCountUnsigned =
        static_cast<std::uint32_t>(oldTrackCount);
    // 自动采样 Registry 是当前编辑状态权威来源，可能包含尚未同步到 BeatMap
    // 的项。
    const auto sampleView =
        ctx.sampleRegistry.view<const MMM::Logic::SampleComponent>();
    for ( const auto entity : sampleView ) {
        const auto& sample =
            sampleView.get<const MMM::Logic::SampleComponent>(entity);
        if ( sample.m_track < oldTrackCountUnsigned ) {
            // 自动采样落入玩家区说明现有状态已违反分区，不允许继续迁移扩大歧义。
            error = fmt::format(
                "无法将玩家轨道数从 {} 调整为 {}：自动采样轨道 {} "
                "落入玩家轨道区",
                oldTrackCount,
                newTrackCount,
                sample.m_track);
            return false;
        }

        const auto bgmTrack = static_cast<std::uint64_t>(sample.m_track) -
                              static_cast<std::uint64_t>(oldTrackCountUnsigned);
        // 先恢复相对 BGM 轨道，再加新玩家轨道数，保持采样在 BGM 区内的位置。
        const auto migratedTrack =
            static_cast<std::uint64_t>(newTrackCount) + bgmTrack;
        if ( migratedTrack > static_cast<std::uint64_t>(
                                 std::numeric_limits<std::uint32_t>::max()) ) {
            // 使用 64 位中间值检测加法溢出，验证阶段不修改任何采样组件。
            error = fmt::format(
                "无法将玩家轨道数从 {} 调整为 {}：自动采样轨道 {} "
                "迁移后超出可表示范围",
                oldTrackCount,
                newTrackCount,
                sample.m_track);
            return false;
        }
    }
    // 全部采样通过后调用方才可提交轨道数动作，验证过程保持上下文只读。
    return true;
}

/// @brief 在谱面主音轨提示变化时替换时间最早的 Main BGM 自动采样资源。
/// @param ctx 当前谱面会话上下文。
/// @param oldMetadata 更新前的谱面基础元数据。
/// @param updatedMetadata 已规范化的目标谱面基础元数据。
/// @return 找到并实际替换自动采样资源时返回 true。
/// @details
/// Malody 优先用 song_file_hint 表示主音频，旧格式回退
/// main_audio_path。只有提示 变化且目标能解析为 Main 资源时才重定向。候选限于
/// BGM 区内的 Main 自动采样，
/// 并选择有效时间最早的一项，保留后续叠加主轨结构不变。
/// @warning 仅由低频元数据命令调用；替换成功时会同步完整自动采样列表。
bool retargetFirstMainBgmSample(MMM::Logic::SessionContext& ctx,
                                const MMM::BaseMapMeta&     oldMetadata,
                                const MMM::BaseMapMeta&     updatedMetadata)
{
    // Malody 优先采用 song.file，旧格式或空提示回退 main_audio_path。
    const auto& oldAudioHint     = oldMetadata.song_file_hint.empty()
                                       ? oldMetadata.main_audio_path
                                       : oldMetadata.song_file_hint;
    const auto& updatedAudioHint = updatedMetadata.song_file_hint.empty()
                                       ? updatedMetadata.main_audio_path
                                       : updatedMetadata.song_file_hint;
    if ( oldAudioHint == updatedAudioHint || updatedAudioHint.empty() ||
         ctx.trackCount <= 0 ) {
        // 提示未变、目标被清空或轨道布局无效时不推测任何自动采样目标。
        return false;
    }

    const auto* project =
        ctx.collaborationProject
            ? ctx.collaborationProject.get()
            : MMM::Logic::EditorEngine::instance().getCurrentProject();
    // 协作会话使用隔离项目快照，普通会话才查询 EditorEngine 当前项目。
    if ( !project ) return false;

    const auto* targetResource =
        MMM::Logic::ProjectResourceService::findAudioResourceForReference(
            *project,
            updatedMetadata.map_path,
            MMM::Config::pathToUtf8(updatedAudioHint));
    if ( !targetResource ||
         targetResource->m_type != MMM::AudioTrackType::Main ) {
        // 新提示必须能解析到 Main 类型，不能把 Effect 或 Sample 当作主 BGM。
        return false;
    }

    const auto playerTrackCount = static_cast<std::uint32_t>(ctx.trackCount);
    auto sampleView = ctx.sampleRegistry.view<MMM::Logic::SampleComponent>();
    entt::entity firstMainBgmSample = entt::null;
    double       firstEffectiveTime = std::numeric_limits<double>::infinity();
    // 只在 BGM 轨道区寻找引用 Main 资源的最早采样，保留后续叠加主轨不变。
    for ( const auto entity : sampleView ) {
        const auto& sample =
            sampleView.get<const MMM::Logic::SampleComponent>(entity);
        if ( sample.m_track < playerTrackCount ) continue;

        const auto* currentResource =
            MMM::Logic::ProjectResourceService::findAudioResourceForReference(
                *project, oldMetadata.map_path, sample.m_audioResourceId);
        if ( !currentResource ||
             currentResource->m_type != MMM::AudioTrackType::Main ) {
            // 无法解析或非 Main 的采样不属于 song.file 兼容映射目标。
            continue;
        }

        const double effectiveTime = sample.effectiveTime();
        if ( !std::isfinite(effectiveTime) ||
             effectiveTime >= firstEffectiveTime ) {
            // 非有限时间不可排序，相同或更晚采样保持当前已选最早项。
            continue;
        }
        firstEffectiveTime = effectiveTime;
        firstMainBgmSample = entity;
    }
    // 没有现存 Main BGM 自动采样时只更新元数据提示，不自动创建新采样。
    if ( firstMainBgmSample == entt::null ) return false;

    auto& sample =
        sampleView.get<MMM::Logic::SampleComponent>(firstMainBgmSample);
    // 目标已经一致时保持 ECS 干净，不触发无意义同步和描述符重建。
    if ( sample.m_audioResourceId == targetResource->m_id ) return false;

    const auto previousResourceId = sample.m_audioResourceId;
    sample.m_audioResourceId      = targetResource->m_id;
    // 标脏 Sample 域并立即同步 BeatMap，后续元数据动作和保存看到同一引用。
    ctx.m_needsSamplesSync = true;
    MMM::Logic::SessionUtils::syncBeatmap(ctx);
    XINFO("Retargeted first Main BGM sample from '{}' to '{}'",
          previousResourceId,
          targetResource->m_id);
    return true;
}

/// @brief 将已成功保存的谱面基础信息同步到项目谱面入口。
/// @param metadata 已成功写入谱面文件的基础元数据。
/// @return 项目入口的名称发生变化时返回 true。
/// @details
/// 项目条目通过文件系统等价身份匹配，而不是直接比较可能一绝对一相对的字符串。
/// 这里只更新既有条目的难度名称；新文件登记由 EditorEngine 的文件同步负责。
bool syncSavedMetadataToProjectEntry(const MMM::BaseMapMeta& metadata)
{
    auto* project = MMM::Logic::EditorEngine::instance().getCurrentProject();
    // 独立谱面保存不关联项目清单，不创建隐式项目条目。
    if ( !project ) return false;

    // 保存路径先解析到实际位置，项目条目再逐项解析并用文件系统身份比较。
    const auto savedMapPath = resolveCurrentProjectPath(metadata.map_path);
    for ( auto& entry : project->m_beatmaps ) {
        const auto entryPath =
            project->m_projectRoot / MMM::Config::utf8ToPath(entry.m_filePath);
        std::error_code pathError;
        const bool      isSavedEntry =
            std::filesystem::exists(entryPath, pathError) && !pathError &&
            std::filesystem::equivalent(entryPath, savedMapPath, pathError) &&
            !pathError;
        if ( !isSavedEntry ) continue;

        // 项目标签名称对应谱面 version，而非通用 name 字段。
        if ( entry.m_name == metadata.version ) return false;

        entry.m_name = metadata.version;
        XINFO("BeatmapSession: Synced saved name '{}' to project entry",
              entry.m_name);
        return true;
    }
    // 文件尚未登记到当前项目时不新增条目，另存同步由 EditorEngine 统一处理。
    return false;
}

/// @brief 计算谱面文件的 FNV-1a 64 位哈希。
/// @param path 待读取文件路径。
/// @return 成功时返回哈希值，文件不可读时返回空。
/// @details
/// 哈希仅用于同一保存路径的外部修改检测，不承担密码学完整性。文件以固定缓冲
/// 流式读取，避免内存随谱面大小增长；读取错误返回空，由调用方保守请求确认。
std::optional<std::uint64_t> calculateBeatmapFileHash(
    const std::filesystem::path& path)
{
    std::error_code filesystemError;
    if ( !std::filesystem::is_regular_file(path, filesystemError) ||
         filesystemError ) {
        // 目录、缺失文件和查询错误均无法作为外部修改检测基线。
        return std::nullopt;
    }

    std::ifstream file(path, std::ios::binary);
    // 二进制读取避免文本换行转换导致跨平台哈希不一致。
    if ( !file ) return std::nullopt;

    constexpr std::uint64_t fnvOffset = 14695981039346656037ull;
    constexpr std::uint64_t fnvPrime  = 1099511628211ull;

    std::uint64_t               hash = fnvOffset;
    std::array<char, 64 * 1024> buffer{};
    while ( file ) {
        // 固定缓冲分块读取，内存占用不随谱面文件大小增长。
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytesRead = file.gcount();
        for ( std::streamsize index = 0; index < bytesRead; ++index ) {
            // 先转 unsigned char 保留原始字节值，再扩展到哈希整数。
            hash ^= static_cast<std::uint8_t>(static_cast<unsigned char>(
                buffer[static_cast<std::size_t>(index)]));
            hash *= fnvPrime;
        }
    }

    // eof 导致的流失败是正常结束，只有 badbit 表示真实读取错误。
    if ( file.bad() ) return std::nullopt;
    return hash;
}

/// @brief 生成保存哈希缓存使用的路径键。
/// @param path 目标文件路径。
/// @return 规范化后的 UTF-8 路径键。
/// @details
/// 这里只做词法规范化，不要求文件仍存在。统一通用分隔符后，同一会话中的载入、
/// 保存和冲突检测可复用稳定键，不受本机原生路径展示形式影响。
std::string makeBeatmapFileHashPathKey(const std::filesystem::path& path)
{
    // generic UTF-8 统一路径分隔符，缓存键不受本机原生表示差异影响。
    return MMM::Config::pathToUtf8Generic(path.lexically_normal());
}

/// @brief 刷新会话中记录的单个谱面文件哈希。
/// @param savedBeatmapFileHashes 当前会话的谱面文件哈希缓存。
/// @param path 已加载或已成功保存的谱面路径。
/// @details
/// 成功读取时替换基线；文件消失或不可读时删除旧值。保留过期哈希会让下一次
/// 保存错误地认为磁盘仍等于先前内容，因此失败不能简单忽略。
void rememberBeatmapFileHash(
    std::unordered_map<std::string, std::uint64_t>& savedBeatmapFileHashes,
    const std::filesystem::path&                    path)
{
    // 空路径没有稳定缓存身份，通常表示尚未保存的新谱面。
    if ( path.empty() ) return;

    const std::string key = makeBeatmapFileHashPathKey(path);
    // 编码转换得到空键时不污染整个缓存的默认条目。
    if ( key.empty() ) return;

    if ( auto hash = calculateBeatmapFileHash(path) ) {
        // 成功读取后覆盖旧基线，下一次保存前可识别磁盘外部变化。
        savedBeatmapFileHashes[key] = *hash;
    } else {
        // 文件不可读时删除旧基线，不能用过期哈希错误声明内容未变化。
        savedBeatmapFileHashes.erase(key);
    }
}

/// @brief 判断强制 MMM 保存是否需要用户确认覆盖。
/// @param settings 当前编辑器设置。
/// @param savedBeatmapFileHashes 当前会话的谱面文件哈希缓存。
/// @param cmd 保存命令。
/// @param savePath 本次实际写出的目标路径。
/// @return 需要确认时返回 true。
/// @details
/// 只有 ForceMMM 可能重定向到另一个已存在文件。用户确认可越过检查；否则缺少
/// 缓存基线、文件不可读或哈希不同都视为潜在外部修改，实际覆盖留给 UI 决定。
bool shouldConfirmForcedMmmOverwrite(
    const MMM::Config::EditorSettings& settings,
    const std::unordered_map<std::string, std::uint64_t>&
                                      savedBeatmapFileHashes,
    const MMM::Logic::CmdSaveBeatmap& cmd,
    const std::filesystem::path&      savePath)
{
    // 用户已经在冲突对话框确认时直接放行，避免同一命令再次触发循环提示。
    if ( cmd.allowExternallyModifiedOverwrite ) return false;
    if ( settings.saveFormatPreference !=
         MMM::Config::SaveFormatPreference::ForceMMM ) {
        // 保持原格式或其他保存策略不覆盖源文件为 MMM，不适用这项冲突门禁。
        return false;
    }

    std::error_code filesystemError;
    if ( !std::filesystem::exists(savePath, filesystemError) ||
         filesystemError ) {
        // 新文件没有外部修改来源；查询失败不在此声明冲突，由实际保存报告错误。
        return false;
    }

    auto currentHash = calculateBeatmapFileHash(savePath);
    // 文件存在但不可读时无法证明仍等于会话基线，按需要确认的保守结果处理。
    if ( !currentHash ) return true;

    const auto hashIt =
        savedBeatmapFileHashes.find(makeBeatmapFileHashPathKey(savePath));
    // 未记录基线或磁盘内容不同都属于潜在外部修改，必须由用户决定覆盖。
    return hashIt == savedBeatmapFileHashes.end() ||
           hashIt->second != *currentHash;
}

/// @brief 格式化无快照上下文的状态栏时间文本。
/// @param timeSeconds 需要展示的有符号秒数。
/// @return 按当前软件时间格式偏好生成的状态栏文本。
/// @note Beat 格式缺少 BPM 上下文时回退秒格式，不能伪造拍号位置。
/// @details
/// Clock 与 Milliseconds 使用相同的毫秒四舍五入基准。小时保留累计值而不按天
/// 截断；负时间单独处理符号，避免整数取模在预滚动区产生负字段。
std::string formatStatusTime(double timeSeconds)
{
    auto preference = MMM::Config::AppConfig::instance()
                          .getEditorSettings()
                          .timeFormatPreference;
    switch ( preference ) {
    case MMM::Config::TimeFormatPreference::Clock: {
        // 符号与绝对时长分开处理，负预滚动时间仍保持固定时钟字段宽度。
        bool    negative = timeSeconds < 0.0;
        double  absTime  = std::abs(timeSeconds);
        auto    totalMs  = static_cast<int64_t>(std::llround(absTime * 1000.0));
        int64_t ms       = totalMs % 1000;
        int64_t seconds  = (totalMs / 1000) % 60;
        int64_t minutes  = (totalMs / 60000) % 60;
        int64_t hours    = totalMs / 3600000;
        // 小时不截断到 24，长谱面或异常大时间仍能完整展示累计时长。
        return fmt::format("{}{:02}:{:02}:{:02}.{:03}",
                           negative ? "-" : "",
                           hours,
                           minutes,
                           seconds,
                           ms);
    }
    case MMM::Config::TimeFormatPreference::Milliseconds:
        // 毫秒模式统一四舍五入到整数，保持与时钟模式相同精度。
        return fmt::format(
            "{} ms", static_cast<int64_t>(std::llround(timeSeconds * 1000.0)));
    case MMM::Config::TimeFormatPreference::Beat:
    case MMM::Config::TimeFormatPreference::Seconds:
        // 无 SessionSnapshot 时无法解析拍位，Beat 与未知偏好按三位秒数回退。
    default: return fmt::format("{:.3f} s", timeSeconds);
    }
}

/// @brief 判断项目相对路径是否包含越界片段。
/// @param relativePath 待检查的相对路径。
/// @return 路径是否会逃逸项目根目录。
/// @details
/// 检查针对归档身份而非磁盘实体，不解析符号链接。绝对根、盘符和规范化后残留
/// 的 `..` 均拒绝，确保 zip 条目无法在解压时越过目标目录。
bool packageRelativePathEscapesRoot(const std::filesystem::path& relativePath)
{
    if ( relativePath.empty() || relativePath.is_absolute() ||
         relativePath.has_root_name() ) {
        // 包条目必须是非空纯相对路径，盘符和根目录均可能写出目标目录之外。
        return true;
    }
    const auto normalizedPath = relativePath.lexically_normal();
    for ( const auto& part : normalizedPath ) {
        // 规范化后仍存在 .. 表示路径实际越过包根，不能仅按字符串前缀判断。
        if ( part == std::filesystem::path("..") ) return true;
    }
    // 普通点目录和重复分隔符已被规范化，不影响安全的包内相对身份。
    return false;
}

/// @brief 读取完整二进制文件。
/// @param path 待读取文件路径。
/// @param outBytes 输出文件字节。
/// @return 是否读取成功。
/// @details
/// 先用 ate 获取大小并一次分配，随后从文件头完整读取。所有失败路径保持输出为
/// 空或本轮部分数据，但返回 false，调用方不得在失败时消费容器内容。
bool readPackageSourceFile(const std::filesystem::path& path,
                           std::vector<std::uint8_t>&   outBytes)
{
    // 输出先清空，任何失败都不会让调用方误用上一次成功读取的残留字节。
    outBytes.clear();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if ( !file ) return false;

    const auto fileSize = file.tellg();
    // tellg 失败返回负位置，不能转换成巨大的 size_t 分配。
    if ( fileSize < 0 ) return false;
    file.seekg(0, std::ios::beg);

    outBytes.resize(static_cast<std::size_t>(fileSize));
    // 合法空文件无需 read，直接视为完整读取成功。
    if ( outBytes.empty() ) return true;

    file.read(reinterpret_cast<char*>(outBytes.data()),
              static_cast<std::streamsize>(fileSize));
    // 要求完整读满计划大小，短读和 IO 错误均返回失败。
    return file.good();
}

/// @brief 向指定路径写入二进制文件。
/// @param path 输出文件路径。
/// @param data 待写入数据指针。
/// @param size 待写入字节数。
/// @return 是否写入成功。
/// @details
/// 父目录按需创建，目标文件以 trunc 覆盖。零长度文件允许空数据指针；非零长度
/// 指针生命周期由调用方保证覆盖整个同步 write 调用。
bool writePackageOutputFile(const std::filesystem::path& path, const void* data,
                            std::size_t size)
{
    std::error_code filesystemError;
    const auto      parentPath = path.parent_path();
    if ( !parentPath.empty() ) {
        // 导出临时或项目目标可能包含尚未创建的父目录，按需递归建立。
        std::filesystem::create_directories(parentPath, filesystemError);
        if ( filesystemError ) return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    // trunc 保证覆盖后不残留旧文件尾部，二进制模式保持 JSON 外资源原字节。
    if ( !file ) return false;
    if ( size > 0 ) {
        // 空文件允许 data 为 nullptr；非空由调用方保证指针覆盖 size 字节。
        file.write(static_cast<const char*>(data),
                   static_cast<std::streamsize>(size));
    }
    return file.good();
}

/// @brief 构建 EX Rhythm Master VI 上架用 Malody mode_ext。
/// @return 固定的 mode_ext JSON 对象。
nlohmann::json makeMalodyStoreModeExtJson()
{
    // 使用结构化 JSON 构建固定商店元数据，避免手拼字符串转义问题。
    nlohmann::json modeExt = nlohmann::json::object();
    modeExt["bar_begin"]   = 0;
    modeExt["freenote"]    = "请访问商店或官网下载最新EX Rhythm Master VI皮肤";
    modeExt["skinid"]      = 6091;
    return modeExt;
}

/// @brief 判断路径是否为 Malody MC 谱面。
/// @param path 待检查路径。
/// @return 扩展名为 .mc 时返回 true。
bool isMalodyChartPath(const std::filesystem::path& path)
{
    // 统一扩展名比较处理大小写，避免平台文件名形式改变导出选项判断。
    return MMM::packageExtensionEquals(
        MMM::Config::pathToUtf8(path.extension()), ".mc");
}

/// @brief 临时写入上架用 mode_ext 到谱面元数据。
/// @param beatMap 待导出的谱面。
void applyMalodyStoreModeExtMetadata(MMM::BeatMap& beatMap)
{
    // BeatMap 元数据属性保存字符串化 JSON，最终 MC 写出器再嵌入协议对象。
    beatMap.m_metadata
        .map_properties[MMM::MapMetadataType::MALODY]["mode_ext"] =
        makeMalodyStoreModeExtJson().dump();
}

/// @brief 在 MC JSON 文本中替换上架用 mode_ext。
/// @param inputBytes 输入文件字节。
/// @param outBytes 输出文件字节。
/// @return 替换成功时返回 true。
/// @details
/// 解析采用 nlohmann 无异常模式，失败只通过返回值传播。meta 缺失或类型错误时
/// 用对象替换，再写入结构化 mode_ext；重序列化避免把对象误存成转义字符串。
bool patchMalodyStoreModeExtBytes(const std::vector<std::uint8_t>& inputBytes,
                                  std::vector<std::uint8_t>&       outBytes)
{
    // 容错解析返回 discarded，禁止异常机制穿过导出命令边界。
    std::string text(inputBytes.begin(), inputBytes.end());
    auto        fileData = nlohmann::json::parse(text, nullptr, false, true);
    if ( fileData.is_discarded() || !fileData.is_object() ) {
        // 非 JSON 对象不是有效 MC，输出容器保持由调用方忽略的状态。
        return false;
    }

    if ( !fileData.contains("meta") || !fileData["meta"].is_object() ) {
        // 缺失或错误类型的 meta 用新对象替换，确保 mode_ext 写入位置有效。
        fileData["meta"] = nlohmann::json::object();
    }
    fileData["meta"]["mode_ext"] = makeMalodyStoreModeExtJson();
    const std::string outputText = fileData.dump(4);
    // 四空格重写保持导出文件可读；字节容器完整替换原输入内容。
    outBytes.assign(outputText.begin(), outputText.end());
    return true;
}

/// @brief 在已写出的 MC 文件中替换上架用 mode_ext。
/// @param path MC 文件路径。
/// @return 替换成功时返回 true。
bool patchMalodyStoreModeExtFile(const std::filesystem::path& path)
{
    std::vector<std::uint8_t> inputBytes;
    std::vector<std::uint8_t> outputBytes;
    if ( !readPackageSourceFile(path, inputBytes) ) {
        // 读取失败不打开输出文件，原 MC 保持不变。
        return false;
    }
    if ( !patchMalodyStoreModeExtBytes(inputBytes, outputBytes) ) {
        // 解析失败同样不覆盖原文件，避免把空或半成品写回。
        return false;
    }
    return writePackageOutputFile(
        path,
        outputBytes.empty() ? nullptr : outputBytes.data(),
        outputBytes.size());
}

/// @brief 收集谱面中解析为项目 Main 音轨的自动采样引用。
/// @param project 谱面所属项目。
/// @param beatMap 待导出的谱面。
/// @param sourcePath 来源谱面路径，用于解析旧相对路径引用。
/// @return 需要在 MC 写出时清理 vol 的资源引用集合。
/// @details
/// 返回集合保存谱面中原始引用文本，而不是资源规范 ID，因为最终 JSON patch
/// 需要匹配实际序列化字段。可选 targetResource 将结果限制到指定共享 Main。
std::unordered_set<std::string> collectMalodyMainAudioSampleReferences(
    const MMM::Project& project, const MMM::BeatMap& beatMap,
    const std::filesystem::path& sourcePath,
    const MMM::AudioResource*    targetResource = nullptr)
{
    // 保留原引用文本而不是先去重，批量解析结果必须与每个采样索引一一对应。
    std::vector<std::string_view> references;
    references.reserve(beatMap.m_audioSamples.size());
    for ( const auto& sample : beatMap.m_audioSamples ) {
        // 空引用也交给资源服务统一解析，结果位置仍与输入采样保持一致。
        references.emplace_back(sample.m_audioResourceId);
    }

    const auto resolvedResources =
        MMM::Logic::ProjectResourceService::resolveAudioResourceReferences(
            project, sourcePath, references);
    std::unordered_set<std::string> mainReferences;
    mainReferences.reserve(references.size());
    for ( std::size_t index = 0; index < references.size(); ++index ) {
        const auto* resource = resolvedResources[index];
        if ( resource && resource->m_type == MMM::AudioTrackType::Main &&
             (!targetResource || resource == targetResource) ) {
            // 集合保存谱面中的原始写法，后续 JSON patch 按实际字段文本匹配。
            mainReferences.emplace(references[index]);
        }
    }
    return mainReferences;
}

/// @brief 单张谱面启用原点对齐后使用的非 OGG Main 音频计划。
struct MczBeatmapAudioAlignmentPlan {
    /// @brief 目标项目音频资源。
    const MMM::AudioResource* resource{ nullptr };

    /// @brief 首红线折回一拍内后的有符号相位，单位毫秒。
    double phaseMilliseconds{ 0.0 };

    /// @brief 谱面中解析到目标资源的 Main 自动采样引用。
    std::unordered_set<std::string> mainAudioReferences;
};

/// @brief 单张谱面的 MCZ 音频原点对齐计划构建结果。
struct MczAudioAlignmentPlanResult {
    /// @brief 非 OGG Main 音频需要执行的对齐计划；OGG 时为空。
    std::optional<MczBeatmapAudioAlignmentPlan> plan;

    /// @brief 无法安全建立计划时的原因；OGG 或成功时为空。
    std::string errorMessage;
};

/// @brief 为一张待转换为 MC 的谱面建立非 OGG Main 音频原点对齐计划。
/// @param project 谱面所属项目。
/// @param beatMap 已加载的谱面副本。
/// @param sourcePath 来源谱面路径。
/// @return OGG 时返回空计划；非 OGG 时返回计划或具体失败原因。
/// @details
/// 默认主音频必须来自项目 Main 类型。OGG 已满足 MCZ 直接使用条件，返回成功但
/// 无计划；非 OGG 才计算一拍内相位，并收集谱面中实际引用该资源的字段文本。
/// 计划不持有 BeatMap，只在打包事务期间借用项目资源指针。
MczAudioAlignmentPlanResult makeMczAudioAlignmentPlan(
    const MMM::Project& project, const MMM::BeatMap& beatMap,
    const std::filesystem::path& sourcePath)
{
    // 计划只保存对齐所需的资源身份、相位和引用集合，不在构建阶段修改谱面。
    MczAudioAlignmentPlanResult result;
    const auto*                 resource =
        MMM::Logic::ProjectResourceService::findDefaultBeatmapAudioResource(
            project, beatMap, sourcePath);
    if ( !resource || resource->m_type != MMM::AudioTrackType::Main ) {
        // 默认资源缺失或类型不符时无法确定需要转码对齐的主音频。
        result.errorMessage = "没有可识别的 Main 音频资源";
        return result;
    }
    if ( MMM::packageExtensionEquals(
             MMM::Config::pathToUtf8(
                 MMM::Config::utf8ToPath(resource->m_path).extension()),
             ".ogg") ) {
        // OGG 可直接进入 MCZ，不需要生成新的原点对齐音频或修改谱面相位。
        return result;
    }

    const auto timing =
        MMM::Logic::calculateMczAudioOriginAlignmentTiming(beatMap);
    if ( !timing.success ) {
        // 无有效首红线或 BPM 时不能安全计算一拍内相位，保留服务具体原因。
        result.errorMessage = timing.errorMessage;
        return result;
    }
    auto references = collectMalodyMainAudioSampleReferences(
        project, beatMap, sourcePath, resource);
    if ( references.empty() ) {
        // 没有引用目标资源的自动采样时，对齐音频后也无法同步调整谱面事件。
        result.errorMessage = "没有引用目标非 OGG Main 音频的自动采样";
        return result;
    }
    result.plan = MczBeatmapAudioAlignmentPlan{
        // resource 指针借用 project 资源表，计划只能在该项目稳定期间使用。
        .resource            = resource,
        .phaseMilliseconds   = timing.phaseMilliseconds,
        .mainAudioReferences = std::move(references),
    };
    return result;
}

/// @brief 清理已写出的 MC 中 Main 自动音频对象的 vol 字段。
/// @param path MC 文件路径。
/// @param mainAudioReferences 解析为项目 Main 音轨的资源引用集合。
/// @return 文件可解析并成功写回时返回 true。
/// @details
/// 文件先完整读取并以无异常模式解析，任何失败都不会提前截断原文件。修补器只
/// 删除引用集合命中的 Main 自动音频 vol，物件绑定和其他采样音量保持不变。
bool patchMalodyMainAudioVolumeFile(
    const std::filesystem::path&           path,
    const std::unordered_set<std::string>& mainAudioReferences)
{
    std::vector<std::uint8_t> inputBytes;
    // 先完整读入，任何解析或重写失败都不会提前截断源文件。
    if ( !readPackageSourceFile(path, inputBytes) ) return false;

    const std::string text(inputBytes.begin(), inputBytes.end());
    auto fileData = nlohmann::json::parse(text, nullptr, false, true);
    // MC 必须是 JSON 对象；容错解析失败通过返回值传播，不抛异常。
    if ( fileData.is_discarded() || !fileData.is_object() ) return false;

    // 仅清除解析为 Main 的自动音频对象 vol，其他采样和物件音量保持原样。
    MMM::Logic::stripMalodyMainAudioVolumeFields(fileData, mainAudioReferences);
    const std::string outputText = fileData.dump(4);
    return writePackageOutputFile(
        path,
        outputText.empty() ? nullptr : outputText.data(),
        outputText.size());
}

/// @brief 保存谱面，并仅在 MC 导出期间临时应用兼容选项。
/// @param beatMap 待保存谱面。
/// @param outputPath 输出路径。
/// @param malodyExportMode MC 导出时临时覆盖的 Malody 模式。
/// @param addStoreModeExtForMalodyExport 是否写入上架 mode_ext。
/// @param mainAudioReferencesWithoutVolume 需要省略 vol 的 Main
/// 音轨引用；为空时不处理。
/// @return 是否保存成功。
/// @details
/// 非 MC 或无兼容选项时直接使用标准保存器。MC 导出按值备份 MALODY 属性域，
/// 临时设置 mode 与 mode_ext，完成文件级修补后无条件恢复。Main vol 也只从
/// 最终文件移除，不改变活动谱面的采样音量。
bool saveBeatmapWithMalodyExportOptions(
    MMM::BeatMap& beatMap, const std::filesystem::path& outputPath,
    std::optional<MMM::MalodyMode>         malodyExportMode,
    bool                                   addStoreModeExtForMalodyExport,
    const std::unordered_set<std::string>* mainAudioReferencesWithoutVolume)
{
    // 所有保存先刷新当前项目的 song.file 提示，普通格式也得到一致资源引用。
    refreshCurrentProjectSongFileHint(beatMap);
    const bool shouldAddStoreModeExt =
        addStoreModeExtForMalodyExport &&
        (!malodyExportMode || *malodyExportMode == MMM::MalodyMode::Slide);
    // 商店 mode_ext 只适用于 Slide；调用方未指定模式时沿用谱面并允许写入。
    const bool shouldStripMainAudioVolume =
        mainAudioReferencesWithoutVolume &&
        !mainAudioReferencesWithoutVolume->empty();
    if ( !isMalodyChartPath(outputPath) ||
         (!malodyExportMode && !shouldAddStoreModeExt &&
          !shouldStripMainAudioVolume) ) {
        // 非 MC 或没有兼容选项时走原始保存路径，不触碰 Malody 私有元数据。
        return beatMap.saveToFile(outputPath);
    }

    auto previousPropsIt =
        beatMap.m_metadata.map_properties.find(MMM::MapMetadataType::MALODY);
    using MalodyPropertyMap =
        decltype(beatMap.m_metadata.map_properties)::mapped_type;
    std::optional<MalodyPropertyMap> previousProps;
    if ( previousPropsIt != beatMap.m_metadata.map_properties.end() ) {
        // 按值保存整个 MALODY 属性域，临时 mode/mode_ext 不能污染编辑中谱面。
        previousProps = previousPropsIt->second;
    }

    if ( malodyExportMode || shouldAddStoreModeExt ) {
        auto& props =
            beatMap.m_metadata.map_properties[MMM::MapMetadataType::MALODY];
        if ( malodyExportMode ) {
            // mode 使用协议整数的十进制字符串，保持 BeatMap 属性存储约定。
            props["mode"] =
                std::to_string(MMM::malodyModeValue(*malodyExportMode));
        }
        if ( shouldAddStoreModeExt ) {
            // 先写入内存元数据供标准 MC 保存器处理，随后再用 JSON patch 兜底。
            applyMalodyStoreModeExtMetadata(beatMap);
        }
    }

    bool ok = beatMap.saveToFile(outputPath);
    if ( ok && shouldAddStoreModeExt ) {
        // 标准写出成功后再确保 mode_ext 为 JSON 对象而不是错误字符串形式。
        ok = patchMalodyStoreModeExtFile(outputPath);
    }
    if ( ok && shouldStripMainAudioVolume ) {
        // Main vol 清理由最终 MC JSON 执行，避免影响 BeatMap 内存采样音量。
        ok = patchMalodyMainAudioVolumeFile(outputPath,
                                            *mainAudioReferencesWithoutVolume);
    }

    if ( previousProps ) {
        // 无论磁盘写出成功与否都恢复原属性，导出选项不成为会话编辑修改。
        beatMap.m_metadata.map_properties[MMM::MapMetadataType::MALODY] =
            std::move(*previousProps);
    } else {
        // 原先没有 MALODY 属性域时彻底移除临时创建的映射。
        beatMap.m_metadata.map_properties.erase(MMM::MapMetadataType::MALODY);
    }
    return ok;
}

/// @brief 读取源文件，并按需在 MC 字节中替换上架 mode_ext。
/// @param sourcePath 源文件路径。
/// @param addStoreModeExtForMalodyExport 是否写入上架 mode_ext。
/// @param outBytes 输出文件字节。
/// @return 是否读取成功。
/// @details
/// 非 MC 或选项关闭时保持源字节不变；需要修补时先读入独立输入容器，再把解析
/// 结果写入输出容器。解析失败不会把未经处理的 MC 误当作成功结果。
bool readPackageSourceFileWithOptionalMalodyStoreModeExt(
    const std::filesystem::path& sourcePath,
    bool addStoreModeExtForMalodyExport, std::vector<std::uint8_t>& outBytes)
{
    // 输入先读入独立容器，只有无需 patch 时才移动给输出避免额外复制。
    std::vector<std::uint8_t> inputBytes;
    if ( !readPackageSourceFile(sourcePath, inputBytes) ) {
        return false;
    }
    if ( !addStoreModeExtForMalodyExport || !isMalodyChartPath(sourcePath) ) {
        // 非 MC 文件绝不能按 JSON 重写，即使包级选项已开启。
        outBytes = std::move(inputBytes);
        return true;
    }
    return patchMalodyStoreModeExtBytes(inputBytes, outBytes);
}

/// @brief 取得打包格式要求的主谱面扩展名。
/// @param packageTypes 输出包格式对应的文件类型规则。
/// @return 带前导点的目标谱面扩展名。
/// @details
/// 扩展名列表顺序表达包格式偏好，首项是统一转换目标。规则意外为空时回退项目
/// 原生 .mmm，保证转换路径仍有明确格式，而不是生成无扩展名文件。
std::string getPackageBeatmapOutputExtension(
    const MMM::PackageSupportedFileTypes& packageTypes)
{
    // 未声明谱面扩展名的包格式回退项目原生 MMM，保证仍有可写出目标。
    if ( packageTypes.m_beatmapExtensions.empty() ) return ".mmm";
    // 包格式按优先级排列扩展名，转换统一采用首项作为主谱面格式。
    return std::string(packageTypes.m_beatmapExtensions.front());
}

/// @brief 判断谱面来源是否需要转换成当前包格式的谱面文件。
/// @param sourceExtension 来源文件扩展名。
/// @param outputExtension 目标谱面扩展名。
/// @return 是否需要在打包前转换。
/// @details
/// 只有已登记谱面格式参与转换。未知扩展由包类型验证负责拒绝，不能仅因与目标
/// 扩展不同就尝试交给 BeatMap 加载器并产生误导性的空谱面。
bool shouldConvertPackageBeatmapSource(const std::string& sourceExtension,
                                       const std::string& outputExtension)
{
    // 只转换已知谱面格式；未知文件即使扩展名不同也按普通资源原样打包。
    return MMM::isKnownPackageResourceExtension(
               MMM::PackageResourceType::Beatmap, sourceExtension) &&
           !MMM::packageExtensionEquals(sourceExtension, outputExtension);
}

/// @brief 生成临时转换谱面文件路径。
/// @param sourcePath 来源谱面路径。
/// @param outputExtension 转换后的谱面扩展名。
/// @return 临时文件路径，失败时为空路径。
/// @details
/// 文件名保留来源 stem 便于诊断，并附加单调时钟戳降低同轮碰撞概率。目标扩展名
/// 由包格式规则提供；临时目录不可用时明确失败，不回退污染项目目录。
std::filesystem::path makeTemporaryConvertedBeatmapPath(
    const std::filesystem::path& sourcePath, const std::string& outputExtension)
{
    std::error_code filesystemError;
    auto tempRoot = std::filesystem::temp_directory_path(filesystemError);
    // 系统临时目录不可用时让调用方中止转换，不回退写入项目目录。
    if ( filesystemError || tempRoot.empty() ) return {};

    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path fileName = sourcePath.stem();
    // 空 stem 使用固定可识别前缀，时间戳仍区分同轮多个转换文件。
    if ( fileName.empty() ) fileName = "mmm_package_map";
    fileName += "_converted_";
    fileName += std::to_string(stamp);
    fileName += outputExtension;
    return (tempRoot / fileName).lexically_normal();
}

/// @brief 生成 MCZ 主音频原点对齐使用的临时文件路径。
/// @param sourcePath 原始音频路径。
/// @return 保持源扩展名的唯一临时路径；失败时为空。
/// @details
/// 对齐服务依据扩展名选择音频容器，因此临时文件必须保留源扩展名。路径只负责
/// 命名，创建和删除由实际执行转码的资源分支成对管理。
std::filesystem::path makeTemporaryAlignedAudioPath(
    const std::filesystem::path& sourcePath)
{
    std::error_code filesystemError;
    auto tempRoot = std::filesystem::temp_directory_path(filesystemError);
    // 对齐产物只允许进入系统临时目录，失败时不覆盖源音频。
    if ( filesystemError || tempRoot.empty() ) return {};

    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path fileName = sourcePath.stem();
    // 保留源扩展名让编码器和后续包类型判断使用正确容器格式。
    if ( fileName.empty() ) fileName = "mmm_package_audio";
    fileName += "_origin_aligned_";
    fileName += std::to_string(stamp);
    fileName += sourcePath.extension();
    return (tempRoot / fileName).lexically_normal();
}

/// @brief 将谱面源文件转换成指定路径的目标格式文件。
/// @param sourcePath 来源谱面路径。
/// @param project 谱面所属项目。
/// @param outputPath 转换后输出路径。
/// @param metadataOverride 转换时覆盖的基础谱面元数据；为空则使用源谱面元数据。
/// @param malodyExportMode MC 转换产物临时使用的 Malody 模式。
/// @param addStoreModeExtForMalodyExport 是否为 MC 转换产物写入上架 mode_ext。
/// @param stripMainAudioVolumeFromMalodyExport 是否删除 Main 自动采样的 vol
/// 字段。
/// @param audioAlignmentPlan MCZ 导出副本使用的音频原点对齐计划；为空则不对齐。
/// @return 是否转换成功。
/// @details
/// 来源始终加载到独立 BeatMap。Main 引用必须在元数据覆盖前按来源路径解析，
/// 否则覆盖后的 map_path 会改变相对资源含义。对齐与格式私有选项依次作用于
/// 副本，最后由输出扩展名选择保存器，整个过程不修改活动会话。
bool convertPackageBeatmapFile(
    const MMM::Project& project, const std::filesystem::path& sourcePath,
    const std::filesystem::path&        outputPath,
    const MMM::BaseMapMeta*             metadataOverride,
    std::optional<MMM::MalodyMode>      malodyExportMode,
    bool                                addStoreModeExtForMalodyExport,
    bool                                stripMainAudioVolumeFromMalodyExport,
    const MczBeatmapAudioAlignmentPlan* audioAlignmentPlan)
{
    // 转换始终载入独立 BeatMap 值副本，任何导出覆盖都不会改写活动会话对象。
    auto beatMap = MMM::BeatMap::loadFromFile(sourcePath);
    // 加载器以空 map_path 表示无法识别或读取来源，不能继续生成空谱面产物。
    if ( beatMap.m_baseMapMetadata.map_path.empty() ) return false;
    // Main 引用必须在覆盖元数据和对齐之前按来源路径解析，保留原字段文本。
    const auto mainAudioReferences =
        stripMainAudioVolumeFromMalodyExport
            ? collectMalodyMainAudioSampleReferences(
                  project, beatMap, sourcePath)
            : std::unordered_set<std::string>{};
    if ( metadataOverride ) {
        // 包级元数据覆盖只替换基础信息，物件、Timing 和格式私有属性保持来源值。
        beatMap.m_baseMapMetadata = *metadataOverride;
    }
    if ( audioAlignmentPlan ) {
        // 对齐仅作用于本次导出副本，并使用计划中预先验证的一致相位和引用集合。
        std::string alignmentError;
        if ( !MMM::Logic::applyMczAudioOriginAlignment(
                 beatMap,
                 audioAlignmentPlan->mainAudioReferences,
                 audioAlignmentPlan->phaseMilliseconds,
                 alignmentError) ) {
            XERROR("MCZ audio alignment failed for '{}': {}",
                   MMM::Config::pathToUtf8(sourcePath),
                   alignmentError);
            return false;
        }
    }
    // 输出路径写入副本元数据，保存器可据此选择格式并建立相对资源上下文。
    beatMap.m_baseMapMetadata.map_path = outputPath;
    return saveBeatmapWithMalodyExportOptions(
        beatMap,
        outputPath,
        malodyExportMode,
        addStoreModeExtForMalodyExport,
        stripMainAudioVolumeFromMalodyExport ? &mainAudioReferences : nullptr);
}

/// @brief 读取转换后的目标谱面字节。
/// @param sourcePath 来源谱面路径。
/// @param project 谱面所属项目。
/// @param projectOutputPath 保存到项目中时使用的目标路径。
/// @param outputExtension 目标谱面扩展名。
/// @param saveToProject 是否将转换产物留在项目目录中。
/// @param metadataOverride 转换时覆盖的基础谱面元数据；为空则使用源谱面元数据。
/// @param malodyExportMode MC 转换产物临时使用的 Malody 模式。
/// @param addStoreModeExtForMalodyExport 是否为 MC 转换产物写入上架 mode_ext。
/// @param stripMainAudioVolumeFromMalodyExport 是否删除 Main 自动采样的 vol
/// 字段。
/// @param audioAlignmentPlan MCZ 导出副本使用的音频原点对齐计划；为空则不对齐。
/// @param outBytes 输出文件字节。
/// @return 是否成功读取转换结果。
/// @details
/// saveToProject 决定文件生命周期：启用时创建项目目标父目录并保留产物；禁用时
/// 使用系统临时目录并在读取后回收。两条路径都要求转换与完整读取全部成功。
bool readConvertedPackageBeatmapBytes(
    const MMM::Project& project, const std::filesystem::path& sourcePath,
    const std::filesystem::path& projectOutputPath,
    const std::string& outputExtension, bool saveToProject,
    const MMM::BaseMapMeta*             metadataOverride,
    std::optional<MMM::MalodyMode>      malodyExportMode,
    bool                                addStoreModeExtForMalodyExport,
    bool                                stripMainAudioVolumeFromMalodyExport,
    const MczBeatmapAudioAlignmentPlan* audioAlignmentPlan,
    std::vector<std::uint8_t>&          outBytes)
{
    // 用户选择保存转换产物时写入项目目标，否则使用唯一临时文件承接转换。
    const auto conversionPath =
        saveToProject
            ? projectOutputPath
            : makeTemporaryConvertedBeatmapPath(sourcePath, outputExtension);
    // 临时目录规划失败时不尝试使用空路径或当前工作目录。
    if ( conversionPath.empty() ) return false;

    if ( saveToProject ) {
        // 项目目标允许创建缺失父目录；临时目录由系统根保证已存在。
        std::error_code filesystemError;
        const auto      parentPath = conversionPath.parent_path();
        if ( !parentPath.empty() ) {
            std::filesystem::create_directories(parentPath, filesystemError);
            // 创建失败前尚未写出文件，可以直接报告转换失败。
            if ( filesystemError ) return false;
        }
    }

    if ( !convertPackageBeatmapFile(project,
                                    sourcePath,
                                    conversionPath,
                                    metadataOverride,
                                    malodyExportMode,
                                    addStoreModeExtForMalodyExport,
                                    stripMainAudioVolumeFromMalodyExport,
                                    audioAlignmentPlan) ) {
        if ( !saveToProject ) {
            // 转换器可能留下部分临时文件，失败路径尽力删除且不掩盖原错误。
            std::error_code removeError;
            std::filesystem::remove(conversionPath, removeError);
        }
        return false;
    }

    const bool readOk = readPackageSourceFile(conversionPath, outBytes);
    if ( !saveToProject ) {
        // 临时产物无论读取成功与否都只服务本次归档，读取后立即尽力回收。
        std::error_code removeError;
        std::filesystem::remove(conversionPath, removeError);
    }
    // 保存到项目的转换文件保留在目标位置，同时返回其字节用于当前包写入。
    return readOk;
}

/// @brief 将文件字节写入 zip 包，重复包内路径会自动跳过。
/// @param zipArchive 正在写入的 zip 归档。
/// @param archivedNames 已写入的包内路径集合。
/// @param archiveRelativePath 包内相对路径。
/// @param fileBytes 待写入的文件字节。
/// @param sourceRelativeUtf8 日志中使用的来源项目相对路径。
/// @return 写入成功或因重复路径跳过时返回 true。
/// @details
/// archivedNames 同时承担去重与计划登记。重复路径按首项获胜；新路径在调用
/// miniz 前登记，写入失败时外层会放弃整个归档，因此无需回滚集合。空文件使用
/// 空指针和零长度，符合 miniz 内存条目接口契约。
bool addPackageArchiveBytes(mz_zip_archive&                  zipArchive,
                            std::unordered_set<std::string>& archivedNames,
                            const std::filesystem::path& archiveRelativePath,
                            const std::vector<std::uint8_t>& fileBytes,
                            const std::string&               sourceRelativeUtf8)
{
    // zip 条目统一使用通用 UTF-8 分隔符，不暴露 Windows 反斜杠到包协议。
    std::string archiveName =
        MMM::Config::pathToUtf8Generic(archiveRelativePath);
    if ( archiveName.empty() ) {
        // 空名称会让 miniz 产生无效条目，日志保留原项目相对路径便于定位。
        XERROR("PackBeatmap: empty archive path: {}", sourceRelativeUtf8);
        return false;
    }
    if ( !archivedNames.insert(archiveName).second ) {
        // 多个来源映射到同一目标扩展名时首个写入获胜，后续重复安全跳过。
        return true;
    }

    const void* fileData = fileBytes.empty() ? nullptr : fileBytes.data();
    // miniz 接受空文件的空指针与零长度，非空容器在调用期间保持地址稳定。
    if ( !mz_zip_writer_add_mem(&zipArchive,
                                archiveName.c_str(),
                                fileData,
                                fileBytes.size(),
                                MZ_DEFAULT_COMPRESSION) ) {
        XERROR("PackBeatmap: failed to add file to archive: {}",
               sourceRelativeUtf8);
        return false;
    }
    // archivedNames 已在写入前登记；失败时整个归档会被放弃，不需回滚集合。
    return true;
}

/// @brief 判断包内路径是否已在指定集合中。
/// @param archivedNames 包内路径集合。
/// @param archiveRelativePath 待检查的包内相对路径。
/// @return 已存在时返回 true。
bool hasPackageArchivePath(const std::unordered_set<std::string>& archivedNames,
                           const std::filesystem::path& archiveRelativePath)
{
    // 查询采用与写入完全相同的 UTF-8 规范化，避免分隔符差异造成重复条目。
    const std::string archiveName =
        MMM::Config::pathToUtf8Generic(archiveRelativePath);
    return !archiveName.empty() &&
           archivedNames.find(archiveName) != archivedNames.end();
}

/// @brief 将项目相对路径规范化为用于匹配打包元数据覆盖项的 UTF-8 路径。
/// @param relativePath 项目相对路径。
/// @return 规范化后的通用分隔符路径。
std::string normalizePackageRelativePathKey(
    const std::filesystem::path& relativePath)
{
    // 词法规范化折叠点目录但不访问文件系统，适合包计划中的纯身份比较。
    return MMM::Config::pathToUtf8Generic(relativePath.lexically_normal());
}

/// @brief 规范化项目资源身份键，并在 Windows 上兼容文件系统大小写不敏感语义。
/// @param relativePath 项目资源相对路径。
/// @return 用于资源计划和选择匹配的稳定路径键。
/// @details
/// 先统一点目录与分隔符，再按平台文件系统语义处理大小写。转换只覆盖 ASCII，
/// 避免对 UTF-8 多字节序列做区域相关的逐字节大小写操作。
std::string normalizePackageResourceIdentityKey(
    const std::filesystem::path& relativePath)
{
    auto key = normalizePackageRelativePathKey(relativePath);
#if defined(_WIN32)
    // Windows 项目路径按 ASCII 大小写不敏感比较，避免同一资源出现两份计划。
    // 只处理协议常见 ASCII 路径部分，不对 UTF-8 多字节执行区域相关转换。
    std::transform(
        key.begin(), key.end(), key.begin(), [](unsigned char value) {
            return value >= 'A' && value <= 'Z'
                       ? static_cast<char>(value - 'A' + 'a')
                       : static_cast<char>(value);
        });
#endif
    // POSIX 保留原大小写，不把实际可并存的两个资源错误合并。
    return key;
}

/// @brief 构建打包元数据覆盖项查询表。
/// @param metadataOverrides 命令中携带的元数据覆盖项。
/// @return 项目相对路径到基础元数据的映射。
/// @details
/// 命令中的 UTF-8 路径转换为与选择列表一致的规范键，避免点目录或分隔符差异。
/// 同一路径重复出现时保留最后一项，对应 UI 最终编辑快照覆盖旧值。
std::unordered_map<std::string, MMM::BaseMapMeta>
makePackageMetadataOverrideMap(
    const std::vector<MMM::Logic::PackageBeatmapMetadataOverride>&
        metadataOverrides)
{
    // 查询表按覆盖项数量预留，避免构建包计划期间反复扩容。
    std::unordered_map<std::string, MMM::BaseMapMeta> result;
    result.reserve(metadataOverrides.size());
    for ( const auto& metadataOverride : metadataOverrides ) {
        // 命令使用 UTF-8 项目相对路径，转为与选择列表一致的规范键。
        auto relativePath =
            MMM::Config::utf8ToPath(metadataOverride.relativePath);
        result[normalizePackageRelativePathKey(relativePath)] =
            metadataOverride.baseMeta;
        // 重复路径以后出现的覆盖为准，符合 UI 最终编辑值覆盖旧快照的语义。
    }
    return result;
}

/// @brief 构建已选原始 IMD 谱面在包内的路径集合。
/// @param selectedRelativePaths 需要打包的项目相对路径列表。
/// @return 已选 IMD 源文件对应的包内路径集合。
/// @details
/// 集合只用于 legacy IMD 冲突仲裁，不读取文件。用户明确选择的 IMD 应优先于
/// 从同名其他谱面自动生成的兼容副本。
std::unordered_set<std::string> makeSelectedImdArchiveNameSet(
    const std::vector<std::string>& selectedRelativePaths)
{
    std::unordered_set<std::string> result;
    for ( const auto& relativeUtf8 : selectedRelativePaths ) {
        // 先规范化再判断扩展名，集合内容与实际归档条目名称保持一致。
        const auto relativePath =
            MMM::Config::utf8ToPath(relativeUtf8).lexically_normal();
        const auto extension =
            MMM::Config::pathToUtf8(relativePath.extension());
        if ( MMM::packageExtensionEquals(extension, ".imd") ) {
            // 只记录用户明确选择的原始 IMD，用于避免自动生成兼容副本覆盖它。
            result.insert(MMM::Config::pathToUtf8Generic(relativePath));
        }
    }
    return result;
}

/// @brief 写入 zip 兼容的谱面包。
/// @param project 当前项目及其资源分类。
/// @param outputPath 输出包路径。
/// @param selectedRelativePaths 需要打包的项目相对路径列表。
/// @param packageTypes 输出包格式对应的文件类型规则。
/// @param metadataOverrides 转换指定谱面时临时覆盖的基础元数据列表。
/// @param saveConvertedBeatmapsToProject 是否将转换后的谱面文件保存回项目目录。
/// @param includeLegacyImdBeatmapsInPackage 是否额外写入旧皮肤兼容的 IMD 谱面。
/// @param malodyExportMode MCZ 包内 MC 谱面统一使用的 Malody 模式。
/// @param addStoreModeExtForMalodyExport 是否为写出的 MC 谱面写入上架
/// mode_ext。
/// @param stripMainAudioVolumeFromMalodyExport 是否删除 Main 自动采样的 vol
/// 字段。
/// @param alignNonOggMainAudioToOrigin 是否在 MCZ 中对齐非 OGG Main
/// 音频与首红线。
/// @return 是否打包成功。
/// @details
/// 该函数只接收项目相对路径，并把项目根作为唯一文件系统解析基准。选择项不会
/// 递归展开目录，也不会自动补入谱面引用的资源；调用方必须传入完整的显式选择。
/// 这样可以让打包界面展示的资源集合与最终归档内容保持一致。
///
/// 包格式规则由 PackageSupportedFileTypes 提供。首个谱面扩展名是转换目标，
/// 其他已知谱面扩展名需要先加载为 BeatMap 再写成目标格式。音频和图片保持原始
/// 字节，除非 MCZ 的非 OGG Main 原点对齐选项明确要求生成临时音频。
///
/// metadataOverrides 以来源项目相对路径为键。覆盖只用于本次转换副本，不写回
/// 项目中的源谱面；来源已经是目标格式但存在结构化覆盖时，同样必须走重新编码，
/// 不能把原始文件字节直接加入归档。
///
/// MCZ 音频对齐具有跨谱面约束：同一 Main 资源可被多张谱面共享，但所有谱面
/// 必须计算出相同的一拍内相位。相位不同意味着无法用一份包内音频同时满足它们，
/// 因此预检整体失败，不选择任意一张谱面的结果覆盖其他谱面。
///
/// 零相位仍会保留谱面对齐计划，以便规范化首红线和 Main 自动采样；音频本身
/// 不做有损重编码。非零相位才登记到 alignmentResourcesByAudioPath，并在普通
/// 资源分支中用对齐后的临时字节替换对应归档条目。
///
/// legacy IMD 是 MCZ 的附加兼容条目。用户明确选择的原始 IMD 优先于从其他
/// 格式自动生成的同路径 IMD，防止转换结果静默覆盖用户指定文件。重复的普通
/// 路径同样由 archivedNames 保证首个写入获胜。
///
/// miniz writer 的生命周期覆盖整个事务。任何预检或条目写入失败都会结束 writer
/// 并放弃内存归档；只有 finalize 成功后才取得 archiveBuffer。缓冲写入目标后
/// 必须用 mz_free 释放，不能交给 C++ allocator 或容器管理。
bool writeBeatmapPackage(
    const MMM::Project& project, const std::filesystem::path& outputPath,
    const std::vector<std::string>&       selectedRelativePaths,
    const MMM::PackageSupportedFileTypes& packageTypes,
    const std::vector<MMM::Logic::PackageBeatmapMetadataOverride>&
         metadataOverrides,
    bool saveConvertedBeatmapsToProject, bool includeLegacyImdBeatmapsInPackage,
    std::optional<MMM::MalodyMode> malodyExportMode,
    bool                           addStoreModeExtForMalodyExport,
    bool                           stripMainAudioVolumeFromMalodyExport,
    bool                           alignNonOggMainAudioToOrigin)
{
    // 空选择无法形成有意义谱面包，也避免创建只有 zip 尾记录的空文件。
    if ( selectedRelativePaths.empty() ) return false;
    const auto& projectRoot = project.m_projectRoot;

    // 使用内存 zip 先完成全部条目，最终成功后再一次性写出目标文件。
    mz_zip_archive zipArchive{};
    if ( !mz_zip_writer_init_heap(&zipArchive, 0, 0) ) {
        // 初始化失败尚未取得 miniz 资源，无需调用 writer_end。
        return false;
    }

    bool                            success = true;
    std::vector<std::uint8_t>       fileBytes;
    std::unordered_set<std::string> archivedNames;
    // 包格式首选谱面扩展名决定所有异格式来源的转换目标。
    const std::string packageBeatmapExtension =
        getPackageBeatmapOutputExtension(packageTypes);
    const bool includeLegacyImdBeatmaps =
        includeLegacyImdBeatmapsInPackage &&
        MMM::packageExtensionEquals(packageTypes.m_packageExtension, ".mcz");
    // Malody 专有兼容选项仅能作用于 MCZ，其他 zip 包保持原格式内容。
    const bool addStoreModeExtToMc =
        addStoreModeExtForMalodyExport &&
        MMM::packageExtensionEquals(packageTypes.m_packageExtension, ".mcz");
    const bool stripMainAudioVolumeFromMc =
        stripMainAudioVolumeFromMalodyExport &&
        MMM::packageExtensionEquals(packageTypes.m_packageExtension, ".mcz");
    const auto packageMalodyExportMode =
        MMM::packageExtensionEquals(packageTypes.m_packageExtension, ".mcz")
            ? malodyExportMode
            : std::nullopt;
    const auto selectedImdArchiveNames =
        includeLegacyImdBeatmaps
            ? makeSelectedImdArchiveNameSet(selectedRelativePaths)
            : std::unordered_set<std::string>{};
    // 覆盖表在主循环外建立，避免每个谱面线性查找命令列表。
    const auto metadataOverrideMap =
        makePackageMetadataOverrideMap(metadataOverrides);
    const bool alignNonOggMainAudio =
        alignNonOggMainAudioToOrigin &&
        MMM::packageExtensionEquals(packageTypes.m_packageExtension, ".mcz");
    std::unordered_map<std::string, MczBeatmapAudioAlignmentPlan>
                                            alignmentPlansByBeatmapPath;
    std::unordered_map<std::string, double> alignmentPhaseByAudioPath;
    std::unordered_map<std::string, const MMM::AudioResource*>
        alignmentResourcesByAudioPath;
    if ( alignNonOggMainAudio ) {
        // 第一阶段建立全局选择身份集合。
        //
        // 对齐依赖不能通过“项目里存在”隐式满足，因为资源选择是用户对包内容
        // 的明确声明。使用规范化身份键可在 Windows 上把仅大小写不同的路径视为
        // 同一个文件，而 POSIX 仍保持大小写敏感。
        // 对齐预检先于任何 zip 条目写入，保证共享音频相位冲突可整体失败。
        std::unordered_set<std::string> selectedRelativePathKeys;
        selectedRelativePathKeys.reserve(selectedRelativePaths.size());
        for ( const auto& relativeUtf8 : selectedRelativePaths ) {
            // 所有选中资源先建立大小写语义一致的身份集合，供音频依赖校验。
            const auto relativePath =
                MMM::Config::utf8ToPath(relativeUtf8).lexically_normal();
            if ( packageRelativePathEscapesRoot(relativePath) ) {
                // 预检遇到越界路径立即结束 miniz，不能等待主写入循环再失败。
                XERROR("PackBeatmap: path escapes project root: {}",
                       relativeUtf8);
                mz_zip_writer_end(&zipArchive);
                return false;
            }
            selectedRelativePathKeys.emplace(
                normalizePackageResourceIdentityKey(relativePath));
        }
        for ( const auto& relativeUtf8 : selectedRelativePaths ) {
            // 第二阶段只遍历谱面来源并建立逐谱面计划。
            //
            // 此处加载的 BeatMap 是预检副本，既不进入活动 Session，也不更新
            // 项目入口。计划中借用的 AudioResource 指针在函数返回前始终由传入
            // project 的稳定资源数组持有。
            const auto relativePath =
                MMM::Config::utf8ToPath(relativeUtf8).lexically_normal();
            const auto extension =
                MMM::Config::pathToUtf8(relativePath.extension());
            if ( !MMM::isKnownPackageResourceExtension(
                     MMM::PackageResourceType::Beatmap, extension) ) {
                // 对齐计划只由谱面建立，音频和图片资源在依赖校验中被引用。
                continue;
            }

            const auto sourcePath =
                (projectRoot / relativePath).lexically_normal();
            auto beatMap = MMM::BeatMap::loadFromFile(sourcePath);
            if ( beatMap.m_baseMapMetadata.map_path.empty() ) {
                // 任一选中谱面无法加载都使对齐包不完整，预检阶段整体终止。
                XERROR(
                    "PackBeatmap: failed to load source for audio alignment: "
                    "{}",
                    relativeUtf8);
                mz_zip_writer_end(&zipArchive);
                return false;
            }
            auto planResult =
                makeMczAudioAlignmentPlan(project, beatMap, sourcePath);
            if ( !planResult.errorMessage.empty() ) {
                // 非 OGG Main 无法建立安全计划时不降级为未对齐输出。
                XERROR("PackBeatmap: cannot align '{}': {}",
                       relativeUtf8,
                       planResult.errorMessage);
                mz_zip_writer_end(&zipArchive);
                return false;
            }
            // OGG 或无需对齐的谱面返回空计划，仍可在主循环正常写入。
            if ( !planResult.plan ) continue;

            const auto audioRelativePath =
                MMM::Config::utf8ToPath(planResult.plan->resource->m_path)
                    .lexically_normal();
            const auto audioPathKey =
                normalizePackageResourceIdentityKey(audioRelativePath);
            if ( packageRelativePathEscapesRoot(audioRelativePath) ||
                 !selectedRelativePathKeys.contains(audioPathKey) ) {
                // 对齐后的谱面必须引用同包中明确选择的 Main
                // 音频，禁止隐式漏包。
                XERROR(
                    "PackBeatmap: aligned Main audio '{}' is not a selected "
                    "project resource",
                    MMM::Config::pathToUtf8(audioRelativePath));
                mz_zip_writer_end(&zipArchive);
                return false;
            }
            if ( const auto phase =
                     alignmentPhaseByAudioPath.find(audioPathKey);
                 phase != alignmentPhaseByAudioPath.end() &&
                 std::abs(phase->second - planResult.plan->phaseMilliseconds) >
                     1.0e-6 ) {
                // 同一物理音频只能生成一份对齐产物，不同谱面要求不同相位时拒绝。
                XERROR(
                    "PackBeatmap: Main audio '{}' is shared by charts with "
                    "different first-BPM phases",
                    MMM::Config::pathToUtf8(audioRelativePath));
                mz_zip_writer_end(&zipArchive);
                return false;
            }
            alignmentPhaseByAudioPath[audioPathKey] =
                planResult.plan->phaseMilliseconds;
            // 音频路径表表达“每个物理资源最多一份相位”的包级不变量。
            // 浮点比较使用很小容差，仅吸收等价计算的舍入误差；真正不同的首红线
            // 相位不能合并，否则至少一张谱面的节奏原点会产生可听偏移。
            // 相位已为零时原音频本身已经从歌曲原点播放，只规范谱面首红线，
            // 避免没有必要的有损重编码。
            if ( std::abs(planResult.plan->phaseMilliseconds) > 1.0e-6 ) {
                // 非零相位才需要转码音频；零相位仍保存谱面计划以规范首红线。
                alignmentResourcesByAudioPath[audioPathKey] =
                    planResult.plan->resource;
            }
            alignmentPlansByBeatmapPath.emplace(
                // 每张谱面以项目相对路径关联自己的引用集合和统一音频相位。
                normalizePackageRelativePathKey(relativePath),
                std::move(*planResult.plan));
        }
    }
    // 第三阶段按选择顺序生成归档条目。
    //
    // 预检只覆盖可选的音频对齐约束，主循环仍重新执行通用路径和扩展名验证，
    // 保证关闭对齐选项时具有相同的安全边界。遇到首个失败即退出，避免继续进行
    // 昂贵转换并让日志中的根因被后续连锁错误淹没。
    for ( const auto& relativeUtf8 : selectedRelativePaths ) {
        // 主循环严格按用户选择顺序规划归档；archivedNames
        // 解决转换后的路径碰撞。
        const auto relativePath =
            MMM::Config::utf8ToPath(relativeUtf8).lexically_normal();
        const auto relativePathKey =
            normalizePackageRelativePathKey(relativePath);
        if ( packageRelativePathEscapesRoot(relativePath) ) {
            // 即使预检选项关闭，所有实际写入路径仍必须执行根目录逃逸检查。
            XERROR("PackBeatmap: path escapes project root: {}", relativeUtf8);
            success = false;
            break;
        }

        const auto extension =
            MMM::Config::pathToUtf8(relativePath.extension());
        if ( !isPackageCandidateExtensionSupported(packageTypes, extension) ) {
            // 包格式只接收声明支持的谱面、音频和图片扩展，未知资源不静默夹带。
            XERROR("PackBeatmap: unsupported file extension: {}", relativeUtf8);
            success = false;
            break;
        }

        const auto sourcePath = (projectRoot / relativePath).lexically_normal();
        std::error_code filesystemError;
        if ( !std::filesystem::is_regular_file(sourcePath, filesystemError) ||
             filesystemError ) {
            // 选择列表中的每个条目都必须是可读普通文件，目录不能作为递归入口。
            XERROR("PackBeatmap: source file not found: {}", relativeUtf8);
            success = false;
            break;
        }

        const bool isBeatmapSource = MMM::isKnownPackageResourceExtension(
            MMM::PackageResourceType::Beatmap, extension);
        if ( isBeatmapSource ) {
            // 谱面条目有三种准备方式：
            //
            // - 无任何结构化变换时直接读取源文件；
            // - 格式、Malody 选项或对齐要求变化时写入临时转换文件后读取；
            // - 真实格式转换且用户要求保留时写入项目目标，并复用其字节打包。
            //
            // 三者最终都汇合到 addPackageArchiveBytes，因此重复路径、UTF-8 名称
            // 和压缩失败处理保持一致。
            // 谱面可能需要改扩展名转换、Malody 重写、元数据覆盖或音频相位对齐。
            auto       targetArchivePath   = relativePath;
            const bool shouldConvertSource = shouldConvertPackageBeatmapSource(
                extension, packageBeatmapExtension);
            if ( shouldConvertSource ) {
                // 只替换扩展名并保留项目内目录，相关资源相对关系尽量保持不变。
                targetArchivePath.replace_extension(packageBeatmapExtension);
            }
            const auto alignmentPlanIt =
                alignmentPlansByBeatmapPath.find(relativePathKey);
            const MczBeatmapAudioAlignmentPlan* alignmentPlan =
                alignmentPlanIt == alignmentPlansByBeatmapPath.end()
                    ? nullptr
                    : &alignmentPlanIt->second;
            // 任一结构化改写都要求重新载入并保存谱面，不能原样复制源字节。
            const bool shouldReencode =
                shouldConvertSource || packageMalodyExportMode.has_value() ||
                stripMainAudioVolumeFromMc || alignmentPlan != nullptr;

            const auto metadataIt = metadataOverrideMap.find(relativePathKey);
            const MMM::BaseMapMeta* metadataOverride =
                metadataIt == metadataOverrideMap.end() ? nullptr
                                                        : &metadataIt->second;
            // 覆盖指针借用预构建映射，只在本次 writeBeatmapPackage 调用内使用。

            if ( !hasPackageArchivePath(archivedNames, targetArchivePath) ) {
                // 目标路径尚未写入才读取或转换，重复转换来源不会浪费磁盘和
                // CPU。
                if ( shouldReencode ) {
                    const auto projectOutputPath =
                        (projectRoot / targetArchivePath).lexically_normal();
                    // 只有格式确实转换且用户选择保存回项目时保留产物；仅重写
                    // Malody 选项仍使用临时文件，避免覆盖源谱面。
                    if ( !readConvertedPackageBeatmapBytes(
                             project,
                             sourcePath,
                             projectOutputPath,
                             packageBeatmapExtension,
                             saveConvertedBeatmapsToProject &&
                                 shouldConvertSource,
                             metadataOverride,
                             packageMalodyExportMode,
                             addStoreModeExtToMc,
                             stripMainAudioVolumeFromMc,
                             alignmentPlan,
                             fileBytes) ) {
                        XERROR("PackBeatmap: failed to convert source file: {}",
                               relativeUtf8);
                        success = false;
                        break;
                    }
                } else if (
                    !readPackageSourceFileWithOptionalMalodyStoreModeExt(
                        sourcePath, addStoreModeExtToMc, fileBytes) ) {
                    // 原样路径仍可按包级商店选项轻量 patch MC，其余文件纯读取。
                    XERROR("PackBeatmap: failed to read source file: {}",
                           relativeUtf8);
                    success = false;
                    break;
                }

                if ( !addPackageArchiveBytes(zipArchive,
                                             archivedNames,
                                             targetArchivePath,
                                             fileBytes,
                                             relativeUtf8) ) {
                    // miniz 写入失败后不继续产生部分包，统一由尾部丢弃归档。
                    success = false;
                    break;
                }
            }

            if ( includeLegacyImdBeatmaps ) {
                // 兼容 IMD 与主目标谱面是两个独立归档条目。
                // 主条目成功不代表 IMD 可生成；兼容转换失败仍会让整个包失败，
                // 因为用户已经明确启用“包含旧版 IMD”，不能静默降级遗漏文件。
                // MCZ 可额外携带同目录同名 IMD，供旧皮肤或旧客户端兼容读取。
                auto imdArchivePath = relativePath;
                imdArchivePath.replace_extension(".imd");
                const bool sourceIsImd =
                    MMM::packageExtensionEquals(extension, ".imd");
                const bool rawImdSelected =
                    !sourceIsImd &&
                    hasPackageArchivePath(selectedImdArchiveNames,
                                          imdArchivePath);
                // 用户明确选择的原始 IMD 优先；自动转换不能抢占其包内路径。
                if ( !rawImdSelected &&
                     !hasPackageArchivePath(archivedNames, imdArchivePath) ) {
                    if ( sourceIsImd ) {
                        // 来源已经是 IMD 时无需再次转换，直接保留原始字节。
                        if ( !readPackageSourceFile(sourcePath, fileBytes) ) {
                            XERROR(
                                "PackBeatmap: failed to read source file: {}",
                                relativeUtf8);
                            success = false;
                            break;
                        }
                    } else {
                        // 非 IMD 谱面生成临时兼容副本，不保存回项目目录。
                        const auto projectOutputPath =
                            (projectRoot / imdArchivePath).lexically_normal();
                        if ( !readConvertedPackageBeatmapBytes(
                                 project,
                                 sourcePath,
                                 projectOutputPath,
                                 ".imd",
                                 false,
                                 nullptr,
                                 std::nullopt,
                                 false,
                                 false,
                                 nullptr,
                                 fileBytes) ) {
                            XERROR(
                                "PackBeatmap: failed to convert legacy IMD "
                                "file: {}",
                                relativeUtf8);
                            success = false;
                            break;
                        }
                    }

                    if ( !addPackageArchiveBytes(zipArchive,
                                                 archivedNames,
                                                 imdArchivePath,
                                                 fileBytes,
                                                 relativeUtf8) ) {
                        success = false;
                        break;
                    }
                }
            }
            // 谱面及可选兼容副本已经处理，不能落入普通资源写入分支。
            continue;
        }

        if ( hasPackageArchivePath(archivedNames, relativePath) ) {
            // 谱面转换或先前选择可能已经占用同路径，重复普通资源直接跳过。
            continue;
        }
        const auto resourceIdentityKey =
            normalizePackageResourceIdentityKey(relativePath);
        const auto alignmentResource =
            alignmentResourcesByAudioPath.find(resourceIdentityKey);
        if ( alignmentResource != alignmentResourcesByAudioPath.end() ) {
            // 非 OGG Main 的归档名称保持项目原路径，只有内容替换成对齐产物。
            // 谱面中的资源引用因此无需改名，媒体容器扩展名也与编码服务输出一致。
            // 临时文件只跨越转码与完整读取两个步骤，随后无论成功失败都尽力删除。
            // 预检标记的非 OGG Main 需要先生成原点对齐临时音频再写入原包路径。
            const auto alignedAudioPath =
                makeTemporaryAlignedAudioPath(sourcePath);
            if ( alignedAudioPath.empty() ) {
                // 临时路径失败不会回退覆盖项目源音频。
                XERROR("PackBeatmap: failed to create aligned audio path: {}",
                       relativeUtf8);
                success = false;
                break;
            }
            const auto phase =
                alignmentPhaseByAudioPath.find(resourceIdentityKey);
            if ( phase == alignmentPhaseByAudioPath.end() ) {
                // 资源与相位表必须成对建立，缺失表示内部计划不完整，整体中止。
                XERROR("PackBeatmap: incomplete Main audio alignment plan: {}",
                       relativeUtf8);
                std::error_code removeError;
                std::filesystem::remove(alignedAudioPath, removeError);
                success = false;
                break;
            }
            const auto alignmentResult =
                MMM::Audio::AudioOriginAlignmentService::alignToOrigin({
                    .inputPath         = sourcePath,
                    .outputPath        = alignedAudioPath,
                    .phaseMilliseconds = phase->second,
                });
            // 转码成功后立即读入归档字节；任一阶段失败都删除临时文件。
            if ( !alignmentResult.success ||
                 !readPackageSourceFile(alignedAudioPath, fileBytes) ) {
                XERROR("PackBeatmap: failed to align Main audio '{}': {}",
                       relativeUtf8,
                       alignmentResult.errorMessage);
                std::error_code removeError;
                std::filesystem::remove(alignedAudioPath, removeError);
                success = false;
                break;
            }
            std::error_code removeError;
            // 文件字节已复制到内存，临时音频可在 zip 写入前回收。
            std::filesystem::remove(alignedAudioPath, removeError);
        } else if ( !readPackageSourceFile(sourcePath, fileBytes) ) {
            // 不参与对齐的音频、图片等资源保持原始字节。
            XERROR("PackBeatmap: failed to read source file: {}", relativeUtf8);
            success = false;
            break;
        }

        if ( !addPackageArchiveBytes(zipArchive,
                                     archivedNames,
                                     relativePath,
                                     fileBytes,
                                     relativeUtf8) ) {
            // 普通资源沿项目相对路径写入，保持谱面中的相对引用可解析。
            success = false;
            break;
        }
    }

    void*       archiveBuffer = nullptr;
    std::size_t archiveSize   = 0;
    // 第四阶段提交完整内存归档。
    //
    // finalize 失败时 archiveBuffer 的状态由 miniz 管理，writer_end
    // 仍必须调用。 finalize 成功后先结束
    // writer，再把独立缓冲写入文件；此时所有输入资源和
    // 临时转换文件都已不再需要，目标写入是事务唯一的外部提交点。
    // 只有所有条目成功后才完成内存 zip，失败路径不生成可被误用的部分包。
    if ( success && !mz_zip_writer_finalize_heap_archive(
                        &zipArchive, &archiveBuffer, &archiveSize) ) {
        success = false;
    }

    mz_zip_writer_end(&zipArchive);

    if ( success ) {
        // finalize 返回的缓冲在写出完成前保持有效，目标文件由统一助手截断写入。
        success =
            writePackageOutputFile(outputPath, archiveBuffer, archiveSize);
    }
    if ( archiveBuffer ) {
        // miniz 分配的 heap archive 必须由 mz_free 释放，与 std::vector 无关。
        mz_free(archiveBuffer);
    }
    return success;
}
}  // namespace

namespace MMM::Logic
{

/// @brief 按顺序消费编辑指令，文件操作遇到活动 UI 帧时保留到下轮处理。
/// @return 本轮是否处理了会影响持续轮询或状态反馈的非悬停命令。
/// @details
/// 命令按队列顺序串行执行，但文件操作受全局门闩保护，无法立即取得时只保留
/// 当前文件命令并结束本轮，后续命令不会越过它。协作权威替换可在本地手势或
/// 未确认变更期间合并延后；普通编辑命令按 Session、Playback、Interaction 和
/// ActionController 四类职责分派。每批变更合并 mutationFlags，必要时同步
/// BeatMap 并通知观察者，最后恢复仅为当前轨道数临时物化的视觉布局。
///
/// 队列顺序是命令语义的一部分。普通命令从无锁队列逐个取出；若文件命令无法
/// 立即取得门闩，只允许保存这一条命令并终止本轮。下一轮必须先恢复它，不能让
/// 后到的编辑命令越过保存或导出边界。
///
/// processed 与 mutationFlags 的职责不同。processed 告诉外层调度器本轮是否
/// 存在值得继续主动轮询的工作；mutationFlags 告诉协作观察者哪些持久化领域
/// 已改变。鼠标位置和悬停可以改变瞬时状态，但两者都不应触发自动保存。
///
/// publishPendingMutation 是批次提交点。通知前先把脏 ECS 同步回 BeatMap，
/// 再由观察者生成本地对象序号。回调可能同步替换观察者，因此只有原观察者身份
/// 仍然有效时，才把返回序号发布为最新本地对象变更。
///
/// 权威替换以数据类别为单位合并。新替换未携带的类别可从旧延后替换继承；
/// 两者都提供对象差量身份时合并并去重，只要任一方缺少精确身份就退化为全量
/// 对象基线重建。该退化牺牲少量编码性能，但不会错误遗漏远端对象。
///
/// includedLocalMutationSequence 表示权威对象状态已经包含到哪个本地提交。
/// 低于最新本地序号的替换必须延后，否则画布会先恢复旧对象再等待服务端重放，
/// 产生可见回退。收到更高确认序号时，早期延后替换中的对象类别会被清除。
///
/// 活跃画笔只有在“纯对象权威替换、创建新对象、没有框选或其他拖动”时可以
/// 被保存并恢复。这一特例允许协作更新远端对象而不中断本地新建草稿；修改已有
/// 对象或涉及 Timing、元数据、音频的手势仍必须等待，以免坐标基准改变。
///
/// trackLayout 和 judgeline_pos 在 visit 前按当前 Key 数临时物化，因为交互
/// 控制器需要具体布局。普通命令完成后恢复配置模板；CmdUpdateEditorConfig
/// 已替换完整配置，不能再覆盖回旧模板。
///
/// 第一次 visit 负责业务处理和状态栏反馈，第二次 visit 负责从动作栈、脏标记
/// 与命令类型归纳精确 mutationFlags。分开处理可避免每个控制器都直接依赖协作
/// 观察者，同时确保权威远端替换不会被当作新的本地编辑回传。
/// @warning 逻辑 update 调用；文件指令仅尝试门闩，禁止阻塞等待 UI。
/// 手动文件操作是低频阻塞路径，其执行期间 UI 只绘制独立进度。
bool BeatmapSession::processCommands()
{
    // cmd 在循环中复用，deferred 文件命令优先于队列新项恢复原始执行顺序。
    LogicCommand cmd;
    bool         processed = false;
    // 多条连续编辑命令的变更类别按位合并，到同步边界一次性发布观察者通知。
    ::MMM::BeatmapMutationFlags mutationFlags =
        ::MMM::BeatmapMutationFlags::None;
    const auto publishPendingMutation = [this, &mutationFlags]() {
        // 批次发布顺序固定为：更新备份/保存调度、同步领域模型、通知观察者、
        // 清空类别位。任何新增提前返回都必须保持 mutationFlags，不能在同步前
        // 将类别消费掉。
        // 没有累计变更时保持自动保存和观察者状态不动。
        if ( mutationFlags == ::MMM::BeatmapMutationFlags::None ) return;
        // 任一谱面变更都让自动备份基线失效，定时策略由其他轮询路径处理。
        m_autoBackupDirty    = true;
        const auto& autoSave = m_ctx->lastConfig.settings.autoSave;
        if ( autoSave.mode == Config::AutoSaveMode::EventTriggered &&
             autoSave.onObjectModified ) {
            // 事件触发保存只设置待处理标记，当前命令热路径不直接写文件。
            m_triggeredAutoSavePending = true;
        }
        const auto& autoBackup = m_ctx->lastConfig.settings.autoBackup;
        if ( autoBackup.mode == Config::AutoSaveMode::EventTriggered &&
             autoBackup.onObjectModified ) {
            // 自动备份与覆盖原文件的自动保存分别排队，二者策略互不替代。
            m_triggeredAutoBackupPending = true;
        }
        auto observer = m_mutationObserver.load(std::memory_order_acquire);
        if ( observer && m_ctx->currentBeatmap ) {
            // 观察者需要一致领域模型，通知前把本批 ECS 与元数据脏域统一同步。
            SessionUtils::syncBeatmap(*m_ctx);
            const auto sequence = observer->onBeatmapMutated(
                *m_ctx->currentBeatmap, mutationFlags);
            if ( sequence != 0 &&
                 mutationFlags == ::MMM::BeatmapMutationFlags::Objects ) {
                // 仅纯物件变更使用本地序号与远端对象快照协调；混合类别等待
                // 权威状态完整回放，不把单一对象序号解释为全部数据已接受。
                const auto activeObserver =
                    m_mutationObserver.load(std::memory_order_acquire);
                if ( activeObserver == observer ) {
                    // 回调期间观察者可能被替换，只有身份未变才发布其返回序号。
                    m_latestAcceptedLocalObjectMutationSequence.store(
                        sequence, std::memory_order_release);
                }
            }
        }
        // 发布完成后清空累计位，后续命令形成新的同步批次。
        mutationFlags = ::MMM::BeatmapMutationFlags::None;
    };
    const auto localGestureActive = [this]() {
        // 拖动、框选、画笔和橡皮均持有临时交互状态，不适合被权威全量替换中断。
        return m_ctx->isDragging || m_ctx->isSelecting ||
               m_ctx->brushState.isActive || m_ctx->eraserState.isActive;
    };
    const auto replacementHasCategories =
        [](const CmdReplaceBeatmapData& value) {
            // 权威替换清空某一类别后可能变成空命令，用此助手决定是否仍需保留。
            return value.replaceObjects || value.replaceTimelines ||
                   value.replaceMetadata || value.replaceAudioSamples ||
                   value.replaceAnnotations;
        };
    while ( m_deferredFileCommand || m_commandQueue.try_dequeue(cmd) ) {
        // 循环每次只拥有一条 LogicCommand。variant 在处理完成前保持有效，
        // 处理器可以读取其中的字符串和共享对象，但不得保存对 variant 成员的
        // 裸引用供后续 update 使用。
        if ( m_deferredFileCommand ) {
            // 延后文件命令必须先执行，避免后续编辑越过尚未保存的状态边界。
            cmd = std::move(*m_deferredFileCommand);
            m_deferredFileCommand.reset();
        }
        const bool isFileOperation =
            std::holds_alternative<CmdSaveBeatmap>(cmd) ||
            std::holds_alternative<CmdSaveBeatmapAs>(cmd) ||
            std::holds_alternative<CmdPackBeatmap>(cmd) ||
            std::holds_alternative<CmdExportImdPackage>(cmd);
        // 门闩只在文件命令使用，普通交互绝不因 UI 仍在绘制进度帧而等待。
        std::unique_lock fileOperationLock(Event::beatmapFileOperationGate(),
                                           std::defer_lock);
        if ( isFileOperation && !fileOperationLock.try_lock() ) {
            // try_lock 失败后把完整 variant 移出并结束本轮，不使用 sleep
            // 或忙等。
            m_deferredFileCommand = std::move(cmd);
            break;
        }
        /// @brief 保证成功、失败及权限拒绝等所有退出路径均结束进度。
        struct ProgressScope {
            /// @brief 仅为文件指令发布阶段状态。
            bool active;
            /// @brief 清除本次文件指令的进度，不推测写入结果。
            ~ProgressScope()
            {
                // RAII 覆盖 continue 和各处理器内部失败返回，保证 UI
                // 进度最终关闭。
                if ( active )
                    Event::EventBus::instance().publish(
                        Event::BeatmapSaveProgressEvent{ .active = false });
            }
        } progressScope{ isFileOperation };
        if ( isFileOperation ) {
            // 打包与单谱面保存使用不同初始阶段文本，后续处理器可发布更细进度。
            const bool package =
                std::holds_alternative<CmdPackBeatmap>(cmd) ||
                std::holds_alternative<CmdExportImdPackage>(cmd);
            Event::EventBus::instance().publish(Event::BeatmapSaveProgressEvent{
                .stage = package ? "正在准备资源包…" : "正在保存谱面…" });
        }
        if ( blockCollaborationOfflineEdit(cmd) ||
             blockCollaborationUnauthorizedEdit(cmd, true) ) {
            // 权限拒绝仍由 ProgressScope 关闭文件进度，命令不进入任何控制器。
            continue;
        }
        if ( const auto* acknowledgement =
                 std::get_if<CmdAcknowledgeCollaborationMutation>(&cmd) ) {
            // 确认命令绕过通用 visit：它只修改协作序号协议，不改变
            // SessionContext 领域数据，也不应触发状态栏、撤销栈或 mutationFlags
            // 归纳。
            // 回执只推进已接受本地物件序号，不直接修改谱面或触发自动保存。
            const auto latestLocalObjectMutationSequence =
                m_latestAcceptedLocalObjectMutationSequence.load(
                    std::memory_order_acquire);
            if ( acknowledgement->sequence >=
                 latestLocalObjectMutationSequence ) {
                // 接受单调不减序号，迟到的旧回执不能回退本地同步基线。
                m_latestAcceptedLocalObjectMutationSequence.store(
                    acknowledgement->sequence, std::memory_order_release);
            }
            if ( m_deferredAuthoritativeReplacement &&
                 m_deferredAuthoritativeReplacement->replaceObjects &&
                 m_deferredAuthoritativeReplacement
                         ->includedLocalMutationSequence <
                     acknowledgement->sequence ) {
                // 本地提交回执不能让早于该提交的权威快照重新覆盖物件；其他
                // 数据类别仍可继续应用，物件会由随后已重放本地增量的结果更新。
                m_deferredAuthoritativeReplacement->replaceObjects = false;
                if ( !replacementHasCategories(
                         *m_deferredAuthoritativeReplacement) ) {
                    // 物件类别被清除后若没有其他类别，整条延后替换即可丢弃。
                    m_deferredAuthoritativeReplacement.reset();
                }
            }
            processed = true;
            continue;
        }
        auto* authoritativeReplacement =
            std::get_if<CmdReplaceBeatmapData>(&cmd);
        // 只有明确标记 authoritativeRemote 的全量替换参与协作延后与序号协议。
        const bool authoritativeSynchronization =
            authoritativeReplacement &&
            authoritativeReplacement->authoritativeRemote;
        if ( authoritativeSynchronization &&
             m_deferredAuthoritativeReplacement ) {
            // 合并只发生在权威远端命令之间。普通本地 CmdReplaceBeatmapData
            // 仍按队列原顺序执行，不能吸收或覆盖等待中的服务端状态。
            // 新权威状态与旧延后类别合并，确保等待期间没有数据域更新被覆盖丢失。
            const auto& deferred = *m_deferredAuthoritativeReplacement;
            if ( deferred.replaceObjects ) {
                if ( !authoritativeReplacement->replaceObjects ) {
                    // 新状态不含对象时继承旧对象差量身份，随后仍需应用该类别。
                    authoritativeReplacement->objectDeltaIdentities =
                        deferred.objectDeltaIdentities;
                } else if ( authoritativeReplacement->objectDeltaIdentities &&
                            deferred.objectDeltaIdentities ) {
                    // 两份差量身份合并、排序并去重，避免同一对象重复准备基线。
                    auto& identities =
                        *authoritativeReplacement->objectDeltaIdentities;
                    identities.insert(identities.end(),
                                      deferred.objectDeltaIdentities->begin(),
                                      deferred.objectDeltaIdentities->end());
                    std::sort(identities.begin(), identities.end());
                    identities.erase(
                        std::unique(identities.begin(), identities.end()),
                        identities.end());
                } else {
                    // 任一对象替换缺少精确差量身份时退化为全量对象处理。
                    authoritativeReplacement->objectDeltaIdentities.reset();
                }
            }
            if ( deferred.replaceObjects &&
                 !authoritativeReplacement->replaceObjects ) {
                // 继承旧对象类别时，新命令不能声称已经准备其编码基线。
                authoritativeReplacement->objectEncodingBaselinePrepared =
                    false;
            }
            authoritativeReplacement->replaceObjects |= deferred.replaceObjects;
            authoritativeReplacement->replaceTimelines |=
                deferred.replaceTimelines;
            authoritativeReplacement->replaceMetadata |=
                deferred.replaceMetadata;
            authoritativeReplacement->replaceAudioSamples |=
                deferred.replaceAudioSamples;
            authoritativeReplacement->replaceAnnotations |=
                deferred.replaceAnnotations;
        }
        const bool preservesActiveBrush =
            authoritativeSynchronization &&
            authoritativeReplacement->replaceObjects &&
            !authoritativeReplacement->replaceTimelines &&
            !authoritativeReplacement->replaceMetadata &&
            !authoritativeReplacement->replaceAudioSamples &&
            m_ctx->brushState.isActive && !m_ctx->isSelecting &&
            !m_ctx->brushState.replacesExistingObject &&
            !m_ctx->eraserState.isActive &&
            m_ctx->draggedEntity == entt::null && !m_ctx->dragInitialNote &&
            !m_ctx->dragInitialSample;
        // 保留画笔的谓词刻意列出全部互斥交互状态。未来增加新的局部手势时，
        // 必须在这里明确决定是否可跨权威替换恢复，不能默认视为安全。
        // 只有纯对象替换且画笔是新增草稿时可保留；涉及其他数据域或替换已有
        // 对象会改变草稿解释基准，必须等手势结束后再应用。
        if ( authoritativeSynchronization ) publishPendingMutation();
        const auto latestLocalObjectMutationSequence =
            m_latestAcceptedLocalObjectMutationSequence.load(
                std::memory_order_acquire);
        const bool waitsForLocalMutation =
            authoritativeSynchronization &&
            authoritativeReplacement->replaceObjects &&
            authoritativeReplacement->includedLocalMutationSequence <
                latestLocalObjectMutationSequence;
        // 权威快照未包含最新本地序号时即使没有手势也要延后，防止视觉回退。
        if ( authoritativeSynchronization &&
             ((localGestureActive() && !preservesActiveBrush) ||
              waitsForLocalMutation) ) {
            // 活跃手势期间把最新权威状态留在命令队列外；旧实现重新入队后会让
            // Unlimited 逻辑线程持续执行完整 Session 更新。若本地变化尚未包含
            // 在该状态中，也必须等待带确认序号的新结果，避免画布先回退再恢复。
            m_deferredAuthoritativeReplacement = *authoritativeReplacement;
            processed                          = true;
            continue;
        }
        if ( authoritativeSynchronization ) {
            if ( authoritativeReplacement->replaceObjects &&
                 authoritativeReplacement->includedLocalMutationSequence >=
                     latestLocalObjectMutationSequence ) {
                m_latestAcceptedLocalObjectMutationSequence.store(
                    authoritativeReplacement->includedLocalMutationSequence,
                    std::memory_order_release);
            }
            m_deferredAuthoritativeReplacement.reset();
        }

        std::optional<SessionContext::BrushState> preservedBrushState;
        bool        preservedBrushDragging = false;
        std::string preservedBrushDragCameraId;
        if ( preservesActiveBrush ) {
            // 纯物件权威替换会重置交互缓存；先保存画笔草稿，使远端物件成为
            // 当前基线后仍可继续原手势并在松键时提交。
            preservedBrushState.emplace(m_ctx->brushState);
            preservedBrushDragging     = m_ctx->isDragging;
            preservedBrushDragCameraId = m_ctx->dragCameraId;
        }

        // 交互命令执行期间临时物化当前 Key 数的坐标布局，结束后恢复配置模板。
        // 这样既能让放置、拖动使用正确坐标，也允许同批命令切换轨道数后重新选择
        // 布局。画布组件仍在 update 末尾一次性物化，避免连续输入复制内部向量。
        auto& visual = m_ctx->lastConfig.visual;
        // 保存配置模板值，命令处理结束后恢复；按 Key 数展开值只服务本次交互。
        const auto  baseTrackLayout          = visual.trackLayout;
        const float baseJudgmentLinePosition = visual.judgeline_pos;
        const auto  effectiveTrackLayout =
            visual.trackLayoutForKeyCount(m_ctx->trackCount);
        // 放置、拖动和框选控制器从 lastConfig 读取布局，因此在 visit
        // 前临时替换。
        visual.trackLayout = effectiveTrackLayout;
        visual.judgeline_pos =
            visual.judgmentLinePositionForKeyCount(m_ctx->trackCount);
        bool replacedEditorConfig = false;
        std::visit(
            [this, &processed, &replacedEditorConfig, &mutationFlags](
                auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr ( std::is_same_v<T, CmdUpdateEditorConfig> ) {
                    // 配置命令会整体替换 visual，循环尾部不能再恢复旧模板。
                    replacedEditorConfig = true;
                }
                if constexpr ( std::is_same_v<T, CmdLoadBeatmap> ) {
                    // 载入新谱面建立全新协作基线，旧权威替换与本地序号全部失效。
                    m_deferredAuthoritativeReplacement.reset();
                    m_latestAcceptedLocalObjectMutationSequence.store(
                        0, std::memory_order_release);
                }
                if constexpr ( !std::is_same_v<T, CmdSetMousePosition> &&
                               !std::is_same_v<T, CmdSetHoveredEntity> ) {
                    // 高频悬停更新不算业务命令，避免 Unlimited
                    // 模式因此持续繁忙。
                    processed = true;
                }
                if constexpr ( std::is_same_v<T, CmdUndo> ||
                               std::is_same_v<T, CmdRedo> ||
                               std::is_same_v<T, CmdLoadBeatmap> ||
                               std::is_same_v<T, CmdCreateBeatmap> ||
                               std::is_same_v<T, CmdRemoveBeatmap> ||
                               std::is_same_v<T, CmdUpdateBeatmapMetadata> ||
                               std::is_same_v<T, CmdUpdateTimelineEvent> ||
                               std::is_same_v<T, CmdUpdateTimelineEvents> ||
                               std::is_same_v<T, CmdUpdateBpmWithKeepSpeedSv> ||
                               std::is_same_v<T, CmdDeleteTimelineEvent> ||
                               std::is_same_v<T, CmdCreateTimelineEvent> ||
                               std::is_same_v<T, CmdCreateTimelineEvents> ||
                               std::is_same_v<T, CmdReplaceBeatmapTimings> ||
                               std::is_same_v<T, CmdSetNoteAnnotation> ||
                               std::is_same_v<T, CmdUpsertBeatmapAnnotation> ||
                               std::is_same_v<T, CmdRemoveBeatmapAnnotation> ||
                               std::is_same_v<T, CmdReplaceBeatmapData> ) {
                    // 这些命令可能改变坐标、Timing、元数据或完整内容，需重建变换。
                    m_ctx->isTransformDirty = true;
                }

                // 状态栏只记录用户可理解的低频动作，不为每个鼠标增量生成文本。
                if constexpr ( std::is_same_v<T, CmdChangeTool> ) {
                    // 工具枚举映射翻译键，未知状态以“就绪”文本作为安全回退。
                    std::string toolName = TR("ui.status.ready").data();
                    switch ( arg.tool ) {
                    case EditTool::Move:
                        toolName = TR("ui.status.tool.select_move").data();
                        break;
                    case EditTool::Marquee:
                        toolName = TR("ui.status.tool.marquee").data();
                        break;
                    case EditTool::Draw:
                        toolName = TR("ui.status.tool.draw_brush").data();
                        break;
                    case EditTool::ColorBrush:
                        toolName = TR("ui.status.tool.color_brush").data();
                        break;
                    case EditTool::ColorEraser:
                        toolName = TR("ui.status.tool.color_eraser").data();
                        break;
                    case EditTool::Layout:
                        toolName = TR("ui.status.tool.layout").data();
                        break;
                    }
                    m_ctx->lastActionMessage = fmt::format(
                        "{} {}", TR("ui.status.category.tool"), toolName);
                } else if constexpr ( std::is_same_v<T, CmdLoadBeatmap> ) {
                    if ( arg.beatmap ) {
                        // 载入反馈同时展示歌曲名与难度版本，便于多标签区分。
                        m_ctx->lastActionMessage =
                            fmt::format("{} {}: {} [{}]",
                                        TR("ui.status.category.beatmap"),
                                        TR("ui.status.beatmap.loaded"),
                                        arg.beatmap->m_baseMapMetadata.name,
                                        arg.beatmap->m_baseMapMetadata.version);
                    } else {
                        // 空载图命令明确反馈未加载，不读取空 shared_ptr
                        // 元数据。
                        m_ctx->lastActionMessage =
                            fmt::format("{} {}",
                                        TR("ui.status.category.beatmap"),
                                        TR("ui.status.beatmap.no_load"));
                    }
                } else if constexpr ( std::is_same_v<T, CmdSaveBeatmapAs> ) {
                    m_ctx->lastActionMessage =
                        fmt::format("{} {}",
                                    TR("ui.status.category.beatmap"),
                                    TR("ui.status.beatmap.saved"));
                } else if constexpr ( std::is_same_v<T, CmdSaveBeatmap> ) {
                    if ( arg.kind == BeatmapSaveKind::Manual ) {
                        // 自动和内部保存使用专门反馈事件，不覆盖用户最近操作描述。
                        m_ctx->lastActionMessage =
                            fmt::format("{} {}",
                                        TR("ui.status.category.beatmap"),
                                        TR("ui.status.beatmap.saved"));
                    }
                } else if constexpr ( std::is_same_v<T, CmdMirrorSelected> ) {
                    m_ctx->lastActionMessage =
                        fmt::format("{} {}",
                                    TR("ui.status.category.action"),
                                    TR("ui.edit.mirror"));
                } else if constexpr ( std::is_same_v<
                                          T,
                                          CmdAlignSelectedToCommonBeats> ) {
                    m_ctx->lastActionMessage =
                        fmt::format("{} {}",
                                    TR("ui.status.category.action"),
                                    TR("ui.tools.align_beats"));
                } else if constexpr ( std::is_same_v<T, CmdSeek> ) {
                    if ( !arg.isScrubbing ) {
                        // 连续拖动播放头不刷状态栏，手势结束的 seek
                        // 才记录最终时间。
                        const auto timeText = formatStatusTime(arg.time);
                        m_ctx->lastActionMessage =
                            fmt::format("{} {} {}",
                                        TR("ui.status.category.playback"),
                                        TR("ui.status.playback.seek"),
                                        timeText);
                    }
                } else if constexpr ( std::is_same_v<T, CmdSetPlaybackSpeed> ) {
                    m_ctx->lastActionMessage =
                        fmt::format("{} {}: {:.2f}x",
                                    TR("ui.status.category.playback"),
                                    TR("ui.status.playback.speed"),
                                    arg.speed);
                } else if constexpr ( std::is_same_v<T, CmdUpdateTrackCount> ) {
                    m_ctx->lastActionMessage =
                        fmt::format("{} {} {}",
                                    TR("ui.status.category.project"),
                                    TR("ui.status.project.track_count"),
                                    arg.trackCount);
                } else if constexpr ( std::is_same_v<T,
                                                     CmdUpdateBgmTrackCount> ) {
                    m_ctx->lastActionMessage =
                        fmt::format("{} {} {}",
                                    TR("ui.status.category.project"),
                                    TR("ui.status.project.bgm_track_count"),
                                    arg.bgmTrackCount);
                } else if constexpr ( std::is_same_v<
                                          T,
                                          CmdUpdateDraftTrackCount> ) {
                    // 状态栏先显示用户请求；占用或非单步拒绝会由控制器覆盖为原因。
                    // 草稿宽度属于项目侧车数据，文本仍归入项目类别而非正式内容。
                    // 这里只生成反馈，不提前修改上下文或触发项目同步。
                    m_ctx->lastActionMessage =
                        fmt::format("{} {} {}",
                                    TR("ui.status.category.project"),
                                    TR("ui.status.project.draft_track_count"),
                                    arg.draftTrackCount);
                } else if constexpr ( std::is_same_v<T, CmdSelectAll> ) {
                    m_ctx->lastActionMessage = fmt::format(
                        "{} {}",
                        TR("ui.status.category.selection"),
                        TR(arg.scope == SelectAllScope::AllTrackAreas
                               ? "ui.status.selection.all_selected"
                               : "ui.status.selection."
                                 "current_track_area_selected"));
                }

                // 会话自身处理配置、文件、协作资源和元数据等跨控制器命令。
                if constexpr (
                    std::is_same_v<T, CmdUpdateEditorConfig> ||
                    std::is_same_v<T, CmdUpdateViewport> ||
                    std::is_same_v<T, CmdLoadBeatmap> ||
                    std::is_same_v<T, CmdSetCollaborationResources> ||
                    std::is_same_v<T, CmdSetCollaborationOfflineReadOnly> ||
                    std::is_same_v<T, CmdSetCollaborationClipboardIsolation> ||
                    std::is_same_v<T, CmdSaveBeatmap> ||
                    std::is_same_v<T, CmdSaveBeatmapAs> ||
                    std::is_same_v<T, CmdExportImdPackage> ||
                    std::is_same_v<T, CmdPackBeatmap> ||
                    std::is_same_v<T, CmdUpdateBeatmapMetadata> ||
                    std::is_same_v<T, CmdMarkBeatmapMetadataDirty> ) {
                    if constexpr ( std::is_same_v<T,
                                                  CmdUpdateBeatmapMetadata> ) {
                        // 元数据处理器返回精确变更类别，加入本批观察者通知。
                        mutationFlags |= this->handleCommand(arg);
                    } else {
                        this->handleCommand(arg);
                    }
                }
                // PlaybackController 独占
                // transport、seek、滚动和平移的时间语义。
                else if constexpr (
                    std::is_same_v<T, CmdSetPlayState> ||
                    std::is_same_v<T, CmdSeek> ||
                    std::is_same_v<T, CmdSetPlaybackSpeed> ||
                    std::is_same_v<T, CmdSetKeySoundTrackMute> ||
                    std::is_same_v<T, CmdSetKeySoundTrackGain> ||
                    std::is_same_v<T, CmdSetKeySoundEffectGroupGain> ||
                    std::is_same_v<T, CmdSetDraftKeySoundAreaMute> ||
                    std::is_same_v<T, CmdSetBgmKeySoundAreaMute> ||
                    std::is_same_v<T, CmdScroll> ||
                    std::is_same_v<T, CmdPanCanvas> ) {
                    m_playback->handleCommand(arg);
                }
                // InteractionController
                // 管理选择、手势、画笔和轨道布局即时状态。
                else if constexpr (
                    std::is_same_v<T, CmdSetHoveredEntity> ||
                    std::is_same_v<T, CmdSelectEntity> ||
                    std::is_same_v<T, CmdStartDrag> ||
                    std::is_same_v<T, CmdUpdateDrag> ||
                    std::is_same_v<T, CmdEndDrag> ||
                    std::is_same_v<T, CmdCreateAudioSample> ||
                    std::is_same_v<T, CmdUpdateAudioSampleProperties> ||
                    std::is_same_v<T, CmdUpdateObjectSampleVolume> ||
                    std::is_same_v<T, CmdUpdateSelectedObjectSampleVolume> ||
                    std::is_same_v<T, CmdChangeTool> ||
                    std::is_same_v<T, CmdSetMousePosition> ||
                    std::is_same_v<T, CmdUpdateTrackCount> ||
                    std::is_same_v<T, CmdUpdateBgmTrackCount> ||
                    std::is_same_v<T, CmdUpdateDraftTrackCount> ||
                    std::is_same_v<T, CmdSetBrushNoteColor> ||
                    std::is_same_v<T, CmdSetBrushNotePalette> ||
                    std::is_same_v<T, CmdSetBrushAudioResource> ||
                    std::is_same_v<T, CmdStartMarquee> ||
                    std::is_same_v<T, CmdUpdateMarquee> ||
                    std::is_same_v<T, CmdEndMarquee> ||
                    std::is_same_v<T, CmdRemoveMarqueeAt> ||
                    std::is_same_v<T, CmdStartBrush> ||
                    std::is_same_v<T, CmdUpdateBrush> ||
                    std::is_same_v<T, CmdEndBrush> ||
                    std::is_same_v<T, CmdStartErase> ||
                    std::is_same_v<T, CmdUpdateErase> ||
                    std::is_same_v<T, CmdEndErase> ||
                    std::is_same_v<T, CmdSelectAll> ) {
                    m_interaction->handleCommand(arg);
                }
                // ActionController 管理可撤销的数据变更及剪贴板事务。
                else if constexpr (
                    std::is_same_v<T, CmdUndo> || std::is_same_v<T, CmdRedo> ||
                    std::is_same_v<T, CmdCopy> || std::is_same_v<T, CmdCut> ||
                    std::is_same_v<T, CmdPaste> ||
                    std::is_same_v<T, CmdUpdateTimelineEvent> ||
                    std::is_same_v<T, CmdUpdateTimelineEvents> ||
                    std::is_same_v<T, CmdUpdateBpmWithKeepSpeedSv> ||
                    std::is_same_v<T, CmdDeleteTimelineEvent> ||
                    std::is_same_v<T, CmdCreateTimelineEvents> ||
                    std::is_same_v<T, CmdReplaceBeatmapTimings> ||
                    std::is_same_v<T, CmdSetNoteAnnotation> ||
                    std::is_same_v<T, CmdUpsertBeatmapAnnotation> ||
                    std::is_same_v<T, CmdRemoveBeatmapAnnotation> ||
                    std::is_same_v<T, CmdReplaceBeatmapData> ||
                    std::is_same_v<T, CmdApplyNoteColorToSelection> ||
                    std::is_same_v<T, CmdApplyNotePaletteToSelection> ||
                    std::is_same_v<T, CmdApplyBrushPaletteToEntity> ||
                    std::is_same_v<T, CmdClearNoteColorOverrides> ||
                    std::is_same_v<T, CmdDeleteSelected> ||
                    std::is_same_v<T, CmdMirrorSelected> ||
                    std::is_same_v<T, CmdAlignSelectedToCommonBeats> ||
                    std::is_same_v<T, CmdCreateTimelineEvent> ) {
                    m_actions->handleCommand(arg);
                }
            },
            cmd);
        // 到此业务处理已经结束，但 mutationFlags 尚未完全归纳。先恢复临时视觉
        // 模板，保证随后观察者同步或下一条命令看到的是长期配置而非物化副本。
        if ( preservedBrushState ) {
            // 权威纯对象替换完成后恢复本地草稿，继续沿原 cameraId
            // 接收手势增量。
            m_ctx->brushState   = std::move(*preservedBrushState);
            m_ctx->isDragging   = preservedBrushDragging;
            m_ctx->dragCameraId = std::move(preservedBrushDragCameraId);
        }
        if ( !replacedEditorConfig ) {
            // 普通命令结束后恢复配置模板；配置命令的新值本身已经是权威模板。
            visual.trackLayout   = baseTrackLayout;
            visual.judgeline_pos = baseJudgmentLinePosition;
        }

        std::visit(
            [this, &mutationFlags](const auto& arg) {
                using T = std::decay_t<decltype(arg)>;
                constexpr bool isMutationCommand =
                    std::is_same_v<T, CmdUndo> || std::is_same_v<T, CmdRedo> ||
                    std::is_same_v<T, CmdEndDrag> ||
                    std::is_same_v<T, CmdCreateAudioSample> ||
                    std::is_same_v<T, CmdUpdateAudioSampleProperties> ||
                    std::is_same_v<T, CmdUpdateObjectSampleVolume> ||
                    std::is_same_v<T, CmdUpdateSelectedObjectSampleVolume> ||
                    std::is_same_v<T, CmdUpdateTrackCount> ||
                    std::is_same_v<T, CmdUpdateBgmTrackCount> ||
                    std::is_same_v<T, CmdUpdateDraftTrackCount> ||
                    std::is_same_v<T, CmdPaste> ||
                    std::is_same_v<T, CmdDeleteSelected> ||
                    std::is_same_v<T, CmdMirrorSelected> ||
                    std::is_same_v<T, CmdAlignSelectedToCommonBeats> ||
                    std::is_same_v<T, CmdApplyNoteColorToSelection> ||
                    std::is_same_v<T, CmdApplyNotePaletteToSelection> ||
                    std::is_same_v<T, CmdApplyBrushPaletteToEntity> ||
                    std::is_same_v<T, CmdClearNoteColorOverrides> ||
                    std::is_same_v<T, CmdUpdateTimelineEvent> ||
                    std::is_same_v<T, CmdUpdateTimelineEvents> ||
                    std::is_same_v<T, CmdUpdateBpmWithKeepSpeedSv> ||
                    std::is_same_v<T, CmdDeleteTimelineEvent> ||
                    std::is_same_v<T, CmdCreateTimelineEvent> ||
                    std::is_same_v<T, CmdCreateTimelineEvents> ||
                    std::is_same_v<T, CmdReplaceBeatmapTimings> ||
                    std::is_same_v<T, CmdSetNoteAnnotation> ||
                    std::is_same_v<T, CmdUpsertBeatmapAnnotation> ||
                    std::is_same_v<T, CmdRemoveBeatmapAnnotation> ||
                    std::is_same_v<T, CmdReplaceBeatmapData> ||
                    std::is_same_v<T, CmdEndBrush> ||
                    std::is_same_v<T, CmdEndErase> ||
                    std::is_same_v<T, CmdUpdateBeatmapMetadata> ||
                    std::is_same_v<T, CmdMarkBeatmapMetadataDirty>;
                if constexpr ( !isMutationCommand ) {
                    // 查询、悬停、播放和纯配置命令不参与谱面变更观察者聚合。
                    return;
                }
                // 动作栈记录撤销动作产生的精确类别，读取后清除其待发布状态。
                const auto actionMutationFlags =
                    m_ctx->actionStack.takePendingMutationFlags();
                if constexpr ( std::is_same_v<T, CmdReplaceBeatmapData> ) {
                    if ( !arg.notifyMutationObserver ||
                         arg.authoritativeRemote ) {
                        // 权威远端替换和明确静默替换不能重新作为本地变更回传。
                        return;
                    }
                }

                mutationFlags |= actionMutationFlags;
                if ( m_ctx->m_needsNotesSync &&
                     !hasBeatmapMutationFlag(
                         actionMutationFlags,
                         ::MMM::BeatmapMutationFlags::Annotations) ) {
                    // Note
                    // 脏标记通常代表对象变化；纯批注动作虽附着音符，类别保持批注。
                    mutationFlags |= ::MMM::BeatmapMutationFlags::Objects;
                }
                if ( m_ctx->m_needsTimingsSync ) {
                    // 控制器直接修改 Timing
                    // 缓存时补充动作栈未记录的时间线类别。
                    mutationFlags |= ::MMM::BeatmapMutationFlags::Timelines;
                }
                if ( m_ctx->m_needsSamplesSync ) {
                    // 自动采样位于独立 Registry，需要独立 AudioSamples 标记。
                    mutationFlags |= ::MMM::BeatmapMutationFlags::AudioSamples;
                }
                if constexpr ( std::is_same_v<T, CmdUpdateTrackCount> ||
                               std::is_same_v<T, CmdUpdateBgmTrackCount> ||
                               std::is_same_v<T, CmdUpdateBeatmapMetadata> ||
                               std::is_same_v<T,
                                              CmdMarkBeatmapMetadataDirty> ) {
                    mutationFlags |= ::MMM::BeatmapMutationFlags::Metadata;
                } else if constexpr ( std::is_same_v<T,
                                                     CmdReplaceBeatmapData> ) {
                    // 替换命令按其实际类别位补充标记，未选择的数据域不产生通知。
                    if ( arg.replaceMetadata ) {
                        mutationFlags |= ::MMM::BeatmapMutationFlags::Metadata;
                    }
                    if ( arg.replaceAudioSamples ) {
                        mutationFlags |=
                            ::MMM::BeatmapMutationFlags::AudioSamples;
                    }
                    if ( arg.replaceAnnotations ) {
                        mutationFlags |=
                            ::MMM::BeatmapMutationFlags::Annotations;
                    }
                }
            },
            cmd);
        // 第二次 visit 只读取命令类别和动作栈结果，不再次调用业务处理器。
        // 这保证一条可撤销命令只执行一次，同时允许控制器以统一脏标记补充类别。
        if ( authoritativeSynchronization && m_ctx->currentBeatmap ) {
            // 权威应用完成后通知协作观察者更新编码基线或接收完整同步快照。
            auto observer = m_mutationObserver.load(std::memory_order_acquire);
            if ( observer ) {
                if ( authoritativeReplacement
                         ->objectEncodingBaselinePrepared ) {
                    // 已准备差量基线时只提交修订与本地包含序号，避免再次序列化谱面。
                    observer->onAuthoritativeBeatmapApplied(
                        authoritativeReplacement->authoritativeRevision,
                        authoritativeReplacement
                            ->includedLocalMutationSequence);
                } else {
                    // 缺少编码基线时同步当前 BeatMap 并走完整观察者重建路径。
                    SessionUtils::syncBeatmap(*m_ctx);
                    observer->onBeatmapSynchronized(*m_ctx->currentBeatmap);
                }
            }
        }
    }

    publishPendingMutation();
    // 退出循环可能是队列耗尽，也可能是文件门闩暂不可用。文件命令本身尚未执行
    // 时不会产生 mutationFlags，但它之前已经处理的编辑仍必须在本轮立即发布。
    // 队列耗尽后发布尾批变更，保证最后一条编辑命令不会等待下一次 update。
    const auto deferredCoversLocalObjects =
        m_deferredAuthoritativeReplacement &&
        (!m_deferredAuthoritativeReplacement->replaceObjects ||
         m_deferredAuthoritativeReplacement->includedLocalMutationSequence >=
             m_latestAcceptedLocalObjectMutationSequence.load(
                 std::memory_order_acquire));
    if ( m_deferredAuthoritativeReplacement && !localGestureActive() &&
         deferredCoversLocalObjects ) {
        // 手势结束且权威状态覆盖最新本地序号时重新入队，下一轮按正常顺序应用。
        m_commandQueue.enqueue(
            LogicCommand(std::move(*m_deferredAuthoritativeReplacement)));
        m_deferredAuthoritativeReplacement.reset();
    }
    // processed 供 Unlimited 门控判断是否还有业务工作，悬停命令不影响该结果。
    return processed;
}

/// @brief 进入协作离线只读状态时终止全部尚未提交的本地交互手势。
/// @param cmd 新的只读状态；恢复可写时不自动恢复先前草稿。
/// @details
/// 只读切换是一个取消边界，不是普通的输入禁用。拖动固定实体、框选几何、画笔
/// 临时折线和橡皮目标集合都可能在松键时生成领域动作，因此进入只读时必须作为
/// 一个状态组清空。已经提交到 ActionStack 的动作不受影响。
///
/// Note 与 Sample 使用不同 Registry 和固定实体集合，必须分别解除 dragging。
/// 恢复可写时不重建已取消状态，因为原鼠标位置、相机和远端权威基线可能已经
/// 改变，继续旧手势会产生不可预测的增量。
/// @warning 协作状态切换低频路径：只访问当前交互索引和临时集合，不扫描全谱。
void BeatmapSession::handleCommand(
    const CmdSetCollaborationOfflineReadOnly& cmd)
{
    // 只读解除只开放后续输入，已取消手势不能安全自动重建。
    if ( !cmd.readOnly ) return;

    // 先清除 Note 渲染固定实体的 dragging 标记，再丢弃会话级拖动身份。
    for ( const auto entity : m_ctx->dragRenderPinnedEntities ) {
        if ( m_ctx->noteRegistry.valid(entity) &&
             m_ctx->noteRegistry.all_of<InteractionComponent>(entity) ) {
            m_ctx->noteRegistry.get<InteractionComponent>(entity).isDragging =
                false;
        }
    }
    // Sample 使用独立 Registry 和固定集合，需要采用相同清理协议。
    for ( const auto entity : m_ctx->dragSampleRenderPinnedEntities ) {
        if ( m_ctx->sampleRegistry.valid(entity) &&
             m_ctx->sampleRegistry.all_of<InteractionComponent>(entity) ) {
            m_ctx->sampleRegistry.get<InteractionComponent>(entity).isDragging =
                false;
        }
    }

    // 拖动目标、部件、初始快照和相机身份必须作为一个状态组重置。
    m_ctx->isDragging        = false;
    m_ctx->draggedEntity     = entt::null;
    m_ctx->draggedObjectKind = ChartObjectKind::PlayerNote;
    m_ctx->draggedPart       = HoverPart::None;
    m_ctx->draggedSubIndex   = -1;
    m_ctx->dragInitialNote.reset();
    m_ctx->dragInitialSample.reset();
    m_ctx->dragCameraId.clear();
    m_ctx->dragRenderPinnedEntities.clear();
    m_ctx->dragSampleRenderPinnedEntities.clear();

    // 离线后框选草稿不再具有提交语义，连同加选模式和几何框全部清除。
    m_ctx->isSelecting             = false;
    m_ctx->hasMarqueeSelection     = false;
    m_ctx->marqueeIsAdditive       = false;
    m_ctx->isMarqueeSelectionDirty = false;
    m_ctx->marqueeBoxes.clear();

    // 画笔可能携带音频资源和折线临时段，只读切换不能让松键后继续提交。
    m_ctx->brushState.isActive           = false;
    m_ctx->brushState.createsAudioSample = false;
    m_ctx->brushState.activeAudioResourceId.clear();
    m_ctx->brushState.activeSampleBinding.reset();
    m_ctx->brushState.polylineSegments.clear();
    m_ctx->brushState.holdStartTime = -1.0;
    m_ctx->brushState.duration      = 0.0;
    m_ctx->brushState.dtrack        = 0;

    // 橡皮目标集合同样属于未提交手势，清空后不会在恢复在线时批量删除。
    m_ctx->eraserState.isActive         = false;
    m_ctx->eraserState.isShiftDown      = false;
    m_ctx->eraserState.targetObjectKind = ChartObjectKind::PlayerNote;
    m_ctx->eraserState.targetEntities.clear();
}

/// @brief 切换协作会话的剪贴板隔离范围。
/// @param cmd 是否隔离及对应协作范围 ID。
/// @note 解除隔离时清除会话私有和编辑器中由本会话创建的载荷，防止跨域泄漏。
/// @details
/// 隔离开启时 scopeId 参与粘贴权限匹配；关闭时必须归零，避免之后重新开启不同
/// 会话却沿用旧范围。清理顺序先通知 EditorEngine 移除全局关联载荷，再清空
/// SessionContext 中的音符和自动采样剪贴板。
void BeatmapSession::handleCommand(
    const CmdSetCollaborationClipboardIsolation& cmd)
{
    if ( !cmd.isolated ) {
        // 先按 SessionContext 身份清除全局剪贴板，再清理会话私有载荷。
        EditorEngine::instance().clearClipboardForContext(m_ctx.get());
        m_ctx->clipboard.clear();
        m_ctx->sampleClipboard.clear();
    }
    // scopeId 只在隔离开启时有效，关闭状态统一写零避免旧范围误匹配。
    m_ctx->collaborationClipboardIsolated = cmd.isolated;
    m_ctx->collaborationClipboardScopeId  = cmd.isolated ? cmd.scopeId : 0U;
}

/// @brief 安装或撤销协作项目资源快照，并刷新会话媒体解析环境。
/// @param cmd 协作项目及服务端路径重映射；空项目表示恢复本地项目资源。
/// @details
/// collaborationProject 是远端会话资源的只读所有权快照，pathRemap 把服务端
/// 元数据路径映射到已下载的本地缓存。安装快照后，音频描述符、激活请求和对外
/// 指纹都需重新生成，不能继续复用本地项目的解析结果。
///
/// Effect 音频通过 AudioManager 独立登记；Main 和 Sample 属于组合时间线，
/// 不在此处逐个加载。撤销协作快照时先卸载远端 Effect，再恢复当前本地项目的
/// Effect 登记，避免相同资源 ID 暂时指向错误文件。
///
/// 背景尺寸探测需要实际本地路径，因此协作映射必须在调用 SessionUtils 前应用。
/// 若没有当前谱面，安装请求保持原资源状态，等待载图流程建立完整解析上下文。
/// @warning 协作资源切换低频路径：登记或卸载 Effect，并重新探测背景尺寸。
void BeatmapSession::handleCommand(const CmdSetCollaborationResources& cmd)
{
    if ( !cmd.project ) {
        // 撤销前卸载协作项目独有 Effect，避免恢复本地项目后同 ID
        // 仍指向远端路径。
        if ( m_ctx->collaborationProject ) {
            for ( const auto& resource :
                  m_ctx->collaborationProject->m_audioResources ) {
                if ( resource.m_type != ::MMM::AudioTrackType::Effect ) {
                    // Main 与 Sample 随组合时间线重建，不存在独立音效池条目。
                    continue;
                }
                Audio::AudioManager::instance().unloadSoundEffect(
                    resource.m_id);
            }
        }
        m_ctx->collaborationProject.reset();
        m_ctx->collaborationPathRemap.clear();
        // 资源解释基准改变后描述符、激活状态和对外指纹都必须在安全点刷新。
        m_ctx->isAudioTimelineDescriptorDirty           = true;
        m_ctx->isAudioTimelineActivationPending         = true;
        m_ctx->isAudioTimelineFingerprintPublishPending = true;
        EditorEngine::instance().registerCurrentProjectEffectSoundEffects();

        if ( m_ctx->currentBeatmap ) {
            // 恢复本地项目后用原元数据重新解析图片或视频背景尺寸。
            SessionUtils::updateBackgroundSize(
                *m_ctx,
                m_ctx->currentBeatmap->m_baseMapMetadata,
                EditorEngine::instance().getCurrentProject());
        }
        return;
    }
    // 空谱面没有可应用协作资源的媒体引用，保持当前资源状态等待载图。
    if ( !m_ctx->currentBeatmap ) return;
    m_ctx->collaborationProject                     = cmd.project;
    m_ctx->collaborationPathRemap                   = cmd.pathRemap;
    m_ctx->isAudioTimelineDescriptorDirty           = true;
    m_ctx->isAudioTimelineActivationPending         = true;
    m_ctx->isAudioTimelineFingerprintPublishPending = true;

    // 协作项目中的 Effect 使用其缓存根目录解析绝对路径并覆盖登记。
    for ( const auto& resource : cmd.project->m_audioResources ) {
        if ( resource.m_type != ::MMM::AudioTrackType::Effect ) continue;
        const auto absolutePath =
            cmd.project->m_projectRoot / Config::utf8ToPath(resource.m_path);
        Audio::AudioManager::instance().registerSoundEffect(
            resource.m_id, Config::pathToUtf8(absolutePath), resource.m_config);
    }

    auto backgroundMetadata = m_ctx->currentBeatmap->m_baseMapMetadata;
    // 服务端资源路径可能已下载到本地缓存，背景探测前应用精确路径映射。
    const auto source = Config::pathToUtf8(backgroundMetadata.main_cover_path);
    if ( const auto iterator = m_ctx->collaborationPathRemap.find(source);
         iterator != m_ctx->collaborationPathRemap.end() ) {
        backgroundMetadata.main_cover_path =
            Config::utf8ToPath(iterator->second);
    }
    SessionUtils::updateBackgroundSize(
        *m_ctx, backgroundMetadata, m_ctx->collaborationProject.get());
}

/// @brief 应用全局编辑配置，并结束已隐藏草稿区的交互状态。
/// @param cmd 新的完整编辑器配置快照。
/// @details
/// 关闭专业模式时只清理 Draft 相关选择、悬停和手势；关闭 Polyline 或 BMS
/// 编辑能力时清理所有新配置下不可编辑的对应对象。配置提交后把 ScrollCache
/// 标脏，使轨道布局、判定线和滚动映射在下一次更新中使用新参数。
///
/// 能力关闭采用“结束活动手势、清理局部索引、修正全局身份”的顺序。活动拖动
/// 优先走标准 EndDrag，复用控制器已经实现的提交或撤销规则；选中集合和悬停
/// 身份随后清理，避免 UI 继续引用新配置下不可见的实体。
///
/// Professional、Polyline 和 BMS 是三个独立能力域。关闭 Professional 只影响
/// Draft；关闭 Polyline 按新的 isNoteEditable 规则筛选 Note；关闭 BMS 则清空
/// Sample Registry 的全部交互状态。一个能力域的清理不能误伤其他类型选择。
/// @warning
/// 配置变更低频路径：草稿清理仅访问已选索引与当前交互实体，禁止整谱扫描。
void BeatmapSession::handleCommand(const CmdUpdateEditorConfig& cmd)
{
    if ( !cmd.config.settings.professionalMode ) {
        // 选择集合是局部索引，可快速判断拖动组中是否含 Draft，不遍历 Registry。
        const bool hasSelectedDraft = std::any_of(
            m_ctx->selectedNoteEntities.begin(),
            m_ctx->selectedNoteEntities.end(),
            [this](entt::entity entity) {
                const auto* note =
                    m_ctx->noteRegistry.try_get<const NoteComponent>(entity);
                return note && note->m_isDraft;
            });
        if ( m_ctx->isDragging &&
             (m_ctx->draggedObjectKind == ChartObjectKind::DraftNote ||
              hasSelectedDraft) ) {
            // 通过标准 EndDrag
            // 路径撤销或提交当前拖动，避免手工遗漏固定实体状态。
            m_interaction->handleCommand(CmdEndDrag{});
        }
        if ( m_ctx->brushState.isActive && m_ctx->brushState.track < 0 ) {
            // 负轨道画笔属于 Draft 区，隐藏前结束手势防止不可见草稿继续增长。
            m_interaction->handleCommand(CmdEndBrush{});
        }
        for ( auto it = m_ctx->selectedNoteEntities.begin();
              it != m_ctx->selectedNoteEntities.end(); ) {
            const auto* note =
                m_ctx->noteRegistry.try_get<const NoteComponent>(*it);
            if ( !note || !note->m_isDraft ) {
                // 无效实体或普通玩家物件保留选择，只有 Draft 从集合剔除。
                ++it;
                continue;
            }
            if ( auto* interaction =
                     m_ctx->noteRegistry.try_get<InteractionComponent>(*it) ) {
                // 重置完整交互组件，清除选中、悬停、拖动和剪切等派生标志。
                *interaction = InteractionComponent{};
            }
            it = m_ctx->selectedNoteEntities.erase(it);
        }
        if ( m_ctx->hoveredObjectKind == ChartObjectKind::DraftNote ) {
            // 当前悬停 Draft
            // 可能已经不在选择集合，需单独清理实体组件和全局身份。
            if ( auto* interaction =
                     m_ctx->noteRegistry.try_get<InteractionComponent>(
                         m_ctx->hoveredEntity) ) {
                interaction->isHovered = false;
                interaction->hoveredPart =
                    static_cast<std::uint8_t>(HoverPart::None);
                interaction->hoveredSubIndex = -1;
            }
            m_ctx->hoveredEntity     = entt::null;
            m_ctx->hoveredObjectKind = ChartObjectKind::PlayerNote;
            m_ctx->hoveredPart     = static_cast<std::int32_t>(HoverPart::None);
            m_ctx->hoveredSubIndex = -1;
        }
    }
    const bool disablePolylineEditing =
        m_ctx->lastConfig.settings.enablePolylineEditing &&
        !cmd.config.settings.enablePolylineEditing;
    const bool disableBmsEditing =
        m_ctx->lastConfig.settings.enableBmsEditing &&
        !cmd.config.settings.enableBmsEditing;
    // 先根据旧新值计算能力关闭边沿，再提交新配置供 isNoteEditable 判断。
    m_ctx->lastConfig = cmd.config;
    if ( disablePolylineEditing ) {
        // 能力关闭是低频设置动作，允许遍历 Note 交互视图清理不可编辑对象。
        auto view =
            m_ctx->noteRegistry.view<NoteComponent, InteractionComponent>();
        for ( const auto entity : view ) {
            const auto& note = view.get<NoteComponent>(entity);
            if ( SessionUtils::isNoteEditable(note, cmd.config.settings) ) {
                // 普通音符和仍获许可的类型保持当前交互状态。
                continue;
            }
            auto& interaction      = view.get<InteractionComponent>(entity);
            interaction.isSelected = false;
            interaction.isHovered  = false;
            interaction.isDragging = false;
            interaction.isCut      = false;
            interaction.hoveredPart =
                static_cast<std::uint8_t>(HoverPart::None);
            interaction.hoveredSubIndex = -1;
            m_ctx->selectedNoteEntities.erase(entity);
        }

        if ( (m_ctx->hoveredObjectKind == ChartObjectKind::PlayerNote ||
              m_ctx->hoveredObjectKind == ChartObjectKind::DraftNote) &&
             m_ctx->hoveredEntity != entt::null &&
             m_ctx->noteRegistry.valid(m_ctx->hoveredEntity) &&
             m_ctx->noteRegistry.all_of<NoteComponent>(m_ctx->hoveredEntity) &&
             !SessionUtils::isNoteEditable(
                 m_ctx->noteRegistry.get<const NoteComponent>(
                     m_ctx->hoveredEntity),
                 cmd.config.settings) ) {
            // 全局悬停身份必须与组件清理同步，否则 UI
            // 仍可能展示已禁用目标详情。
            m_ctx->hoveredEntity   = entt::null;
            m_ctx->hoveredPart     = static_cast<std::int32_t>(HoverPart::None);
            m_ctx->hoveredSubIndex = -1;
        }
    }
    if ( disableBmsEditing ) {
        // BMS 编辑关闭后全部 Sample 都不可交互，清理独立 Registry 的状态。
        auto view = m_ctx->sampleRegistry.view<InteractionComponent>();
        for ( const auto entity : view ) {
            auto& interaction      = view.get<InteractionComponent>(entity);
            interaction.isSelected = false;
            interaction.isHovered  = false;
            interaction.isDragging = false;
            interaction.isCut      = false;
            interaction.hoveredPart =
                static_cast<std::uint8_t>(HoverPart::None);
            interaction.hoveredSubIndex = -1;
        }
        m_ctx->selectedSampleEntities.clear();
        if ( m_ctx->hoveredObjectKind == ChartObjectKind::AudioSample ) {
            // 当前 Sample 悬停身份回退普通玩家物件类型和空实体。
            m_ctx->hoveredEntity     = entt::null;
            m_ctx->hoveredObjectKind = ChartObjectKind::PlayerNote;
            m_ctx->hoveredPart     = static_cast<std::int32_t>(HoverPart::None);
            m_ctx->hoveredSubIndex = -1;
        }
        if ( m_ctx->brushState.createsAudioSample ) {
            // 正在创建自动采样的画笔不能在能力关闭后继续到 EndBrush 提交。
            m_ctx->brushState.isActive           = false;
            m_ctx->brushState.createsAudioSample = false;
        }
    }
    auto* cache = m_ctx->timelineRegistry.ctx().find<System::ScrollCache>();
    if ( cache ) {
        // 配置可能改变 BPM 映射、缩放和轨道布局，统一要求下一帧重建滚动缓存。
        cache->isDirty = true;
    }
}

/// @brief 创建或更新指定相机视口，并按宽度变化保持横向平移比例。
/// @param cmd 相机 ID 与新的视口宽高。
/// @details
/// 每个画布以 cameraId 持有独立 CameraInfo。首次更新建立零偏移相机；后续宽度
/// 变化先按旧视口换算 horizontalOffsetX，再覆盖尺寸，保持用户看到的横向中心
/// 相对位置。高度不参与横向换算，只更新垂直可视范围。
/// @warning Resize 事件路径：只更新常量级相机状态，不触发文件或全谱操作。
void BeatmapSession::handleCommand(const CmdUpdateViewport& cmd)
{
    if ( m_ctx->cameras.find(cmd.cameraId) == m_ctx->cameras.end() ) {
        // 首次见到相机时使用零偏移和命令尺寸建立独立 CameraInfo。
        m_ctx->cameras[cmd.cameraId] =
            CameraInfo{ cmd.cameraId, cmd.width, cmd.height };
    } else {
        auto& camera = m_ctx->cameras[cmd.cameraId];
        // 先用旧宽与新宽换算横移，随后才覆盖 viewportWidth。
        camera.horizontalOffsetX = resizeCanvasHorizontalOffset(
            camera.horizontalOffsetX, camera.viewportWidth, cmd.width);
        camera.viewportWidth  = cmd.width;
        camera.viewportHeight = cmd.height;
    }
}

/// @brief 用新谱面替换会话内容，并重置保存、协作和文件哈希基线。
/// @param cmd 待载入谱面共享对象；空对象由 SessionUtils 处理为空会话。
/// @details
/// 载图前必须先落盘旧谱面的尾随元数据；失败时保持旧会话不变。成功越过此边界
/// 后，旧谱面的自动备份计时、协作资源快照、外部修改哈希及 BPM 缓存身份都不再
/// 有效，需要以新谱面重新建立。
///
/// SessionUtils::loadBeatmap 负责领域对象和 ECS 的完整替换。诊断只能在该步骤
/// 完成后发布，确保报告的是实际进入会话的格式和资源解析状态，而不是输入对象
/// 尚未规范化的临时值。
/// @warning 载图低频路径：可能同步写出旧元数据并完整重建 ECS 与派生索引。
void BeatmapSession::handleCommand(const CmdLoadBeatmap& cmd)
{
    if ( m_metadataAutoSavePending && !flushPendingMetadataAutoSave() ) {
        // 旧谱面尾随元数据无法保存时拒绝替换，避免载图导致未落盘修改丢失。
        XERROR(
            "BeatmapSession: cannot replace beatmap because pending "
            "metadata could not be saved");
        return;
    }
    // 新谱面不继承旧谱面的尾随保存计时和自动备份脏状态。
    m_metadataAutoSavePending         = false;
    m_metadataAutoSaveTimerNeedsReset = false;
    m_autoBackupDirty                 = false;
    m_triggeredAutoBackupPending      = false;
    m_timedAutoBackupDeadline         = 0.0;
    m_timedAutoBackupIntervalSeconds  = 0.0;
    m_requestedAutoBackupTriggers.store(0U, std::memory_order_relaxed);
    // 载图建立本地项目解析环境，旧协作资源快照和路径映射全部失效。
    m_ctx->collaborationProject.reset();
    m_ctx->collaborationPathRemap.clear();
    SessionUtils::loadBeatmap(*m_ctx, cmd.beatmap);
    if ( m_ctx->currentBeatmap ) {
        // 诊断在完整载入后发布，能检查实际格式解析和资源引用结果。
        publishBeatmapLoadDiagnostics(*m_ctx->currentBeatmap);
    }
    m_savedBeatmapFileHashes.clear();
    if ( m_ctx->currentBeatmap ) {
        // 当前磁盘内容成为外部修改检测基线，路径按当前项目根解析。
        rememberBeatmapFileHash(
            m_savedBeatmapFileHashes,
            resolveCurrentProjectPath(
                m_ctx->currentBeatmap->m_baseMapMetadata.map_path));
    }
    // 新谱面的 Timing 已整体替换，BPM 事件缓存必须在下一次使用前重建。
    m_ctx->isBpmEventsDirty = true;
}

/// @brief 保存当前谱面，并同步文件路径、项目入口和外部修改哈希基线。
/// @param cmd 保存来源及是否已确认覆盖外部修改。
/// @details
/// 保存先暂时取下元数据尾随任务并确定实际格式路径，必要时发布覆盖冲突。
/// 写盘前同步 Timing、Note 和打击事件，成功后更新会话路径、哈希、撤销栈保存点
/// 及项目谱面入口。任何失败都会恢复原尾随元数据任务，供后续重试。
///
/// ForceMMM 只改变本次实际保存路径。覆盖检查使用目标路径及上一次成功写盘哈希，
/// 不能使用旧扩展名的源路径。用户尚未确认外部修改时，处理器只发布冲突事件，
/// 不执行任何磁盘或项目状态提交。
///
/// saveToFile 成功是事务提交点。之前的同步只更新内存领域副本，之后才能更新
/// currentBeatmap.map_path、哈希基线和 ActionStack
/// 保存点。项目入口更新最后执行， 并通过 saveProject
/// 把路径或元数据变化一起持久化。
/// @warning 显式或自动保存低频路径：执行完整谱面同步和文件写入。
void BeatmapSession::handleCommand(const CmdSaveBeatmap& cmd)
{
    if ( m_ctx->currentBeatmap ) {
        // 记录进入保存前是否存在尾随元数据，失败路径按原状态恢复。
        const bool hadPendingMetadataAutoSave = m_metadataAutoSavePending;
        /// @brief 保存失败时恢复尾随任务，避免后续打包读取旧文件。
        const auto restorePendingMetadataAutoSave = [&]() {
            if ( !hadPendingMetadataAutoSave ) return;
            // 重新计时避免失败后立即在同一轮反复尝试阻塞写盘。
            m_metadataAutoSavePending         = true;
            m_metadataAutoSaveTimerNeedsReset = true;
        };
        m_metadataAutoSavePending         = false;
        m_metadataAutoSaveTimerNeedsReset = false;
        auto oldPath  = m_ctx->currentBeatmap->m_baseMapMetadata.map_path;
        auto savePath = resolveCurrentProjectPath(oldPath);
        if ( m_ctx->lastConfig.settings.saveFormatPreference ==
             Config::SaveFormatPreference::ForceMMM ) {
            // ForceMMM 改变实际输出扩展名，但旧路径保留到成功后用于项目重映射。
            savePath.replace_extension(".mmm");
        }
        if ( shouldConfirmForcedMmmOverwrite(m_ctx->lastConfig.settings,
                                             m_savedBeatmapFileHashes,
                                             cmd,
                                             savePath) ) {
            // 冲突事件只请求用户确认，不写文件、不推进保存哈希或撤销栈保存点。
            Event::EventBus::instance().publish(Event::BeatmapSaveConflictEvent{
                .path = Config::pathToUtf8(savePath),
            });
            restorePendingMetadataAutoSave();
            return;
        }

        m_ctx->m_needsTimingsSync = true;
        m_ctx->m_needsNotesSync   = true;
        // 保存必须包含当前 ECS 和 Timeline
        // 全量状态，不能依赖此前脏标记是否准确。
        SessionUtils::syncBeatmap(*m_ctx);
        SessionUtils::ensureHitEvents(*m_ctx);
        refreshCurrentProjectSongFileHint(*m_ctx->currentBeatmap);

        bool ok = m_ctx->currentBeatmap->saveToFile(savePath);
        if ( !ok ) {
            // 失败结果携带与请求来源匹配的展示策略，自动保存不会弹手动提示。
            XERROR("SaveBeatmap: failed to save to {}",
                   Config::pathToUtf8(savePath));
            Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
                .path         = Config::pathToUtf8(savePath),
                .success      = false,
                .isExport     = false,
                .presentation = savePresentationFor(cmd.kind),
            });
            restorePendingMetadataAutoSave();
            return;
        }
        Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
            // 成功先通知文件结果，随后更新内存路径与项目清单基线。
            .path         = Config::pathToUtf8(savePath),
            .success      = true,
            .isExport     = false,
            .presentation = savePresentationFor(cmd.kind),
        });
        auto storedSavePath = makeCurrentProjectRelativePath(savePath);
        // 会话长期路径尽量项目相对化，避免项目整体移动后失去谱面身份。
        m_ctx->currentBeatmap->m_baseMapMetadata.map_path = storedSavePath;
        // 成功写出的字节成为新的外部修改比较基线。
        rememberBeatmapFileHash(m_savedBeatmapFileHashes, savePath);
        // 撤销栈只在写盘成功后标记保存点，失败仍保持脏状态。
        m_ctx->actionStack.markSaved();
        if ( oldPath != storedSavePath ) {
            // 扩展名或路径变化时更新项目条目以及所有打开会话的路径身份。
            EditorEngine::instance().updateBeatmapFilePathInProject(
                oldPath, storedSavePath);
            // 项目侧草稿以谱面路径隔离，另存后当前会话必须跟随新的组键。
            auto* draftProject =
                m_ctx->collaborationProject
                    ? m_ctx->collaborationProject.get()
                    : EditorEngine::instance().getCurrentProject();
            // 组本体已由项目路径更新流程重命名，这里只迁移会话缓存的查找键。
            ProjectDraftLaneService::rebindBeatmapPath(*m_ctx, draftProject);
        } else {
            // 路径未变仍同步项目文件信息和音频指纹，覆盖内容可能改变资源引用。
            EditorEngine::instance().syncProjectWithFile(savePath);
        }
        static_cast<void>(syncSavedMetadataToProjectEntry(
            m_ctx->currentBeatmap->m_baseMapMetadata));
        // 名称等项目入口变化在最后一次项目保存中与路径状态共同落盘。
        EditorEngine::instance().saveProject();
    }
}

/// @brief 将当前谱面导出到指定路径，不接管当前会话的谱面路径或保存状态。
/// @param cmd 输出路径及仅用于本次 Malody 导出的兼容选项。
/// @details
/// 另存导出读取活动 ECS 的最新状态，但目标文件不是会话后续保存位置。成功后
/// 不修改 map_path、不调用 markSaved，也不清除元数据尾随任务。目标位于项目中
/// 时只让 EditorEngine 重新识别该文件，以便它作为额外项目资源出现。
///
/// Malody 模式和商店 mode_ext 由保存助手在临时元数据域中应用并恢复。IMD
/// 单文件失败时给出资源包替代提示，因为资源包流程还能拼装完整音频和封面。
/// @warning 用户导出低频路径：同步完整谱面并写文件，但不改变撤销栈保存点。
void BeatmapSession::handleCommand(const CmdSaveBeatmapAs& cmd)
{
    if ( m_ctx->currentBeatmap ) {
        // 导出内容同样必须包含未落盘编辑，先强制同步 Timing 和 Note 域。
        m_ctx->m_needsTimingsSync = true;
        m_ctx->m_needsNotesSync   = true;
        SessionUtils::syncBeatmap(*m_ctx);
        SessionUtils::ensureHitEvents(*m_ctx);
        auto savePath = resolveCurrentProjectPath(Config::utf8ToPath(cmd.path));
        bool ok       = saveBeatmapWithMalodyExportOptions(
            *m_ctx->currentBeatmap,
            savePath,
            cmd.malodyExportMode,
            cmd.addStoreModeExtForMalodyExport,
            nullptr);
        if ( !ok ) {
            // IMD 单文件失败提供资源包替代建议，其他格式沿用通用导出失败提示。
            XERROR("SaveBeatmapAs: failed to save to {}", cmd.path);
            const bool isImd = packageExtensionEquals(
                Config::pathToUtf8(savePath.extension()), ".imd");
            Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
                .path     = Config::pathToUtf8(savePath),
                .success  = false,
                .isExport = true,
                .errorMessage =
                    isImd ? "RM/IMD 导出失败：请检查目标路径，或改用“RM/IMD "
                            "资源包”自动拼装音频与资源"
                          : std::string{},
            });
            return;
        }
        Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
            // 成功仅报告导出文件，不修改 currentBeatmap.map_path 或 markSaved。
            .path     = Config::pathToUtf8(savePath),
            .success  = true,
            .isExport = true,
        });

        // 导出目标若位于项目中可成为新资源或谱面条目，但当前会话仍编辑原文件。
        EditorEngine::instance().syncProjectWithFile(savePath);
    }
}

/// @brief 将当前谱面、背景图和完整拼装音频导出为 RM/IMD 资源包。
/// @param cmd 资源包输出路径。
/// @details
/// RM/IMD 资源包与普通 zip 打包不同：它需要把 Main、自动采样和玩家物件绑定
/// 音效离线混合成一条完整音频，并结合当前谱面及可选封面交给专用导出服务。
/// 因此活动项目及其资源表是必要输入，独立谱面不能仅凭相对引用完成导出。
///
/// chartEndSeconds 由当前同步后的谱面内容计算，用于限制离线混音长度。基础音频
/// 描述符先收集 Main 和自动采样，HitFXSystem 的玩家物件绑定随后追加；任一资源
/// 无法解析都会终止事务，不能生成静默缺失部分音效的“成功”包。
///
/// 导出服务负责解码、混音、编码和归档，并通过回调发布阶段文案。处理器只统一
/// 将服务结果转换为 BeatmapSaveResultEvent；成功产物是导出副本，不改变当前
/// 谱面的路径、项目入口或撤销栈保存点。
/// @warning 用户触发的低频导出路径：会同步谱面、解析全部音频资源并执行完整
/// 离线混音和压缩，禁止在 Session update 热路径中调用。
void BeatmapSession::handleCommand(const CmdExportImdPackage& cmd)
{
    // 所有前置校验和服务失败统一转成同结构结果事件，UI 不需解析日志。
    const auto publishFailure = [&cmd](std::string message) {
        XERROR("ExportImdPackage: {}", message);
        Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
            .path         = cmd.path,
            .success      = false,
            .isExport     = true,
            .errorMessage = std::move(message),
        });
    };

    if ( !m_ctx->currentBeatmap ) {
        // 无谱面时没有时间线、元数据或资源依赖可供拼装。
        publishFailure("没有可导出的当前谱面");
        return;
    }
    auto* project = EditorEngine::instance().getCurrentProject();
    if ( !project || project->m_projectRoot.empty() ) {
        // IMD 资源包需要项目资源表解析 Main、Sample、Effect 和封面实际路径。
        publishFailure("导出 IMD 资源包前需要先打开项目");
        return;
    }

    m_ctx->m_needsTimingsSync = true;
    m_ctx->m_needsNotesSync   = true;
    // 离线混音必须读取当前未保存编辑和最新物件绑定，先同步所有相关域。
    SessionUtils::syncBeatmap(*m_ctx);
    SessionUtils::ensureHitEvents(*m_ctx);

    const double chartEndSeconds =
        SessionUtils::calculateChartContentEndSeconds(*m_ctx->currentBeatmap);
    // 描述符先包含 Main 与自动采样时间线，结束时间限定导出混音长度。
    auto descriptor = buildAudioTimelineDescriptor(
        *m_ctx->currentBeatmap,
        *project,
        m_ctx->currentBeatmap->m_baseMapMetadata.map_path,
        chartEndSeconds);
    if ( !descriptor.m_diagnostics.empty() ) {
        // 任一资源诊断意味着拼装音频不完整，采用首条具体原因终止导出。
        publishFailure(descriptor.m_diagnostics.front().m_message);
        return;
    }

    std::string boundSampleError;
    // 玩家物件绑定音效不属于自动采样描述符，需要按 HitEvent 额外追加。
    if ( !appendBoundSampleTimelineEvents(*project,
                                          *m_ctx->currentBeatmap,
                                          m_ctx->hitEvents,
                                          descriptor.m_events,
                                          boundSampleError) ) {
        publishFailure(std::move(boundSampleError));
        return;
    }

    const auto coverPath =
        resolveImdPackageCoverPath(*project, *m_ctx->currentBeatmap);
    // 输出路径允许项目相对表示，进入导出服务前解析为实际文件系统路径。
    const auto outputPath =
        resolveCurrentProjectPath(Config::utf8ToPath(cmd.path));
    const auto result = ImdPackageExportService::exportPackage(
        *m_ctx->currentBeatmap,
        descriptor.m_events,
        chartEndSeconds,
        coverPath,
        outputPath,
        [](std::string_view stage) {
            // 转码、混音和压缩阶段由服务回调逐项更新现有进度 UI。
            Event::EventBus::instance().publish(
                Event::BeatmapSaveProgressEvent{ .stage = std::string(stage) });
        });
    if ( !result.success ) {
        // 服务失败可能发生在音频解码、混音或归档写出，保留其完整错误文本。
        publishFailure(result.errorMessage);
        return;
    }

    XINFO("ExportImdPackage: package written to {}", cmd.path);
    Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
        // 资源包是导出产物，不改变当前谱面路径或撤销栈保存状态。
        .path     = Config::pathToUtf8(outputPath),
        .success  = true,
        .isExport = true,
    });
}

/// @brief 将用户选择的项目资源转换并写入目标谱面包。
/// @param cmd 输出路径、资源列表及格式特有转换选项。
/// @details
/// 普通谱面包直接读取项目磁盘文件，所以开始前必须请求 EditorEngine 保存全部
/// 已打开的脏谱面。任何会话保存失败都会阻止打包，避免当前标签显示的新内容与
/// 包内旧磁盘字节不一致。
///
/// 目标扩展名选择 PackageSupportedFileTypes，并约束可接受的谱面、音频和图片
/// 类型。未知包扩展名直接失败，不能退化为没有格式契约的 zip。格式特有选项
/// 原样传给 writeBeatmapPackage，由其负责跨资源预检和内存归档事务。
///
/// 进度事件在耗时工作前切换为转换/音频/压缩阶段。最终结果无论成功失败都发布
/// isExport=true，由 processCommands 的 ProgressScope 负责关闭活动进度状态。
/// @warning 用户打包低频路径：先同步保存所有打开谱面，再执行转换、音频处理
/// 和 zip 压缩；由文件操作门闩保证 UI 进度帧与写入不并发。
void BeatmapSession::handleCommand(const CmdPackBeatmap& cmd)
{
    // 打包读取磁盘项目文件，必须先把所有打开会话的脏内容完整落盘。
    if ( !EditorEngine::instance().saveDirtyBeatmapsForPackaging() ) {
        XERROR("PackBeatmap: dirty beatmaps could not be saved");
        Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
            .path     = cmd.exportPath,
            .success  = false,
            .isExport = true,
        });
        return;
    }

    auto* project = EditorEngine::instance().getCurrentProject();
    if ( !project || project->m_projectRoot.empty() ) {
        // 选择列表是项目相对路径，没有有效项目根时无法安全解析资源。
        XERROR("PackBeatmap: no project is opened");
        Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
            .path     = cmd.exportPath,
            .success  = false,
            .isExport = true,
        });
        return;
    }

    const auto  outputPath   = Config::utf8ToPath(cmd.exportPath);
    const auto  extension    = Config::pathToUtf8(outputPath.extension());
    const auto* packageTypes = findPackageSupportedFileTypes(extension);
    if ( !packageTypes ) {
        // 包类型由目标扩展名决定，未知扩展不能默认生成普通 zip 冒充格式。
        XERROR("PackBeatmap: unsupported package extension: {}",
               cmd.exportPath);
        Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
            .path     = cmd.exportPath,
            .success  = false,
            .isExport = true,
        });
        return;
    }

    Event::EventBus::instance().publish(Event::BeatmapSaveProgressEvent{
        // 进入可能耗时的转换与压缩前替换初始“准备资源包”阶段文案。
        .stage = "正在转换谱面、处理音频并压缩资源…" });
    const bool success =
        writeBeatmapPackage(*project,
                            outputPath,
                            cmd.selectedProjectRelativePaths,
                            *packageTypes,
                            cmd.metadataOverrides,
                            cmd.saveConvertedBeatmapsToProject,
                            cmd.includeLegacyImdBeatmapsInPackage,
                            cmd.malodyExportMode,
                            cmd.addStoreModeExtForMalodyExport,
                            cmd.stripMainAudioVolumeFromMalodyExport,
                            cmd.alignNonOggMainAudioToOrigin);
    if ( success ) {
        // writeBeatmapPackage 只有最终目标文件完整写出才返回成功。
        XINFO("PackBeatmap: package written to {}", cmd.exportPath);
    } else {
        XERROR("PackBeatmap: failed to write package {}", cmd.exportPath);
    }

    Event::EventBus::instance().publish(Event::BeatmapSaveResultEvent{
        // 打包不改变当前谱面会话保存点，结果统一标记为导出。
        .path     = cmd.exportPath,
        .success  = success,
        .isExport = true,
    });
}

/// @brief 更新谱面元数据，并同步主音轨提示对应的首个 Main BGM 采样。
/// @param cmd 新的谱面基础元数据。
/// @return 自动采样发生同步替换时包含 AudioSamples，否则返回 None。
/// @details
/// 输入路径先规范化成项目持久化形式，玩家轨道数至少为一；BGM 轨道数不接受
/// 元数据面板直接覆盖，而是保留 SessionContext 当前值，避免绕过自动采样迁移。
/// 完全相同的元数据是无操作，不污染撤销栈或重置尾随保存计时。
///
/// 玩家轨道数变化会改变自动采样的绝对轨道编号。处理器先验证所有采样都能保持
/// 原有 BGM 相对索引，再委托 InteractionController 执行可撤销迁移。控制器未
/// 达到目标值时整次元数据提交失败，防止布局和基础元数据分裂。
///
/// 主音频提示变化会尝试重定向最早的 Main BGM 自动采样，并以 AudioSamples
/// 类别通知观察者。其他基础元数据统一以 Metadata 类别由 processCommands
/// 补充；处理器返回的标志只表达这里额外发生的跨域变化。
///
/// 提交后根据字段差异分别刷新派生状态：map_path 影响音频资源解析，BPM 和
/// track_count 影响滚动缓存，背景路径与类型影响媒体尺寸。协作会话探测背景前
/// 还需应用服务端到本地缓存的路径映射。
::MMM::BeatmapMutationFlags BeatmapSession::handleCommand(
    const CmdUpdateBeatmapMetadata& cmd)
{
    // 默认无变更，只有实际字段差异或自动采样重定向才向观察者发布类别。
    auto mutationFlags = ::MMM::BeatmapMutationFlags::None;
    if ( m_ctx->currentBeatmap ) {
        // 旧值按值保留，用于判等、轨道迁移、资源重定向和派生缓存比较。
        const auto oldMetadata = m_ctx->currentBeatmap->m_baseMapMetadata;
        auto       updatedMeta = cmd.baseMeta;
        // UI 输入在进入会话后统一规范化路径和结构边界，避免多个入口行为不一。
        normalizeCurrentProjectMetadataPaths(updatedMeta);
        updatedMeta.track_count = std::max(1, updatedMeta.track_count);
        // BGM 轨道数由专门交互命令维护，元数据编辑不能绕过采样迁移逻辑覆盖。
        updatedMeta.bgm_track_count = m_ctx->bgmTrackCount;
        if ( baseMapMetadataEqual(oldMetadata, updatedMeta) ) {
            // 完全相同输入不污染撤销栈、不启动尾随保存，也不重建派生缓存。
            return mutationFlags;
        }

        // 所有元数据编辑入口最终都在此处转入可撤销的原子改键操作，避免 UI
        // 直接覆盖 track_count 后丢失自动采样的 BGM 相对轨道。
        if ( m_ctx->trackCount != updatedMeta.track_count ) {
            // 先只读验证全部 Sample 的相对 BGM 轨道，再调用可撤销轨道迁移动作。
            std::string migrationError;
            if ( !validateSampleTrackCountMigration(*m_ctx,
                                                    m_ctx->trackCount,
                                                    updatedMeta.track_count,
                                                    migrationError) ) {
                m_ctx->lastActionMessage = migrationError;
                XERROR("BeatmapSession: {}", migrationError);
                return mutationFlags;
            }

            m_interaction->handleCommand(
                CmdUpdateTrackCount{ updatedMeta.track_count });
            if ( m_ctx->trackCount != updatedMeta.track_count ) {
                // 控制器可能因其他边界拒绝操作，元数据必须保持旧值以免布局分裂。
                if ( m_ctx->lastActionMessage.empty() ) {
                    m_ctx->lastActionMessage =
                        "玩家轨道数变更未能完成，元数据保持不变";
                }
                XERROR("BeatmapSession: {}", m_ctx->lastActionMessage);
                return mutationFlags;
            }
        }

        if ( retargetFirstMainBgmSample(*m_ctx, oldMetadata, updatedMeta) ) {
            // song.file 变化实际修改 Sample ECS 时补充独立音频采样变更类别。
            mutationFlags |= ::MMM::BeatmapMutationFlags::AudioSamples;
        }

        m_ctx->currentBeatmap->m_baseMapMetadata = updatedMeta;
        // 元数据值提交后标记撤销栈脏，并安排用户停止编辑后的尾随保存。
        m_ctx->actionStack.markDirty();
        m_metadataAutoSavePending         = true;
        m_metadataAutoSaveTimerNeedsReset = true;
        XINFO("BeatmapSession: Metadata updated for {}",
              m_ctx->currentBeatmap->m_baseMapMetadata.name);
        if ( oldMetadata.map_path != updatedMeta.map_path ) {
            // 谱面目录变化会改变相对音频资源解析，组合描述符必须重建并激活。
            m_ctx->isAudioTimelineDescriptorDirty   = true;
            m_ctx->isAudioTimelineActivationPending = true;
        }

        // BPM 或玩家轨道数改变会影响时间到像素及轨道投影，刷新滚动与 BPM 缓存。
        if ( oldMetadata.preference_bpm != updatedMeta.preference_bpm ||
             oldMetadata.track_count != updatedMeta.track_count ) {
            XINFO(
                "BeatmapSession: Critical metadata changed, dirtying "
                "ScrollCache...");
            auto* cache =
                m_ctx->timelineRegistry.ctx().find<System::ScrollCache>();
            if ( cache ) {
                // ScrollCache 位于 Timeline Registry
                // 上下文，存在时只设置脏标志。
                cache->isDirty = true;
            }
            m_ctx->isBpmEventsDirty = true;
        }

        // 背景路径或资源类型改变时，按图片/视频分支重新探测媒体尺寸。
        if ( oldMetadata.main_cover_path != updatedMeta.main_cover_path ||
             oldMetadata.cover_type != updatedMeta.cover_type ) {
            auto       backgroundMetadata = updatedMeta;
            const auto source =
                Config::pathToUtf8(backgroundMetadata.main_cover_path);
            if ( const auto iterator =
                     m_ctx->collaborationPathRemap.find(source);
                 iterator != m_ctx->collaborationPathRemap.end() ) {
                // 协作会话的远端路径先映射到本地缓存，再交给媒体探测器。
                backgroundMetadata.main_cover_path =
                    Config::utf8ToPath(iterator->second);
            }
            SessionUtils::updateBackgroundSize(
                *m_ctx,
                backgroundMetadata,
                m_ctx->collaborationProject
                    ? m_ctx->collaborationProject.get()
                    : EditorEngine::instance().getCurrentProject());
        }
    }
    // 返回值由 processCommands 合并到本批 mutationFlags，不在处理器内直接通知。
    return mutationFlags;
}

/// @brief 标记 UI 直接修改的扩展元数据，并安排一次尾随自动保存。
/// @param cmd 仅作为扩展元数据已修改的信号，不携带新的字段副本。
/// @note 适用于属性表直接写入当前 BeatMap 的兼容入口，实际保存由低频轮询处理。
/// @details
/// 这是兼容仍直接编辑 map_properties 的 UI 入口，不复制或比较字段。信号只把
/// ActionStack 标为未保存并重置尾随计时；实际磁盘写入留给低频自动保存路径，
/// 避免属性控件连续输入时每个字符都阻塞文件系统。
void BeatmapSession::handleCommand(const CmdMarkBeatmapMetadataDirty& cmd)
{
    (void)cmd;
    // 没有谱面时无可持久化元数据，忽略信号而不创建虚假脏状态。
    if ( !m_ctx->currentBeatmap ) return;

    // 扩展元数据已由调用方原地更新，只需推进保存点与尾随计时状态。
    m_ctx->actionStack.markDirty();
    m_metadataAutoSavePending         = true;
    m_metadataAutoSaveTimerNeedsReset = true;
}

}  // namespace MMM::Logic
