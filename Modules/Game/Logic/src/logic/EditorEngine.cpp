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
#include "logic/UnlimitedIdleUpdateGate.h"
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
#include <utility>
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
    // 总开关与未绑定、已绑定两组开关分别同步，保留各组独立静音偏好。
    // 增益仍单独写入，解除静音后可恢复当前配置而不需要重载资源。
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
    // 此处只限制单个路径分量，避免重命名输入携带目录移动语义。
    // 并未检查所有平台保留字符或目标是否已存在，后续文件操作仍需报告失败。
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
    // 工作区保存与标签切换需要连续时间，而不是上一次 update 的离散值。
    // 统一使用调用方提供的单调时刻，多个会话可在同一时间基准比较。
    if ( ctx.playbackVisualClock.initialized() ) {
        resolvedTime = ctx.playbackVisualClock.currentTimeAt(nowSteadySeconds);
    }
    // 视觉时钟结果异常时先回退会话时间，二者都无效才采用零。
    // 避免把非有限播放位置写入工作区或传播给下一会话。
    if ( !std::isfinite(resolvedTime) ) {
        resolvedTime = std::isfinite(ctx.currentTime) ? ctx.currentTime : 0.0;
    }

    const double totalTime = SessionUtils::getEffectiveTotalTimeSeconds(ctx);
    // 只钳制有限总时长上界，保留负时间视觉前置区间。
    // 这里不把负播放位置压到零，避免切换标签时丢失预滚动状态。
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
    // 事件路径使用统一 UTF-8 表示，避免状态栏与错误提示各自转换编码。
    // isPackage 保留失败入口类别，接收方无需再从扩展名猜测。
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

/// @brief 取得适合状态栏展示的项目加载路径名称。
/// @param path 项目、谱面包或谱面路径。
/// @return 优先返回文件名，无法取得时返回完整路径。
std::string projectOpenProgressPathDetail(const std::filesystem::path& path)
{
    const auto fileName = path.filename();
    return Config::pathToUtf8(fileName.empty() ? path : fileName);
}

/// @brief 发布项目打开流程的分阶段进度。
/// @param stage 当前加载阶段。
/// @param fraction 当前总进度，范围为 0 到 1。
/// @param detail 当前处理对象名称。
void publishProjectOpenProgress(Event::ProjectOpenProgressStage stage,
                                float fraction, std::string detail)
{
    Event::ProjectOpenProgressEvent event;
    event.m_stage    = stage;
    event.m_fraction = fraction;
    // 进度详情按值进入事件，调用方的临时字符串可在发布后释放。
    // 该助手不验证阶段或钳制进度数值，调用者须保持阶段与总进度一致。
    event.m_detail = std::move(detail);
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

    // 跳转或播放不连续时先重置事件游标，并恢复当前时刻仍有效的持续特效。
    // 直接返回避免把重建后的历史事件再次作为本轮新触发消费。
    if ( resetHitIndex ) {
        SessionUtils::syncHitIndex(ctx);
        ctx.hitFXSystem.restoreActiveHoldEffects(
            ctx.animateTime, ctx.hitEvents, config);
        return;
    }

    // 正常推进只收集上一动画时刻之后、本次时刻之前的事件。
    // 当前使用局部向量，存在随触发数量增长的分配成本。
    std::vector<System::HitFXSystem::HitEvent> triggeredEvents;
    while ( ctx.nextHitIndex < ctx.hitEvents.size() &&
            ctx.hitEvents[ctx.nextHitIndex].timestamp <= ctx.animateTime ) {
        const auto& ev = ctx.hitEvents[ctx.nextHitIndex];
        // 前闭合边界的事件已经被前一轮消费，严格大于可避免重复触发。
        // 游标对所有不晚于当前时刻的事件推进，包括不需重新触发的旧事件。
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

        // 限频等待属于用户帧率设置的节拍控制，不用于等待业务状态同步。
        // 先粗睡眠再睡到剩余截止点，避免用 yield 在最后阶段忙等。
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
    // 后台预算限制在既定范围内，设备报告零或异常低刷新率仍得到有效间隔。
    // 此处只计算 duration，不实际阻塞任何会话线程。
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
    // 统计尚未稳定或目标无效时保持原预算，避免启动阶段误降刷新率。
    // Unlimited 的零目标同样不适用这套相对健康度计算。
    if ( logicUps <= 1.0 || targetUps <= 1.0 || !std::isfinite(logicUps) ||
         !std::isfinite(targetUps) ) {
        return snapshotHz;
    }

    // 分段按负载健康度降低快照生成预算，为逻辑推进保留处理时间。
    // 倍率作用于传入预算，不在此设置线程睡眠或修改用户目标 UPS。
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
    // 从已排序 BPM 缓存查找当前时间之后的首事件，再取前项作为有效 BPM。
    // 首次 BPM 事件之前沿用谱面偏好值，不扫描原始 Timing 容器。
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
/// 快照，调用者必须持有 SessionRegistry 锁以同步 SessionContext 读取。
float resolveActiveCursorSmokeLifeOverride(
    const std::vector<SessionSnapshotEntry>& sessions, int32_t activeIndex)
{
    // 活跃值是注册表稳定索引，不是当前快照向量的位置。
    // 按 entry.index 查找可兼容中间会话关闭后留下的索引空洞。
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
    // 这里返回 SessionEntry 列表位置，与带索引快照中的 entry.index
    // 查询方式不同。 不检查 session
    // 是否存在，后续路由需自行验证目标可接收命令。
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
    // 先确认悬浮目标索引与会话有效，不能只凭 cameraId 命中就放行。
    // 活动项检查在目标验证之后，空活动槽也不会误获得滚轮权限。
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

    // 不同会话只有共享非空主音轨同步键才允许悬浮滚动路由。
    // 两个空键不代表同步关系，必须显式排除空指纹相等的情况。
    const auto& activeFingerprint =
        sessions[static_cast<size_t>(activeIndex)].mainAudioSyncFingerprint;
    const auto& targetFingerprint =
        sessions[static_cast<size_t>(targetIndex)].mainAudioSyncFingerprint;
    return !activeFingerprint.empty() && activeFingerprint == targetFingerprint;
}

/// @brief 将持久化的项目相对路径解析为文件系统路径。
/// @param project 提供相对路径解释基准的项目。
/// @param path 持久化路径，可为空或绝对路径。
/// @return 词法规范化的路径，不检查目标是否存在。
/// @note 空路径保持空，绝对路径不再拼接项目根。
std::filesystem::path resolveProjectPath(const Project&               project,
                                         const std::filesystem::path& path)
{
    if ( path.empty() || path.is_absolute() ) {
        return path.lexically_normal();
    }
    // 相对路径只在项目根下解释，不使用进程当前目录。
    // 词法规范化折叠冗余分量，但不解析符号链接。
    return (project.m_projectRoot / path).lexically_normal();
}

/// @brief 将文件系统路径转换为稳定的项目相对路径。
/// @param project 相对路径的根目录来源。
/// @param path 待持久化的文件系统路径。
/// @return 相对路径；文件系统转换失败时回退文件名。
/// @warning 低频路径转换可能访问文件系统，禁止放入逐帧坐标或时间同步。
std::filesystem::path makeProjectRelativePath(const Project& project,
                                              const std::filesystem::path& path)
{
    if ( path.empty() ) return {};
    // 已经相对的输入直接规范化，避免再次添加项目目录前缀。
    // 绝对路径才需要查询项目根并执行相对转换。
    if ( path.is_relative() ) return path.lexically_normal();

    std::error_code ec;
    auto            root = std::filesystem::absolute(project.m_projectRoot, ec);
    // 无法解析项目根时仍给出可展示的文件名，而不是传播无效相对结果。
    // 这个回退可能失去目录区分度，调用方不能把它当作完整唯一身份。
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
/// @warning 打开与查重的低频路径：weakly_canonical 可能访问文件系统。
/// @note 路径规范化失败时仍使用词法路径，不通过异常中断项目打开。
std::string makeBeatmapPathKey(const Project*               project,
                               const std::filesystem::path& path)
{
    if ( path.empty() ) {
        return "";
    }

    std::filesystem::path keyPath = path;
    // 有项目时以项目根解释相对路径，未关联项目时才使用进程绝对路径转换。
    // 两种入口最终都转成同一 UTF-8 键格式。
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
    // 弱规范化允许部分路径尚不存在，同时尽量消除已有前缀的路径别名。
    // 失败时保留先前路径，最后的词法整理仍可处理点目录冗余。
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
    // 当前谱面尚无路径时退回会话保存的稳定路径键。
    // 诊断不能因为内存谱面未同步路径就直接丢失来源信息。
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
    // 所有路径来源都不可用时才使用显示名或通用占位描述。
    // 该结果用于用户诊断，不应再作为资源解析的唯一文件身份。
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
        // Logo 占位与空会话不携带谱面依赖，先过滤以免访问无效上下文。
        // 这里只收集已打开谱面，磁盘中其他谱面由资源服务的扫描路径补充。
        if ( entry.isLogoPlaceholder || !entry.session ) continue;

        auto& ctx = entry.session->getContextMutable();
        if ( !ctx.currentBeatmap ) continue;

        // 先把未保存 ECS 修改同步到内存谱面，再提取当前引用。
        // 否则删除资源时可能遗漏刚绑定但尚未落盘的音符或自动采样。
        SessionUtils::syncBeatmap(ctx);
        const auto beatmapPath =
            getOpenSessionBeatmapDiagnosticPath(project, entry, ctx);
        auto references = ProjectResourceService::collectBeatmapAudioReferences(
            *ctx.currentBeatmap, beatmapPath);
        // 每个会话收集结果按移动迭代器并入总表，保留引用来源路径与类别。
        // 不在此跨会话去重，以免丢失需要显示的多个阻塞谱面。
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
    // 先发布完整结构化结果，UI 可同时读取操作类型、资源 ID 和阻塞路径。
    // 日志仅补充失败诊断，不能替代事件中的可定位来源。
    Event::EventBus::instance().publish(event);
    if ( !success ) {
        XWARN("Audio resource mutation failed for '{}': {}",
              resourceId,
              errorMessage);
        // 逐条记录阻塞谱面，避免主错误消息只说明失败却没有引用来源。
        // 该日志位于低频资源操作结果路径，不进入每次会话更新。
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
/// @warning 资源移动后的低频更新：完整遍历当前会话音符和采样，禁止逐 update
/// 调用。
/// @note 计数对应组件存储字段，不是去重后的谱面对象数量。
SessionAudioReferenceRemapResult remapSessionEcsAudioReferences(
    const Project& project, SessionContext& ctx, const std::string& beatmapPath,
    const AudioResource& previousResource)
{
    SessionAudioReferenceRemapResult result;
    // 匹配使用移动前资源快照，旧路径才能按原谱面目录正确解析。
    // 引用种类随字段传递，避免把 Note 绑定和歌曲提示当成同一种依赖。
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
    // 已有稳定 ID 无需改写，只有匹配旧路径的绑定才计入变化数。
    // 原实例音量和其他绑定属性保留，只更新资源身份字符串。
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

    // 根组件和内嵌子段都可能保存绑定，必须逐存储位置更新。
    // 独立 ECS 子实体也在视图内，因此计数不能直接解释为音效事件数量。
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
        // 匹配数包含已经使用稳定 ID 的采样，用于识别该会话仍依赖资源。
        // 实际改写数只在引用字符串变化后增加，两者服务不同后续判断。
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
/// @warning 显式重命名的低频会话扫描，调用者负责会话锁及后续派生状态刷新。
/// @pre 旧 ID 与新 ID 代表一次实际重命名；本助手不自行检查二者不同。
/// @note 本入口只做精确 ID 比较，路径兼容匹配由移动引用入口处理。
SessionAudioReferenceRemapResult remapSessionEcsAudioResourceId(
    SessionContext& ctx, std::string_view oldResourceId,
    std::string_view newResourceId)
{
    SessionAudioReferenceRemapResult result;
    // 只改精确命中的绑定身份，保持未命中的路径引用原样。
    // 新 ID 从 string_view 复制到组件字段，不保存调用方临时字符串视图。
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

    // 采样匹配和改写在这个精确重命名入口同步计数。
    // 它与路径规范化入口的“已匹配但无需改写”分支不同。
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
/// @param project 项目相对路径的解释基准。
/// @param mapDirectory 当前谱面绝对目录。
/// @param path 原始资源路径，可为空或绝对路径。
/// @param preferProjectRoot 是否优先尝试项目根下的相对路径。
/// @return 首个存在的候选，均不存在时保留首选解释。
/// @warning 低频保存路径：使用文件存在性查询，不得放入渲染或逻辑热循环。
std::filesystem::path resolveMetadataResourcePath(
    const Project& project, const std::filesystem::path& mapDirectory,
    const std::filesystem::path& path, bool preferProjectRoot)
{
    if ( path.empty() || path.is_absolute() ) {
        return path.lexically_normal();
    }

    // 同一个相对字符串可能源于项目格式或外部谱面格式。
    // 分别构造两种候选，避免盲目把外部谱面资源解释到项目根。
    auto projectPath = resolveProjectPath(project, path);
    auto mapPath     = (mapDirectory / path).lexically_normal();

    std::error_code ec;
    // 首选存在时直接返回，第二候选只作为缺失回退。
    // 两个候选都不存在也保留首选路径，使未到位资源仍可持久化其预期位置。
    if ( preferProjectRoot ) {
        if ( std::filesystem::exists(projectPath, ec) ) return projectPath;
        // 独立文件查询前清除上一候选错误，避免混用两次检查的状态。
        // 存在性查询失败与不存在一样继续回退，不在此抛异常中止保存。
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
/// @param beatMap 要原地更新长期资源路径的谱面。
/// @param project 提供统一持久化根目录的项目。
/// @warning 低频打开及保存路径：包括路径解析和存在性检查。
/// @note 只处理元数据路径，不改变 Note 或自动采样的稳定资源 ID。
void normalizeBeatmapMetadataPathsForProject(BeatMap&       beatMap,
                                             const Project& project)
{
    auto& meta = beatMap.m_baseMapMetadata;
    // 没有谱面路径就缺少解释外部相对资源的可靠目录，保持原元数据。
    // 不能使用进程当前工作目录替代尚未确定的谱面位置。
    if ( meta.map_path.empty() ) return;

    auto absoluteMapPath = resolveProjectPath(project, meta.map_path);
    auto mapDirectory    = absoluteMapPath.parent_path();
    auto mapExtension    = Config::pathToUtf8(absoluteMapPath.extension());
    std::transform(mapExtension.begin(),
                   mapExtension.end(),
                   mapExtension.begin(),
                   ::tolower);
    // MMM 以项目相对路径为首选，其他格式优先采用谱面目录。
    // 扩展名先统一为小写，避免大小写形式改变同一种文件的解析规则。
    bool preferProjectRoot = (mapExtension == ".mmm");

    // 先保存绝对谱面目录供资源解析，再把谱面自身路径改为项目相对形式。
    // 顺序不能倒置，否则后续资源解析可能把相对目录再次拼接项目根。
    meta.map_path = makeProjectRelativePath(project, absoluteMapPath);

    // 空字段保持空，不生成指向项目根的假资源引用。
    // 每个非空字段独立解析，未找到文件仍保留首选位置的项目相对表示。
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
/// @param tool 当前会话工具。
/// @return 工作区格式中的稳定名称；临时 Layout 与未知值回退 Move。
std::string editToolToWorkspaceName(EditTool tool)
{
    // 工作区保存的是稳定文本而非枚举数值，避免枚举排列调整影响旧文件。
    // Layout 不作为恢复后的默认编辑状态，统一回退到移动工具。
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
/// @param name 工作区保存的工具文本。
/// @return 支持的工具，未知名称按 Move 兼容恢复。
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
    // 兼容未认识或旧版本工具名，不让工作区加载因单个偏好失败。
    // 恢复不会猜测新增工具的枚举编号。
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
    // 未识别文本采用当前分拍，兼容旧工作区缺省值和未来新增模式。
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
    // 有效标记区分已保存的项目偏好与旧项目尚未建立的工具栏状态。
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
    // 同时保留旧布尔字段；NearCursor 仍属于可绘制拍线，不能等同于 Hidden。
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
/// @pre 调用者已决定采用该工作区，函数不检查 m_valid。
/// @note 同主音轨同步开关属于引擎状态，由调用者单独恢复。
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
    // 持久化数据可能来自其他版本，只接纳当前实现认识的分拍位。
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
    // 分拍必须为正数，缩放限制沿用工具栏可编辑区间；这里不进行 UI 更新。
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
    // 皮肤选择先由设置页写入 AppConfig；配色刷新可能仍携带旧引擎快照。
    // 保留软件级选择，避免当前已热加载新皮肤却把旧目录保存供下次启动。
    target.settings.selectedSkinDirectory =
        source.settings.selectedSkinDirectory;
    target.settings.defaultCreator     = source.settings.defaultCreator;
    target.settings.showTimelineWindow = source.settings.showTimelineWindow;
    target.settings.professionalMode   = source.settings.professionalMode;
    target.settings.showPreviewWindow  = source.settings.showPreviewWindow;
    target.settings.enableToolbarValueWheelAdjustment =
        source.settings.enableToolbarValueWheelAdjustment;
    Config::preserveGlobalToolbarDisplaySettings(target.settings,
                                                 source.settings);
    target.settings.enablePolylineEditing =
        source.settings.enablePolylineEditing;
    target.settings.enableBmsEditing = source.settings.enableBmsEditing;
    target.settings.disableVerticalObjectDrag =
        source.settings.disableVerticalObjectDrag;
    target.settings.autoUploadPgoProfiles =
        source.settings.autoUploadPgoProfiles;
    target.settings.pgoProfileUploadConsentAsked =
        source.settings.pgoProfileUploadConsentAsked;
    target.settings.m_showWelcomeOnStartup =
        source.settings.m_showWelcomeOnStartup;
    target.settings.rtcDiagnosticLogging = source.settings.rtcDiagnosticLogging;
    target.settings.autoSave             = source.settings.autoSave;
    target.settings.autoBackup           = source.settings.autoBackup;
    target.settings.collaborationViewportRenderMode =
        source.settings.collaborationViewportRenderMode;
    target.settings.shortcutConfig = source.settings.shortcutConfig;
    target.settings.bpmMeasurementToolPreferences =
        source.settings.bpmMeasurementToolPreferences;
}

/// @brief 判断逻辑指令是否会修改临时项目内容。
/// @param cmd 待检查的逻辑指令。
/// @return 指令会修改谱面或项目资源时返回 true。
/// @warning 逻辑热路径低频分支：仅在命令入队时做 variant 类型判断。
bool isTemporaryProjectMutationCommand(const LogicCommand& cmd)
{
    // 这里按命令的潜在写入能力分类，不检查当前选择是否为空或操作能否成功。
    // 新增会写入谱面、资源或保存结果的命令时，需要同时维护这份入口门禁。
    if ( std::holds_alternative<CmdCreateBeatmap>(cmd) ||
         std::holds_alternative<CmdStartDrag>(cmd) ||
         std::holds_alternative<CmdUpdateDrag>(cmd) ||
         std::holds_alternative<CmdCreateAudioSample>(cmd) ||
         std::holds_alternative<CmdUpdateAudioSampleProperties>(cmd) ||
         std::holds_alternative<CmdUpdateObjectSampleVolume>(cmd) ||
         std::holds_alternative<CmdUpdateSelectedObjectSampleVolume>(cmd) ||
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
         std::holds_alternative<CmdUpdateBpmWithKeepSpeedSv>(cmd) ||
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

    // 仅导出包不会回写临时项目；转换后保存到项目才需要写权限。
    if ( const auto* pack = std::get_if<CmdPackBeatmap>(&cmd) ) {
        return pack->saveConvertedBeatmapsToProject;
    }

    return false;
}
}  // namespace

/// @brief 返回进程内共享的编辑引擎，首次访问时建立事件订阅。
/// @return 静态实例的非拥有引用。
EditorEngine& EditorEngine::instance()
{
    static EditorEngine instance;
    return instance;
}

/// @brief 建立配置缓存和事件路由；会话由运行入口随后创建。
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
            // 先保留最新尺寸，使尚未创建或随后切换的会话也能使用该视口。
            // 事件回调只投递命令，实际相机更新在会话处理命令时完成。
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
            // 全局配置走统一缓存入口，其余指令按当前会话和项目权限路由。
            if ( std::holds_alternative<CmdUpdateEditorConfig>(e.command) ) {
                setEditorConfig(
                    std::get<CmdUpdateEditorConfig>(e.command).config);
            } else {
                pushCommand(MMM::Logic::LogicCommand(e.command));
            }
        });
}

/// @brief 析构前结束逻辑线程，避免线程继续借用引擎成员。
/// @warning 进程退出低频路径：stop 可能等待逻辑线程结束。
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

/// @brief 接收渲染侧帧率，供逻辑线程计算快照生成预算。
/// @param fps 当前有效采样帧率；非正数与非有限值被忽略。
/// @warning UI 每帧可写、逻辑每 update 可读；relaxed 原子只传递预算数值，
/// 不发布渲染资源，禁止在此等待逻辑线程或访问会话。
void EditorEngine::publishRenderFps(float fps)
{
    // 无效采样不覆盖上一次有效值，避免短暂统计异常改变预算来源。
    if ( !std::isfinite(fps) || fps <= 0.0f ) {
        return;
    }
    m_renderFps.store(fps, std::memory_order_relaxed);
}

/// @brief 将渲染需求和逻辑吞吐换算为建议的快照生成间隔。
/// @param config 本轮使用的编辑器配置快照。
/// @param secondaryCamera 辅助相机使用较低生成倍率与上限。
/// @return 正的秒数；调用者据此跳过非必要快照，函数自身不等待。
/// @warning 逻辑每 update 或每辅助相机调用；FPS 由 UI 写入、UPS 由逻辑
/// 发布，relaxed 读取只用于近似预算，禁止扩展为 ECS 扫描或阻塞同步。
double EditorEngine::adaptiveRenderSnapshotMinInterval(
    const Config::EditorConfig& config, bool secondaryCamera) const
{
    const double renderFps =
        static_cast<double>(m_renderFps.load(std::memory_order_relaxed));
    const double logicUps =
        static_cast<double>(m_logicUps.load(std::memory_order_relaxed));
    // 主画布保留更密的快照以支持交互；辅助视图独立封顶，控制重复生成成本。
    const double maxSnapshotHz = secondaryCamera
                                     ? RENDER_SNAPSHOT_SECONDARY_MAX_HZ
                                     : RENDER_SNAPSHOT_MAIN_MAX_HZ;
    const double fpsDrivenHz   = std::isfinite(renderFps) && renderFps > 1.0
                                     ? renderFps * (secondaryCamera ? 1.5 : 2.0)
                                     : RENDER_SNAPSHOT_FALLBACK_HZ;
    double       snapshotHz =
        std::clamp(fpsDrivenHz, RENDER_SNAPSHOT_MIN_HZ, maxSnapshotHz);

    // 不限 UPS 时借用有效 FPS 估算负载目标，仍保留有限的快照预算。
    double targetUps = frameLimitTargetUps(config.settings.frameLimit);
    if ( targetUps <= 1.0 && std::isfinite(renderFps) && renderFps > 1.0 ) {
        targetUps = std::clamp(renderFps * 2.0,
                               RENDER_SNAPSHOT_MIN_HZ,
                               RENDER_SNAPSHOT_MAIN_MAX_HZ);
    }

    // 先按实际逻辑吞吐回压，再守住快照频率上下界，保证倒数有定义。
    snapshotHz = applyUpsBackpressure(snapshotHz, logicUps, targetUps);
    snapshotHz = std::clamp(snapshotHz, RENDER_SNAPSHOT_MIN_HZ, maxSnapshotHz);
    return 1.0 / snapshotHz;
}

/// @brief 判断打开另一个项目之前是否存在需要关闭的编辑会话。
/// @return 仅有 Logo 占位画布时返回 false；不以当前是否绑定项目代替判断。
bool EditorEngine::needsCanvasCloseBeforeProjectOpen() const
{
    return m_sessionRegistry.hasNonLogoSession();
}

/// @brief 将已打开谱面的视图位置和工具栏偏好写入当前项目的内存设置。
/// @note 不保存谱面正文，也不直接把项目设置写入磁盘。
/// @warning 保存、切换项目等低频入口；持有会话锁遍历列表并规范化路径，
/// 可能访问文件系统，不得移入每 update 的连续状态发布路径。
void EditorEngine::captureProjectWorkspaceState()
{
    auto* project = ProjectController::instance().currentProject();
    if ( !project ) {
        return;
    }

    auto& workspace = project->m_settings.m_workspace;
    // 捕获完整替换旧快照，关闭的画布和旧活动谱面不能残留到下次恢复。
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
    const auto& sessions    = m_sessionRegistry.entriesUnsafe();
    const auto  activeIndex = m_sessionRegistry.activeIndex();
    // 所有播放时间外推共享同一次单调时钟采样，避免遍历耗时产生偏差。
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

        // 保存项目相对路径便于整体搬迁；相机 ID 保持布局映射，名称用于显示。
        ProjectWorkspaceBeatmapState beatmapState;
        beatmapState.m_filePath     = Config::pathToUtf8(relativeMapPath);
        beatmapState.m_cameraId     = entry.cameraId;
        beatmapState.m_displayName  = entry.displayName;
        beatmapState.m_playbackTime = ctx.currentTime;
        // 横移按视口宽度归一化，恢复时可适配不同窗口大小。
        // 尚无视口或尺寸无效时保留状态默认值，避免除零和持久化非有限结果。
        const auto camera = ctx.cameras.find(entry.cameraId);
        if ( camera != ctx.cameras.end() &&
             std::isfinite(camera->second.horizontalOffsetX) &&
             std::isfinite(camera->second.viewportWidth) &&
             camera->second.viewportWidth > 0.0F ) {
            beatmapState.m_canvasHorizontalOffsetRatio =
                camera->second.horizontalOffsetX / camera->second.viewportWidth;
        }
        // 只有正在播放的活动会话需要连续时间外推；其他画布保留逻辑时间。
        if ( i == activeIndex && ctx.isPlaying ) {
            beatmapState.m_playbackTime =
                resolveContinuousSessionTime(ctx, workspaceCaptureTime);
        }
        workspace.m_openBeatmaps.push_back(beatmapState);

        if ( i == activeIndex ) {
            workspace.m_activeBeatmapPath  = beatmapState.m_filePath;
            workspace.m_activePlaybackTime = beatmapState.m_playbackTime;
            // 旧项目读取路径仍使用名称字段，与完整工作区同时维护。
            project->m_settings.m_lastOpenedBeatmap = entry.displayName;
        }
    }
}

/// @brief 按项目工作区重建画布，并将活动会话切换延后到会话更新之后。
/// @param explicitBeatmapPath 显式打开的谱面路径；非空时跳过整组恢复。
/// @note 恢复跳转位置，不自动恢复播放状态；缺失文件逐项跳过。
/// @warning 项目打开低频路径：加载磁盘谱面、分配会话并短暂复制共享所有权，
/// 局部 shared_ptr 保持解锁后的会话存活，不得用于每帧同步视图。
void EditorEngine::restoreProjectWorkspace(
    const std::filesystem::path& explicitBeatmapPath)
{
    // 用户指定谱面时由打开流程处理，旧工作区不能抢占这一目标。
    if ( !explicitBeatmapPath.empty() ) {
        return;
    }

    auto* project = ProjectController::instance().currentProject();
    if ( !project ) {
        return;
    }

    std::vector<ProjectWorkspaceBeatmapState> beatmaps =
        project->m_settings.m_workspace.m_openBeatmaps;
    // 兼容仅保存最后打开名称的旧项目；匹配首个同名条目，不猜测磁盘路径。
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

    // 有历史相机身份时先清理不匹配的 Logo，避免占用恢复布局的画布位置。
    // 第一项没有保存 ID 时保留现有占位行为，兼容旧工作区。
    bool hasSavedCameraId =
        std::any_of(beatmaps.begin(), beatmaps.end(), [](const auto& state) {
            return !state.m_cameraId.empty();
        });
    const std::string firstWorkspaceCameraId = beatmaps.front().m_cameraId;
    if ( hasSavedCameraId && !firstWorkspaceCameraId.empty() ) {
        std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
        auto& sessions = m_sessionRegistry.entriesUnsafe();
        // 逆序删除保持尚未检查的索引有效，并同步移除相机渲染缓存。
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

    // 活动项用持久化路径匹配；没有匹配项时退回最后一次创建返回的索引。
    std::size_t beatmapIndex = 0;
    for ( const auto& state : beatmaps ) {
        const std::size_t currentBeatmapIndex = beatmapIndex++;
        if ( state.m_filePath.empty() ) {
            continue;
        }

        auto mapPath =
            resolveProjectPath(*project, Config::utf8ToPath(state.m_filePath));
        // 工作区加载只占打开流程的 88%～96%；按原列表计数，跳过项也占份额。
        const float beatmapProgress =
            0.88F + 0.08F * static_cast<float>(currentBeatmapIndex + 1) /
                        static_cast<float>(beatmaps.size());
        publishProjectOpenProgress(
            Event::ProjectOpenProgressStage::LoadingBeatmaps,
            beatmapProgress,
            projectOpenProgressPathDetail(mapPath));
        // 单张谱面丢失不阻断其他画布恢复；访问失败也沿用跳过路径。
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

        // 注册表访问只在锁内进行；后续设置相机与投递跳转持有独立生命周期。
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
            // 仅恢复有意义的有限偏移；零偏移继续使用新相机的初始位置。
            if ( !restoredCameraId.empty() &&
                 std::isfinite(state.m_canvasHorizontalOffsetRatio) &&
                 std::abs(state.m_canvasHorizontalOffsetRatio) > 1e-6F ) {
                auto& context           = restoredSession->getContextMutable();
                auto [camera, inserted] = context.cameras.try_emplace(
                    restoredCameraId,
                    CameraInfo{ restoredCameraId, 1.0F, 1.0F });
                (void)inserted;
                // 尺寸事件尚未到达时以单位宽度还原比例，避免无效尺寸传播。
                const float viewportWidth =
                    std::isfinite(camera->second.viewportWidth) &&
                            camera->second.viewportWidth > 0.0F
                        ? camera->second.viewportWidth
                        : 1.0F;
                camera->second.horizontalOffsetX =
                    state.m_canvasHorizontalOffsetRatio * viewportWidth;
            }
            // 跳转走会话命令入口，使时间相关缓存随正常更新流程一并处理。
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
    // 逻辑循环先处理各会话的恢复跳转，再应用活动索引，避免切换时读到旧时间。
    // 没有任何可用项时保留 -1，不制造无效的活动会话请求。
    m_pendingWorkspaceActiveIndex = restoredActiveIndex;
}

/// @brief 校验项目路径并切换到指定普通项目。
/// @param projectPath 要打开的项目目录或谱面文件路径。
/// @param creationOptions 新建项目初始设置；普通打开时为空。
/// @param origin 发起打开动作的入口，用于交互完成事件归因。
/// @warning 用户打开项目的低频路径：检查目录、保存旧项目和加载谱面可能阻塞。
void EditorEngine::openProject(
    const std::filesystem::path&                 projectPath,
    const std::optional<ProjectCreationOptions>& creationOptions,
    Event::ProjectOpenOrigin                     origin)
{
    // 文件输入以父目录检查项目可用性，原始文件路径继续交给控制器选择谱面。
    // 新建项目可能尚无目录，不套用普通打开的存在性门禁。
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
    // 按文件系统身份识别同一目录，允许路径拼写不同；检查出错时不走快捷返回。
    if ( requestedPathIsDirectory && currentProject &&
         std::filesystem::equivalent(
             actualProjectPath, currentProject->m_projectRoot, openPathError) &&
         !openPathError ) {
        XINFO("忽略当前项目目录的重复打开请求：{}",
              Config::pathToUtf8(actualProjectPath));
        Event::ProjectOpenInteractionEvent event;
        // 重复打开仍回应交互完成，让发起入口结束等待，但不重发项目加载事件。
        event.m_origin    = origin;
        event.m_completed = true;
        event.m_path      = Config::pathToUtf8(projectPath);
        Event::EventBus::instance().publish(event);
        return;
    }

    publishProjectOpenStarted(projectPath, false);
    publishProjectOpenProgress(Event::ProjectOpenProgressStage::Validating,
                               0.02F,
                               projectOpenProgressPathDetail(projectPath));
    publishProjectOpenProgress(
        Event::ProjectOpenProgressStage::ClosingCurrentProject,
        0.08F,
        projectOpenProgressPathDetail(projectPath));
    if ( !closeProject() ) {
        publishProjectOpenFailed(
            projectPath, "当前项目的元数据保存失败，已取消项目切换", false);
        return;
    }

    // 旧项目成功关闭后才建立新项目；打开失败不会在这里恢复旧项目。
    /// @brief 项目控制器打开项目后的结果。
    auto openResult =
        ProjectController::instance().openProject(projectPath, creationOptions);
    if ( !openResult.m_opened ) {
        return;
    }
    const bool beatmapOpened = finishOpenProject(openResult);
    Event::ProjectOpenInteractionEvent event;
    event.m_origin        = origin;
    event.m_completed     = true;
    event.m_path          = Config::pathToUtf8(projectPath);
    event.m_beatmapOpened = beatmapOpened;
    Event::EventBus::instance().publish(event);
}

/// @brief 打开谱面包为临时只读项目。
/// @param packagePath 需要临时阅览的谱面包路径。
/// @param origin 发起打开动作的入口。
/// @warning 包解压和旧项目关闭属于低频阻塞工作，不得放入连续播放更新。
void EditorEngine::openTemporaryProjectPackage(
    const std::filesystem::path& packagePath, Event::ProjectOpenOrigin origin)
{
    publishProjectOpenStarted(packagePath, true);
    publishProjectOpenProgress(
        Event::ProjectOpenProgressStage::ExtractingPackage,
        0.03F,
        projectOpenProgressPathDetail(packagePath));
    // 先验证并准备缓存，再关闭现有项目；无效谱面包不会打断当前工作区。
    auto prepared =
        ProjectController::instance().prepareTemporaryProjectPackage(
            packagePath);
    if ( !prepared.m_success ) {
        XERROR("Temporary package open failed: {}", prepared.m_errorMessage);
        publishProjectOpenFailed(packagePath, prepared.m_errorMessage, true);
        return;
    }

    publishProjectOpenProgress(
        Event::ProjectOpenProgressStage::ClosingCurrentProject,
        0.08F,
        projectOpenProgressPathDetail(packagePath));
    // 当前项目无法安全关闭时丢弃新缓存，避免留下未接管的目录。
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
    // 控制器未接管临时项目，仍由此入口回收解压目录。
    if ( !openResult.m_opened ) {
        std::error_code filesystemError;
        std::filesystem::remove_all(prepared.m_temporaryInfo.m_cacheProjectPath,
                                    filesystemError);
        return;
    }
    const bool beatmapOpened = finishOpenProject(openResult);
    Event::ProjectOpenInteractionEvent event;
    event.m_origin    = origin;
    event.m_completed = true;
    // 对 UI 保留用户选择的源包路径，缓存目录只属于内部项目生命周期。
    event.m_path          = Config::pathToUtf8(packagePath);
    event.m_readOnly      = true;
    event.m_beatmapOpened = beatmapOpened;
    Event::EventBus::instance().publish(event);
}

/// @brief 应用项目控制器打开项目后的逻辑副作用。
/// @param openResult 项目控制器返回的打开结果。
/// @return 显式目标谱面是否创建了会话；工作区恢复不计入此返回值。
/// @pre 控制器已成功打开新项目，调用者负责此前旧项目的关闭。
/// @warning 打开项目低频路径：登记音效、加载谱面和恢复画布可能分配或访问磁盘。
bool EditorEngine::finishOpenProject(const OpenProjectResult& openResult)
{
    // 旧工作区的延后切换请求不能应用到新项目的会话索引。
    m_pendingWorkspaceActiveIndex = -1;
    if ( auto* project = ProjectController::instance().currentProject() ) {
        setProjectAutoBackupOverride(project->m_settings.m_autoBackupOverride);
        const auto& workspace = project->m_settings.m_workspace;
        // 资源 ID 只在项目内有效，先清空旧选择，再匹配新资源表中的记录。
        m_brushAudioResourceId.clear();
        m_brushAudioTrackType = AudioTrackType::Effect;
        // 音量允许超过 1 的增益，只排除负数和非有限持久化数据。
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
        // 已保存的项目工具栏覆盖当前配置；旧项目保留全局偏好，仅重置同步开关。
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

    // 在会话恢复前登记 Effect，使新谱面的绑定能使用本项目的音效身份。
    // 空登记列表不会进入循环，进度份额的除数只在非空时使用。
    std::size_t effectIndex = 0;
    for ( const auto& registration : openResult.m_effectRegistrations ) {
        const float effectProgress =
            0.82F +
            0.04F * static_cast<float>(effectIndex + 1) /
                static_cast<float>(openResult.m_effectRegistrations.size());
        publishProjectOpenProgress(
            Event::ProjectOpenProgressStage::PreparingAudio,
            effectProgress,
            registration.m_resource.m_id);
        Audio::AudioManager::instance().registerSoundEffect(
            registration.m_resource.m_id,
            Config::pathToUtf8(registration.m_absolutePath),
            registration.m_resource.m_config);
        ++effectIndex;
    }

    XINFO("Project '{}' loaded successfully with {} beatmaps.",
          openResult.m_projectTitle,
          openResult.m_beatmapCount);

    /// @brief 指定谱面是否实际创建会话，避免解析失败误报完成。
    bool beatmapOpened = false;
    // 如果指定了谱面路径，则通过 createSession 加载它
    if ( !openResult.m_targetBeatmapPath.empty() ) {
        publishProjectOpenProgress(
            Event::ProjectOpenProgressStage::LoadingBeatmaps,
            0.92F,
            projectOpenProgressPathDetail(openResult.m_targetBeatmapPath));
        XINFO("Auto loading beatmap: {}",
              Config::pathToUtf8(openResult.m_targetBeatmapPath));
        auto loadedMap = BeatMap::loadFromFile(openResult.m_targetBeatmapPath);
        // 空谱面路径表达未成功解析，不能据项目已打开就宣称开图成功。
        if ( loadedMap.m_baseMapMetadata.map_path.empty() ) {
            XERROR("Failed to auto load beatmap {}",
                   Config::pathToUtf8(openResult.m_targetBeatmapPath));
        } else {
            auto map = std::make_shared<BeatMap>(std::move(loadedMap));
            beatmapOpened =
                createSession(map, map->m_baseMapMetadata.name) >= 0;
        }
    } else {
        restoreProjectWorkspace(openResult.m_targetBeatmapPath);
    }

    publishProjectOpenProgress(Event::ProjectOpenProgressStage::Finalizing,
                               0.98F,
                               openResult.m_projectTitle);

    /// 音频预加载和谱面会话或工作区恢复完成后，UI 才能安全读取
    /// 已就绪的项目状态。
    Event::ProjectLoadedEvent loadedEvent;
    loadedEvent.m_projectTitle = openResult.m_projectTitle;
    loadedEvent.m_projectPath =
        Config::pathToUtf8(openResult.m_actualProjectPath);
    loadedEvent.m_beatmapCount = openResult.m_beatmapCount;
    Event::EventBus::instance().publish(loadedEvent);
    return beatmapOpened;
}

/// @brief 保存当前工作区并释放项目关联的音频资源。
/// @return 无项目或关闭成功返回 true；元数据、设置保存失败时取消关闭。
/// @note 会话关闭由外围流程管理，本函数不遍历删除所有编辑画布。
/// @warning 项目切换及退出低频路径：保存文件、停止音频和回收堆页可能阻塞。
bool EditorEngine::closeProject()
{
    if ( !ProjectController::instance().currentProject() ) return true;
    // 先落盘待保存的谱面元数据，再捕获工作区；失败时保留当前项目供用户处理。
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

    // 控制器移出项目所有权后，结果暂时保留资源表用于逐项卸载。
    /// @brief 项目控制器关闭当前项目后的结果。
    auto closeResult = ProjectController::instance().closeProject();
    if ( !closeResult.m_closed || !closeResult.m_project ) {
        return false;
    }
    setProjectAutoBackupOverride(std::nullopt);

    // 先停止播放、清空调度和时间轴，再卸载 Effect，避免继续引用旧资源。
    auto& audio = Audio::AudioManager::instance();
    audio.stop();
    audio.clearAllScheduledSoundEffects();
    audio.unloadAudioTimeline();

    for ( const auto& res : closeResult.m_project->m_audioResources ) {
        if ( res.m_type == AudioTrackType::Effect ) {
            audio.unloadSoundEffect(res.m_id);
        }
    }
    // 释放项目数据后再回收无引用轨道缓存与空闲堆页，避免频繁播放时抖动。
    closeResult.m_project.reset();
    static_cast<void>(audio.releaseUnusedTrackCache());
    releaseUnusedHeapPages();

    XINFO("Project '{}' closed.", closeResult.m_projectTitle);
    return true;
}

/// @brief 从应用配置初始化逻辑状态，并向应用线程池提交长驻循环。
/// @pre 启停由运行生命周期入口串行调用，原子标记不替代并发 start 的互斥。
/// @warning 启动低频路径；运行标记由启停入口写入、逻辑循环读取，
/// release/acquire 用于跨线程生命周期可见性，配置本体仍由配置锁保护。
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

    // 线程池由应用初始化；缺失时撤回运行标记，不能留下没有执行者的运行状态。
    auto* appThreadPool = MMM::Runtime::AppThreadPool::instance().get();
    if ( !appThreadPool ) {
        m_running.store(false, std::memory_order_release);
        XERROR("AppThreadPool is not initialized before EditorEngine::start.");
        return;
    }

    // 任务借用 this，future 交给 stop 等待，保证销毁成员前循环已经退出。
    m_loopFuture = appThreadPool->enqueue([this]() { loop(); });
    XINFO("EditorEngine logic thread started.");
}

/// @brief 停止目录监视并请求逻辑循环退出，等待其不再访问引擎成员。
/// @warning 退出低频路径：监视器停止与 future::wait 可能阻塞；不得从逻辑
/// 循环自身调用而等待自己。启停入口写运行原子、逻辑循环读，exchange
/// 使用 acq_rel 取得本次停止责任并发布退出请求，不提供超时或任务强制取消。
void EditorEngine::stop()
{
    // 先切断外部目录事件来源，再退出逻辑消费端，避免关闭期间继续引入变化。
    ProjectController::instance().stopDirectoryWatcher();

    if ( m_running.exchange(false, std::memory_order_acq_rel) ) {
        if ( m_loopFuture.valid() ) {
            m_loopFuture.wait();
            // 已完成任务句柄不跨下一次启动复用；wait 只等待，不提取任务结果。
            m_loopFuture = std::future<void>{};
        }
        XINFO("EditorEngine logic thread stopped.");
    }
}

/// @brief 处理新建谱面指令并执行引擎侧保存和开图副作用。
/// @param cmd 谱面模板、目标轨道与保存选项。
/// @warning 新建命令低频路径：持有会话递归锁调用项目保存与会话创建。
void EditorEngine::handleCreateBeatmap(const CmdCreateBeatmap& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    /// @brief 项目命令服务的新建谱面处理结果。
    auto result = ProjectController::instance().createBeatmap(cmd);
    if ( !result.m_created || !result.m_beatmap ) {
        return;
    }

    // 控制器返回有效谱面后才推进项目设置保存和开图，避免产生空会话。
    saveProject();

    createSession(result.m_beatmap, result.m_displayName);
}

/// @brief 处理导入音频指令并执行音效登记和项目保存副作用。
/// @param cmd 导入来源及资源选项。
/// @warning 用户导入低频路径：可能复制文件、修改资源表并保存项目。
void EditorEngine::handleImportAudio(const CmdImportAudio& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    /// @brief 项目命令服务的导入音频处理结果。
    auto result = ProjectController::instance().importAudio(cmd);
    if ( !result.m_imported ) {
        return;
    }

    // 不需要独立音效登记的导入仍需保存项目，只跳过此可选副作用。
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
std::vector<ClipboardItem> EditorEngine::getClipboard(
    const SessionContext* targetContext) const
{
    return m_clipboard.get(targetContext);
}

/// @brief 获取编辑器级自动采样剪贴板副本。
std::vector<SampleClipboardItem> EditorEngine::getSampleClipboard(
    const SessionContext* targetContext) const
{
    return m_clipboard.getSamples(targetContext);
}

/// @brief 获取编辑器级 Timeline 剪贴板副本。
std::vector<TimelineClipboardItem> EditorEngine::getTimelineClipboard(
    const SessionContext* targetContext) const
{
    return m_clipboard.getTimelines(targetContext);
}

/// @brief 判断当前剪贴板是否为指定会话的剪切内容。
bool EditorEngine::isClipboardCutFrom(const SessionContext* context) const
{
    return m_clipboard.isCutFrom(context);
}

/// @brief 若剪贴板为其他会话剪切内容，则删除源会话原物件。
/// @param pasteContext 粘贴目标上下文，用于来源判断和目标编辑能力过滤。
/// @pre 目标粘贴已成功，才可消费源剪切内容。
/// @warning 粘贴命令低频路径：持有会话锁扫描源物件，折线子项另有全表查找，
/// 并构造撤销动作；不得在拖动预览或每 update 中重复调用。
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
        // 仅比较上下文身份；找到仍注册的会话后，才通过该会话访问源数据。
        if ( &sourceCtx != sourceContext ) {
            continue;
        }

        std::vector<BatchNoteAction::Entry> noteEntries;
        // 根物件与子物件可能同时带剪切标记，按实体去重避免重复生成删除动作。
        std::unordered_set<entt::entity> collectedNoteEntities;
        auto noteView = sourceCtx.noteRegistry.view<InteractionComponent>();
        for ( auto entity : noteView ) {
            auto& ic = sourceCtx.noteRegistry.get<InteractionComponent>(entity);
            if ( !ic.isCut ||
                 !sourceCtx.noteRegistry.all_of<NoteComponent>(entity) ) {
                continue;
            }
            if ( !collectedNoteEntities.insert(entity).second ) continue;

            // 撤销快照在删除前按值保存；目标编辑开关不允许的物件保留在源会话。
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

            // 删除折线根时连同派生子实体入批，不能留下指向已删除父实体的组件。
            // 这里沿用 Registry 扫描寻找父子关系，集合处理主遍历中的重复命中。
            if ( oldNote.m_type == ::MMM::NoteType::POLYLINE &&
                 !oldNote.m_subNotes.empty() ) {
                for ( auto subEnt :
                      sourceCtx.noteRegistry.view<NoteComponent>() ) {
                    const auto& subNC =
                        sourceCtx.noteRegistry.get<NoteComponent>(subEnt);
                    if ( subNC.m_isSubNote &&
                         subNC.m_parentPolyline == entity &&
                         collectedNoteEntities.insert(subEnt).second ) {
                        // 子实体可以没有交互组件，撤销记录以空可选值保留这一差异。
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

        // 音符与自动采样来自独立 Registry；混合剪切组合成源会话的一次撤销。
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
        // 动作执行可能删除实体，先清除视觉剪切标记，避免遍历已经失效的视图。
        // 被编辑能力过滤而留下的物件也结束本次剪切状态。
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

    // 源会话已关闭也结束剪切身份，避免后续粘贴继续删除不存在的源对象。
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
/// @param text 系统剪贴板提供的文本，交由 MMM 协议解析。
/// @return 隔离会话拒绝导入或协议不被接受时返回 false。
/// @warning 用户粘贴低频路径：取得活动会话的共享句柄以跨越注册表解锁，
/// 保证权限检查期间对象存活；文本解析和分配不能用于每帧探测剪贴板。
bool EditorEngine::importSystemClipboardText(std::string_view text)
{
    auto activeSession = getActiveSession();
    // 隔离策略在协议解析之前执行，禁止系统载荷进入当前协作剪贴板范围。
    if ( activeSession && activeSession->isCollaborationClipboardIsolated() ) {
        return false;
    }
    return m_clipboard.importSystemText(text);
}

/// @brief 清除指定来源会话持有的编辑器剪贴板内容。
/// @param context 来源身份；清理其他会话时不应误删当前来源的载荷。
void EditorEngine::clearClipboardForContext(const SessionContext* context)
{
    m_clipboard.clearForContext(context);
}

/// @brief 同步单个谱面文件到项目配置并在发生变化时保存。
/// @param mapPath 新建、另存或更新后需要同步的谱面文件路径。
/// @warning 文件变更低频路径：持有会话递归锁，可能规范化路径并保存项目，
/// 不得作为每 update 检查谱面是否变化的手段。
void EditorEngine::syncProjectWithFile(const std::filesystem::path& mapPath)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    /// @brief 项目命令服务的单文件同步结果。
    auto result = ProjectController::instance().syncProjectWithFile(mapPath);
    if ( result.m_changed ) {
        saveProject();
    }

    // 文件同步即使没有改变项目条目，也重新关联会话路径和音频指纹。
    // 另存可能先改变内存谱面路径，不能只依赖项目条目的 changed 标志。
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

/// @brief 执行项目级命令，并把其余指令路由到对应会话队列。
/// @param cmd 待转移的指令；成功投递后调用者不得依赖原载荷内容。
/// @note 名称表示统一入口，并非所有命令都异步入队；项目操作在此直接调用。
/// @warning 鼠标等连续交互可每帧进入，路由仅检查命令类型和会话列表，
/// 不得为等待焦点、视口或同步数据而休眠；文件操作只属于显式低频命令。
void EditorEngine::pushCommand(LogicCommand&& cmd)
{
    // 临时项目另存是离开只读缓存的入口，先于普通修改命令门禁处理。
    if ( std::holds_alternative<CmdSaveTemporaryProject>(cmd) ) {
        handleSaveTemporaryProject(std::get<CmdSaveTemporaryProject>(cmd));
        return;
    }

    // 拒绝事件携带源包和缓存身份，UI 可提示另存；被拒命令不进入会话。
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
        // 枚举立即供 UI 查询，命令再让每个会话清理旧工具状态并接收新工具。
        // relaxed 只发布工具选择，不表示会话已经处理完命令。
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
        // 与工作区恢复采用相同音量边界，非有限输入回退默认增益。
        m_brushAudioVolume = std::isfinite(audioResource->volume)
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

    // 缓存值供未来会话初始化，当前会话仍需处理相机更新，因此继续下发。
    // 拦截视口更新指令，缓存最新的尺寸
    if ( std::holds_alternative<CmdUpdateViewport>(cmd) ) {
        const auto& v = std::get<CmdUpdateViewport>(cmd);
        m_renderSyncRegistry.cacheViewportSize(v.cameraId,
                                               { v.width, v.height });
    }

    // 主画布悬停坐标按 cameraId 精确路由，使后台 Session 能独立生成
    // NearCursor 分拍线，同时不接收任何编辑手势命令。
    if ( std::holds_alternative<CmdSetMousePosition>(cmd) ) {
        const auto& mouse = std::get<CmdSetMousePosition>(cmd);
        // 主画布目标缺失时丢弃，不能回退到活动会话造成幽灵悬浮；辅助视图走末尾路由。
        if ( SessionUtils::isMainCanvasCameraId(mouse.cameraId) ) {
            std::lock_guard<std::recursive_mutex> lock(
                m_sessionRegistry.mutex());
            auto&         sessions = m_sessionRegistry.entriesUnsafe();
            const int32_t targetIndex =
                findSessionIndexByCameraIdUnsafe(sessions, mouse.cameraId);
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

    // 主画布滚轮按 cameraId 精确路由，确保焦点切换帧的滚动命令进入新活动
    // Session，而不是仍落到上一帧的活动画布。
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
            // 后台滚轮只允许与活动会话共享有效 Main
            // 指纹的画布，避免滚动无关谱面。
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
        // 平移依据目标相机，不套用滚轮的同主音轨门禁；每次增量立即入目标队列。
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
    // 缓存颜色供以后创建或切换的会话恢复；本条命令仍只投递给活动会话。
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
    // 无活动索引时不构造替代会话，缓存的全局画笔选择仍可用于下次创建。
    int32_t idx = m_sessionRegistry.activeIndex();
    if ( idx >= 0 && idx < static_cast<int32_t>(sessions.size()) ) {
        sessions[idx].session->pushCommand(std::move(cmd));
    }
}

/// @brief 将已初始化的全局颜色逐槽投递给指定会话。
/// @param session 正在创建、复用或切换的目标会话。
/// @pre 调用者持有会话注册表锁，保证画笔缓存读取一致。
/// @note 未初始化时保留目标会话默认颜色，不能用空缓存覆盖皮肤配置。
void EditorEngine::restoreBrushNoteColorsUnsafe(BeatmapSession& session) const
{
    if ( !m_brushNoteColorsInitialized ) return;

    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        session.pushCommand(CmdSetBrushNoteColor{ static_cast<NoteColorSlot>(i),
                                                  m_brushNoteColors[i] });
    }
}

/// @brief 将全局音频画笔选择、类型和增益作为一个命令恢复到会话。
/// @param session 接收当前画笔状态的会话。
/// @pre 调用者持有会话注册表锁；空资源 ID 也需投递以清理目标旧选择。
void EditorEngine::restoreBrushAudioResourceUnsafe(
    BeatmapSession& session) const
{
    session.pushCommand(LogicCommand(CmdSetBrushAudioResource{
        m_brushAudioResourceId,
        m_brushAudioTrackType,
        m_brushAudioVolume,
    }));
}

/// @brief 查询是否有会话撤销栈处于未保存状态。
/// @return 任一有效会话 actionStack 为脏时返回 true。
/// @note 不比较磁盘文件，也不检查尚未进入撤销栈的项目配置变更。
/// @warning 关闭提示或 UI 状态查询可重复调用；只遍历已打开会话，
/// 禁止在查询中序列化谱面或扫描完整 ECS。
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

/// @brief 判断当前活跃 Session 是否存在已选谱面物件。
/// @return 玩家物件或自动采样至少有一个被选中时返回 true。
/// @warning UI 热路径：菜单状态每帧读取；会短暂锁定 SessionRegistry，
/// 只检查常量级选择索引，不遍历 ECS，也不复制 shared_ptr 所有权。
bool EditorEngine::hasActiveChartObjectSelection() const
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    const auto& sessions = m_sessionRegistry.entriesUnsafe();
    const auto  idx      = m_sessionRegistry.activeIndex();
    if ( idx >= 0 && idx < static_cast<int32_t>(sessions.size()) &&
         sessions[idx].session ) {
        const auto& ctx = sessions[idx].session->getContext();
        return !ctx.selectedNoteEntities.empty() ||
               !ctx.selectedSampleEntities.empty();
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
                auto& ctx = entry.session->getContextMutable();
                ctx.isAudioTimelineSyncFollower = false;
                ctx.m_audioTimelineSyncSourceFingerprint.clear();
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
            entry.mainAudioSyncFingerprint.clear();
            continue;
        }

        const auto& ctx = entry.session->getContext();
        entry.audioTimelineFingerprint =
            ctx.audioTimelineDescriptor.m_fingerprint;
        entry.mainAudioSyncFingerprint =
            ctx.audioTimelineDescriptor.m_mainAudioSyncFingerprint;
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

/// @brief 刷新同主音轨候选并清理与活动源不兼容的 follower。
void EditorEngine::refreshMainAudioSyncPeerStateUnsafe()
{
    auto&         sessions    = m_sessionRegistry.entriesUnsafe();
    const int32_t activeIndex = m_sessionRegistry.activeIndex();
    const bool    hasActiveSyncSource =
        activeIndex >= 0 &&
        activeIndex < static_cast<int32_t>(sessions.size()) &&
        sessions[static_cast<size_t>(activeIndex)].session &&
        !sessions[static_cast<size_t>(activeIndex)]
             .mainAudioSyncFingerprint.empty();
    const std::string_view activeSyncFingerprint =
        hasActiveSyncSource
            ? std::string_view(sessions[static_cast<size_t>(activeIndex)]
                                   .mainAudioSyncFingerprint)
            : std::string_view{};

    for ( int32_t index = 0; index < static_cast<int32_t>(sessions.size());
          ++index ) {
        auto& entry = sessions[static_cast<size_t>(index)];
        if ( !entry.session ) continue;
        auto&      ctx = entry.session->getContextMutable();
        const bool canFollowActive =
            hasActiveSyncSource && index != activeIndex &&
            entry.mainAudioSyncFingerprint == activeSyncFingerprint;
        if ( ctx.isAudioTimelineSyncFollower && !canFollowActive ) {
            ctx.isAudioTimelineSyncFollower = false;
            ctx.m_audioTimelineSyncSourceFingerprint.clear();
        }
    }

    for ( size_t i = 0; i < sessions.size(); ++i ) {
        const auto& key = sessions[i].mainAudioSyncFingerprint;
        if ( key.empty() || sessions[i].isLogoPlaceholder ||
             !sessions[i].session ) {
            continue;
        }

        for ( size_t j = i + 1; j < sessions.size(); ++j ) {
            if ( sessions[j].isLogoPlaceholder || !sessions[j].session ) {
                continue;
            }
            if ( sessions[j].mainAudioSyncFingerprint == key ) {
                m_hasMainAudioSyncPeers.store(true, std::memory_order_relaxed);
                return;
            }
        }
    }

    m_hasMainAudioSyncPeers.store(false, std::memory_order_relaxed);
}

/// @brief 将使用同一主音轨的非活跃会话同步到当前活跃会话时间。
/// @warning 逻辑热路径/原子：每次 Session update 后可能执行；开关读取使用
/// relaxed，后续短暂持有 SessionRegistry 递归锁并遍历已打开 Session 列表。
void EditorEngine::syncSameMainAudioCanvases()
{
    syncSameMainAudioCanvasesFromIndex(m_sessionRegistry.activeIndex());
}

/// @brief 从指定源 Session 同步同主音轨的其他画布时间。
/// @warning 逻辑热路径/原子：每次 Session update 后可能执行；开关读取使用
/// relaxed，并短暂持有 SessionRegistry 递归锁以串行化 SessionContext 访问。
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
        std::lock_guard<std::recursive_mutex> sessionLock(
            m_sessionRegistry.mutex());
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
        const auto& sourceKey = sourceEntry->mainAudioSyncFingerprint;
        const auto& sourceTimelineFingerprint =
            sourceEntry->audioTimelineFingerprint;
        if ( sourceKey.empty() || sourceTimelineFingerprint.empty() ) {
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
            if ( entry.index == sourceIndex || !entry.session ) {
                continue;
            }

            auto& ctx = entry.session->getContextMutable();
            if ( entry.mainAudioSyncFingerprint != sourceKey ) {
                ctx.isAudioTimelineSyncFollower = false;
                ctx.m_audioTimelineSyncSourceFingerprint.clear();
                continue;
            }

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
            if ( ctx.isAudioTimelineSyncFollower ) {
                if ( ctx.m_audioTimelineSyncSourceFingerprint !=
                     sourceTimelineFingerprint ) {
                    ctx.m_audioTimelineSyncSourceFingerprint =
                        sourceTimelineFingerprint;
                }
            } else {
                ctx.m_audioTimelineSyncSourceFingerprint.clear();
            }
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
    /// @brief 新 Session 在处理载入指令后发布 Main 音轨同步指纹。
    const std::string requestedMainAudioSyncFingerprint;

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
                sessions[i].mainAudioSyncFingerprint =
                    requestedMainAudioSyncFingerprint;
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
                /// @brief 复用占位画布前的活动 Session，用于补交谱面切换事件。
                const int32_t previousIndex = m_sessionRegistry.activeIndex();
                if ( previousIndex >= 0 && previousIndex != i &&
                     previousIndex < static_cast<int32_t>(sessions.size()) &&
                     sessions[static_cast<size_t>(previousIndex)].session ) {
                    sessions[static_cast<size_t>(previousIndex)]
                        .session->requestAutoSave(
                            AutoSaveTrigger::BeatmapSwitch);
                }
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
    entry.mainAudioSyncFingerprint = requestedMainAudioSyncFingerprint;
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
    entry.mainAudioSyncFingerprint.clear();
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
    std::string previousTimelineFingerprint;
    std::string previousMainAudioSyncFingerprint;
    if ( previousIndex >= 0 &&
         previousIndex < static_cast<int32_t>(sessions.size()) &&
         sessions[previousIndex].session ) {
        if ( previousIndex != index ) {
            sessions[previousIndex].session->requestAutoSave(
                AutoSaveTrigger::BeatmapSwitch);
        }
        auto& previousCtx =
            sessions[previousIndex].session->getContextMutable();
        previousTimelineFingerprint =
            sessions[previousIndex].audioTimelineFingerprint;
        previousMainAudioSyncFingerprint =
            sessions[previousIndex].mainAudioSyncFingerprint;
        if ( previousCtx.isPlaying &&
             audio.getLoadedAudioTimelineFingerprint() ==
                 previousTimelineFingerprint ) {
            previousCtx.currentTime =
                resolveContinuousSessionTime(previousCtx, sessionSwitchTime);
            previousWasPlaying = true;
        }
        previousTime                            = previousCtx.currentTime;
        previousCtx.isPlaying                   = false;
        previousCtx.isAudioTimelineSyncFollower = false;
        previousCtx.m_audioTimelineSyncSourceFingerprint.clear();
        previousCtx.isActiveSession = false;
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
    ctx.m_audioTimelineSyncSourceFingerprint.clear();
    if ( ctx.isAudioTimelineDescriptorDirty ) {
        SessionUtils::rebuildAudioTimelineDescriptor(ctx, getCurrentProject());
    }
    sessions[index].audioTimelineFingerprint =
        ctx.audioTimelineDescriptor.m_fingerprint;
    sessions[index].mainAudioSyncFingerprint =
        ctx.audioTimelineDescriptor.m_mainAudioSyncFingerprint;
    ctx.isAudioTimelineFingerprintPublishPending = false;

    const auto switchDecision = SessionUtils::resolveAudioTimelineSwitch(
        previousMainAudioSyncFingerprint,
        sessions[index].mainAudioSyncFingerprint,
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
    XINFO(
        "Switched active session to #{} cameraId={} fingerprint={} "
        "mainSyncFingerprint={}",
        index,
        sessions[index].cameraId,
        sessions[index].audioTimelineFingerprint,
        sessions[index].mainAudioSyncFingerprint);

    const auto sharedViewportSizes =
        m_renderSyncRegistry.getSharedViewportSizes();
    for ( const auto& [cameraId, size] : sharedViewportSizes ) {
        activeSession->pushCommand(
            CmdUpdateViewport{ cameraId, size.x, size.y });
    }
}

/// @brief 为当前活动谱面提交一次跨线程自动保存事件。
void EditorEngine::requestAutoSaveForActiveSession(AutoSaveTrigger trigger)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    auto&         sessions    = m_sessionRegistry.entriesUnsafe();
    const int32_t activeIndex = m_sessionRegistry.activeIndex();
    if ( activeIndex < 0 ||
         activeIndex >= static_cast<int32_t>(sessions.size()) ||
         !sessions[static_cast<size_t>(activeIndex)].session ) {
        return;
    }
    sessions[static_cast<size_t>(activeIndex)].session->requestAutoSave(
        trigger);
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
    target = m_editorConfig;
    if ( m_projectAutoBackupOverride ) {
        target.settings.autoBackup = *m_projectAutoBackupOverride;
    }
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

    Config::EditorConfig sessionConfig = updatedConfig;
    {
        std::lock_guard<std::mutex> lock(m_editorConfigMutex);
        m_editorConfig = updatedConfig;
        if ( m_projectAutoBackupOverride ) {
            sessionConfig.settings.autoBackup = *m_projectAutoBackupOverride;
        }
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
                    LogicCommand(CmdUpdateEditorConfig{ sessionConfig }));
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

/// @brief 更新当前项目对软件级谱面自动备份配置的可选覆盖。
void EditorEngine::setProjectAutoBackupOverride(
    std::optional<Config::AutoBackupConfig> config)
{
    std::lock_guard<std::mutex> lock(m_editorConfigMutex);
    m_projectAutoBackupOverride = std::move(config);
    m_editorConfigRevision.fetch_add(1, std::memory_order_release);
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
/// @warning 逻辑热路径：按配置 UPS 频率执行；每个实际 Session update 会持有
/// SessionRegistry 递归锁，以与 UI 的低频 SessionContext/ECS 读取串行化；锁内
/// 普通路径禁止等待、文件系统操作、额外完整 entt 遍历、完整排序、try/catch
/// 和可避免的 shared_ptr 拷贝。Unlimited 无播放命令的视觉维护轮次必须在锁
/// 外合并，播放时钟和待处理命令不得受门控限制。自动保存、自动备份与元数据
/// 尾随保存仅允许在既有低频到期或事件分支阻塞。
void EditorEngine::loop()
{
    auto                    lastTime     = FrameLimitClock::now();
    auto                    nextDeadline = lastTime;
    double                  lastTargetDt = 0.0;
    UnlimitedIdleUpdateGate unlimitedIdleUpdateGate;
    m_lastUpsTime      = lastTime;
    m_logicUpdateCount = 0;
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
        unlimitedIdleUpdateGate.accumulateElapsedSeconds(dt);

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
                openTemporaryProjectPackage(projectAction.m_projectPathToOpen,
                                            projectAction.m_origin);
            } else {
                openProject(projectAction.m_projectPathToOpen,
                            projectAction.m_projectCreationOptions,
                            projectAction.m_origin);
            }
        }

        (void)refreshEditorConfigSnapshot(editorConfigSnapshot,
                                          editorConfigSnapshotRevision);

        // 多 Session 轮询更新
        /// @brief 当前已发布的 Session 快照读取句柄，避免为复制 SessionEntry
        /// 列表额外获取注册表锁。
        /// @warning 逻辑热路径/原子：每轮只做一次 acquire shared_ptr
        /// 读取，以保证 UI 替换快照后本轮会话生命周期仍然有效；具体
        /// SessionContext 更新仍须使用注册表锁串行化。
        const auto publishedSessionUpdateSnapshot =
            m_sessionRegistry.publishedSnapshot();
        const auto& sessionUpdateSnapshot =
            publishedSessionUpdateSnapshot->sessions;

        if ( !sessionUpdateSnapshot.empty() ) {
            const bool hasPendingSessionCommands = std::any_of(
                sessionUpdateSnapshot.begin(),
                sessionUpdateSnapshot.end(),
                [](const SessionSnapshotEntry& entry) {
                    return entry.session && entry.session->hasPendingCommands();
                });
            const bool shouldPollSessions =
                frameLimit != Config::FrameLimitPreference::Unlimited ||
                unlimitedIdleUpdateGate.shouldPoll(currentTime,
                                                   hasPendingSessionCommands);

            if ( shouldPollSessions ) {
                const double sessionDt =
                    unlimitedIdleUpdateGate.consumeElapsedSeconds();
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
                /// @brief 本轮由指令驱动发生时间变化的
                /// Session，用于同主音轨同步。
                int32_t commandSyncSourceIndex = -1;
                /// @brief 本轮结束后是否仍有播放时钟需逐 update 推进。
                bool hasUnlimitedSessionWork = false;
                for ( const auto& entry : sessionUpdateSnapshot ) {
                    std::lock_guard<std::recursive_mutex> sessionLock(
                        m_sessionRegistry.mutex());
                    const bool isActiveSession = entry.index == activeIndex;
                    const bool isVisibleSession =
                        isActiveSession || entry.isCanvasVisible;
                    const bool hadPendingCommands =
                        entry.session->hasPendingCommands();
                    const bool hasPendingMetadataAutoSave =
                        entry.session->hasPendingMetadataAutoSave();
                    const bool needsAutoSavePolling =
                        entry.session->needsAutoSavePolling(
                            editorConfigSnapshot.settings.autoSave,
                            editorConfigSnapshot.settings.autoBackup);
                    bool shouldUpdateSession = isActiveSession;
                    if ( !shouldUpdateSession ) {
                        const bool needsRealtimeUpdate =
                            entry.session->needsRealtimeUpdate();
                        if ( isVisibleSession && needsRealtimeUpdate ) {
                            shouldUpdateSession = true;
                        } else if ( hadPendingCommands ) {
                            shouldUpdateSession = true;
                        } else if ( !isVisibleSession &&
                                    !hasPendingMetadataAutoSave &&
                                    !needsAutoSavePolling ) {
                            shouldUpdateSession = false;
                        } else {
                            const auto& lastBackgroundUpdate =
                                m_backgroundSessionUpdateTimes
                                    [static_cast<size_t>(entry.index)];
                            shouldUpdateSession =
                                lastBackgroundUpdate ==
                                    FrameLimitClock::time_point{} ||
                                currentTime - lastBackgroundUpdate >=
                                    backgroundInterval;
                        }
                    }

                    if ( shouldUpdateSession ) {
                        const double previousCurrentTime =
                            entry.session->getContext().currentTime;
                        entry.session->update(
                            sessionDt, editorConfigSnapshot, isActiveSession);
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
                }
                if ( m_pendingWorkspaceActiveIndex >= 0 ) {
                    int32_t requestedActiveIndex =
                        m_pendingWorkspaceActiveIndex;
                    m_pendingWorkspaceActiveIndex = -1;
                    setActiveSessionIndex(requestedActiveIndex);
                    activeIndex = m_sessionRegistry.activeIndex();
                }

                if ( commandSyncSourceIndex >= 0 ) {
                    syncSameMainAudioCanvasesFromIndex(commandSyncSourceIndex);
                }

                bool  shouldSyncMainAudioCanvases = false;
                float cursorSmokeLifeOverride     = -1.0F;
                {
                    std::lock_guard<std::recursive_mutex> sessionLock(
                        m_sessionRegistry.mutex());
                    for ( const auto& entry : sessionUpdateSnapshot ) {
                        if ( !entry.session ) {
                            continue;
                        }
                        hasUnlimitedSessionWork =
                            hasUnlimitedSessionWork ||
                            entry.session->needsUnlimitedPolling();
                        if ( entry.index != activeIndex ) {
                            continue;
                        }

                        const auto& activeCtx = entry.session->getContext();
                        shouldSyncMainAudioCanvases =
                            activeCtx.isPlaying ||
                            activeIndex != m_lastMainAudioSyncActiveIndex ||
                            std::abs(activeCtx.currentTime -
                                     m_lastMainAudioSyncTime) >
                                MAIN_AUDIO_SYNC_TIME_EPSILON;
                        if ( shouldSyncMainAudioCanvases ) {
                            m_lastMainAudioSyncActiveIndex = activeIndex;
                            m_lastMainAudioSyncTime = activeCtx.currentTime;
                        }
                    }
                    cursorSmokeLifeOverride =
                        resolveActiveCursorSmokeLifeOverride(
                            sessionUpdateSnapshot,
                            m_sessionRegistry.activeIndex());
                }
                if ( shouldSyncMainAudioCanvases ) {
                    syncSameMainAudioCanvases();
                }
                m_cursorSmokeLifeOverride.store(cursorSmokeLifeOverride,
                                                std::memory_order_relaxed);
                unlimitedIdleUpdateGate.completePoll(FrameLimitClock::now(),
                                                     hasUnlimitedSessionWork);
            }
        } else {
            unlimitedIdleUpdateGate.requestPoll();
            unlimitedIdleUpdateGate.discardElapsedSeconds();
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

    if ( syncResult.m_scanSucceeded ) {
        Event::EventBus::instance().publish(
            Event::ProjectDirectoryRefreshedEvent{
                .m_resourcesChanged = syncResult.m_changed,
            });
    }
}

}  // namespace MMM::Logic
