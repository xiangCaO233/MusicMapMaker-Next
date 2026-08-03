#include "logic/EditorEngine.h"
#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/FrameLimitUtils.h"
#include "config/Utf8Path.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "event/logic/EditorConfigChangedEvent.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/project/ProjectEvents.h"
#include "event/ui/menu/ProjectLoadedEvent.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ProjectController.h"
#include "logic/ProjectResourceService.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/session/EditorAction.h"
#include "logic/session/NoteAction.h"
#include "logic/session/SampleAction.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "runtime/AppThreadPool.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <ice/thread/ThreadPool.hpp>
#include <iterator>
#include <limits>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(__GLIBC__)
#    include <malloc.h>
#endif

namespace MMM::Logic
{

namespace
{
/// @brief 逻辑循环限频等待使用的时钟类型，要求单调递增避免系统时间跳变影响。
using FrameLimitClock = std::chrono::steady_clock;

/// @brief 限频粗睡眠预留量，给操作系统调度精度留出少量余量。
constexpr auto FRAME_LIMIT_SLEEP_MARGIN = std::chrono::microseconds(250);

/// @brief 后台非活跃谱面画布的最高快照更新频率下限。
constexpr int BACKGROUND_SESSION_MIN_UPS = 60;

/// @brief 后台非活跃谱面画布的最高快照更新频率上限。
constexpr int BACKGROUND_SESSION_MAX_UPS = 240;

/// @brief RenderSnapshot 自适应预算的最低刷新率。
constexpr double RENDER_SNAPSHOT_MIN_HZ = 60.0;

/// @brief 主画布 RenderSnapshot 自适应预算的最高刷新率。
constexpr double RENDER_SNAPSHOT_MAIN_MAX_HZ = 480.0;

/// @brief 将项目卸载后已空闲的堆页尽快归还操作系统。
/// @warning 项目关闭低频路径：glibc 会遍历分配器空闲区，禁止放入逻辑、
/// UI 或渲染热路径；其他运行库保持默认回收策略。
void releaseUnusedHeapPages() noexcept
{
#if defined(__GLIBC__)
    static_cast<void>(malloc_trim(0));
#endif
}

/// @brief 辅助画布 RenderSnapshot 自适应预算的最高刷新率。
constexpr double RENDER_SNAPSHOT_SECONDARY_MAX_HZ = 240.0;

/// @brief 将持久化打击音效配置同步到全局实时混音控制库。
/// @param config 当前打击音效配置快照。
/// @warning 低频配置路径：只写入固定数量的 lock-free 原子控制字。
void syncKeySoundControls(const Config::SfxConfig& config)
{
    auto& audio = Audio::AudioManager::instance();
    audio.setPlayerKeySoundAreaMuted(!config.enableHitSfx);
    audio.setKeySoundEffectGroupMuted(Audio::KeySoundEffectGroup::Unbound,
                                      !config.enableUnboundHitSfx);
    audio.setKeySoundEffectGroupGain(Audio::KeySoundEffectGroup::Unbound,
                                     config.unboundHitSfxGain);
    audio.setKeySoundEffectGroupMuted(Audio::KeySoundEffectGroup::Bound,
                                      !config.enableBoundHitSfx);
    audio.setKeySoundEffectGroupGain(Audio::KeySoundEffectGroup::Bound,
                                     config.boundHitSfxGain);
}

/// @brief 没有可用 FPS 统计时的 RenderSnapshot 回退刷新率。
constexpr double RENDER_SNAPSHOT_FALLBACK_HZ = 240.0;

/// @brief 同主音轨画布同步的逻辑时间变化阈值。
constexpr double MAIN_AUDIO_SYNC_TIME_EPSILON = 1e-6;

/// @brief 同步播放中 follower 本地插值领先 active 时允许的回退容差。
constexpr double MAIN_AUDIO_SYNC_BACKWARD_RESET_EPSILON = 0.01;

/// @brief 判断显式音频重命名输入是否为单个有效文件名。
/// @param filename UTF-8 文件名。
/// @return 非空、非点目录且不含任一平台路径分隔符时返回 true。
bool isValidAudioResourceFileName(std::string_view filename)
{
    return !filename.empty() && filename != "." && filename != ".." &&
           filename.find('/') == std::string_view::npos &&
           filename.find('\\') == std::string_view::npos;
}

/// @brief 将会话播放位置解析到指定 steady_clock 时刻。
/// @param ctx 待读取的会话状态。
/// @param nowSteadySeconds 本次低频操作共享的单调时钟秒数。
/// @return 保留负视觉前置区间并按谱面总时长限制上界的连续播放时间。
/// @warning 低频工作区保存与标签切换路径：只执行常量级时钟计算。
[[nodiscard]] double resolveContinuousSessionTime(const SessionContext& ctx,
                                                  double nowSteadySeconds)
{
    double resolvedTime = ctx.currentTime;
    if ( ctx.playbackVisualClock.initialized() ) {
        resolvedTime = ctx.playbackVisualClock.currentTimeAt(nowSteadySeconds);
    }
    if ( !std::isfinite(resolvedTime) ) {
        resolvedTime = std::isfinite(ctx.currentTime) ? ctx.currentTime : 0.0;
    }

    const double totalTime = SessionUtils::getEffectiveTotalTimeSeconds(ctx);
    if ( std::isfinite(totalTime) ) {
        resolvedTime = std::min(resolvedTime, totalTime);
    }
    return resolvedTime;
}

/// @brief 发布项目或谱面包打开失败事件。
/// @param path 尝试打开的路径。
/// @param message 失败原因。
/// @param isPackage 是否为谱面包打开失败。
void publishProjectOpenFailed(const std::filesystem::path& path,
                              const std::string& message, bool isPackage)
{
    Event::ProjectOpenFailedEvent event;
    event.m_projectPath  = Config::pathToUtf8(path);
    event.m_errorMessage = message;
    event.m_isPackage    = isPackage;
    Event::EventBus::instance().publish(event);
}

/// @brief 发布项目即将进入关闭旧项目并加载新项目阶段的事件。
/// @param path 正在打开的项目目录、谱面文件或谱面包路径。
/// @param isPackage 当前是否正在打开临时谱面包。
void publishProjectOpenStarted(const std::filesystem::path& path,
                               bool                         isPackage)
{
    Event::ProjectOpenStartedEvent event;
    event.m_projectPath = Config::pathToUtf8(path);
    event.m_isPackage   = isPackage;
    Event::EventBus::instance().publish(event);
}

/// @brief 为同主音轨后台跟随谱面推进动画时间上的打击特效事件。
/// @warning 逻辑热路径：同主音轨同步时调用；普通路径只线性消费已排序
/// hitEvents；播放不连续的低频重置分支允许扫描事件序列补建持续 Hold，只有
/// 音符变更后的脏分支允许重建事件序列。
void updateFollowerHitEffects(SessionContext& ctx, double previousAnimateTime,
                              bool resetHitIndex)
{
    const auto& config = ctx.lastConfig;
    SessionUtils::ensureHitEvents(ctx);

    if ( resetHitIndex ) {
        SessionUtils::syncHitIndex(ctx);
        ctx.hitFXSystem.restoreActiveHoldEffects(
            ctx.animateTime, ctx.hitEvents, config);
        return;
    }

    std::vector<System::HitFXSystem::HitEvent> triggeredEvents;
    while ( ctx.nextHitIndex < ctx.hitEvents.size() &&
            ctx.hitEvents[ctx.nextHitIndex].timestamp <= ctx.animateTime ) {
        const auto& ev = ctx.hitEvents[ctx.nextHitIndex];
        if ( ev.timestamp > previousAnimateTime ) {
            triggeredEvents.push_back(ev);
        }
        ctx.nextHitIndex++;
    }

    ctx.hitFXSystem.update(
        ctx.animateTime, triggeredEvents, ctx.trackCount, config);
}

/// @brief 等待到目标逻辑更新时间点，避免用 yield 反复忙等。
/// @warning 逻辑热路径等待：UPS 提前到达目标间隔时执行；只能包含 sleep
/// 和时间查询，禁止加入锁、分配或业务逻辑。
void sleepUntilFrameDeadline(FrameLimitClock::time_point deadline)
{
    while ( true ) {
        auto now = FrameLimitClock::now();
        if ( now >= deadline ) {
            return;
        }

        auto remaining = deadline - now;
        if ( remaining > FRAME_LIMIT_SLEEP_MARGIN ) {
            std::this_thread::sleep_for(remaining - FRAME_LIMIT_SLEEP_MARGIN);
        } else {
            std::this_thread::sleep_until(deadline);
            return;
        }
    }
}

/// @brief 根据设备刷新率计算后台谱面画布的快照更新间隔。
/// @warning 逻辑热路径：每 update 调用；只做常量级整数夹取和 duration 转换。
FrameLimitClock::duration backgroundSessionUpdateInterval(int refreshRate)
{
    int backgroundUps = std::clamp(
        refreshRate, BACKGROUND_SESSION_MIN_UPS, BACKGROUND_SESSION_MAX_UPS);
    return std::chrono::duration_cast<FrameLimitClock::duration>(
        std::chrono::duration<double>(1.0 /
                                      static_cast<double>(backgroundUps)));
}

/// @brief 根据帧率限制设置计算逻辑线程目标 UPS。
/// @param frameLimit 当前帧率限制偏好。
/// @return 有固定目标时返回目标 UPS；Unlimited 返回 0。
/// @warning 逻辑热路径：自适应快照预算调用；只读取设备刷新率并做常量级计算。
double frameLimitTargetUps(Config::FrameLimitPreference frameLimit)
{
    return Config::frameLimitTargetRate(
        frameLimit, Config::AppConfig::instance().getDeviceRefreshRate());
}

/// @brief 根据 UPS 健康度降低快照刷新率预算。
/// @param snapshotHz 当前快照预算。
/// @param logicUps 当前实测 UPS。
/// @param targetUps 当前目标 UPS。
/// @return 调整后的快照预算。
/// @warning 逻辑热路径：只做常量级数值计算。
double applyUpsBackpressure(double snapshotHz, double logicUps,
                            double targetUps)
{
    if ( logicUps <= 1.0 || targetUps <= 1.0 || !std::isfinite(logicUps) ||
         !std::isfinite(targetUps) ) {
        return snapshotHz;
    }

    const double health = logicUps / targetUps;
    if ( health < 0.70 ) {
        return snapshotHz * 0.50;
    }
    if ( health < 0.85 ) {
        return snapshotHz * 0.65;
    }
    if ( health < 0.95 ) {
        return snapshotHz * 0.80;
    }
    return snapshotHz;
}

/// @brief 根据 Session 上下文计算软件光标 BPM 同步烟雾寿命。
/// @param ctx 当前活跃 Session 上下文。
/// @return 当前 BPM 对应的一拍时长；无有效 BPM 时返回 -1。
/// @warning 逻辑热路径：每 update 为活跃 Session 执行；只读取已缓存排序的
/// bpmEvents 并二分查找，禁止回退为完整 timings 遍历或加锁访问 UI 状态。
float calculateCursorSmokeLifeOverride(const SessionContext& ctx)
{
    if ( !ctx.currentBeatmap ) {
        return -1.0f;
    }

    double bpm = ctx.currentBeatmap->m_baseMapMetadata.preference_bpm;
    auto it = std::upper_bound(ctx.bpmEvents.begin(),
                               ctx.bpmEvents.end(),
                               ctx.currentTime,
                               [](double time, const TimelineComponent* event) {
                                   return time < event->m_timestamp;
                               });

    if ( it != ctx.bpmEvents.begin() ) {
        const auto* event = *std::prev(it);
        if ( event ) {
            bpm = event->m_value;
        }
    }

    if ( bpm <= 0.0 ) {
        return -1.0f;
    }
    return static_cast<float>(60.0 / bpm);
}

/// @brief 从带索引 Session 快照中解析活跃 Session 的光标烟雾寿命。
/// @param sessions 当前逻辑线程持有的带索引 Session 快照。
/// @param activeIndex 当前活跃 Session 的注册表索引。
/// @return 当前活跃 Session 的烟雾寿命覆盖值；无活跃 Session 时返回 -1。
/// @warning 逻辑热路径：每 update 执行；只遍历当前打开的 Session
/// 快照，不访问注册表锁。
float resolveActiveCursorSmokeLifeOverride(
    const std::vector<SessionSnapshotEntry>& sessions, int32_t activeIndex)
{
    for ( const auto& entry : sessions ) {
        if ( entry.index == activeIndex && entry.session ) {
            return calculateCursorSmokeLifeOverride(
                entry.session->getContext());
        }
    }
    return -1.0f;
}

/// @brief 在已持锁的 Session 列表中按 cameraId 查找索引。
/// @param sessions 当前 Session 条目列表。
/// @param cameraId 目标画布 cameraId。
/// @return 找到时返回 Session 索引，否则返回 -1。
/// @warning 逻辑/UI 热路径辅助：调用者必须已经持有 SessionRegistry 锁。
int32_t findSessionIndexByCameraIdUnsafe(
    const std::vector<SessionEntry>& sessions, const std::string& cameraId)
{
    for ( int32_t index = 0; index < static_cast<int32_t>(sessions.size());
          ++index ) {
        if ( sessions[static_cast<size_t>(index)].cameraId == cameraId ) {
            return index;
        }
    }
    return -1;
}

/// @brief 判断目标 Session 是否允许接收当前 hover 滚轮。
/// @param sessions 当前 Session 条目列表。
/// @param activeIndex 当前活动 Session 索引。
/// @param targetIndex 鼠标悬停的目标 Session 索引。
/// @return 目标为活动项或两者主音轨同步键相同时返回 true。
/// @warning 逻辑/UI 热路径辅助：调用者必须已经持有 SessionRegistry 锁。
bool canUseHoverScrollTargetUnsafe(const std::vector<SessionEntry>& sessions,
                                   int32_t activeIndex, int32_t targetIndex)
{
    if ( targetIndex < 0 ||
         targetIndex >= static_cast<int32_t>(sessions.size()) ||
         !sessions[static_cast<size_t>(targetIndex)].session ) {
        return false;
    }
    if ( targetIndex == activeIndex ) {
        return true;
    }
    if ( activeIndex < 0 ||
         activeIndex >= static_cast<int32_t>(sessions.size()) ||
         !sessions[static_cast<size_t>(activeIndex)].session ) {
        return false;
    }

    const auto& activeFingerprint =
        sessions[static_cast<size_t>(activeIndex)].audioTimelineFingerprint;
    const auto& targetFingerprint =
        sessions[static_cast<size_t>(targetIndex)].audioTimelineFingerprint;
    return !activeFingerprint.empty() && activeFingerprint == targetFingerprint;
}

/// @brief 将持久化的项目相对路径解析为文件系统路径。
std::filesystem::path resolveProjectPath(const Project&               project,
                                         const std::filesystem::path& path)
{
    if ( path.empty() || path.is_absolute() ) {
        return path.lexically_normal();
    }
    return (project.m_projectRoot / path).lexically_normal();
}

/// @brief 将文件系统路径转换为稳定的项目相对路径。
std::filesystem::path makeProjectRelativePath(const Project& project,
                                              const std::filesystem::path& path)
{
    if ( path.empty() ) return {};
    if ( path.is_relative() ) return path.lexically_normal();

    std::error_code ec;
    auto            root = std::filesystem::absolute(project.m_projectRoot, ec);
    if ( ec ) return path.filename();

    auto relativePath = std::filesystem::relative(path, root, ec);
    if ( !ec && !relativePath.empty() ) {
        return relativePath.lexically_normal();
    }
    return path.filename();
}

/// @brief 生成用于判断谱面是否已打开的稳定路径键。
/// @param project 当前项目；存在时相对路径按项目根目录解析。
/// @param path 谱面文件路径。
/// @return 规范化后的 UTF-8 路径键，空路径返回空字符串。
std::string makeBeatmapPathKey(const Project*               project,
                               const std::filesystem::path& path)
{
    if ( path.empty() ) {
        return "";
    }

    std::filesystem::path keyPath = path;
    if ( project ) {
        keyPath = resolveProjectPath(*project, keyPath);
    } else if ( keyPath.is_relative() ) {
        std::error_code ec;
        auto            absolutePath = std::filesystem::absolute(keyPath, ec);
        if ( !ec ) {
            keyPath = absolutePath;
        }
    }

    std::error_code ec;
    auto canonicalPath = std::filesystem::weakly_canonical(keyPath, ec);
    if ( !ec ) {
        keyPath = canonicalPath;
    }

    return Config::pathToUtf8(keyPath.lexically_normal());
}

/// @brief 取得打开会话用于资源引用诊断的项目相对谱面路径。
/// @param project 当前项目。
/// @param entry 当前 Session 条目。
/// @param ctx 当前 Session 上下文。
/// @return 优先返回项目相对谱面路径，无法解析时返回显示名。
std::string getOpenSessionBeatmapDiagnosticPath(const Project&        project,
                                                const SessionEntry&   entry,
                                                const SessionContext& ctx)
{
    std::filesystem::path mapPath;
    if ( ctx.currentBeatmap ) {
        mapPath = ctx.currentBeatmap->m_baseMapMetadata.map_path;
    }
    if ( mapPath.empty() && !entry.beatmapPathKey.empty() ) {
        mapPath = Config::utf8ToPath(entry.beatmapPathKey);
    }
    if ( !mapPath.empty() ) {
        const auto relativePath = makeProjectRelativePath(
            project, resolveProjectPath(project, mapPath));
        if ( !relativePath.empty() ) {
            return Config::pathToUtf8(relativePath);
        }
    }
    return entry.displayName.empty() ? std::string("<opened beatmap>")
                                     : entry.displayName;
}

/// @brief 同步并收集全部打开会话的当前内存音频引用。
/// @param project 当前项目。
/// @param sessions 调用者持锁访问的 Session 条目。
/// @return 可与磁盘扫描结果共同校验的内存引用列表。
/// @warning 低频项目资源操作路径：会按需遍历每个脏 Session 的完整 ECS。
std::vector<BeatmapAudioReference> collectOpenSessionAudioReferencesUnsafe(
    const Project& project, std::vector<SessionEntry>& sessions)
{
    std::vector<BeatmapAudioReference> result;
    for ( auto& entry : sessions ) {
        if ( entry.isLogoPlaceholder || !entry.session ) continue;

        auto& ctx = entry.session->getContextMutable();
        if ( !ctx.currentBeatmap ) continue;

        SessionUtils::syncBeatmap(ctx);
        const auto beatmapPath =
            getOpenSessionBeatmapDiagnosticPath(project, entry, ctx);
        auto references = ProjectResourceService::collectBeatmapAudioReferences(
            *ctx.currentBeatmap, beatmapPath);
        result.insert(result.end(),
                      std::make_move_iterator(references.begin()),
                      std::make_move_iterator(references.end()));
    }
    return result;
}

/// @brief 发布音频资源变更结果，供 UI 显示成功或引用阻止原因。
/// @param operation 本次资源操作类型。
/// @param resourceId 目标资源 ID。
/// @param success 操作是否成功。
/// @param blockingBeatmapPaths 阻止操作的具体谱面路径。
/// @param errorMessage 失败原因。
void publishAudioResourceMutationResult(
    Event::AudioResourceMutationOperation operation,
    const std::string& resourceId, bool success,
    const std::vector<std::string>& blockingBeatmapPaths,
    const std::string&              errorMessage)
{
    Event::AudioResourceMutationResultEvent event;
    event.m_operation            = operation;
    event.m_resourceId           = resourceId;
    event.m_success              = success;
    event.m_blockingBeatmapPaths = blockingBeatmapPaths;
    event.m_errorMessage         = errorMessage;
    Event::EventBus::instance().publish(event);
    if ( !success ) {
        XWARN("Audio resource mutation failed for '{}': {}",
              resourceId,
              errorMessage);
        for ( const auto& beatmapPath : blockingBeatmapPaths ) {
            XWARN("  Blocking beatmap: {}", beatmapPath);
        }
    }
}

/// @brief 单个打开会话 ECS 的音频引用重映射结果。
struct SessionAudioReferenceRemapResult {
    /// @brief 实际改写的玩家物件绑定数量。
    std::size_t m_changedNoteBindingCount{ 0U };

    /// @brief 匹配目标资源的自动采样数量。
    std::size_t m_audioSampleReferenceCount{ 0U };

    /// @brief 实际改写的自动采样数量。
    std::size_t m_changedAudioSampleCount{ 0U };
};

/// @brief 将打开会话 ECS 中的移动前路径引用改为稳定资源 ID。
/// @param project 当前项目。
/// @param ctx 待更新的会话上下文。
/// @param beatmapPath 用于相对路径匹配的具体谱面路径。
/// @param previousResource 移动前资源快照。
/// @return ECS 匹配和实际重写数量。
SessionAudioReferenceRemapResult remapSessionEcsAudioReferences(
    const Project& project, SessionContext& ctx, const std::string& beatmapPath,
    const AudioResource& previousResource)
{
    SessionAudioReferenceRemapResult result;
    const auto matchesPreviousResource = [&](const std::string& audioReference,
                                             BeatmapAudioReferenceKind kind) {
        return ProjectResourceService::audioReferenceMatchesResource(
            project,
            BeatmapAudioReference{
                beatmapPath,
                audioReference,
                kind,
            },
            previousResource);
    };
    const auto remapBinding = [&](std::optional<AudioSampleBinding>& binding) {
        if ( !binding ||
             !matchesPreviousResource(
                 binding->m_audioResourceId,
                 BeatmapAudioReferenceKind::NoteSampleBinding) ||
             binding->m_audioResourceId == previousResource.m_id ) {
            return;
        }
        binding->m_audioResourceId = previousResource.m_id;
        ++result.m_changedNoteBindingCount;
    };

    auto noteView = ctx.noteRegistry.view<NoteComponent>();
    for ( auto entity : noteView ) {
        auto& note = noteView.get<NoteComponent>(entity);
        remapBinding(note.m_sampleBinding);
        for ( auto& subNote : note.m_subNotes ) {
            remapBinding(subNote.sampleBinding);
        }
    }

    auto sampleView = ctx.sampleRegistry.view<SampleComponent>();
    for ( auto entity : sampleView ) {
        auto& sample = sampleView.get<SampleComponent>(entity);
        if ( !matchesPreviousResource(
                 sample.m_audioResourceId,
                 BeatmapAudioReferenceKind::AudioSampleEvent) ) {
            continue;
        }
        ++result.m_audioSampleReferenceCount;
        if ( sample.m_audioResourceId == previousResource.m_id ) continue;
        sample.m_audioResourceId = previousResource.m_id;
        ++result.m_changedAudioSampleCount;
    }
    return result;
}

/// @brief 精确重命名打开会话 ECS 中的玩家绑定和自动采样资源 ID。
/// @param ctx 待更新会话。
/// @param oldResourceId 旧资源 ID。
/// @param newResourceId 新资源 ID。
/// @return 实际改写的 ECS 字段数量。
SessionAudioReferenceRemapResult remapSessionEcsAudioResourceId(
    SessionContext& ctx, std::string_view oldResourceId,
    std::string_view newResourceId)
{
    SessionAudioReferenceRemapResult result;
    const auto remapBinding = [&](std::optional<AudioSampleBinding>& binding) {
        if ( !binding || binding->m_audioResourceId != oldResourceId ) return;
        binding->m_audioResourceId = newResourceId;
        ++result.m_changedNoteBindingCount;
    };

    auto noteView = ctx.noteRegistry.view<NoteComponent>();
    for ( auto entity : noteView ) {
        auto& note = noteView.get<NoteComponent>(entity);
        remapBinding(note.m_sampleBinding);
        for ( auto& subNote : note.m_subNotes ) {
            remapBinding(subNote.sampleBinding);
        }
    }

    auto sampleView = ctx.sampleRegistry.view<SampleComponent>();
    for ( auto entity : sampleView ) {
        auto& sample = sampleView.get<SampleComponent>(entity);
        if ( sample.m_audioResourceId != oldResourceId ) continue;
        ++result.m_audioSampleReferenceCount;
        sample.m_audioResourceId = newResourceId;
        ++result.m_changedAudioSampleCount;
    }
    return result;
}

/// @brief 在写入项目前解析元数据资源路径。
std::filesystem::path resolveMetadataResourcePath(
    const Project& project, const std::filesystem::path& mapDirectory,
    const std::filesystem::path& path, bool preferProjectRoot)
{
    if ( path.empty() || path.is_absolute() ) {
        return path.lexically_normal();
    }

    auto projectPath = resolveProjectPath(project, path);
    auto mapPath     = (mapDirectory / path).lexically_normal();

    std::error_code ec;
    if ( preferProjectRoot ) {
        if ( std::filesystem::exists(projectPath, ec) ) return projectPath;
        ec.clear();
        if ( std::filesystem::exists(mapPath, ec) ) return mapPath;
        return projectPath;
    }

    if ( std::filesystem::exists(mapPath, ec) ) return mapPath;
    ec.clear();
    if ( std::filesystem::exists(projectPath, ec) ) return projectPath;
    return mapPath;
}

/// @brief 将谱面元数据中的长期路径规范化为项目相对路径。
void normalizeBeatmapMetadataPathsForProject(BeatMap&       beatMap,
                                             const Project& project)
{
    auto& meta = beatMap.m_baseMapMetadata;
    if ( meta.map_path.empty() ) return;

    auto absoluteMapPath = resolveProjectPath(project, meta.map_path);
    auto mapDirectory    = absoluteMapPath.parent_path();
    auto mapExtension    = Config::pathToUtf8(absoluteMapPath.extension());
    std::transform(mapExtension.begin(),
                   mapExtension.end(),
                   mapExtension.begin(),
                   ::tolower);
    bool preferProjectRoot = (mapExtension == ".mmm");

    meta.map_path = makeProjectRelativePath(project, absoluteMapPath);

    auto normalizeResourcePath = [&](std::filesystem::path& path) {
        if ( path.empty() ) return;
        auto resolved = resolveMetadataResourcePath(
            project, mapDirectory, path, preferProjectRoot);
        path = makeProjectRelativePath(project, resolved);
    };

    normalizeResourcePath(meta.main_audio_path);
    normalizeResourcePath(meta.song_file_hint);
    normalizeResourcePath(meta.main_cover_path);
    normalizeResourcePath(meta.cover_path);
}

/// @brief 将编辑工具枚举转换为项目工作区中的稳定文本。
std::string editToolToWorkspaceName(EditTool tool)
{
    switch ( tool ) {
    case EditTool::Marquee: return "Marquee";
    case EditTool::Draw: return "Draw";
    case EditTool::ColorBrush: return "ColorBrush";
    case EditTool::ColorEraser: return "ColorEraser";
    case EditTool::Layout:
    case EditTool::Move:
    default: return "Move";
    }
}

/// @brief 将项目工作区中的稳定文本转换为编辑工具枚举。
EditTool workspaceNameToEditTool(const std::string& name)
{
    if ( name == "Marquee" ) {
        return EditTool::Marquee;
    }
    if ( name == "Draw" ) {
        return EditTool::Draw;
    }
    if ( name == "ColorBrush" ) {
        return EditTool::ColorBrush;
    }
    if ( name == "ColorEraser" ) {
        return EditTool::ColorEraser;
    }
    return EditTool::Move;
}

/// @brief 将分拍线显示模式转换为项目工作区稳定文本。
/// @param mode 当前分拍线显示模式。
/// @return 可持久化的稳定模式文本。
const char* beatLineDisplayModeToWorkspaceName(Config::BeatLineDisplayMode mode)
{
    switch ( mode ) {
    case Config::BeatLineDisplayMode::NearCursor: return "NearCursor";
    case Config::BeatLineDisplayMode::Hidden: return "Hidden";
    case Config::BeatLineDisplayMode::Always:
    default: return "Always";
    }
}

/// @brief 将项目工作区稳定文本转换为分拍线显示模式。
/// @param name 工作区中保存的模式文本。
/// @return 对应的分拍线显示模式。
Config::BeatLineDisplayMode workspaceNameToBeatLineDisplayMode(
    const std::string& name)
{
    if ( name == "NearCursor" ) {
        return Config::BeatLineDisplayMode::NearCursor;
    }
    if ( name == "Hidden" ) {
        return Config::BeatLineDisplayMode::Hidden;
    }
    return Config::BeatLineDisplayMode::Always;
}

/// @brief 将物件放置磁吸模式转换为项目工作区稳定文本。
/// @param mode 当前物件放置磁吸模式。
/// @return 可持久化的稳定模式文本。
const char* objectPlacementSnapModeToWorkspaceName(
    Config::ObjectPlacementSnapMode mode)
{
    switch ( mode ) {
    case Config::ObjectPlacementSnapMode::CommonBeatDivisors:
        return "CommonBeatDivisors";
    case Config::ObjectPlacementSnapMode::CurrentBeatDivisor:
    default: return "CurrentBeatDivisor";
    }
}

/// @brief 将项目工作区稳定文本转换为物件放置磁吸模式。
/// @param name 工作区中保存的模式文本。
/// @return 对应的物件放置磁吸模式。
Config::ObjectPlacementSnapMode workspaceNameToObjectPlacementSnapMode(
    const std::string& name)
{
    if ( name == "CommonBeatDivisors" ) {
        return Config::ObjectPlacementSnapMode::CommonBeatDivisors;
    }
    return Config::ObjectPlacementSnapMode::CurrentBeatDivisor;
}

/// @brief 捕获工具栏开关到项目工作区状态。
/// @param workspace 需要写入的项目工作区状态。
/// @param editorConfig 当前编辑器配置。
/// @param syncSameMainAudioCanvases 当前多画布同主音轨同步开关。
void captureToolbarWorkspaceState(ProjectWorkspaceState&      workspace,
                                  const Config::EditorConfig& editorConfig,
                                  bool syncSameMainAudioCanvases)
{
    auto& toolbarState           = workspace.m_toolbarState;
    toolbarState.m_valid         = true;
    toolbarState.m_reverseScroll = editorConfig.settings.reverseScroll;
    toolbarState.m_scrollSnap    = editorConfig.settings.scrollSnap;
    toolbarState.m_objectPlacementSnap =
        editorConfig.settings.objectPlacementSnap;
    toolbarState.m_objectPlacementSnapMode =
        objectPlacementSnapModeToWorkspaceName(
            editorConfig.settings.objectPlacementSnapMode);
    toolbarState.m_commonBeatDivisorMask =
        editorConfig.settings.commonBeatDivisorMask;
    toolbarState.m_snapFloor = editorConfig.settings.snapFloor;
    toolbarState.m_enableLinearScrollMapping =
        editorConfig.visual.enableLinearScrollMapping;
    toolbarState.m_beatLineDisplayMode = beatLineDisplayModeToWorkspaceName(
        editorConfig.visual.beatLineDisplayMode);
    toolbarState.m_drawBeatLines = editorConfig.visual.beatLineDisplayMode !=
                                   Config::BeatLineDisplayMode::Hidden;
    toolbarState.m_stopPlaybackOnScroll =
        editorConfig.settings.stopPlaybackOnScroll;
    toolbarState.m_enableHitEffects = editorConfig.visual.enableHitEffects;
    toolbarState.m_beatDivisor      = editorConfig.settings.beatDivisor;
    toolbarState.m_timelineZoom     = editorConfig.visual.timelineZoom;
    toolbarState.m_syncSameMainAudioCanvases = syncSameMainAudioCanvases;
}

/// @brief 将项目工作区工具栏状态应用到编辑器配置。
/// @param editorConfig 需要修改的编辑器配置。
/// @param toolbarState 项目工作区中保存的工具栏状态。
void applyToolbarWorkspaceState(
    Config::EditorConfig&               editorConfig,
    const ProjectWorkspaceToolbarState& toolbarState)
{
    editorConfig.settings.reverseScroll = toolbarState.m_reverseScroll;
    editorConfig.settings.scrollSnap    = toolbarState.m_scrollSnap;
    editorConfig.settings.objectPlacementSnap =
        toolbarState.m_objectPlacementSnap;
    editorConfig.settings.objectPlacementSnapMode =
        workspaceNameToObjectPlacementSnapMode(
            toolbarState.m_objectPlacementSnapMode);
    editorConfig.settings.commonBeatDivisorMask =
        toolbarState.m_commonBeatDivisorMask &
        Config::COMMON_BEAT_DIVISOR_MASK_ALL;
    editorConfig.settings.snapFloor = toolbarState.m_snapFloor;
    editorConfig.visual.enableLinearScrollMapping =
        toolbarState.m_enableLinearScrollMapping;
    editorConfig.visual.beatLineDisplayMode =
        workspaceNameToBeatLineDisplayMode(toolbarState.m_beatLineDisplayMode);
    editorConfig.settings.stopPlaybackOnScroll =
        toolbarState.m_stopPlaybackOnScroll;
    editorConfig.visual.enableHitEffects = toolbarState.m_enableHitEffects;
    editorConfig.settings.beatDivisor =
        std::clamp(toolbarState.m_beatDivisor, 1, 64);
    editorConfig.visual.timelineZoom =
        std::clamp(toolbarState.m_timelineZoom, 0.1f, 10.0f);
}

/// @brief 保留由 AppConfig 直接维护的全局软件级状态。
/// @param target 即将写回引擎和 AppConfig 的配置。
/// @param source 当前 AppConfig 中的全局配置快照。
void preserveGlobalAppManagedSettings(Config::EditorConfig&       target,
                                      const Config::EditorConfig& source)
{
    target.settings.showTimelineWindow = source.settings.showTimelineWindow;
    target.settings.timelineProfessionalMode =
        source.settings.timelineProfessionalMode;
    target.settings.showPreviewWindow = source.settings.showPreviewWindow;
    target.settings.showToolLabels    = source.settings.showToolLabels;
    target.settings.fixedToolWindow   = source.settings.fixedToolWindow;
    target.settings.showManagerLabels = source.settings.showManagerLabels;
    target.settings.enablePolylineEditing =
        source.settings.enablePolylineEditing;
    target.settings.enableBmsEditing = source.settings.enableBmsEditing;
    target.settings.autoUploadPgoProfiles =
        source.settings.autoUploadPgoProfiles;
    target.settings.pgoProfileUploadConsentAsked =
        source.settings.pgoProfileUploadConsentAsked;
    target.settings.bpmMeasurementToolPreferences =
        source.settings.bpmMeasurementToolPreferences;
}

/// @brief 判断逻辑指令是否会修改临时项目内容。
/// @param cmd 待检查的逻辑指令。
/// @return 指令会修改谱面或项目资源时返回 true。
/// @warning 逻辑热路径低频分支：仅在命令入队时做 variant 类型判断。
bool isTemporaryProjectMutationCommand(const LogicCommand& cmd)
{
    if ( std::holds_alternative<CmdCreateBeatmap>(cmd) ||
         std::holds_alternative<CmdStartDrag>(cmd) ||
         std::holds_alternative<CmdUpdateDrag>(cmd) ||
         std::holds_alternative<CmdCreateAudioSample>(cmd) ||
         std::holds_alternative<CmdUpdateAudioSampleProperties>(cmd) ||
         std::holds_alternative<CmdUpdateObjectSampleVolume>(cmd) ||
         std::holds_alternative<CmdUpdateTrackCount>(cmd) ||
         std::holds_alternative<CmdUpdateBgmTrackCount>(cmd) ||
         std::holds_alternative<CmdUndo>(cmd) ||
         std::holds_alternative<CmdRedo>(cmd) ||
         std::holds_alternative<CmdPaste>(cmd) ||
         std::holds_alternative<CmdCut>(cmd) ||
         std::holds_alternative<CmdDeleteSelected>(cmd) ||
         std::holds_alternative<CmdMirrorSelected>(cmd) ||
         std::holds_alternative<CmdAlignSelectedToCommonBeats>(cmd) ||
         std::holds_alternative<CmdApplyNoteColorToSelection>(cmd) ||
         std::holds_alternative<CmdApplyNotePaletteToSelection>(cmd) ||
         std::holds_alternative<CmdApplyBrushPaletteToEntity>(cmd) ||
         std::holds_alternative<CmdClearNoteColorOverrides>(cmd) ||
         std::holds_alternative<CmdSaveBeatmap>(cmd) ||
         std::holds_alternative<CmdSaveBeatmapAs>(cmd) ||
         std::holds_alternative<CmdUpdateTimelineEvent>(cmd) ||
         std::holds_alternative<CmdUpdateTimelineEvents>(cmd) ||
         std::holds_alternative<CmdDeleteTimelineEvent>(cmd) ||
         std::holds_alternative<CmdCreateTimelineEvent>(cmd) ||
         std::holds_alternative<CmdCreateTimelineEvents>(cmd) ||
         std::holds_alternative<CmdReplaceBeatmapTimings>(cmd) ||
         std::holds_alternative<CmdReplaceBeatmapData>(cmd) ||
         std::holds_alternative<CmdStartBrush>(cmd) ||
         std::holds_alternative<CmdUpdateBrush>(cmd) ||
         std::holds_alternative<CmdStartErase>(cmd) ||
         std::holds_alternative<CmdUpdateErase>(cmd) ||
         std::holds_alternative<CmdUpdateBeatmapMetadata>(cmd) ||
         std::holds_alternative<CmdMarkBeatmapMetadataDirty>(cmd) ||
         std::holds_alternative<CmdImportAudio>(cmd) ||
         std::holds_alternative<CmdUpdateAudioResource>(cmd) ||
         std::holds_alternative<CmdRenameAudioResource>(cmd) ||
         std::holds_alternative<CmdUpdateAudioResourceConfig>(cmd) ||
         std::holds_alternative<CmdRemoveAudioResource>(cmd) ||
         std::holds_alternative<CmdRemoveBeatmap>(cmd) ) {
        return true;
    }

    if ( const auto* pack = std::get_if<CmdPackBeatmap>(&cmd) ) {
        return pack->saveConvertedBeatmapsToProject;
    }

    return false;
}
}  // namespace

EditorEngine& EditorEngine::instance()
{
    static EditorEngine instance;
    return instance;
}

EditorEngine::EditorEngine()
{
    // 从全局配置初始化本地缓存
    const auto initialConfig = Config::AppConfig::instance().getEditorConfig();
    {
        std::lock_guard<std::mutex> lock(m_editorConfigMutex);
        m_editorConfig = initialConfig;
        m_editorConfigRevision.fetch_add(1, std::memory_order_release);
    }
    m_frameLimitPreference.store(initialConfig.settings.frameLimit,
                                 std::memory_order_relaxed);

    /// @brief 初始化项目控制器单例并建立项目事件订阅。
    (void)ProjectController::instance();

    // 不在此处创建初始 Session，改由 GameLoop 通过 createSession() 创建 Logo
    // 画布

    // 订阅画布尺寸改变事件 — 路由到拥有该 cameraId 的 Session
    Event::EventBus::instance().subscribe<Event::CanvasResizeEvent>(
        [this](const Event::CanvasResizeEvent& e) {
            // 视口更新指令需要分发到拥有该 cameraId 的 Session
            CmdUpdateViewport cmd{ e.canvasName,
                                   static_cast<float>(e.newSize.x),
                                   static_cast<float>(e.newSize.y) };
            // 缓存视口尺寸
            m_renderSyncRegistry.cacheViewportSize(cmd.cameraId,
                                                   { cmd.width, cmd.height });
            // 推送到拥有该 camera 的 session
            /// @brief 保护本次画布尺寸事件路由期间的会话列表访问。
            std::lock_guard<std::recursive_mutex> lock(
                m_sessionRegistry.mutex());
            /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
            auto& sessions = m_sessionRegistry.entriesUnsafe();
            for ( auto& entry : sessions ) {
                if ( entry.cameraId == e.canvasName ) {
                    entry.session->pushCommand(LogicCommand(cmd));
                    break;
                }
            }
            // 也推送给共享视口 (Preview, Timeline 等)
            if ( e.canvasName == "Preview" || e.canvasName == "Timeline" ) {
                /// @brief 当前活跃 Session 索引快照。
                int32_t idx = m_sessionRegistry.activeIndex();
                if ( idx >= 0 && idx < static_cast<int32_t>(sessions.size()) ) {
                    sessions[idx].session->pushCommand(LogicCommand(cmd));
                }
            }
        });

    // 订阅逻辑指令事件
    Event::EventBus::instance().subscribe<Event::LogicCommandEvent>(
        [this](const Event::LogicCommandEvent& e) {
            if ( std::holds_alternative<CmdUpdateEditorConfig>(e.command) ) {
                setEditorConfig(
                    std::get<CmdUpdateEditorConfig>(e.command).config);
            } else {
                pushCommand(MMM::Logic::LogicCommand(e.command));
            }
        });
}

EditorEngine::~EditorEngine()
{
    stop();
}

/// @brief 获取当前项目。
/// @return 未打开项目时返回 nullptr。
Project* EditorEngine::getCurrentProject()
{
    return ProjectController::instance().currentProject();
}

/// @brief 获取当前项目的只读指针。
/// @return 未打开项目时返回 nullptr。
const Project* EditorEngine::getCurrentProject() const
{
    return ProjectController::instance().currentProject();
}

/// @brief 当前是否打开了临时只读项目。
/// @return 当前项目为临时项目时返回 true。
bool EditorEngine::isTemporaryProjectOpen() const
{
    return ProjectController::instance().isCurrentProjectTemporary();
}

/// @brief 获取当前临时项目的运行时路径信息。
/// @return 当前临时项目源包与缓存目录；非临时项目时返回默认值。
TemporaryProjectInfo EditorEngine::currentTemporaryProjectInfo() const
{
    return ProjectController::instance().currentTemporaryProjectInfo();
}

void EditorEngine::publishRenderFps(float fps)
{
    if ( !std::isfinite(fps) || fps <= 0.0f ) {
        return;
    }
    m_renderFps.store(fps, std::memory_order_relaxed);
}

double EditorEngine::adaptiveRenderSnapshotMinInterval(
    const Config::EditorConfig& config, bool secondaryCamera) const
{
    const double renderFps =
        static_cast<double>(m_renderFps.load(std::memory_order_relaxed));
    const double logicUps =
        static_cast<double>(m_logicUps.load(std::memory_order_relaxed));
    const double maxSnapshotHz = secondaryCamera
                                     ? RENDER_SNAPSHOT_SECONDARY_MAX_HZ
                                     : RENDER_SNAPSHOT_MAIN_MAX_HZ;
    const double fpsDrivenHz   = std::isfinite(renderFps) && renderFps > 1.0
                                     ? renderFps * (secondaryCamera ? 1.5 : 2.0)
                                     : RENDER_SNAPSHOT_FALLBACK_HZ;
    double       snapshotHz =
        std::clamp(fpsDrivenHz, RENDER_SNAPSHOT_MIN_HZ, maxSnapshotHz);

    double targetUps = frameLimitTargetUps(config.settings.frameLimit);
    if ( targetUps <= 1.0 && std::isfinite(renderFps) && renderFps > 1.0 ) {
        targetUps = std::clamp(renderFps * 2.0,
                               RENDER_SNAPSHOT_MIN_HZ,
                               RENDER_SNAPSHOT_MAIN_MAX_HZ);
    }

    snapshotHz = applyUpsBackpressure(snapshotHz, logicUps, targetUps);
    snapshotHz = std::clamp(snapshotHz, RENDER_SNAPSHOT_MIN_HZ, maxSnapshotHz);
    return 1.0 / snapshotHz;
}

bool EditorEngine::needsCanvasCloseBeforeProjectOpen() const
{
    return m_sessionRegistry.hasNonLogoSession();
}

void EditorEngine::captureProjectWorkspaceState()
{
    auto* project = ProjectController::instance().currentProject();
    if ( !project ) {
        return;
    }

    auto& workspace = project->m_settings.m_workspace;
    workspace.m_openBeatmaps.clear();
    workspace.m_activeBeatmapPath.clear();
    workspace.m_activePlaybackTime = 0.0;
    workspace.m_activeEditTool =
        editToolToWorkspaceName(m_currentTool.load(std::memory_order_relaxed));
    const auto editorConfig = getEditorConfig();
    captureToolbarWorkspaceState(
        workspace,
        editorConfig,
        m_syncSameMainAudioCanvases.load(std::memory_order_relaxed));

    /// @brief 保护工作区状态捕获期间的会话列表访问。
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    const auto&  sessions    = m_sessionRegistry.entriesUnsafe();
    const auto   activeIndex = m_sessionRegistry.activeIndex();
    const double workspaceCaptureTime =
        std::chrono::duration<double>(FrameLimitClock::now().time_since_epoch())
            .count();

    for ( int32_t i = 0; i < static_cast<int32_t>(sessions.size()); ++i ) {
        const auto& entry = sessions[i];
        if ( entry.isLogoPlaceholder || !entry.session ) {
            continue;
        }

        const auto& ctx = entry.session->getContext();
        if ( !ctx.currentBeatmap ) {
            continue;
        }

        auto absoluteMapPath = resolveProjectPath(
            *project, ctx.currentBeatmap->m_baseMapMetadata.map_path);
        auto relativeMapPath =
            makeProjectRelativePath(*project, absoluteMapPath);

        ProjectWorkspaceBeatmapState beatmapState;
        beatmapState.m_filePath     = Config::pathToUtf8(relativeMapPath);
        beatmapState.m_cameraId     = entry.cameraId;
        beatmapState.m_displayName  = entry.displayName;
        beatmapState.m_playbackTime = ctx.currentTime;
        const auto camera           = ctx.cameras.find(entry.cameraId);
        if ( camera != ctx.cameras.end() &&
             std::isfinite(camera->second.horizontalOffsetX) &&
             std::isfinite(camera->second.viewportWidth) &&
             camera->second.viewportWidth > 0.0F ) {
            beatmapState.m_canvasHorizontalOffsetRatio =
                camera->second.horizontalOffsetX / camera->second.viewportWidth;
        }
        if ( i == activeIndex && ctx.isPlaying ) {
            beatmapState.m_playbackTime =
                resolveContinuousSessionTime(ctx, workspaceCaptureTime);
        }
        workspace.m_openBeatmaps.push_back(beatmapState);

        if ( i == activeIndex ) {
            workspace.m_activeBeatmapPath  = beatmapState.m_filePath;
            workspace.m_activePlaybackTime = beatmapState.m_playbackTime;
            project->m_settings.m_lastOpenedBeatmap = entry.displayName;
        }
    }
}

void EditorEngine::restoreProjectWorkspace(
    const std::filesystem::path& explicitBeatmapPath)
{
    if ( !explicitBeatmapPath.empty() ) {
        return;
    }

    auto* project = ProjectController::instance().currentProject();
    if ( !project ) {
        return;
    }

    std::vector<ProjectWorkspaceBeatmapState> beatmaps =
        project->m_settings.m_workspace.m_openBeatmaps;
    if ( beatmaps.empty() &&
         !project->m_settings.m_lastOpenedBeatmap.empty() ) {
        for ( const auto& entry : project->m_beatmaps ) {
            if ( entry.m_name != project->m_settings.m_lastOpenedBeatmap ) {
                continue;
            }

            ProjectWorkspaceBeatmapState state;
            state.m_filePath    = entry.m_filePath;
            state.m_displayName = entry.m_name;
            beatmaps.push_back(state);
            break;
        }
    }

    if ( beatmaps.empty() ) {
        return;
    }

    m_currentTool.store(workspaceNameToEditTool(
                            project->m_settings.m_workspace.m_activeEditTool),
                        std::memory_order_relaxed);

    bool hasSavedCameraId =
        std::any_of(beatmaps.begin(), beatmaps.end(), [](const auto& state) {
            return !state.m_cameraId.empty();
        });
    const std::string firstWorkspaceCameraId = beatmaps.front().m_cameraId;
    if ( hasSavedCameraId && !firstWorkspaceCameraId.empty() ) {
        std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
        auto& sessions = m_sessionRegistry.entriesUnsafe();
        for ( int32_t i = static_cast<int32_t>(sessions.size()) - 1; i >= 0;
              --i ) {
            if ( !sessions[i].isLogoPlaceholder ) {
                continue;
            }

            const std::string logoCameraId = sessions[i].cameraId;
            bool shouldKeepLogo = logoCameraId == firstWorkspaceCameraId;
            if ( shouldKeepLogo ) {
                continue;
            }

            m_sessionRegistry.erase(i);
            m_renderSyncRegistry.eraseCamera(logoCameraId);
        }
    }

    const std::string& activePath =
        project->m_settings.m_workspace.m_activeBeatmapPath;
    int32_t fallbackActiveIndex = -1;
    int32_t restoredActiveIndex = -1;

    for ( const auto& state : beatmaps ) {
        if ( state.m_filePath.empty() ) {
            continue;
        }

        auto mapPath =
            resolveProjectPath(*project, Config::utf8ToPath(state.m_filePath));
        std::error_code existsError;
        if ( !std::filesystem::exists(mapPath, existsError) ) {
            XWARN("Workspace restore skipped missing beatmap: {}",
                  Config::pathToUtf8(mapPath));
            continue;
        }

        auto map = std::make_shared<BeatMap>(BeatMap::loadFromFile(mapPath));
        std::string displayName = state.m_displayName.empty()
                                      ? map->m_baseMapMetadata.name
                                      : state.m_displayName;
        int32_t     index       = createSession(map,
                                                displayName,
                                                false,
                                                state.m_cameraId,
                                                !state.m_cameraId.empty());
        fallbackActiveIndex     = index;

        std::shared_ptr<BeatmapSession> restoredSession;
        std::string                     restoredCameraId;
        {
            std::lock_guard<std::recursive_mutex> lock(
                m_sessionRegistry.mutex());
            auto& sessions = m_sessionRegistry.entriesUnsafe();
            if ( index >= 0 && index < static_cast<int32_t>(sessions.size()) ) {
                restoredSession  = sessions[index].session;
                restoredCameraId = sessions[index].cameraId;
            }
        }

        if ( restoredSession ) {
            if ( !restoredCameraId.empty() &&
                 std::isfinite(state.m_canvasHorizontalOffsetRatio) &&
                 std::abs(state.m_canvasHorizontalOffsetRatio) > 1e-6F ) {
                auto& context           = restoredSession->getContextMutable();
                auto [camera, inserted] = context.cameras.try_emplace(
                    restoredCameraId,
                    CameraInfo{ restoredCameraId, 1.0F, 1.0F });
                (void)inserted;
                const float viewportWidth =
                    std::isfinite(camera->second.viewportWidth) &&
                            camera->second.viewportWidth > 0.0F
                        ? camera->second.viewportWidth
                        : 1.0F;
                camera->second.horizontalOffsetX =
                    state.m_canvasHorizontalOffsetRatio * viewportWidth;
            }
            restoredSession->pushCommand(
                LogicCommand(CmdSeek{ state.m_playbackTime }));
        }

        if ( state.m_filePath == activePath ) {
            restoredActiveIndex = index;
        }
    }

    if ( restoredActiveIndex < 0 ) {
        restoredActiveIndex = fallbackActiveIndex;
    }
    m_pendingWorkspaceActiveIndex = restoredActiveIndex;
}

/// @brief 校验项目路径并切换到指定普通项目。
/// @param projectPath 要打开的项目目录或谱面文件路径。
/// @param creationOptions 新建项目初始设置；普通打开时为空。
void EditorEngine::openProject(
    const std::filesystem::path&                 projectPath,
    const std::optional<ProjectCreationOptions>& creationOptions)
{
    /// @brief 实际打开前用于保持旧行为的项目目录校验路径。
    std::filesystem::path actualProjectPath = projectPath;
    std::error_code       openPathError;
    if ( !creationOptions &&
         std::filesystem::exists(projectPath, openPathError) &&
         !openPathError &&
         std::filesystem::is_regular_file(projectPath, openPathError) &&
         !openPathError ) {
        actualProjectPath = projectPath.parent_path();
    }

    openPathError.clear();
    const bool projectDirectoryExists =
        std::filesystem::exists(actualProjectPath, openPathError) &&
        !openPathError;
    openPathError.clear();
    const bool isProjectDirectory =
        std::filesystem::is_directory(actualProjectPath, openPathError) &&
        !openPathError;
    if ( !creationOptions &&
         (!projectDirectoryExists || !isProjectDirectory) ) {
        const std::string message =
            "路径不存在或不是文件夹：" + Config::pathToUtf8(actualProjectPath);
        XERROR(
            "Failed to open project: Path does not exist or is not a "
            "directory: {}",
            Config::pathToUtf8(actualProjectPath));
        publishProjectOpenFailed(projectPath, message, false);
        return;
    }

    /// 同一项目目录的重复打开请求不应关闭并重新加载当前项目；谱面文件输入仍
    /// 保留原有自动打开行为，新建项目请求也必须继续应用 creationOptions。
    openPathError.clear();
    const bool requestedPathIsDirectory =
        !creationOptions &&
        std::filesystem::is_directory(projectPath, openPathError) &&
        !openPathError;
    const auto* currentProject = ProjectController::instance().currentProject();
    openPathError.clear();
    if ( requestedPathIsDirectory && currentProject &&
         std::filesystem::equivalent(
             actualProjectPath, currentProject->m_projectRoot, openPathError) &&
         !openPathError ) {
        XINFO("忽略当前项目目录的重复打开请求：{}",
              Config::pathToUtf8(actualProjectPath));
        return;
    }

    publishProjectOpenStarted(projectPath, false);
    if ( !closeProject() ) {
        publishProjectOpenFailed(
            projectPath, "当前项目的元数据保存失败，已取消项目切换", false);
        return;
    }

    /// @brief 项目控制器打开项目后的结果。
    auto openResult =
        ProjectController::instance().openProject(projectPath, creationOptions);
    if ( !openResult.m_opened ) {
        return;
    }
    finishOpenProject(openResult);
}

/// @brief 打开谱面包为临时只读项目。
/// @param packagePath 需要临时阅览的谱面包路径。
void EditorEngine::openTemporaryProjectPackage(
    const std::filesystem::path& packagePath)
{
    auto prepared =
        ProjectController::instance().prepareTemporaryProjectPackage(
            packagePath);
    if ( !prepared.m_success ) {
        XERROR("Temporary package open failed: {}", prepared.m_errorMessage);
        publishProjectOpenFailed(packagePath, prepared.m_errorMessage, true);
        return;
    }

    publishProjectOpenStarted(packagePath, true);
    if ( !closeProject() ) {
        std::error_code filesystemError;
        std::filesystem::remove_all(prepared.m_temporaryInfo.m_cacheProjectPath,
                                    filesystemError);
        publishProjectOpenFailed(
            packagePath, "当前项目的元数据保存失败，已取消项目切换", true);
        return;
    }

    auto openResult = ProjectController::instance().openProject(
        prepared.m_temporaryInfo.m_cacheProjectPath,
        std::nullopt,
        prepared.m_temporaryInfo);
    if ( !openResult.m_opened ) {
        std::error_code filesystemError;
        std::filesystem::remove_all(prepared.m_temporaryInfo.m_cacheProjectPath,
                                    filesystemError);
        return;
    }
    finishOpenProject(openResult);
}

/// @brief 应用项目控制器打开项目后的逻辑副作用。
/// @param openResult 项目控制器返回的打开结果。
void EditorEngine::finishOpenProject(const OpenProjectResult& openResult)
{
    m_pendingWorkspaceActiveIndex = -1;
    if ( auto* project = ProjectController::instance().currentProject() ) {
        const auto& workspace = project->m_settings.m_workspace;
        m_brushAudioResourceId.clear();
        m_brushAudioTrackType = AudioTrackType::Effect;
        m_brushAudioVolume =
            std::isfinite(workspace.m_projectAudioToolBrushVolume)
                ? std::max(0.0F, workspace.m_projectAudioToolBrushVolume)
                : 1.0F;
        if ( !workspace.m_projectAudioToolSelectedResourceId.empty() ) {
            const auto resourceIterator = std::find_if(
                project->m_audioResources.begin(),
                project->m_audioResources.end(),
                [&](const AudioResource& resource) {
                    return resource.m_id ==
                           workspace.m_projectAudioToolSelectedResourceId;
                });
            if ( resourceIterator != project->m_audioResources.end() ) {
                m_brushAudioResourceId = resourceIterator->m_id;
                m_brushAudioTrackType  = resourceIterator->m_type;
            }
        }
        m_currentTool.store(workspaceNameToEditTool(workspace.m_activeEditTool),
                            std::memory_order_relaxed);
        if ( workspace.m_toolbarState.m_valid ) {
            auto restoredConfig = getEditorConfig();
            applyToolbarWorkspaceState(restoredConfig,
                                       workspace.m_toolbarState);
            m_syncSameMainAudioCanvases.store(
                workspace.m_toolbarState.m_syncSameMainAudioCanvases,
                std::memory_order_relaxed);
            setEditorConfig(restoredConfig);
        } else {
            m_syncSameMainAudioCanvases.store(true, std::memory_order_relaxed);
        }
    }

    for ( const auto& registration : openResult.m_effectRegistrations ) {
        Audio::AudioManager::instance().registerSoundEffect(
            registration.m_resource.m_id,
            Config::pathToUtf8(registration.m_absolutePath),
            registration.m_resource.m_config);
    }

    XINFO("Project '{}' loaded successfully with {} beatmaps.",
          openResult.m_projectTitle,
          openResult.m_beatmapCount);

    // 如果指定了谱面路径，则通过 createSession 加载它
    if ( !openResult.m_targetBeatmapPath.empty() ) {
        XINFO("Auto loading beatmap: {}",
              Config::pathToUtf8(openResult.m_targetBeatmapPath));
        auto loadedMap = BeatMap::loadFromFile(openResult.m_targetBeatmapPath);
        if ( loadedMap.m_baseMapMetadata.map_path.empty() ) {
            XERROR("Failed to auto load beatmap {}",
                   Config::pathToUtf8(openResult.m_targetBeatmapPath));
        } else {
            auto map = std::make_shared<BeatMap>(std::move(loadedMap));
            createSession(map, map->m_baseMapMetadata.name);
        }
    } else {
        restoreProjectWorkspace(openResult.m_targetBeatmapPath);
    }

    /// 音频预加载和谱面会话或工作区恢复完成后，UI 才能安全读取
    /// 已就绪的项目状态。
    Event::ProjectLoadedEvent loadedEvent;
    loadedEvent.m_projectTitle = openResult.m_projectTitle;
    loadedEvent.m_projectPath =
        Config::pathToUtf8(openResult.m_actualProjectPath);
    loadedEvent.m_beatmapCount = openResult.m_beatmapCount;
    Event::EventBus::instance().publish(loadedEvent);
}

bool EditorEngine::closeProject()
{
    if ( !ProjectController::instance().currentProject() ) return true;
    if ( !flushPendingMetadataAutoSaves() ) {
        XERROR(
            "EditorEngine: pending metadata save failed; project close "
            "cancelled");
        return false;
    }
    captureProjectWorkspaceState();
    if ( !ProjectController::instance().saveProject() ) {
        XERROR(
            "EditorEngine: project configuration save failed; project "
            "close cancelled");
        return false;
    }

    /// @brief 项目控制器关闭当前项目后的结果。
    auto closeResult = ProjectController::instance().closeProject();
    if ( !closeResult.m_closed || !closeResult.m_project ) {
        return false;
    }

    auto& audio = Audio::AudioManager::instance();
    audio.stop();
    audio.clearAllScheduledSoundEffects();
    audio.unloadAudioTimeline();

    for ( const auto& res : closeResult.m_project->m_audioResources ) {
        if ( res.m_type == AudioTrackType::Effect ) {
            audio.unloadSoundEffect(res.m_id);
        }
    }
    closeResult.m_project.reset();
    static_cast<void>(audio.releaseUnusedTrackCache());
    releaseUnusedHeapPages();

    XINFO("Project '{}' closed.", closeResult.m_projectTitle);
    return true;
}

void EditorEngine::start()
{
    if ( m_running.load(std::memory_order_acquire) ) {
        return;
    }

    // 从全局配置同步到本地缓存
    const auto initialConfig = Config::AppConfig::instance().getEditorConfig();
    {
        std::lock_guard<std::mutex> lock(m_editorConfigMutex);
        m_editorConfig = initialConfig;
        m_editorConfigRevision.fetch_add(1, std::memory_order_release);
    }
    m_frameLimitPreference.store(initialConfig.settings.frameLimit,
                                 std::memory_order_relaxed);

    m_running.store(true, std::memory_order_release);

    auto* appThreadPool = MMM::Runtime::AppThreadPool::instance().get();
    if ( !appThreadPool ) {
        m_running.store(false, std::memory_order_release);
        XERROR("AppThreadPool is not initialized before EditorEngine::start.");
        return;
    }

    m_loopFuture = appThreadPool->enqueue([this]() { loop(); });
    XINFO("EditorEngine logic thread started.");
}

void EditorEngine::stop()
{
    ProjectController::instance().stopDirectoryWatcher();

    if ( m_running.exchange(false, std::memory_order_acq_rel) ) {
        if ( m_loopFuture.valid() ) {
            m_loopFuture.wait();
            m_loopFuture = std::future<void>{};
        }
        XINFO("EditorEngine logic thread stopped.");
    }
}

/// @brief 处理新建谱面指令并执行引擎侧保存和开图副作用。
void EditorEngine::handleCreateBeatmap(const CmdCreateBeatmap& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    /// @brief 项目命令服务的新建谱面处理结果。
    auto result = ProjectController::instance().createBeatmap(cmd);
    if ( !result.m_created || !result.m_beatmap ) {
        return;
    }

    saveProject();

    createSession(result.m_beatmap, result.m_displayName);
}

/// @brief 处理导入音频指令并执行音效登记和项目保存副作用。
void EditorEngine::handleImportAudio(const CmdImportAudio& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    /// @brief 项目命令服务的导入音频处理结果。
    auto result = ProjectController::instance().importAudio(cmd);
    if ( !result.m_imported ) {
        return;
    }

    if ( result.m_effectRegistration ) {
        Audio::AudioManager::instance().registerSoundEffect(
            result.m_effectRegistration->m_resource.m_id,
            Config::pathToUtf8(result.m_effectRegistration->m_absolutePath),
            result.m_effectRegistration->m_resource.m_config);
    }

    saveProject();
}

/// @brief 重新登记当前项目中按需加载的 Effect 音频资源。
/// @warning 低频资源重载路径：皮肤热切换清空音效池后调用；只访问项目
/// 资源表并更新内存描述，不执行音频解码。
void EditorEngine::registerCurrentProjectEffectSoundEffects()
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    const auto* currentProject = ProjectController::instance().currentProject();
    if ( !currentProject ) {
        return;
    }

    for ( const auto& res : currentProject->m_audioResources ) {
        if ( res.m_type != AudioTrackType::Effect ) {
            continue;
        }

        const auto absolutePath =
            currentProject->m_projectRoot / Config::utf8ToPath(res.m_path);
        Audio::AudioManager::instance().registerSoundEffect(
            res.m_id, Config::pathToUtf8(absolutePath), res.m_config);
    }
}

/// @brief 更新编辑器级剪贴板。
void EditorEngine::setClipboard(std::vector<ClipboardItem> items,
                                const SessionContext* sourceContext, bool isCut)
{
    m_clipboard.set(std::move(items), sourceContext, isCut);
}

/// @brief 更新编辑器级混合谱面物件剪贴板。
void EditorEngine::setChartObjectClipboard(
    std::vector<ClipboardItem> notes, std::vector<SampleClipboardItem> samples,
    const SessionContext* sourceContext, bool isCut)
{
    m_clipboard.setChartObjects(
        std::move(notes), std::move(samples), sourceContext, isCut);
}

/// @brief 更新编辑器级 Timeline 剪贴板。
void EditorEngine::setTimelineClipboard(
    std::vector<TimelineClipboardItem> items,
    const SessionContext* sourceContext, bool isCut)
{
    m_clipboard.setTimelines(std::move(items), sourceContext, isCut);
}

/// @brief 获取编辑器级剪贴板副本。
std::vector<ClipboardItem> EditorEngine::getClipboard() const
{
    return m_clipboard.get();
}

/// @brief 获取编辑器级自动采样剪贴板副本。
std::vector<SampleClipboardItem> EditorEngine::getSampleClipboard() const
{
    return m_clipboard.getSamples();
}

/// @brief 获取编辑器级 Timeline 剪贴板副本。
std::vector<TimelineClipboardItem> EditorEngine::getTimelineClipboard() const
{
    return m_clipboard.getTimelines();
}

/// @brief 判断当前剪贴板是否为指定会话的剪切内容。
bool EditorEngine::isClipboardCutFrom(const SessionContext* context) const
{
    return m_clipboard.isCutFrom(context);
}

/// @brief 若剪贴板为其他会话剪切内容，则删除源会话原物件。
void EditorEngine::consumeCrossSessionCutClipboard(
    const SessionContext* pasteContext)
{
    /// @brief 跨 Session 剪切的来源上下文，仅用于在 Session 列表中定位源会话。
    const SessionContext* sourceContext =
        m_clipboard.getCrossSessionCutSource(pasteContext);
    if ( !sourceContext ) return;

    /// @brief 保护跨 Session 剪切消费期间的会话列表访问。
    std::lock_guard<std::recursive_mutex> sessionLock(
        m_sessionRegistry.mutex());
    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    const auto& sessions = m_sessionRegistry.entriesUnsafe();
    for ( const auto& entry : sessions ) {
        if ( !entry.session ) {
            continue;
        }

        auto& sourceCtx = entry.session->getContextMutable();
        if ( &sourceCtx != sourceContext ) {
            continue;
        }

        std::vector<BatchNoteAction::Entry> noteEntries;
        std::unordered_set<entt::entity>    collectedNoteEntities;
        auto noteView = sourceCtx.noteRegistry.view<InteractionComponent>();
        for ( auto entity : noteView ) {
            auto& ic = sourceCtx.noteRegistry.get<InteractionComponent>(entity);
            if ( !ic.isCut ||
                 !sourceCtx.noteRegistry.all_of<NoteComponent>(entity) ) {
                continue;
            }
            if ( !collectedNoteEntities.insert(entity).second ) continue;

            auto oldNote = sourceCtx.noteRegistry.get<NoteComponent>(entity);
            if ( pasteContext &&
                 !SessionUtils::isNoteEditable(
                     oldNote, pasteContext->lastConfig.settings) ) {
                continue;
            }
            noteEntries.push_back({
                .entity         = entity,
                .before         = oldNote,
                .after          = std::nullopt,
                .beforeSelected = ic.isSelected,
            });

            if ( oldNote.m_type == ::MMM::NoteType::POLYLINE &&
                 !oldNote.m_subNotes.empty() ) {
                for ( auto subEnt :
                      sourceCtx.noteRegistry.view<NoteComponent>() ) {
                    const auto& subNC =
                        sourceCtx.noteRegistry.get<NoteComponent>(subEnt);
                    if ( subNC.m_isSubNote &&
                         subNC.m_parentPolyline == entity &&
                         collectedNoteEntities.insert(subEnt).second ) {
                        const auto* subInteraction =
                            sourceCtx.noteRegistry
                                .try_get<InteractionComponent>(subEnt);
                        noteEntries.push_back({
                            .entity = subEnt,
                            .before = subNC,
                            .after  = std::nullopt,
                            .beforeSelected =
                                subInteraction ? std::optional<bool>(
                                                     subInteraction->isSelected)
                                               : std::nullopt,
                        });
                    }
                }
            }
        }

        std::vector<BatchSampleAction::Entry> sampleEntries;
        auto sampleView = sourceCtx.sampleRegistry
                              .view<InteractionComponent, SampleComponent>();
        for ( auto entity : sampleView ) {
            const auto& interaction =
                sampleView.get<InteractionComponent>(entity);
            if ( !interaction.isCut ) continue;
            sampleEntries.push_back({
                .entity         = entity,
                .before         = sampleView.get<SampleComponent>(entity),
                .after          = std::nullopt,
                .beforeSelected = interaction.isSelected,
            });
        }

        std::vector<std::unique_ptr<IEditorAction>> actions;
        actions.reserve(2);
        if ( !noteEntries.empty() ) {
            actions.push_back(std::make_unique<BatchNoteAction>(
                std::move(noteEntries), "Cut Across Canvas"));
        }
        if ( !sampleEntries.empty() ) {
            actions.push_back(std::make_unique<BatchSampleAction>(
                std::move(sampleEntries), "跨画布剪切自动采样"));
        }
        for ( auto entity : noteView ) {
            sourceCtx.noteRegistry.get<InteractionComponent>(entity).isCut =
                false;
        }
        for ( auto entity : sampleView ) {
            sourceCtx.sampleRegistry.get<InteractionComponent>(entity).isCut =
                false;
        }
        if ( !actions.empty() ) {
            std::unique_ptr<IEditorAction> action;
            if ( actions.size() == 1 ) {
                action = std::move(actions.front());
            } else {
                action = std::make_unique<CompositeEditorAction>(
                    std::move(actions), "跨画布剪切谱面物件");
            }
            sourceCtx.actionStack.pushAndExecute(std::move(action), sourceCtx);
        }
        markCutClipboardConsumed();
        return;
    }

    markCutClipboardConsumed();
}

/// @brief 将当前剪切剪贴板标记为已消费。
void EditorEngine::markCutClipboardConsumed()
{
    m_clipboard.markCutConsumed();
}

/// @brief 消费需要由 UI 线程发布到系统剪贴板的文本载荷。
std::optional<std::string> EditorEngine::consumePendingSystemClipboardText()
{
    return m_clipboard.consumePendingSystemText();
}

/// @brief 从系统剪贴板文本导入 MMM 剪贴板载荷。
bool EditorEngine::importSystemClipboardText(std::string_view text)
{
    return m_clipboard.importSystemText(text);
}

/// @brief 同步单个谱面文件到项目配置并在发生变化时保存。
void EditorEngine::syncProjectWithFile(const std::filesystem::path& mapPath)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    /// @brief 项目命令服务的单文件同步结果。
    auto result = ProjectController::instance().syncProjectWithFile(mapPath);
    if ( result.m_changed ) {
        saveProject();
    }

    /// @brief 当前项目指针，仅用于刷新已打开 Session 的谱面路径键。
    const auto* currentProject = ProjectController::instance().currentProject();
    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    for ( auto& entry : sessions ) {
        if ( !entry.session ) {
            continue;
        }

        const auto& ctx = entry.session->getContext();
        if ( !ctx.currentBeatmap ) {
            continue;
        }

        entry.beatmapPathKey = makeBeatmapPathKey(
            currentProject, ctx.currentBeatmap->m_baseMapMetadata.map_path);
    }
    refreshAudioTimelineFingerprintsUnsafe();
}

void EditorEngine::pushCommand(LogicCommand&& cmd)
{
    if ( std::holds_alternative<CmdSaveTemporaryProject>(cmd) ) {
        handleSaveTemporaryProject(std::get<CmdSaveTemporaryProject>(cmd));
        return;
    }

    if ( ProjectController::instance().isCurrentProjectTemporary() &&
         isTemporaryProjectMutationCommand(cmd) ) {
        const auto info =
            ProjectController::instance().currentTemporaryProjectInfo();
        Event::TemporaryProjectEditBlockedEvent event;
        event.m_sourcePackagePath =
            Config::pathToUtf8(info.m_sourcePackagePath);
        event.m_cacheProjectPath = Config::pathToUtf8(info.m_cacheProjectPath);
        Event::EventBus::instance().publish(event);
        return;
    }

    // 拦截创建谱面等引擎级别的指令
    if ( std::holds_alternative<CmdCreateBeatmap>(cmd) ) {
        handleCreateBeatmap(std::get<CmdCreateBeatmap>(cmd));
        return;
    }

    // 拦截项目资源管理指令
    if ( std::holds_alternative<CmdUpdateAudioResource>(cmd) ) {
        handleUpdateAudioResource(std::get<CmdUpdateAudioResource>(cmd));
        return;
    }
    if ( std::holds_alternative<CmdRenameAudioResource>(cmd) ) {
        handleRenameAudioResource(std::get<CmdRenameAudioResource>(cmd));
        return;
    }
    if ( std::holds_alternative<CmdUpdateAudioResourceConfig>(cmd) ) {
        handleUpdateAudioResourceConfig(
            std::get<CmdUpdateAudioResourceConfig>(cmd));
        return;
    }
    if ( std::holds_alternative<CmdRemoveAudioResource>(cmd) ) {
        handleRemoveAudioResource(std::get<CmdRemoveAudioResource>(cmd));
        return;
    }
    if ( std::holds_alternative<CmdRemoveBeatmap>(cmd) ) {
        handleRemoveBeatmap(std::get<CmdRemoveBeatmap>(cmd));
        return;
    }

    // 拦截导入音频指令
    if ( std::holds_alternative<CmdImportAudio>(cmd) ) {
        handleImportAudio(std::get<CmdImportAudio>(cmd));
        return;
    }

    // 编辑工具是全局状态，所有画布保持一致
    if ( std::holds_alternative<CmdChangeTool>(cmd) ) {
        auto tool = std::get<CmdChangeTool>(cmd).tool;
        m_currentTool.store(tool, std::memory_order_relaxed);
        if ( auto* project = ProjectController::instance().currentProject() ) {
            project->m_settings.m_workspace.m_activeEditTool =
                editToolToWorkspaceName(tool);
        }

        std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
        /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
        auto& sessions = m_sessionRegistry.entriesUnsafe();
        for ( auto& entry : sessions ) {
            if ( entry.session ) {
                entry.session->pushCommand(LogicCommand(CmdChangeTool{ tool }));
            }
        }
        return;
    }

    // 项目音频选择是全局画笔状态，所有画布必须使用同一资源。
    if ( const auto* audioResource =
             std::get_if<CmdSetBrushAudioResource>(&cmd) ) {
        m_brushAudioResourceId = audioResource->audioResourceId;
        m_brushAudioTrackType  = audioResource->audioTrackType;
        m_brushAudioVolume     = std::isfinite(audioResource->volume)
                                     ? std::max(0.0F, audioResource->volume)
                                     : 1.0F;
        if ( auto* project = ProjectController::instance().currentProject() ) {
            auto& workspace = project->m_settings.m_workspace;
            workspace.m_projectAudioToolSelectedResourceId =
                m_brushAudioResourceId;
            workspace.m_projectAudioToolBrushVolume = m_brushAudioVolume;
        }

        std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
        auto& sessions = m_sessionRegistry.entriesUnsafe();
        for ( auto& entry : sessions ) {
            if ( entry.session ) {
                entry.session->pushCommand(
                    LogicCommand(CmdSetBrushAudioResource{
                        m_brushAudioResourceId,
                        m_brushAudioTrackType,
                        m_brushAudioVolume,
                    }));
            }
        }
        return;
    }

    // 拦截视口更新指令，缓存最新的尺寸
    if ( std::holds_alternative<CmdUpdateViewport>(cmd) ) {
        const auto& v = std::get<CmdUpdateViewport>(cmd);
        m_renderSyncRegistry.cacheViewportSize(v.cameraId,
                                               { v.width, v.height });
    }

    // 主画布滚轮按 cameraId 路由，以允许同主音轨后台画布在 hover
    // 状态下接收滚动，但不抢占当前活跃画布焦点。
    if ( std::holds_alternative<CmdScroll>(cmd) ) {
        const auto& scroll = std::get<CmdScroll>(cmd);
        if ( SessionUtils::isMainCanvasCameraId(scroll.cameraId) ) {
            std::lock_guard<std::recursive_mutex> lock(
                m_sessionRegistry.mutex());
            /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
            auto& sessions = m_sessionRegistry.entriesUnsafe();
            /// @brief 当前活动 Session 索引快照。
            const int32_t activeIndex = m_sessionRegistry.activeIndex();
            /// @brief 滚轮目标主画布对应的 Session 索引。
            const int32_t targetIndex =
                findSessionIndexByCameraIdUnsafe(sessions, scroll.cameraId);
            if ( !canUseHoverScrollTargetUnsafe(
                     sessions, activeIndex, targetIndex) ) {
                return;
            }

            sessions[static_cast<size_t>(targetIndex)].session->pushCommand(
                std::move(cmd));
            return;
        }
    }

    // 主画布二维平移按 cameraId 精确路由，避免同帧焦点切换把增量发给旧画布。
    if ( std::holds_alternative<CmdPanCanvas>(cmd) ) {
        const auto& pan = std::get<CmdPanCanvas>(cmd);
        if ( SessionUtils::isMainCanvasCameraId(pan.cameraId) ) {
            std::lock_guard<std::recursive_mutex> lock(
                m_sessionRegistry.mutex());
            /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
            auto&         sessions = m_sessionRegistry.entriesUnsafe();
            const int32_t targetIndex =
                findSessionIndexByCameraIdUnsafe(sessions, pan.cameraId);
            if ( targetIndex < 0 ||
                 targetIndex >= static_cast<int32_t>(sessions.size()) ||
                 !sessions[static_cast<size_t>(targetIndex)].session ) {
                return;
            }
            sessions[static_cast<size_t>(targetIndex)].session->pushCommand(
                std::move(cmd));
            return;
        }
    }

    // 分发到当前活跃 Session
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    if ( const auto* palette = std::get_if<CmdSetBrushNotePalette>(&cmd) ) {
        for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
            m_brushNoteColors[i] = palette->colors[i];
        }
        m_brushNoteColorsInitialized = true;
    } else if ( const auto* color = std::get_if<CmdSetBrushNoteColor>(&cmd) ) {
        const auto colorIndex = static_cast<std::size_t>(color->slot);
        if ( colorIndex < m_brushNoteColors.size() ) {
            m_brushNoteColors[colorIndex] = color->color;
            m_brushNoteColorsInitialized  = true;
        }
    }

    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    /// @brief 当前活跃 Session 索引快照。
    int32_t idx = m_sessionRegistry.activeIndex();
    if ( idx >= 0 && idx < static_cast<int32_t>(sessions.size()) ) {
        sessions[idx].session->pushCommand(std::move(cmd));
    }
}

void EditorEngine::restoreBrushNoteColorsUnsafe(BeatmapSession& session) const
{
    if ( !m_brushNoteColorsInitialized ) return;

    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        session.pushCommand(CmdSetBrushNoteColor{ static_cast<NoteColorSlot>(i),
                                                  m_brushNoteColors[i] });
    }
}

void EditorEngine::restoreBrushAudioResourceUnsafe(
    BeatmapSession& session) const
{
    session.pushCommand(LogicCommand(CmdSetBrushAudioResource{
        m_brushAudioResourceId,
        m_brushAudioTrackType,
        m_brushAudioVolume,
    }));
}

bool EditorEngine::hasUnsavedChanges() const
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    // 检查所有 Session 是否有未保存的修改
    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    const auto& sessions = m_sessionRegistry.entriesUnsafe();
    for ( const auto& entry : sessions ) {
        if ( entry.session &&
             entry.session->getContext().actionStack.isDirty() ) {
            return true;
        }
    }
    return false;
}

/// @brief 获取指定摄像机/画布的同步缓冲区。
/// @warning 逻辑热路径/共享指针：返回 shared_ptr
/// 用于保证本次快照发布期间缓冲区不被 UI 关闭路径释放。
std::shared_ptr<BeatmapSyncBuffer> EditorEngine::getSyncBuffer(
    const std::string& cameraId)
{
    return m_renderSyncRegistry.getSyncBuffer(cameraId);
}

std::shared_ptr<const std::unordered_map<uint32_t, glm::vec4>>
EditorEngine::getAtlasUVMap(const std::string& cameraId) const
{
    return m_renderSyncRegistry.getAtlasUVMap(cameraId);
}

/// @brief 按修订号将指定画布的图集 UV 映射同步到快照缓存。
/// @warning 逻辑/渲染热路径：每个快照生成时调用；普通路径不复制 UV 表。
void EditorEngine::updateSnapshotAtlasUVMap(
    const std::string&                       cameraId,
    std::unordered_map<uint32_t, glm::vec4>& target,
    std::uint64_t&                           targetRevision,
    Common::AsciiFontAtlasMetrics&           targetAsciiFontAtlasMetrics,
    Common::UnicodeFontMetrics&              targetUnicodeFontMetrics) const
{
    m_renderSyncRegistry.updateSnapshotAtlasUVMap(cameraId,
                                                  target,
                                                  targetRevision,
                                                  targetAsciiFontAtlasMetrics,
                                                  targetUnicodeFontMetrics);
}

/// @brief 为外部谱面路径生成与 Session 条目一致的稳定路径键。
/// @param beatmapPath 待规范化的谱面路径。
/// @return 规范化绝对路径键；空路径返回空字符串。
/// @warning
/// 低频路径：可能访问文件系统解析规范路径，只能在文件选择、打开或打包流程调用。
std::string EditorEngine::makeBeatmapPathKeyForPath(
    const std::filesystem::path& beatmapPath) const
{
    return makeBeatmapPathKey(getCurrentProject(), beatmapPath);
}

/// @brief 判断指定主画布是否允许通过悬停滚轮接管滚动。
/// @warning UI 热路径辅助：只允许在滚轮输入分支调用；会短暂持有
/// SessionRegistry 锁。
bool EditorEngine::canHoverScrollCamera(const std::string& cameraId) const
{
    if ( !SessionUtils::isMainCanvasCameraId(cameraId) ) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    const auto& sessions = m_sessionRegistry.entriesUnsafe();
    /// @brief 当前活动 Session 索引快照。
    const int32_t activeIndex = m_sessionRegistry.activeIndex();
    /// @brief 悬停目标主画布对应的 Session 索引。
    const int32_t targetIndex =
        findSessionIndexByCameraIdUnsafe(sessions, cameraId);
    return canUseHoverScrollTargetUnsafe(sessions, activeIndex, targetIndex);
}

/// @brief 更新指定主画布窗口在 UI 中的可见状态。
/// @warning UI 热路径：Basic2DCanvas 每帧写入；只修改注册表中的布尔状态。
void EditorEngine::setSessionCanvasVisible(const std::string& cameraId,
                                           bool               isVisible)
{
    if ( !SessionUtils::isMainCanvasCameraId(cameraId) ) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    for ( auto& entry : sessions ) {
        if ( entry.cameraId == cameraId ) {
            if ( entry.isCanvasVisible == isVisible ) {
                return;
            }
            entry.isCanvasVisible = isVisible;
            m_sessionRegistry.publishSnapshotUnsafe();
            return;
        }
    }
}

/// @brief 获取当前工具类型。
/// @warning 逻辑/UI 热路径原子：只读取工具枚举状态，使用 relaxed。
EditTool EditorEngine::getCurrentTool() const
{
    return m_currentTool.load(std::memory_order_relaxed);
}

bool EditorEngine::isPlaybackPlaying() const
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    const auto& sessions = m_sessionRegistry.entriesUnsafe();
    /// @brief 当前活跃 Session 索引快照。
    int32_t idx = m_sessionRegistry.activeIndex();
    if ( idx >= 0 && idx < static_cast<int32_t>(sessions.size()) ) {
        return sessions[idx].session->getContext().isPlaying;
    }
    return false;
}

/// @brief 判断当前活跃 Session 是否正在拖拽框选区域。
/// @warning UI 热路径：会短暂锁定 SessionRegistry 并读取活跃 Session
/// 的常量状态， 且不复制 shared_ptr 所有权。
bool EditorEngine::isActiveSessionSelectingMarquee() const
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    const auto& sessions = m_sessionRegistry.entriesUnsafe();
    const auto  idx      = m_sessionRegistry.activeIndex();
    if ( idx >= 0 && idx < static_cast<int32_t>(sessions.size()) &&
         sessions[idx].session ) {
        const auto& ctx = sessions[idx].session->getContext();
        return ctx.currentTool == EditTool::Marquee && ctx.isSelecting;
    }
    return false;
}

/// @brief 判断当前活跃 Session 是否正在拖拽物件。
/// @warning UI 热路径：会短暂锁定 SessionRegistry 并读取活跃 Session
/// 的常量状态，且不复制 shared_ptr 所有权。
bool EditorEngine::isActiveSessionDraggingNote() const
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    const auto& sessions = m_sessionRegistry.entriesUnsafe();
    const auto  idx      = m_sessionRegistry.activeIndex();
    if ( idx >= 0 && idx < static_cast<int32_t>(sessions.size()) &&
         sessions[idx].session ) {
        const auto& ctx = sessions[idx].session->getContext();
        return ctx.currentTool == EditTool::Move && ctx.isDragging &&
               ctx.draggedEntity != entt::null;
    }
    return false;
}

/// @brief 判断当前活跃 Session 是否正在使用画笔绘制。
/// @warning UI 热路径：会短暂锁定 SessionRegistry 并读取活跃 Session
/// 的常量状态，且不复制 shared_ptr 所有权。
bool EditorEngine::isActiveSessionDrawingBrush() const
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    const auto& sessions = m_sessionRegistry.entriesUnsafe();
    const auto  idx      = m_sessionRegistry.activeIndex();
    if ( idx >= 0 && idx < static_cast<int32_t>(sessions.size()) &&
         sessions[idx].session ) {
        const auto& ctx = sessions[idx].session->getContext();
        return ctx.currentTool == EditTool::Draw && ctx.brushState.isActive;
    }
    return false;
}

/// @brief 设置同主音轨多画布时间同步开关。
/// @warning 逻辑/UI 热路径原子：只写入同步开关状态，使用 relaxed。
void EditorEngine::setSyncSameMainAudioCanvases(bool enabled)
{
    m_syncSameMainAudioCanvases.store(enabled, std::memory_order_relaxed);
    if ( !enabled ) {
        std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
        auto& sessions = m_sessionRegistry.entriesUnsafe();
        for ( auto& entry : sessions ) {
            if ( entry.session ) {
                entry.session->getContextMutable().isAudioTimelineSyncFollower =
                    false;
            }
        }
    }
    if ( auto* project = ProjectController::instance().currentProject() ) {
        const auto editorConfig = getEditorConfig();
        captureToolbarWorkspaceState(
            project->m_settings.m_workspace, editorConfig, enabled);
    }
    if ( enabled ) {
        syncSameMainAudioCanvases();
    }
}

/// @brief 刷新已打开 Session 的主音轨同步路径键。
void EditorEngine::refreshAudioTimelineFingerprints()
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    refreshAudioTimelineFingerprintsUnsafe();
}

/// @brief 发布已打开 Session 的复合时间线指纹，调用者必须持有注册表锁。
void EditorEngine::refreshAudioTimelineFingerprintsUnsafe()
{
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    for ( auto& entry : sessions ) {
        if ( entry.isLogoPlaceholder || !entry.session ) {
            entry.audioTimelineFingerprint.clear();
            continue;
        }

        const auto& ctx = entry.session->getContext();
        entry.audioTimelineFingerprint =
            ctx.audioTimelineDescriptor.m_fingerprint;
    }
    refreshMainAudioSyncPeerStateUnsafe();
    m_sessionRegistry.publishSnapshotUnsafe();
    m_lastMainAudioSyncActiveIndex = -1;
}

/// @brief 标记全部或引用指定资源的已打开谱面描述符需要低频重建。
void EditorEngine::markAudioTimelineDescriptorsDirtyUnsafe(
    std::string_view resourceId)
{
    for ( auto& entry : m_sessionRegistry.entriesUnsafe() ) {
        if ( entry.isLogoPlaceholder || !entry.session ) continue;
        auto& ctx = entry.session->getContextMutable();
        if ( !resourceId.empty() && !ctx.isAudioTimelineDescriptorDirty &&
             !audioTimelineDescriptorReferencesResource(
                 ctx.audioTimelineDescriptor, resourceId) ) {
            continue;
        }
        ctx.isAudioTimelineDescriptorDirty   = true;
        ctx.isAudioTimelineActivationPending = true;
    }
}

/// @brief 刷新是否存在同主音轨同步候选，调用者必须持有注册表锁。
void EditorEngine::refreshMainAudioSyncPeerStateUnsafe()
{
    const auto& sessions = m_sessionRegistry.entriesUnsafe();
    for ( size_t i = 0; i < sessions.size(); ++i ) {
        const auto& key = sessions[i].audioTimelineFingerprint;
        if ( key.empty() || sessions[i].isLogoPlaceholder ||
             !sessions[i].session ) {
            continue;
        }

        for ( size_t j = i + 1; j < sessions.size(); ++j ) {
            if ( sessions[j].isLogoPlaceholder || !sessions[j].session ) {
                continue;
            }
            if ( sessions[j].audioTimelineFingerprint == key ) {
                m_hasMainAudioSyncPeers.store(true, std::memory_order_relaxed);
                return;
            }
        }
    }

    m_hasMainAudioSyncPeers.store(false, std::memory_order_relaxed);
}

/// @brief 将使用同一主音轨的非活跃会话同步到当前活跃会话时间。
/// @warning 逻辑热路径/原子：每次 Session update 后可能执行；开关读取使用
/// relaxed，后续只遍历已打开 Session 列表。
void EditorEngine::syncSameMainAudioCanvases()
{
    syncSameMainAudioCanvasesFromIndex(m_sessionRegistry.activeIndex());
}

/// @brief 从指定源 Session 同步同主音轨的其他画布时间。
/// @warning 逻辑热路径/原子：每次 Session update 后可能执行；开关读取使用
/// relaxed，后续只遍历已打开 Session 列表。
void EditorEngine::syncSameMainAudioCanvasesFromIndex(int32_t sourceIndex)
{
    const auto editorConfig = getEditorConfig();
    if ( !m_syncSameMainAudioCanvases.load(std::memory_order_relaxed) ) {
        return;
    }
    if ( !m_hasMainAudioSyncPeers.load(std::memory_order_relaxed) ) {
        return;
    }

    {
        const auto  publishedSnapshot = m_sessionRegistry.publishedSnapshot();
        const auto& sessions          = publishedSnapshot->sessions;
        const auto  sourceEntry =
            std::find_if(sessions.begin(),
                         sessions.end(),
                         [sourceIndex](const SessionSnapshotEntry& entry) {
                             return entry.index == sourceIndex;
                         });
        if ( sourceEntry == sessions.end() || !sourceEntry->session ) {
            return;
        }

        auto&       sourceCtx = sourceEntry->session->getContext();
        const auto& sourceKey = sourceEntry->audioTimelineFingerprint;
        if ( sourceKey.empty() ) {
            return;
        }
        if ( sourceCtx.isAudioTimelineSyncFollower && !sourceCtx.isPlaying ) {
            return;
        }

        const double syncSteadyTime =
            std::chrono::duration<double>(
                FrameLimitClock::now().time_since_epoch())
                .count();
        double sourceClockSteadyTime = syncSteadyTime;
        if ( sourceCtx.playbackVisualClock.initialized() ) {
            const double resolvedSteadyTime =
                sourceCtx.playbackVisualClock.lastResolvedSteadyTime();
            if ( std::isfinite(resolvedSteadyTime) &&
                 resolvedSteadyTime > 0.0 &&
                 resolvedSteadyTime <= syncSteadyTime ) {
                sourceClockSteadyTime = resolvedSteadyTime;
            }
        }
        const double playbackRate =
            Audio::AudioManager::instance().getPlaybackSpeed();

        for ( const auto& entry : sessions ) {
            if ( entry.index == sourceIndex || !entry.session ||
                 entry.audioTimelineFingerprint != sourceKey ) {
                continue;
            }

            auto& ctx = entry.session->getContextMutable();

            const double sourceAnimateTarget =
                sourceCtx.currentTime +
                editorConfig.visual.getEffectiveVisualOffset();
            const double sourceResetTime     = sourceCtx.isPlaying
                                                   ? sourceCtx.animateTime
                                                   : sourceAnimateTarget;
            const bool   wasFollowing        = ctx.isAudioTimelineSyncFollower;
            const double previousAnimateTime = ctx.animateTime;
            const bool   shouldClearHitEffects =
                wasFollowing != sourceCtx.isPlaying ||
                sourceResetTime + MAIN_AUDIO_SYNC_BACKWARD_RESET_EPSILON <
                    previousAnimateTime ||
                std::abs(sourceResetTime - previousAnimateTime) > 0.2;

            ctx.currentTime = sourceCtx.currentTime;
            if ( sourceCtx.isPlaying ) {
                ctx.animateTime       = sourceCtx.animateTime;
                ctx.animateTimeTarget = sourceCtx.animateTimeTarget;
                ctx.animateTimeAnimationActive =
                    sourceCtx.animateTimeAnimationActive;
            } else if ( std::isfinite(ctx.animateTime) &&
                        std::isfinite(sourceAnimateTarget) ) {
                ctx.animateTimeTarget = sourceAnimateTarget;
                ctx.animateTimeAnimationActive =
                    std::abs(ctx.animateTimeTarget - ctx.animateTime) >
                    MAIN_AUDIO_SYNC_TIME_EPSILON;
            } else {
                ctx.animateTime                = sourceAnimateTarget;
                ctx.animateTimeTarget          = sourceAnimateTarget;
                ctx.animateTimeAnimationActive = false;
            }
            ctx.animatedTimelineZoom = sourceCtx.animatedTimelineZoom;
            ctx.animatedTimelineZoomTarget =
                sourceCtx.animatedTimelineZoomTarget;
            ctx.animatedTimelineZoomAnimationActive =
                sourceCtx.animatedTimelineZoomAnimationActive;
            ctx.isPlaying                   = false;
            ctx.isAudioTimelineSyncFollower = sourceCtx.isPlaying;
            ctx.playbackVisualClock.rebase(sourceCtx.currentTime,
                                           sourceClockSteadyTime,
                                           playbackRate,
                                           sourceCtx.isPlaying);
            if ( shouldClearHitEffects ) {
                ctx.hitFXSystem.clearActiveEffects();
            }
            if ( ctx.isAudioTimelineSyncFollower && entry.isCanvasVisible ) {
                updateFollowerHitEffects(
                    ctx, previousAnimateTime, shouldClearHitEffects);
            }
        }
        return;
    }
}

int32_t EditorEngine::createSession(std::shared_ptr<MMM::BeatMap> beatmap,
                                    const std::string&            displayName,
                                    bool               isLogoPlaceholder,
                                    const std::string& preferredCameraId,
                                    bool               restoreDockFromWorkspace)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    const auto                            editorConfig = getEditorConfig();
    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    auto& sessions      = m_sessionRegistry.entriesUnsafe();
    auto  cameraIdInUse = [&](const std::string& cameraId) {
        return std::any_of(
            sessions.begin(), sessions.end(), [&](const SessionEntry& entry) {
                return entry.cameraId == cameraId;
            });
    };

    /// @brief 当前项目指针快照，用于规范化新会话谱面中的项目相对路径。
    const auto* currentProject = ProjectController::instance().currentProject();
    if ( currentProject && beatmap ) {
        normalizeBeatmapMetadataPathsForProject(*beatmap, *currentProject);
    }

    /// @brief 本次请求打开的谱面稳定路径键。
    const std::string requestedBeatmapKey =
        beatmap ? makeBeatmapPathKey(currentProject,
                                     beatmap->m_baseMapMetadata.map_path)
                : std::string{};
    /// @brief 新 Session 在处理载入指令后发布完整时间线指纹。
    const std::string requestedAudioTimelineFingerprint;

    if ( !isLogoPlaceholder && beatmap ) {
        if ( !requestedBeatmapKey.empty() ) {
            for ( int32_t i = 0; i < static_cast<int32_t>(sessions.size());
                  ++i ) {
                const auto& entry = sessions[static_cast<size_t>(i)];
                if ( entry.isLogoPlaceholder || !entry.session ) {
                    continue;
                }

                const auto&       ctx = entry.session->getContext();
                const std::string openedBeatmapKey =
                    !entry.beatmapPathKey.empty()
                        ? entry.beatmapPathKey
                        : (ctx.currentBeatmap
                               ? makeBeatmapPathKey(
                                     currentProject,
                                     ctx.currentBeatmap->m_baseMapMetadata
                                         .map_path)
                               : std::string{});
                if ( openedBeatmapKey != requestedBeatmapKey ) {
                    continue;
                }

                requestSessionFocus(i);
                XINFO("Beatmap already open. Focusing Session #{} cameraId={}",
                      i,
                      entry.cameraId);
                return i;
            }
        }
    }

    // 检查是否可以复用 Logo 占位画布
    if ( !isLogoPlaceholder && beatmap ) {
        for ( int32_t i = 0; i < static_cast<int32_t>(sessions.size()); ++i ) {
            if ( sessions[i].isLogoPlaceholder ) {
                if ( !preferredCameraId.empty() &&
                     sessions[i].cameraId != preferredCameraId ) {
                    continue;
                }
                // 复用此画布：加载谱面到它的 Session
                sessions[i].isLogoPlaceholder        = false;
                sessions[i].restoreDockFromWorkspace = restoreDockFromWorkspace;
                sessions[i].displayName = displayName.empty()
                                              ? beatmap->m_baseMapMetadata.name
                                              : displayName;
                sessions[i].beatmapPathKey = requestedBeatmapKey;
                sessions[i].audioTimelineFingerprint =
                    requestedAudioTimelineFingerprint;
                if ( !preferredCameraId.empty() ) {
                    m_sessionRegistry.reserveCameraId(preferredCameraId);
                }
                sessions[i].session->pushCommand(
                    LogicCommand(CmdUpdateEditorConfig{ editorConfig }));
                sessions[i].session->pushCommand(LogicCommand(CmdChangeTool{
                    m_currentTool.load(std::memory_order_relaxed) }));
                restoreBrushNoteColorsUnsafe(*sessions[i].session);
                restoreBrushAudioResourceUnsafe(*sessions[i].session);
                sessions[i].session->pushCommand(
                    LogicCommand(CmdLoadBeatmap{ beatmap }));
                m_sessionRegistry.setActiveIndex(i);
                refreshMainAudioSyncPeerStateUnsafe();
                m_sessionRegistry.publishSnapshotUnsafe();
                m_lastMainAudioSyncActiveIndex = -1;

                XINFO("Reused Logo canvas {} for beatmap: {}",
                      sessions[i].cameraId,
                      sessions[i].displayName);
                return i;
            }
        }
    }

    // 生成唯一 cameraId
    /// @brief 新 Session 对应的唯一画布 cameraId。
    std::string cameraId;
    if ( !preferredCameraId.empty() && !cameraIdInUse(preferredCameraId) ) {
        cameraId = preferredCameraId;
        m_sessionRegistry.reserveCameraId(cameraId);
    } else {
        cameraId = m_sessionRegistry.createNextCameraId();
    }

    // 创建新 Session
    /// @brief 新创建的谱面逻辑 Session。
    auto newSession = std::make_shared<BeatmapSession>();

    // 同步历史视口尺寸
    /// @brief 当前共享视口尺寸快照，用于初始化新 Session 的共享视口。
    auto sharedViewportSizes = m_renderSyncRegistry.getSharedViewportSizes();
    for ( const auto& [cid, size] : sharedViewportSizes ) {
        {
            // 将共享视口 (Preview, Timeline) 的尺寸同步给新 Session
        }
        newSession->pushCommand(CmdUpdateViewport{ cid, size.x, size.y });
    }

    // 预注册该画布的 SyncBuffer
    getSyncBuffer(cameraId);

    // 推送初始配置
    newSession->pushCommand(
        LogicCommand(CmdUpdateEditorConfig{ editorConfig }));
    newSession->pushCommand(LogicCommand(
        CmdChangeTool{ m_currentTool.load(std::memory_order_relaxed) }));
    restoreBrushNoteColorsUnsafe(*newSession);
    restoreBrushAudioResourceUnsafe(*newSession);

    // 如果有谱面，加载它
    if ( beatmap ) {
        newSession->pushCommand(LogicCommand(CmdLoadBeatmap{ beatmap }));
    }

    // 添加到 Session 列表
    /// @brief 即将注册到会话列表的新 Session 条目。
    SessionEntry entry;
    entry.session  = newSession;
    entry.cameraId = cameraId;
    entry.displayName =
        displayName.empty()
            ? (beatmap ? beatmap->m_baseMapMetadata.name : "New Canvas")
            : displayName;
    entry.beatmapPathKey           = requestedBeatmapKey;
    entry.audioTimelineFingerprint = requestedAudioTimelineFingerprint;
    entry.isLogoPlaceholder        = isLogoPlaceholder;
    entry.restoreDockFromWorkspace = restoreDockFromWorkspace;
    /// @brief 新 Session 在注册表中的索引。
    int32_t newIndex = m_sessionRegistry.append(std::move(entry));
    refreshMainAudioSyncPeerStateUnsafe();
    m_lastMainAudioSyncActiveIndex = -1;

    XINFO("Created Session #{} cameraId={} name={} (logo={})",
          newIndex,
          cameraId,
          sessions[newIndex].displayName,
          isLogoPlaceholder);

    return newIndex;
}

void EditorEngine::closeSession(int32_t index, bool updateWorkspace)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    const auto& sessions = m_sessionRegistry.entriesUnsafe();

    if ( index < 0 || index >= static_cast<int32_t>(sessions.size()) ) {
        XWARN("closeSession: invalid index {}", index);
        return;
    }

    /// @brief 被关闭 Session 对应的 cameraId 快照。
    std::string cameraId = sessions[index].cameraId;
    XINFO("Closing Session #{} cameraId={}", index, cameraId);

    // 移除 Session
    m_sessionRegistry.erase(index);
    refreshMainAudioSyncPeerStateUnsafe();
    m_lastMainAudioSyncActiveIndex = -1;

    // 清理对应的 SyncBuffer
    m_renderSyncRegistry.eraseCamera(cameraId);

    if ( updateWorkspace ) {
        captureProjectWorkspaceState();
    }
}

/// @brief 将指定 Session 原地重置为 Logo 占位画布。
void EditorEngine::resetSessionToLogoPlaceholder(int32_t            index,
                                                 const std::string& displayName,
                                                 bool updateWorkspace)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    const auto                            editorConfig = getEditorConfig();
    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    auto& sessions = m_sessionRegistry.entriesUnsafe();

    if ( index < 0 || index >= static_cast<int32_t>(sessions.size()) ) {
        XWARN("resetSessionToLogoPlaceholder: invalid index {}", index);
        return;
    }

    /// @brief 即将保留并重置的 Session 条目。
    auto& entry = sessions[static_cast<size_t>(index)];
    if ( entry.isLogoPlaceholder ) {
        return;
    }

    /// @brief 新占位会话，用于清空原谱面状态但保留 UI 画布。
    auto newSession = std::make_shared<BeatmapSession>();
    for ( const auto& [cid, size] :
          m_renderSyncRegistry.getSharedViewportSizes() ) {
        newSession->pushCommand(CmdUpdateViewport{ cid, size.x, size.y });
    }
    if ( auto mainViewportSize =
             m_renderSyncRegistry.getViewportSize(entry.cameraId) ) {
        newSession->pushCommand(CmdUpdateViewport{
            entry.cameraId, mainViewportSize->x, mainViewportSize->y });
    }
    newSession->pushCommand(
        LogicCommand(CmdUpdateEditorConfig{ editorConfig }));
    newSession->pushCommand(LogicCommand(
        CmdChangeTool{ m_currentTool.load(std::memory_order_relaxed) }));
    restoreBrushNoteColorsUnsafe(*newSession);
    restoreBrushAudioResourceUnsafe(*newSession);

    entry.session     = std::move(newSession);
    entry.displayName = displayName.empty() ? "Welcome" : displayName;
    entry.beatmapPathKey.clear();
    entry.audioTimelineFingerprint.clear();
    entry.isLogoPlaceholder        = true;
    entry.restoreDockFromWorkspace = false;

    m_sessionRegistry.setActiveIndex(index);
    refreshMainAudioSyncPeerStateUnsafe();
    m_sessionRegistry.publishSnapshotUnsafe();
    m_lastMainAudioSyncActiveIndex = -1;

    if ( updateWorkspace ) {
        captureProjectWorkspaceState();
    }
}

/// @brief 设置当前活跃 Session，并切换全局复合音频 transport 所属谱面。
/// @param index 目标 Session 索引。
/// @warning 低频视图切换路径：可能解析并解码完整自动采样时间线。
void EditorEngine::setActiveSessionIndex(int32_t index)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    const auto                            editorConfig = getEditorConfig();
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    if ( index < 0 || index >= static_cast<int32_t>(sessions.size()) ) {
        return;
    }

    auto&         audio         = Audio::AudioManager::instance();
    const int32_t previousIndex = m_sessionRegistry.activeIndex();
    const double  sessionSwitchTime =
        std::chrono::duration<double>(FrameLimitClock::now().time_since_epoch())
            .count();
    double      previousTime       = 0.0;
    bool        previousWasPlaying = false;
    std::string previousFingerprint;
    if ( previousIndex >= 0 &&
         previousIndex < static_cast<int32_t>(sessions.size()) &&
         sessions[previousIndex].session ) {
        auto& previousCtx =
            sessions[previousIndex].session->getContextMutable();
        previousFingerprint = sessions[previousIndex].audioTimelineFingerprint;
        if ( previousCtx.isPlaying &&
             audio.getLoadedAudioTimelineFingerprint() ==
                 previousFingerprint ) {
            previousCtx.currentTime =
                resolveContinuousSessionTime(previousCtx, sessionSwitchTime);
            previousWasPlaying = true;
        }
        previousTime                            = previousCtx.currentTime;
        previousCtx.isPlaying                   = false;
        previousCtx.isAudioTimelineSyncFollower = false;
        previousCtx.isActiveSession             = false;
    }

    m_sessionRegistry.setActiveIndex(index);
    auto& activeSession = sessions[index].session;
    if ( !activeSession ) {
        audio.unloadAudioTimeline();
        return;
    }

    restoreBrushNoteColorsUnsafe(*activeSession);
    restoreBrushAudioResourceUnsafe(*activeSession);
    auto& ctx                       = activeSession->getContextMutable();
    ctx.isActiveSession             = true;
    ctx.isPlaying                   = false;
    ctx.isAudioTimelineSyncFollower = false;
    if ( ctx.isAudioTimelineDescriptorDirty ) {
        SessionUtils::rebuildAudioTimelineDescriptor(ctx, getCurrentProject());
    }
    sessions[index].audioTimelineFingerprint =
        ctx.audioTimelineDescriptor.m_fingerprint;
    ctx.isAudioTimelineFingerprintPublishPending = false;

    const auto switchDecision = SessionUtils::resolveAudioTimelineSwitch(
        previousFingerprint,
        sessions[index].audioTimelineFingerprint,
        previousTime,
        ctx.currentTime,
        previousWasPlaying,
        editorConfig.settings.stopPlaybackOnScroll,
        m_syncSameMainAudioCanvases.load(std::memory_order_relaxed));
    ctx.currentTime       = switchDecision.m_targetTime;
    bool transferPlayback = switchDecision.m_resumePlayback;

    const bool timelineReady = !sessions[index].isLogoPlaceholder &&
                               SessionUtils::activateAudioTimeline(ctx, false);
    double     totalTime     = SessionUtils::getEffectiveTotalTimeSeconds(ctx);
    double     minTime       = -editorConfig.visual.getEffectiveVisualOffset();
    if ( minTime > totalTime ) minTime = totalTime;
    ctx.currentTime = std::clamp(ctx.currentTime, minTime, totalTime);
    if ( timelineReady ) {
        audio.seek(ctx.currentTime);
        if ( transferPlayback ) {
            audio.play();
            ctx.isPlaying = true;
        }
    } else {
        audio.unloadAudioTimeline();
        transferPlayback = false;
    }

    ctx.animateTime =
        ctx.currentTime + editorConfig.visual.getEffectiveVisualOffset();
    ctx.animateTimeTarget                   = ctx.animateTime;
    ctx.animateTimeAnimationActive          = false;
    ctx.animatedTimelineZoom                = editorConfig.visual.timelineZoom;
    ctx.animatedTimelineZoomTarget          = ctx.animatedTimelineZoom;
    ctx.animatedTimelineZoomAnimationActive = false;
    ctx.currentTool = m_currentTool.load(std::memory_order_relaxed);
    ctx.hitFXSystem.clearActiveEffects();

    m_sessionRegistry.publishSnapshotUnsafe();
    refreshMainAudioSyncPeerStateUnsafe();
    m_lastMainAudioSyncActiveIndex = -1;
    XINFO("Switched active session to #{} cameraId={} fingerprint={}",
          index,
          sessions[index].cameraId,
          sessions[index].audioTimelineFingerprint);

    const auto sharedViewportSizes =
        m_renderSyncRegistry.getSharedViewportSizes();
    for ( const auto& [cameraId, size] : sharedViewportSizes ) {
        activeSession->pushCommand(
            CmdUpdateViewport{ cameraId, size.x, size.y });
    }
}

/// @brief 请求 UI 线程将指定 Session 对应的画布窗口聚焦到前台。
void EditorEngine::requestSessionFocus(int32_t index)
{
    m_pendingFocusSessionIndex.store(index, std::memory_order_relaxed);
}

/// @brief 消费一次待聚焦 Session 请求。
int32_t EditorEngine::consumePendingFocusSessionIndex()
{
    return m_pendingFocusSessionIndex.exchange(-1, std::memory_order_relaxed);
}

/// @brief 获取当前编辑器配置的线程安全值快照。
/// @warning UI 热路径：只在调用期间持有配置互斥锁；调用者应在本帧复用副本。
Config::EditorConfig EditorEngine::getEditorConfig() const
{
    std::lock_guard<std::mutex> lock(m_editorConfigMutex);
    return m_editorConfig;
}

/// @brief 按修订号刷新逻辑线程持有的编辑器配置快照。
/// @warning 逻辑热路径：未变化时仅执行一次 acquire
/// 原子读取，配置变化时才加锁复制。
bool EditorEngine::refreshEditorConfigSnapshot(
    Config::EditorConfig& target, std::uint64_t& targetRevision) const
{
    const std::uint64_t publishedRevision =
        m_editorConfigRevision.load(std::memory_order_acquire);
    if ( publishedRevision == targetRevision ) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_editorConfigMutex);
    target         = m_editorConfig;
    targetRevision = m_editorConfigRevision.load(std::memory_order_relaxed);
    return true;
}

void EditorEngine::setEditorConfig(const Config::EditorConfig& config)
{
    std::lock_guard<std::recursive_mutex> updateLock(m_editorConfigUpdateMutex);

    // 关键修复：从全局 AppConfig 中同步软件级状态，防止被
    // UI/项目工作区恢复覆盖。
    const auto& globalConfig = Config::AppConfig::instance().getEditorConfig();
    const auto& globalRecent = globalConfig.recentProjects;
    const auto  globalColorPalettes = globalConfig.settings.colorPalettes;
    const auto  globalDefaultColorPalette =
        globalConfig.settings.defaultColorPaletteSchemeName;

    Config::EditorConfig updatedConfig   = config;
    updatedConfig.recentProjects         = globalRecent;
    updatedConfig.settings.colorPalettes = globalColorPalettes;
    updatedConfig.settings.defaultColorPaletteSchemeName =
        globalDefaultColorPalette;
    auto& sfxConfig = updatedConfig.settings.sfxConfig;
    sfxConfig.unboundHitSfxGain =
        Config::sanitizeHitSfxGain(sfxConfig.unboundHitSfxGain);
    sfxConfig.boundHitSfxGain =
        Config::sanitizeHitSfxGain(sfxConfig.boundHitSfxGain);
    preserveGlobalAppManagedSettings(updatedConfig, globalConfig);
    m_frameLimitPreference.store(updatedConfig.settings.frameLimit,
                                 std::memory_order_relaxed);
    if ( auto* project = ProjectController::instance().currentProject() ) {
        captureToolbarWorkspaceState(
            project->m_settings.m_workspace,
            updatedConfig,
            m_syncSameMainAudioCanvases.load(std::memory_order_relaxed));
    }

    {
        std::lock_guard<std::mutex> lock(m_editorConfigMutex);
        m_editorConfig = updatedConfig;
        m_editorConfigRevision.fetch_add(1, std::memory_order_release);
    }

    // 同步回全局 AppConfig 实例
    Config::AppConfig::instance().getEditorConfig() = updatedConfig;
    syncKeySoundControls(updatedConfig.settings.sfxConfig);

    // 向所有 Session 广播配置变更
    {
        std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
        /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
        auto& sessions = m_sessionRegistry.entriesUnsafe();
        for ( auto& entry : sessions ) {
            if ( entry.session ) {
                entry.session->pushCommand(
                    LogicCommand(CmdUpdateEditorConfig{ updatedConfig }));
            }
        }
    }

    const char* limitNames[] = { "VSync",
                                 "2x Refresh Rate",
                                 "4x Refresh Rate",
                                 "8x Refresh Rate",
                                 "Unlimited" };
    XINFO("EditorEngine: Updated config. Frame Limit: {}",
          limitNames[static_cast<int>(updatedConfig.settings.frameLimit)]);

    // 发布配置更新事件，供 UI 层订阅
    Event::EventBus::instance().publish(
        Event::EditorConfigChangedEvent{ updatedConfig });
}

void EditorEngine::saveProject()
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    captureProjectWorkspaceState();
    ProjectController::instance().saveProject();
}

/// @brief 立即完成全部已打开会话中等待空闲期的元数据自动保存。
/// @warning 低频阻塞路径：仅允许逻辑线程在打包或关闭项目前调用；会持有
/// Session 注册表锁并可能同步写入多个谱面。
bool EditorEngine::flushPendingMetadataAutoSaves()
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    /// @brief 需要检查尾随元数据保存的当前会话列表。
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    bool  success  = true;
    for ( auto& entry : sessions ) {
        if ( entry.session && !entry.session->flushPendingMetadataAutoSave() ) {
            success = false;
        }
    }
    return success;
}

/// @brief 在打包前保存所有已打开会话的完整未落盘谱面修改。
/// @warning
/// 低频阻塞路径：仅允许逻辑线程在打包前调用；会持有 Session
/// 注册表锁并同步写入多个谱面。
bool EditorEngine::saveDirtyBeatmapsForPackaging()
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    bool  success  = true;
    for ( auto& entry : sessions ) {
        if ( entry.session && !entry.session->saveDirtyBeatmapForPackaging() ) {
            success = false;
        }
    }
    return success;
}

/// @brief 逻辑线程的主循环。
/// @warning 逻辑热路径：按配置 UPS 频率执行；禁止每 update 文件系统操作、完整
/// entt 遍历、完整排序、try/catch 和可避免的 shared_ptr 拷贝。
void EditorEngine::loop()
{
    auto   lastTime     = FrameLimitClock::now();
    auto   nextDeadline = lastTime;
    double lastTargetDt = 0.0;
    m_lastUpsTime       = lastTime;
    m_logicUpdateCount  = 0;
    m_logicUps.store(0.0f, std::memory_order_relaxed);
    /// @brief 项目控制器单例引用，用于低频消费项目切换和目录监听状态。
    auto& projectController = ProjectController::instance();
    /// @brief 逻辑线程独占的编辑器配置快照，配置修订变化时才刷新。
    Config::EditorConfig editorConfigSnapshot;
    std::uint64_t        editorConfigSnapshotRevision =
        std::numeric_limits<std::uint64_t>::max();
    (void)refreshEditorConfigSnapshot(editorConfigSnapshot,
                                      editorConfigSnapshotRevision);

    while ( m_running.load(std::memory_order_acquire) ) {
        // 动态获取当前的延迟目标，并与渲染循环共用相同换算规则。
        Config::FrameLimitPreference frameLimit =
            m_frameLimitPreference.load(std::memory_order_relaxed);
        const int refreshRate =
            Config::AppConfig::instance().getDeviceRefreshRate();
        const double targetDt =
            Config::frameLimitTargetInterval(frameLimit, refreshRate);

        auto currentTime = FrameLimitClock::now();

        if ( targetDt > 0.0 ) {
            const auto targetDuration =
                std::chrono::duration_cast<FrameLimitClock::duration>(
                    std::chrono::duration<double>(targetDt));

            if ( targetDt != lastTargetDt ) {
                nextDeadline = currentTime + targetDuration;
                lastTargetDt = targetDt;
            }

            if ( currentTime < nextDeadline ) {
                sleepUntilFrameDeadline(nextDeadline);
                currentTime = FrameLimitClock::now();
            }

            if ( currentTime - nextDeadline > targetDuration ) {
                nextDeadline = currentTime + targetDuration;
            } else {
                nextDeadline += targetDuration;
            }
        } else {
            nextDeadline = currentTime;
            lastTargetDt = targetDt;
        }

        std::chrono::duration<double> passed = currentTime - lastTime;
        lastTime                             = currentTime;
        double dt                            = passed.count();

        // 统计逻辑线程实时刷新率 (UPS)
        m_logicUpdateCount++;
        std::chrono::duration<double> upsElapsed = currentTime - m_lastUpsTime;
        if ( upsElapsed.count() >= 0.5 ) {
            m_logicUps.store(
                static_cast<float>(m_logicUpdateCount / upsElapsed.count()),
                std::memory_order_relaxed);
            m_logicUpdateCount = 0;
            m_lastUpsTime      = currentTime;
        }

        // 如果有待处理的项目路径，在锁外处理（避免 EventBus 锁内与 subscribe
        // 交叉）
        /// @brief 项目控制器消费出的本轮项目打开或关闭动作。
        ProjectController::PendingProjectAction projectAction;
        if ( projectController.hasPendingProjectAction() ) {
            projectAction = projectController.consumePendingProjectAction(
                needsCanvasCloseBeforeProjectOpen());
        }
        bool projectCloseSucceeded = true;
        if ( projectAction.m_closeProject ) {
            projectCloseSucceeded = closeProject();
        }
        if ( projectCloseSucceeded &&
             !projectAction.m_projectPathToOpen.empty() ) {
            if ( projectAction.m_projectOpenMode ==
                 ProjectController::ProjectOpenMode::TemporaryPackage ) {
                openTemporaryProjectPackage(projectAction.m_projectPathToOpen);
            } else {
                openProject(projectAction.m_projectPathToOpen,
                            projectAction.m_projectCreationOptions);
            }
        }

        (void)refreshEditorConfigSnapshot(editorConfigSnapshot,
                                          editorConfigSnapshotRevision);

        // 多 Session 轮询更新
        /// @brief 当前已发布的 Session 快照读取句柄，避免每 update
        /// 获取注册表锁。
        /// @warning 逻辑热路径/原子：每轮只做一次 acquire shared_ptr
        /// 读取，以保证 UI 替换快照后本轮会话生命周期仍然有效。
        const auto publishedSessionUpdateSnapshot =
            m_sessionRegistry.publishedSnapshot();
        const auto& sessionUpdateSnapshot =
            publishedSessionUpdateSnapshot->sessions;

        if ( !sessionUpdateSnapshot.empty() ) {
            int32_t activeIndex     = m_sessionRegistry.activeIndex();
            int32_t maxSessionIndex = -1;
            for ( const auto& entry : sessionUpdateSnapshot ) {
                maxSessionIndex = std::max(maxSessionIndex, entry.index);
            }
            if ( maxSessionIndex >= 0 &&
                 m_backgroundSessionUpdateTimes.size() <=
                     static_cast<size_t>(maxSessionIndex) ) {
                m_backgroundSessionUpdateTimes.resize(
                    static_cast<size_t>(maxSessionIndex) + 1);
            }

            const auto backgroundInterval =
                backgroundSessionUpdateInterval(refreshRate);
            /// @brief 本轮由指令驱动发生时间变化的 Session，用于同主音轨同步。
            int32_t commandSyncSourceIndex = -1;
            for ( const auto& entry : sessionUpdateSnapshot ) {
                const bool isActiveSession = entry.index == activeIndex;
                const bool isVisibleSession =
                    isActiveSession || entry.isCanvasVisible;
                const bool hadPendingCommands =
                    entry.session->hasPendingCommands();
                const bool hasPendingMetadataAutoSave =
                    entry.session->hasPendingMetadataAutoSave();
                bool shouldUpdateSession = isActiveSession;
                if ( !shouldUpdateSession ) {
                    const bool needsRealtimeUpdate =
                        entry.session->needsRealtimeUpdate();
                    if ( isVisibleSession && needsRealtimeUpdate ) {
                        shouldUpdateSession = true;
                    } else if ( hadPendingCommands ) {
                        shouldUpdateSession = true;
                    } else if ( !isVisibleSession &&
                                !hasPendingMetadataAutoSave ) {
                        shouldUpdateSession = false;
                    } else {
                        auto& lastBackgroundUpdate =
                            m_backgroundSessionUpdateTimes[static_cast<size_t>(
                                entry.index)];
                        shouldUpdateSession =
                            lastBackgroundUpdate ==
                                FrameLimitClock::time_point{} ||
                            currentTime - lastBackgroundUpdate >=
                                backgroundInterval;
                    }
                }

                if ( !shouldUpdateSession ) {
                    continue;
                }

                const double previousCurrentTime =
                    entry.session->getContext().currentTime;
                entry.session->update(
                    dt, editorConfigSnapshot, isActiveSession);
                if ( isActiveSession && hadPendingCommands &&
                     std::abs(entry.session->getContext().currentTime -
                              previousCurrentTime) >
                         MAIN_AUDIO_SYNC_TIME_EPSILON ) {
                    commandSyncSourceIndex = entry.index;
                }
                if ( entry.index != activeIndex ) {
                    m_backgroundSessionUpdateTimes[static_cast<size_t>(
                        entry.index)] = currentTime;
                }
            }
            if ( m_pendingWorkspaceActiveIndex >= 0 ) {
                int32_t requestedActiveIndex  = m_pendingWorkspaceActiveIndex;
                m_pendingWorkspaceActiveIndex = -1;
                setActiveSessionIndex(requestedActiveIndex);
                activeIndex = m_sessionRegistry.activeIndex();
            }

            if ( commandSyncSourceIndex >= 0 ) {
                syncSameMainAudioCanvasesFromIndex(commandSyncSourceIndex);
            }

            bool shouldSyncMainAudioCanvases = false;
            for ( const auto& entry : sessionUpdateSnapshot ) {
                if ( entry.index != activeIndex || !entry.session ) {
                    continue;
                }

                const auto& activeCtx = entry.session->getContext();
                shouldSyncMainAudioCanvases =
                    activeCtx.isPlaying ||
                    activeIndex != m_lastMainAudioSyncActiveIndex ||
                    std::abs(activeCtx.currentTime - m_lastMainAudioSyncTime) >
                        MAIN_AUDIO_SYNC_TIME_EPSILON;
                if ( shouldSyncMainAudioCanvases ) {
                    m_lastMainAudioSyncActiveIndex = activeIndex;
                    m_lastMainAudioSyncTime        = activeCtx.currentTime;
                }
                break;
            }
            if ( shouldSyncMainAudioCanvases ) {
                syncSameMainAudioCanvases();
            }
            m_cursorSmokeLifeOverride.store(
                resolveActiveCursorSmokeLifeOverride(
                    sessionUpdateSnapshot, m_sessionRegistry.activeIndex()),
                std::memory_order_relaxed);
        } else {
            m_cursorSmokeLifeOverride.store(-1.0f, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // 检查文件夹监听器是否捕获到了任何文件系统变更事件
        static auto lastChangeTime   = FrameLimitClock::now();
        static bool hasPendingChange = false;

        if ( projectController.consumeDirectoryChangePending() ) {
            hasPendingChange = true;
            lastChangeTime   = FrameLimitClock::now();
        }

        if ( hasPendingChange ) {
            auto now = FrameLimitClock::now();
            // 去抖动延时（200ms），在所有批量写操作静止后再执行安全扫描
            if ( std::chrono::duration<double>(now - lastChangeTime).count() >=
                 0.2 ) {
                XINFO(
                    "Directory Watcher: filesystem changes settled, rescanning "
                    "directory...");
                scanProjectDirectory();
                hasPendingChange = false;
            }
        }
    }
}

/// @brief 处理音频资源更新指令并执行音效登记和项目保存副作用。
void EditorEngine::handleUpdateAudioResource(const CmdUpdateAudioResource& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    auto* project = ProjectController::instance().currentProject();
    if ( !project ) {
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::UpdateType,
            cmd.id,
            false,
            {},
            "当前没有可更新音频资源的项目");
        return;
    }

    auto&      sessions = m_sessionRegistry.entriesUnsafe();
    const auto openBeatmapReferences =
        collectOpenSessionAudioReferencesUnsafe(*project, sessions);

    /// @brief 项目命令服务的音频资源更新结果。
    auto result = ProjectController::instance().updateAudioResource(
        cmd, openBeatmapReferences);
    if ( !result.m_updated ) {
        const std::string errorMessage =
            result.m_blockingBeatmapPaths.empty()
                ? "未找到要更新的音频资源"
                : "音频资源仍被玩家物件绑定，不能改为 Main";
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::UpdateType,
            cmd.id,
            false,
            result.m_blockingBeatmapPaths,
            errorMessage);
        return;
    }

    if ( result.m_effectResourceIdToUnload ) {
        Audio::AudioManager::instance().unloadSoundEffect(
            *result.m_effectResourceIdToUnload);
    }
    if ( result.m_effectRegistration ) {
        Audio::AudioManager::instance().registerSoundEffect(
            result.m_effectRegistration->m_resource.m_id,
            Config::pathToUtf8(result.m_effectRegistration->m_absolutePath),
            result.m_effectRegistration->m_resource.m_config);
    }
    if ( m_brushAudioResourceId == cmd.id ) {
        const auto resourceIterator =
            std::find_if(project->m_audioResources.begin(),
                         project->m_audioResources.end(),
                         [&](const AudioResource& resource) {
                             return resource.m_id == cmd.id;
                         });
        if ( resourceIterator != project->m_audioResources.end() ) {
            m_brushAudioTrackType = resourceIterator->m_type;
            for ( auto& entry : sessions ) {
                if ( entry.session ) {
                    entry.session->pushCommand(
                        LogicCommand(CmdSetBrushAudioResource{
                            m_brushAudioResourceId,
                            m_brushAudioTrackType,
                            m_brushAudioVolume,
                        }));
                }
            }
        }
    }
    markAudioTimelineDescriptorsDirtyUnsafe();
    saveProject();
    publishAudioResourceMutationResult(
        Event::AudioResourceMutationOperation::UpdateType,
        cmd.id,
        true,
        {},
        {});
}

/// @brief 增量重命名项目音频文件、资源 ID 和全部内存引用。
/// @param cmd 旧资源 ID 与新文件名。
/// @warning 低频项目资源路径：执行文件系统改名、谱面引用事务写回和
/// 已打开会话增量同步，禁止从每帧热路径调用。
void EditorEngine::handleRenameAudioResource(const CmdRenameAudioResource& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    auto* project = ProjectController::instance().currentProject();
    if ( !project ) {
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Rename,
            cmd.id,
            false,
            {},
            "当前没有可重命名音频资源的项目");
        return;
    }

    const auto resourceIterator = std::find_if(
        project->m_audioResources.begin(),
        project->m_audioResources.end(),
        [&](const AudioResource& resource) { return resource.m_id == cmd.id; });
    if ( resourceIterator == project->m_audioResources.end() ) {
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Rename,
            cmd.id,
            false,
            {},
            "未找到要重命名的音频资源");
        return;
    }
    if ( !isValidAudioResourceFileName(cmd.newFileName) ) {
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Rename,
            cmd.id,
            false,
            {},
            "文件名不能为空、点目录或包含路径分隔符");
        return;
    }

    const AudioResource previousResource = *resourceIterator;
    const auto storedPath = Config::utf8ToPath(resourceIterator->m_path);
    const auto oldPath =
        (storedPath.is_absolute() ? storedPath
                                  : project->m_projectRoot / storedPath)
            .lexically_normal();
    auto requestedFileName = Config::utf8ToPath(cmd.newFileName);
    if ( requestedFileName.extension().empty() ) {
        requestedFileName += oldPath.extension();
    }
    if ( requestedFileName.extension() != oldPath.extension() ) {
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Rename,
            cmd.id,
            false,
            {},
            "重命名不能改变音频文件扩展名");
        return;
    }

    const std::string newResourceId =
        Config::pathToUtf8(requestedFileName.filename());
    if ( !isValidAudioResourceFileName(newResourceId) ) {
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Rename,
            cmd.id,
            false,
            {},
            "目标音频文件名无效");
        return;
    }
    if ( newResourceId == cmd.id && requestedFileName == oldPath.filename() ) {
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Rename,
            newResourceId,
            true,
            {},
            {});
        return;
    }
    if ( std::ranges::any_of(project->m_audioResources,
                             [&](const AudioResource& candidate) {
                                 return &candidate != &*resourceIterator &&
                                        candidate.m_id == newResourceId;
                             }) ) {
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Rename,
            cmd.id,
            false,
            {},
            "项目中已存在同名音频轨道");
        return;
    }

    const auto newPath =
        (oldPath.parent_path() / requestedFileName).lexically_normal();
    std::error_code filesystemError;
    if ( !std::filesystem::exists(oldPath, filesystemError) ||
         filesystemError ) {
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Rename,
            cmd.id,
            false,
            {},
            "源音频文件不存在或不可访问");
        return;
    }
    filesystemError.clear();
    if ( std::filesystem::exists(newPath, filesystemError) ||
         filesystemError ) {
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Rename,
            cmd.id,
            false,
            {},
            "目标音频文件已存在");
        return;
    }

    const auto validationError =
        ProjectResourceService::validateAudioResourceMove(
            *project, oldPath, newPath);
    if ( !validationError.empty() ) {
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Rename,
            cmd.id,
            false,
            {},
            validationError);
        return;
    }

    std::filesystem::rename(oldPath, newPath, filesystemError);
    if ( filesystemError ) {
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Rename,
            cmd.id,
            false,
            {},
            "重命名音频文件失败：" + filesystemError.message());
        return;
    }

    std::string pathRemapError;
    if ( remapAudioResourcePathsAfterMove(oldPath, newPath, &pathRemapError) !=
         1U ) {
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Rename,
            cmd.id,
            false,
            {},
            pathRemapError.empty() ? "音频路径增量同步失败" : pathRemapError);
        return;
    }

    const auto beatmapIdRemap =
        ProjectResourceService::remapProjectBeatmapAudioResourceId(
            *project,
            previousResource,
            resourceIterator->m_path,
            newResourceId);
    if ( !beatmapIdRemap.m_success ) {
        filesystemError.clear();
        std::filesystem::rename(newPath, oldPath, filesystemError);
        std::string rollbackError;
        if ( !filesystemError ) {
            (void)remapAudioResourcePathsAfterMove(
                newPath, oldPath, &rollbackError);
        }
        std::string errorMessage = beatmapIdRemap.m_errorMessage;
        if ( filesystemError ) {
            errorMessage +=
                "；文件名自动回滚失败：" + filesystemError.message();
        } else if ( !rollbackError.empty() ) {
            errorMessage += "；路径状态回滚失败：" + rollbackError;
        } else {
            errorMessage += "；文件名与路径状态已回滚";
        }
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Rename,
            cmd.id,
            false,
            {},
            errorMessage);
        return;
    }

    const AudioTrackType renamedType   = resourceIterator->m_type;
    const auto           renamedConfig = resourceIterator->m_config;
    resourceIterator->m_id             = newResourceId;

    for ( auto& beatmapEntry : project->m_beatmaps ) {
        if ( beatmapEntry.m_audioTrackId == cmd.id ) {
            beatmapEntry.m_audioTrackId = newResourceId;
        }
    }
    auto& workspace = project->m_settings.m_workspace;
    if ( workspace.m_projectAudioToolSelectedResourceId == cmd.id ) {
        workspace.m_projectAudioToolSelectedResourceId = newResourceId;
    }
    if ( workspace.m_bpmMeasurementAudioTrackId == cmd.id ) {
        workspace.m_bpmMeasurementAudioTrackId = newResourceId;
    }
    for ( auto& controller : workspace.m_audioControllers ) {
        if ( controller.m_trackId != cmd.id ) continue;
        controller.m_trackId   = newResourceId;
        controller.m_trackName = newResourceId;
    }
    for ( auto& placement : workspace.m_projectAudioToolPlacements ) {
        if ( placement.m_audioResourceId == cmd.id ) {
            placement.m_audioResourceId = newResourceId;
        }
    }

    auto& sessions = m_sessionRegistry.entriesUnsafe();
    for ( auto& entry : sessions ) {
        if ( entry.isLogoPlaceholder || !entry.session ) continue;
        auto& ctx = entry.session->getContextMutable();
        if ( !ctx.currentBeatmap ) continue;

        const auto domainChanged =
            ProjectResourceService::remapBeatmapAudioResourceId(
                *ctx.currentBeatmap, cmd.id, newResourceId);
        const auto ecsChanged =
            remapSessionEcsAudioResourceId(ctx, cmd.id, newResourceId);
        if ( ecsChanged.m_changedNoteBindingCount > 0U ) {
            ctx.m_needsNotesSync = true;
            SessionUtils::markHitEventsDirty(ctx);
        }
        if ( ecsChanged.m_changedAudioSampleCount > 0U ) {
            ctx.m_needsSamplesSync = true;
        }
        if ( ecsChanged.m_changedNoteBindingCount > 0U ||
             ecsChanged.m_changedAudioSampleCount > 0U ) {
            SessionUtils::syncBeatmap(ctx);
        }
        if ( domainChanged > 0U ||
             ecsChanged.m_audioSampleReferenceCount > 0U ) {
            ctx.isAudioTimelineDescriptorDirty   = true;
            ctx.isAudioTimelineActivationPending = true;
        }
    }

    auto& audio = Audio::AudioManager::instance();
    if ( renamedType == AudioTrackType::Effect ) {
        audio.unloadSoundEffect(cmd.id);
        audio.registerSoundEffect(
            newResourceId, Config::pathToUtf8(newPath), renamedConfig);
    }
    if ( m_brushAudioResourceId == cmd.id ) {
        m_brushAudioResourceId = newResourceId;
        for ( auto& entry : sessions ) {
            if ( !entry.session ) continue;
            entry.session->pushCommand(LogicCommand(CmdSetBrushAudioResource{
                newResourceId,
                renamedType,
                m_brushAudioVolume,
            }));
        }
    }
    markAudioTimelineDescriptorsDirtyUnsafe();
    saveProject();
    publishAudioResourceMutationResult(
        Event::AudioResourceMutationOperation::Rename,
        newResourceId,
        true,
        {},
        {});
}

/// @brief 更新音频资源配置并使所有引用它的已打开时间线失效。
void EditorEngine::handleUpdateAudioResourceConfig(
    const CmdUpdateAudioResourceConfig& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    auto* project = ProjectController::instance().currentProject();
    if ( !project ) {
        XWARN("Cannot update audio resource config without an open project");
        return;
    }

    const auto resourceIterator = std::find_if(
        project->m_audioResources.begin(),
        project->m_audioResources.end(),
        [&](const AudioResource& resource) { return resource.m_id == cmd.id; });
    if ( resourceIterator == project->m_audioResources.end() ) {
        XWARN("Cannot update missing audio resource config: {}", cmd.id);
        return;
    }

    resourceIterator->m_config = cmd.config;
    if ( resourceIterator->m_type == AudioTrackType::Effect ) {
        const auto absolutePath = project->m_projectRoot /
                                  Config::utf8ToPath(resourceIterator->m_path);
        Audio::AudioManager::instance().registerSoundEffect(
            cmd.id, Config::pathToUtf8(absolutePath), cmd.config);
    }

    markAudioTimelineDescriptorsDirtyUnsafe(cmd.id);
    saveProject();
}

/// @brief 处理删除音频资源指令并执行音效卸载和项目保存副作用。
void EditorEngine::handleRemoveAudioResource(const CmdRemoveAudioResource& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    auto* project = ProjectController::instance().currentProject();
    if ( !project ) {
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Remove,
            cmd.id,
            false,
            {},
            "当前没有可删除音频资源的项目");
        return;
    }

    auto&      sessions = m_sessionRegistry.entriesUnsafe();
    const auto openBeatmapReferences =
        collectOpenSessionAudioReferencesUnsafe(*project, sessions);

    /// @brief 项目命令服务的音频资源删除结果。
    auto result = ProjectController::instance().removeAudioResource(
        cmd, openBeatmapReferences);
    if ( !result.m_removed ) {
        const std::string errorMessage =
            !result.m_errorMessage.empty() ? result.m_errorMessage
            : result.m_blockingBeatmapPaths.empty()
                ? "未找到要删除的音频资源"
                : "音频资源仍被玩家物件或自动采样引用，不能删除";
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Remove,
            cmd.id,
            false,
            result.m_blockingBeatmapPaths,
            errorMessage);
        return;
    }

    if ( result.m_effectResourceIdToUnload ) {
        Audio::AudioManager::instance().unloadSoundEffect(
            *result.m_effectResourceIdToUnload);
    }
    if ( m_brushAudioResourceId == cmd.id ) {
        m_brushAudioResourceId.clear();
        m_brushAudioTrackType = AudioTrackType::Effect;
        m_brushAudioVolume    = 1.0F;
        project->m_settings.m_workspace.m_projectAudioToolSelectedResourceId
            .clear();
        for ( auto& entry : sessions ) {
            if ( entry.session ) {
                entry.session->pushCommand(
                    LogicCommand(CmdSetBrushAudioResource{
                        {},
                        AudioTrackType::Effect,
                        1.0F,
                    }));
            }
        }
    }
    markAudioTimelineDescriptorsDirtyUnsafe();
    saveProject();
    publishAudioResourceMutationResult(
        Event::AudioResourceMutationOperation::Remove, cmd.id, true, {}, {});
}

/// @brief 处理删除谱面指令并在项目发生变化时保存。
void EditorEngine::handleRemoveBeatmap(const CmdRemoveBeatmap& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    /// @brief 当前项目指针，用于把待删除谱面路径规范成 Session 路径键。
    const auto* currentProject = ProjectController::instance().currentProject();
    /// @brief 待删除谱面的稳定路径键。
    const std::string removedBeatmapKey =
        makeBeatmapPathKey(currentProject, Config::utf8ToPath(cmd.filePath));

    /// @brief 项目命令服务的谱面删除结果。
    auto result = ProjectController::instance().removeBeatmap(cmd);
    if ( !result.m_changed ) {
        return;
    }

    /// @brief 需要同步关闭的已打开谱面 Session 索引。
    std::vector<int32_t> sessionsToClose;
    if ( !removedBeatmapKey.empty() ) {
        /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
        const auto& sessions = m_sessionRegistry.entriesUnsafe();
        for ( int32_t i = 0; i < static_cast<int32_t>(sessions.size()); ++i ) {
            const auto& entry = sessions[static_cast<size_t>(i)];
            if ( entry.isLogoPlaceholder || !entry.session ) {
                continue;
            }

            std::string openedBeatmapKey = entry.beatmapPathKey;
            if ( openedBeatmapKey.empty() ) {
                const auto& ctx = entry.session->getContext();
                if ( ctx.currentBeatmap ) {
                    openedBeatmapKey = makeBeatmapPathKey(
                        currentProject,
                        ctx.currentBeatmap->m_baseMapMetadata.map_path);
                }
            }

            if ( openedBeatmapKey == removedBeatmapKey ) {
                sessionsToClose.push_back(i);
            }
        }
    }

    for ( auto it = sessionsToClose.rbegin(); it != sessionsToClose.rend();
          ++it ) {
        closeSession(*it, false);
    }
    if ( !sessionsToClose.empty() ) {
        captureProjectWorkspaceState();
    }

    saveProject();
}

/// @brief 将当前临时项目保存到正式项目目录。
/// @param cmd 保存临时项目指令。
void EditorEngine::handleSaveTemporaryProject(
    const CmdSaveTemporaryProject& cmd)
{
    ProjectController::SaveTemporaryProjectResult result;
    {
        std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

        if ( const auto* currentProject =
                 ProjectController::instance().currentProject() ) {
            auto& sessions = m_sessionRegistry.entriesUnsafe();
            for ( auto& entry : sessions ) {
                if ( entry.isLogoPlaceholder || !entry.session ) {
                    continue;
                }

                const auto& ctx = entry.session->getContext();
                if ( ctx.currentBeatmap ) {
                    normalizeBeatmapMetadataPathsForProject(*ctx.currentBeatmap,
                                                            *currentProject);
                }
            }
        }

        captureProjectWorkspaceState();
        result = ProjectController::instance().saveTemporaryProjectTo(
            Config::utf8ToPath(cmd.destinationPath));

        if ( result.m_success ) {
            if ( const auto* currentProject =
                     ProjectController::instance().currentProject() ) {
                auto& sessions = m_sessionRegistry.entriesUnsafe();
                for ( auto& entry : sessions ) {
                    if ( entry.isLogoPlaceholder || !entry.session ) {
                        entry.beatmapPathKey.clear();
                        continue;
                    }

                    const auto& ctx = entry.session->getContext();
                    if ( !ctx.currentBeatmap ) {
                        entry.beatmapPathKey.clear();
                        continue;
                    }

                    normalizeBeatmapMetadataPathsForProject(*ctx.currentBeatmap,
                                                            *currentProject);
                    entry.beatmapPathKey = makeBeatmapPathKey(
                        currentProject,
                        ctx.currentBeatmap->m_baseMapMetadata.map_path);
                }
            }
            refreshAudioTimelineFingerprintsUnsafe();
            captureProjectWorkspaceState();
            ProjectController::instance().saveProject();
        }
    }

    Event::TemporaryProjectSaveResultEvent event;
    event.m_success          = result.m_success;
    event.m_savedProjectPath = Config::pathToUtf8(result.m_savedProjectPath);
    event.m_errorMessage     = result.m_errorMessage;

    Event::EventBus::instance().publish(event);
}

/// @brief 更新项目内谱面文件路径关联并在项目发生变化时保存。
void EditorEngine::updateBeatmapFilePathInProject(
    const std::filesystem::path& oldPath, const std::filesystem::path& newPath)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    /// @brief 当前项目指针，仅用于更新已打开 Session 的谱面路径键。
    const auto* currentProject = ProjectController::instance().currentProject();
    /// @brief 旧谱面路径键。
    const std::string oldBeatmapKey =
        makeBeatmapPathKey(currentProject, oldPath);
    /// @brief 新谱面路径键。
    const std::string newBeatmapKey =
        makeBeatmapPathKey(currentProject, newPath);

    /// @brief 项目命令服务的谱面路径更新结果。
    auto result =
        ProjectController::instance().updateBeatmapFilePath(oldPath, newPath);
    if ( result.m_changed ) {
        saveProject();
    }

    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    if ( !oldBeatmapKey.empty() && !newBeatmapKey.empty() ) {
        for ( auto& entry : sessions ) {
            if ( entry.beatmapPathKey == oldBeatmapKey ) {
                entry.beatmapPathKey = newBeatmapKey;
            }
        }
    }
    refreshAudioTimelineFingerprintsUnsafe();
}

/// @brief 在文件系统操作前验证外部谱面的音频引用可安全保持。
/// @param oldPath 计划移动的文件或目录路径。
/// @param newPath 计划移动到的文件或目录路径。
/// @return 允许移动时为空；否则返回可直接展示的阻止原因。
/// @warning 低频文件操作路径：会读取项目中的 osu! 谱面并检查全部
/// RM/IMD 隐式音频关联。
std::string EditorEngine::validateAudioResourceMove(
    const std::filesystem::path& oldPath, const std::filesystem::path& newPath)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    const auto* project = ProjectController::instance().currentProject();
    if ( !project ) return {};
    return ProjectResourceService::validateAudioResourceMove(
        *project, oldPath, newPath);
}

/// @brief 文件或目录移动后同步项目音频路径和已打开会话引用。
/// @param oldPath 移动前的文件或目录路径。
/// @param newPath 移动后的文件或目录路径。
/// @param errorMessage 失败时接收面向用户的错误和回滚状态。
/// @return 路径发生变化的项目音频资源数量。
/// @warning 低频文件操作路径：会同步全部打开会话并扫描项目谱面文件。
std::size_t EditorEngine::remapAudioResourcePathsAfterMove(
    const std::filesystem::path& oldPath, const std::filesystem::path& newPath,
    std::string* errorMessage)
{
    if ( errorMessage ) errorMessage->clear();
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    auto* project = ProjectController::instance().currentProject();
    if ( !project ) return 0U;

    auto& sessions = m_sessionRegistry.entriesUnsafe();
    (void)collectOpenSessionAudioReferencesUnsafe(*project, sessions);

    /// @brief 移动前资源表快照，用于识别路径变化并匹配旧引用。
    const auto resourcesBeforeMove = project->m_audioResources;
    const auto changedCount =
        ProjectResourceService::remapAudioResourcePathsAfterMove(
            *project, oldPath, newPath, errorMessage);
    if ( changedCount == 0U ) return 0U;

    /// @brief 单个路径发生变化的资源前后快照。
    struct ChangedAudioResourcePath {
        /// @brief 移动前资源状态。
        AudioResource m_before;

        /// @brief 移动后资源状态。
        AudioResource m_after;
    };
    std::vector<ChangedAudioResourcePath> changedResources;
    changedResources.reserve(changedCount);
    for ( const auto& resourceAfter : project->m_audioResources ) {
        const auto resourceBefore =
            std::find_if(resourcesBeforeMove.begin(),
                         resourcesBeforeMove.end(),
                         [&](const AudioResource& candidate) {
                             return candidate.m_id == resourceAfter.m_id;
                         });
        if ( resourceBefore == resourcesBeforeMove.end() ||
             resourceBefore->m_path == resourceAfter.m_path ) {
            continue;
        }
        changedResources.push_back(
            ChangedAudioResourcePath{ *resourceBefore, resourceAfter });
    }

    for ( auto& entry : sessions ) {
        if ( entry.isLogoPlaceholder || !entry.session ) continue;

        auto& ctx = entry.session->getContextMutable();
        if ( !ctx.currentBeatmap ) continue;

        const auto beatmapPath =
            getOpenSessionBeatmapDiagnosticPath(*project, entry, ctx);
        bool noteEcsChanged                  = false;
        bool sampleEcsChanged                = false;
        bool referencesMovedTimelineResource = false;
        for ( const auto& changedResource : changedResources ) {
            const auto beatmapRemap =
                ProjectResourceService::remapBeatmapAudioReferencesAfterMove(
                    *project,
                    *ctx.currentBeatmap,
                    beatmapPath,
                    changedResource.m_before,
                    changedResource.m_after.m_path);
            const auto ecsRemap = remapSessionEcsAudioReferences(
                *project, ctx, beatmapPath, changedResource.m_before);
            noteEcsChanged |= ecsRemap.m_changedNoteBindingCount > 0U;
            sampleEcsChanged |= ecsRemap.m_changedAudioSampleCount > 0U;
            referencesMovedTimelineResource |=
                beatmapRemap.m_audioSampleReferenceCount > 0U ||
                ecsRemap.m_audioSampleReferenceCount > 0U;
        }

        if ( noteEcsChanged ) {
            ctx.m_needsNotesSync = true;
            SessionUtils::markHitEventsDirty(ctx);
        }
        if ( sampleEcsChanged ) {
            ctx.m_needsSamplesSync = true;
        }
        if ( noteEcsChanged || sampleEcsChanged ) {
            SessionUtils::syncBeatmap(ctx);
        }
        if ( referencesMovedTimelineResource ) {
            ctx.isAudioTimelineDescriptorDirty   = true;
            ctx.isAudioTimelineActivationPending = true;
        }
    }

    auto& audio = Audio::AudioManager::instance();
    for ( const auto& changedResource : changedResources ) {
        if ( changedResource.m_before.m_type == AudioTrackType::Effect ) {
            audio.unloadSoundEffect(changedResource.m_before.m_id);
        }
        if ( changedResource.m_after.m_type == AudioTrackType::Effect ) {
            const auto absolutePath =
                project->m_projectRoot /
                Config::utf8ToPath(changedResource.m_after.m_path);
            audio.registerSoundEffect(changedResource.m_after.m_id,
                                      Config::pathToUtf8(absolutePath),
                                      changedResource.m_after.m_config);
        }
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::MovePath,
            changedResource.m_after.m_id,
            true,
            {},
            {});
    }

    saveProject();
    return changedCount;
}

void EditorEngine::scanProjectDirectory()
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    /// @brief 当前项目指针，仅用于解析音效资源绝对路径。
    const auto* currentProject = ProjectController::instance().currentProject();
    if ( !currentProject ) return;

    /// @brief 当前目录资源同步结果。
    auto syncResult = ProjectController::instance().scanProjectDirectory();

    // 登记新发现的音效，首次显式使用时再解码。
    for ( const auto& res : syncResult.m_effectResourcesToRegister ) {
        /// @brief 新音效资源的项目内绝对路径。
        auto absAudioPath =
            currentProject->m_projectRoot / Config::utf8ToPath(res.m_path);
        Audio::AudioManager::instance().registerSoundEffect(
            res.m_id, Config::pathToUtf8(absAudioPath), res.m_config);
    }

    // 如果有任何文件发现/删除/更新，保存项目配置
    if ( syncResult.m_changed ) {
        markAudioTimelineDescriptorsDirtyUnsafe();
        saveProject();
    }
}

}  // namespace MMM::Logic
