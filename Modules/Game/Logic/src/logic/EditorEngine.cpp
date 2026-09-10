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

/**
 * @file EditorEngine.cpp
 * @brief 实现编辑器逻辑线程、项目生命周期、会话路由与跨画布共享状态。
 *
 * EditorEngine 是 UI/EventBus 与 BeatmapSession 之间的编排层。它不实现具体
 * 音符编辑算法，而是决定命令属于项目事务、全局广播、指定 cameraId 还是
 * 当前活动会话，并保证这些入口采用一致的只读权限和生命周期规则。
 *
 * 项目打开与关闭、谱面包解压、目录扫描及资源改名属于显式低频路径，可以
 * 执行文件系统操作；主 loop、输入路由和快照预算属于热路径，只允许轻量
 * 原子读取、稳定索引查询及会话队列投递。两类路径不得相互借用等待机制。
 *
 * SessionRegistry 的已发布快照负责跨线程保活会话对象，递归锁负责串行化
 * SessionContext 和条目元数据访问。持有 shared_ptr 的理由仅限解锁后仍需
 * 使用会话的低频流程，热路径优先借用快照中的已有引用而不复制所有权。
 *
 * AudioManager 的全局 transport 始终由活动会话控制；使用相同主音轨的后台
 * 会话只同步视觉时间、缩放和特效，不把自身标记为实际播放。完整时间线指纹
 * 决定 transport 能否复用，主音轨指纹仅决定多画布跟随资格。
 */

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
    // 开始事件不保证后续成功，只建立一次打开尝试的 UI 生命周期。
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
    // filename 对根路径等输入可能为空，此时保留完整路径避免进度详情丢失。
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
    // 阶段、比例和详情组成同一事件快照，订阅方不需要查询可变控制器状态。
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
    // 设备刷新率由 AppConfig 统一探测，本助手不缓存以响应运行期显示器变化。
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
        // 无谱面时用负值表达“不覆盖皮肤默认寿命”，而不是伪造 120 BPM。
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
        // 非正 BPM 无法换算拍长，同样回退到调用方默认视觉参数。
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
    // 没有匹配活动索引时返回哨兵，不使用任一后台会话代替。
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
    // 空字段具有明确“未配置”语义，不能规范化成当前目录点路径。
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
    // 跨卷、权限或其他相对转换失败时只保留文件名，避免持久化机器绝对路径。
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
/// @details
/// 该事件是资源更新、重命名、移动和删除的统一反馈通道。成功事件至少携带
/// 操作类型与最终资源 ID；失败事件可同时携带总体错误和多个引用阻塞来源。
/// 发布与日志相互独立，UI 不需要解析日志文本恢复结构化失败信息。
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
    // 本函数只修改实时 ECS；调用者负责把脏域同步回 BeatMap、刷新描述符并保存。
    SessionAudioReferenceRemapResult result;
    // 匹配使用移动前资源快照，旧路径才能按原谱面目录正确解析。
    // 引用种类随字段传递，避免把 Note 绑定和歌曲提示当成同一种依赖。
    const auto matchesPreviousResource = [&](const std::string& audioReference,
                                             BeatmapAudioReferenceKind kind) {
        // 匹配逻辑由资源服务统一处理稳定 ID、项目相对路径和谱面相对路径兼容。
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
        // 根 Note 和内嵌 subNotes 属于同一组件，但各自可以绑定不同音效资源。
        auto& note = noteView.get<NoteComponent>(entity);
        remapBinding(note.m_sampleBinding);
        for ( auto& subNote : note.m_subNotes ) {
            remapBinding(subNote.sampleBinding);
        }
    }

    auto sampleView = ctx.sampleRegistry.view<SampleComponent>();
    for ( auto entity : sampleView ) {
        // 自动采样引用参与组合时间线，匹配数量即使无需改写也用于描述符失效判断。
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
    // 显式重命名已经完成路径事务，此处只接受旧 ID 精确相等，不能再做路径猜测。
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
        // 每个 NoteComponent 的根绑定和内嵌子段绑定均可能独立引用旧资源。
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
        // 绝对路径不重新解释，后续统一相对化时再决定能否放入项目根表示。
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
    // 外部格式的首选是谱面目录，即使资源暂缺也保留这一预期解析位置。
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
    // song_file_hint 可能与 main_audio_path
    // 指向同一文件，仍分别规范化以保留字段语义。
    normalizeResourcePath(meta.song_file_hint);
    // 主封面和普通封面来自不同格式映射，不因最终路径相同而合并字段。
    normalizeResourcePath(meta.main_cover_path);
    normalizeResourcePath(meta.cover_path);
}

/// @brief 将编辑工具枚举转换为项目工作区中的稳定文本。
/// @param tool 当前会话工具。
/// @return 工作区格式中的稳定名称；临时 Layout 与未知值回退 Move。
/// @note 文本是项目文件兼容接口，不能直接序列化枚举底层整数。
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
/// @note 只识别可持久化的编辑工具，临时布局工具不会从工作区恢复。
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
/// @note 返回值为静态字面量，调用方写入工作区字符串时立即复制。
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
/// @note 未知文本恢复传统 Always 默认值，不让新增枚举破坏旧项目加载。
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
/// @note 当前默认分支同时为未来未知枚举提供兼容持久化值。
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
/// @note 旧项目缺失字段时采用 CurrentBeatDivisor，保持原放置行为。
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
/// @details
/// 该快照只包含可按项目恢复的创作偏好。软件级窗口开关、皮肤和快捷键继续
/// 由 AppConfig 管理，不能在切换项目时由此函数覆盖。枚举采用稳定文本，
/// 数值字段保留原始用户值并由恢复入口执行版本兼容钳制。
void captureToolbarWorkspaceState(ProjectWorkspaceState&      workspace,
                                  const Config::EditorConfig& editorConfig,
                                  bool syncSameMainAudioCanvases)
{
    // 有效标记区分已保存的项目偏好与旧项目尚未建立的工具栏状态。
    auto& toolbarState   = workspace.m_toolbarState;
    toolbarState.m_valid = true;
    // valid 必须先随整份内存快照写入；实际磁盘序列化只会在函数完成后发生。
    toolbarState.m_reverseScroll = editorConfig.settings.reverseScroll;
    toolbarState.m_scrollSnap    = editorConfig.settings.scrollSnap;
    // 放置磁吸总开关、具体模式和公共分拍掩码共同描述完整吸附策略。
    toolbarState.m_objectPlacementSnap =
        editorConfig.settings.objectPlacementSnap;
    toolbarState.m_objectPlacementSnapMode =
        objectPlacementSnapModeToWorkspaceName(
            editorConfig.settings.objectPlacementSnapMode);
    toolbarState.m_commonBeatDivisorMask =
        editorConfig.settings.commonBeatDivisorMask;
    toolbarState.m_snapFloor = editorConfig.settings.snapFloor;
    // 视觉映射和拍线模式按项目保存，让不同谱面工作流可保持独立展示习惯。
    toolbarState.m_enableLinearScrollMapping =
        editorConfig.visual.enableLinearScrollMapping;
    toolbarState.m_beatLineDisplayMode = beatLineDisplayModeToWorkspaceName(
        editorConfig.visual.beatLineDisplayMode);
    // 同时保留旧布尔字段；NearCursor 仍属于可绘制拍线，不能等同于 Hidden。
    toolbarState.m_drawBeatLines = editorConfig.visual.beatLineDisplayMode !=
                                   Config::BeatLineDisplayMode::Hidden;
    toolbarState.m_stopPlaybackOnScroll =
        editorConfig.settings.stopPlaybackOnScroll;
    // 打击特效、分拍数和缩放属于项目创作上下文，不影响其他项目默认值。
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
/// @details
/// 恢复只修改项目级字段，并对可能来自旧版本或手工编辑项目文件的掩码、分拍
/// 和缩放做边界兼容。调用完成后配置仍需通过 setEditorConfig 统一合并全局
/// 软件偏好并广播会话，不能直接把局部结果写入 AppConfig。
void applyToolbarWorkspaceState(
    Config::EditorConfig&               editorConfig,
    const ProjectWorkspaceToolbarState& toolbarState)
{
    // 布尔偏好可直接恢复，枚举与数值字段则通过兼容转换和边界钳制处理。
    editorConfig.settings.reverseScroll = toolbarState.m_reverseScroll;
    editorConfig.settings.scrollSnap    = toolbarState.m_scrollSnap;
    editorConfig.settings.objectPlacementSnap =
        toolbarState.m_objectPlacementSnap;
    // 文本模式解析失败时由转换函数给出兼容默认值。
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
/// @details
/// 项目工作区恢复和局部 UI 更新都可能携带不完整或较旧的 EditorConfig。
/// 本函数逐项覆盖不能按项目隔离的软件偏好，使 setEditorConfig 的最终结果
/// 始终以 AppConfig 为权威。调色板与近期项目由调用方在进入本函数前处理。
void preserveGlobalAppManagedSettings(Config::EditorConfig&       target,
                                      const Config::EditorConfig& source)
{
    // 皮肤选择先由设置页写入 AppConfig；配色刷新可能仍携带旧引擎快照。
    // 保留软件级选择，避免当前已热加载新皮肤却把旧目录保存供下次启动。
    target.settings.selectedSkinDirectory =
        source.settings.selectedSkinDirectory;
    // 创作者默认值和窗口可见性属于整个应用，切换项目不应改变它们。
    target.settings.defaultCreator     = source.settings.defaultCreator;
    target.settings.showTimelineWindow = source.settings.showTimelineWindow;
    target.settings.professionalMode   = source.settings.professionalMode;
    target.settings.showPreviewWindow  = source.settings.showPreviewWindow;
    target.settings.enableToolbarValueWheelAdjustment =
        source.settings.enableToolbarValueWheelAdjustment;
    // 工具栏显示组合由公共助手维护，避免新增全局开关时散落多处复制代码。
    Config::preserveGlobalToolbarDisplaySettings(target.settings,
                                                 source.settings);
    target.settings.enablePolylineEditing =
        source.settings.enablePolylineEditing;
    target.settings.enableBmsEditing = source.settings.enableBmsEditing;
    target.settings.disableVerticalObjectDrag =
        source.settings.disableVerticalObjectDrag;
    target.settings.autoUploadPgoProfiles =
        source.settings.autoUploadPgoProfiles;
    // PGO 同意状态是用户隐私选择，任何项目文件都不得覆盖。
    target.settings.pgoProfileUploadConsentAsked =
        source.settings.pgoProfileUploadConsentAsked;
    target.settings.m_showWelcomeOnStartup =
        source.settings.m_showWelcomeOnStartup;
    // 诊断日志、自动保存和协作视口同属软件级行为，项目只可通过专门覆盖项调整备份。
    target.settings.rtcDiagnosticLogging = source.settings.rtcDiagnosticLogging;
    target.settings.autoSave             = source.settings.autoSave;
    target.settings.autoBackup           = source.settings.autoBackup;
    target.settings.collaborationViewportRenderMode =
        source.settings.collaborationViewportRenderMode;
    target.settings.shortcutConfig = source.settings.shortcutConfig;
    // BPM 测量工具偏好跨项目共享，避免打开项目时重置用户输入习惯。
    target.settings.bpmMeasurementToolPreferences =
        source.settings.bpmMeasurementToolPreferences;
}

/// @brief 判断逻辑指令是否会修改临时项目内容。
/// @param cmd 待检查的逻辑指令。
/// @return 指令会修改谱面或项目资源时返回 true。
/// @warning 逻辑热路径低频分支：仅在命令入队时做 variant 类型判断。
/// @note 分类按命令能力而非本次执行结果；新增写命令必须显式加入此门禁。
bool isTemporaryProjectMutationCommand(const LogicCommand& cmd)
{
    // 这里按命令的潜在写入能力分类，不检查当前选择是否为空或操作能否成功。
    // 创建、拖动与绘制命令会新增或修改玩家物件和自动采样。
    // 撤销、重做、粘贴、剪切与删除可能重放任意历史写操作。
    // 对齐、镜像、颜色和音量命令会改变已存在物件属性。
    // 时间线事件、BPM 与谱面数据替换会改变节奏结构或整个数据域。
    // 元数据、保存、导入和资源管理命令会写入项目文件或资源目录。
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

    // 打包命令具有两种模式：纯导出只读取临时项目，因此允许执行；选择将
    // 转换后的谱面保存回项目时才具有写能力，必须进入只读阻止流程。
    if ( const auto* pack = std::get_if<CmdPackBeatmap>(&cmd) ) {
        return pack->saveConvertedBeatmapsToProject;
    }

    return false;
}
}  // namespace

/// @brief 返回进程内共享的编辑引擎，首次访问时建立事件订阅。
/// @return 静态实例的非拥有引用。
/// @note 静态对象析构时会通过析构函数停止仍在运行的逻辑任务。
EditorEngine& EditorEngine::instance()
{
    static EditorEngine instance;
    return instance;
}

/// @brief 建立配置缓存和事件路由；会话由运行入口随后创建。
/// @details
/// 构造只准备线程安全配置快照、限频原子和 EventBus 订阅，不启动逻辑任务，
/// 也不创建任何会话。这样应用可以先建立停靠布局和共享视口，再显式 start
/// 并按确定顺序创建编辑器、欢迎页及 Logo 画布。
EditorEngine::EditorEngine()
{
    // 构造时复制 AppConfig 的稳定快照，逻辑线程尚未启动，不需要依赖事件回放。
    // 修订号仍按正常发布协议递增，首次 loop 可使用同一刷新机制取得配置。
    const auto initialConfig = Config::AppConfig::instance().getEditorConfig();
    {
        std::lock_guard<std::mutex> lock(m_editorConfigMutex);
        m_editorConfig = initialConfig;
        m_editorConfigRevision.fetch_add(1, std::memory_order_release);
    }
    m_frameLimitPreference.store(initialConfig.settings.frameLimit,
                                 std::memory_order_relaxed);

    // 提前实例化项目控制器，确保其事件订阅先于后续 UI 产生打开请求建立。
    (void)ProjectController::instance();

    // 构造阶段不创建初始 Session；GameLoop 在停靠布局准备好后创建 Logo 画布，
    // 避免欢迎页与编辑器标签的启动顺序由单例首次访问时机偶然决定。

    // Resize 事件可能来自任一主画布或共享辅助视口，按 cameraId 分流处理。
    Event::EventBus::instance().subscribe<Event::CanvasResizeEvent>(
        [this](const Event::CanvasResizeEvent& e) {
            // 事件数据先转为逻辑命令值对象，订阅回调不保存 UI 事件引用。
            CmdUpdateViewport cmd{ e.canvasName,
                                   static_cast<float>(e.newSize.x),
                                   static_cast<float>(e.newSize.y) };
            // 先保留最新尺寸，使尚未创建或随后切换的会话也能使用该视口。
            // 事件回调只投递命令，实际相机更新在会话处理命令时完成。
            m_renderSyncRegistry.cacheViewportSize(cmd.cameraId,
                                                   { cmd.width, cmd.height });
            // 主画布只投递给 cameraId 所属会话，避免所有谱面重复重建投影。
            std::lock_guard<std::recursive_mutex> lock(
                m_sessionRegistry.mutex());
            auto& sessions = m_sessionRegistry.entriesUnsafe();
            for ( auto& entry : sessions ) {
                if ( entry.cameraId == e.canvasName ) {
                    entry.session->pushCommand(LogicCommand(cmd));
                    break;
                }
            }
            // Preview 与 Timeline
            // 共享当前活动谱面，因此尺寸变化另投递给活动项。
            // 若该名称也恰好命中一个条目，前一段只更新其专属相机，不代替此路由。
            if ( e.canvasName == "Preview" || e.canvasName == "Timeline" ) {
                int32_t idx = m_sessionRegistry.activeIndex();
                if ( idx >= 0 && idx < static_cast<int32_t>(sessions.size()) ) {
                    sessions[idx].session->pushCommand(LogicCommand(cmd));
                }
            }
        });

    // EventBus 只负责跨层传递；引擎统一入口决定配置广播、项目命令或会话路由。
    Event::EventBus::instance().subscribe<Event::LogicCommandEvent>(
        [this](const Event::LogicCommandEvent& e) {
            // 全局配置走统一缓存入口，其余指令按当前会话和项目权限路由。
            if ( std::holds_alternative<CmdUpdateEditorConfig>(e.command) ) {
                // 配置不能只发给活动会话，统一入口负责全局合并并广播所有画布。
                setEditorConfig(
                    std::get<CmdUpdateEditorConfig>(e.command).config);
            } else {
                // 复制事件持有的 variant 后转移给路由器，订阅回调不修改原事件。
                pushCommand(MMM::Logic::LogicCommand(e.command));
            }
        });
}

/// @brief 析构前结束逻辑线程，避免线程继续借用引擎成员。
/// @post 返回后逻辑循环 future 已完成，不再访问 SessionRegistry 或配置缓存。
/// @warning 进程退出低频路径：stop 可能等待逻辑线程结束。
EditorEngine::~EditorEngine()
{
    stop();
}

/// @brief 获取当前项目。
/// @return 未打开项目时返回 nullptr。
/// @note 返回非拥有指针，只能在项目切换不会并发发生的调用范围内使用。
/// @warning 不得把返回指针缓存到异步任务或跨越项目打开、关闭事件。
Project* EditorEngine::getCurrentProject()
{
    return ProjectController::instance().currentProject();
}

/// @brief 获取当前项目的只读指针。
/// @return 未打开项目时返回 nullptr。
/// @note 返回非拥有指针，不能跨越可能关闭或替换项目的事件回调保存。
/// @warning const 只限制调用方写入，不延长 ProjectController 中项目的生命周期。
const Project* EditorEngine::getCurrentProject() const
{
    return ProjectController::instance().currentProject();
}

/// @brief 当前是否打开了临时只读项目。
/// @return 当前项目为临时项目时返回 true。
/// @note 该查询不表示项目一定存在，普通无项目状态同样返回 false。
bool EditorEngine::isTemporaryProjectOpen() const
{
    return ProjectController::instance().isCurrentProjectTemporary();
}

/// @brief 获取当前临时项目的运行时路径信息。
/// @return 当前临时项目源包与缓存目录；非临时项目时返回默认值。
/// @note 返回按值快照，调用方不得据此绕过控制器执行缓存目录清理。
/// @warning 缓存路径只在当前临时项目生命周期内有效，另存或关闭后可能被移除。
TemporaryProjectInfo EditorEngine::currentTemporaryProjectInfo() const
{
    return ProjectController::instance().currentTemporaryProjectInfo();
}

/// @brief 接收渲染侧帧率，供逻辑线程计算快照生成预算。
/// @param fps 当前有效采样帧率；非正数与非有限值被忽略。
/// @note 该值是自适应性能提示，不改变用户设置的渲染帧率或逻辑 UPS 上限。
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
/// @details
/// 主画布按渲染 FPS 的两倍估算需求，辅助视口按一点五倍估算并采用更低上限。
/// 实测 UPS 落后目标时分段降低生成频率，为逻辑命令和播放推进保留预算。
/// Unlimited 模式以有效渲染 FPS 推导近似目标；最终频率始终限制在安全区间。
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
/// @note 该结果交给 ProjectController 决定是否请求 UI
/// 关闭标签，不执行关闭本身。
/// @warning 项目打开低频查询，只读取 SessionRegistry 的非 Logo 会话状态。
bool EditorEngine::needsCanvasCloseBeforeProjectOpen() const
{
    return m_sessionRegistry.hasNonLogoSession();
}

/// @brief 将已打开谱面的视图位置和工具栏偏好写入当前项目的内存设置。
/// @details
/// 捕获以当前 SessionRegistry 为权威来源，完整替换工作区中的打开谱面列表。
/// 每个谱面保存项目相对路径、稳定 cameraId、展示名、播放位置和横向视口比例。
/// 活动谱面另写入兼容旧项目格式的最后打开名称，并保存全局编辑工具状态。
/// 正在播放的活动会话使用同一次 steady_clock 采样外推连续时间；其他会话
/// 保留最近一次逻辑更新时间，避免后台节流导致捕获过程主动推进其状态。
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
    // active 字段也同步清零，若当前只有 Logo 或空会话就明确保存无活动谱面。
    workspace.m_openBeatmaps.clear();
    workspace.m_activeBeatmapPath.clear();
    workspace.m_activePlaybackTime = 0.0;
    workspace.m_activeEditTool =
        editToolToWorkspaceName(m_currentTool.load(std::memory_order_relaxed));
    const auto editorConfig = getEditorConfig();
    // 工具栏与打开标签使用同一捕获时刻写入项目设置，避免保存出跨版本组合。
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
        // Logo 和异常空条目只服务运行时布局，不属于可恢复的谱面工作区。
        if ( entry.isLogoPlaceholder || !entry.session ) {
            continue;
        }

        const auto& ctx = entry.session->getContext();
        if ( !ctx.currentBeatmap ) {
            // 载图命令尚未完成的会话没有稳定路径，本次保存不写入半成品条目。
            continue;
        }

        auto absoluteMapPath = resolveProjectPath(
            *project, ctx.currentBeatmap->m_baseMapMetadata.map_path);
        auto relativeMapPath =
            makeProjectRelativePath(*project, absoluteMapPath);

        // 相对路径保证项目整体搬迁后仍可恢复；cameraId 保持停靠窗口身份，
        // displayName 只负责标签展示，不能代替文件身份参与查重。
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
            // 活动项以路径而非列表位置持久化，缺失谱面被跳过后仍能正确匹配。
            workspace.m_activeBeatmapPath  = beatmapState.m_filePath;
            workspace.m_activePlaybackTime = beatmapState.m_playbackTime;
            // 旧项目读取路径仍使用名称字段，与完整工作区同时维护。
            project->m_settings.m_lastOpenedBeatmap = entry.displayName;
        }
    }
}

/// @brief 按项目工作区重建画布，并将活动会话切换延后到会话更新之后。
/// @param explicitBeatmapPath 显式打开的谱面路径；非空时跳过整组恢复。
/// @details
/// 恢复首先兼容旧版仅记录最后打开谱面名称的项目，再按持久化顺序创建会话。
/// 保存过 cameraId 的工作区会优先保持原停靠身份，并清除冲突的 Logo 占位。
/// 每张谱面独立验证存在性，单项缺失只记录警告，不阻断其余标签恢复。
/// 播放位置通过 CmdSeek 排队，相机横移按当前视口宽度从比例还原；全部新会话
/// 至少完成一次 update 后，逻辑主循环才应用 m_pendingWorkspaceActiveIndex。
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
        // 旧格式没有路径和相机状态，只能在项目清单中按显示名寻找首个候选。
        // 同名歧义保持旧行为，不进行额外目录扫描或猜测最近修改文件。
        for ( const auto& entry : project->m_beatmaps ) {
            if ( entry.m_name != project->m_settings.m_lastOpenedBeatmap ) {
                continue;
            }

            ProjectWorkspaceBeatmapState state;
            // 兼容条目只具备最小可恢复字段，其余状态使用结构默认值。
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
    // 工具先写入引擎缓存，随后创建的每个会话都收到同一初始工具命令。

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
                // 第一工作区项可原地复用同 ID
                // Logo，减少窗口销毁和重新停靠闪动。
                continue;
            }

            m_sessionRegistry.erase(i);
            // 注册表和渲染缓存必须同步清理；仅删条目会留下同名旧快照。
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
            // 空路径仍计入原列表进度位置，但无法创建会话，直接跳过。
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
            // exists
            // 的权限错误与文件缺失统一跳过，不能让单项工作区损坏阻断项目。
            XWARN("Workspace restore skipped missing beatmap: {}",
                  Config::pathToUtf8(mapPath));
            continue;
        }

        auto map = std::make_shared<BeatMap>(BeatMap::loadFromFile(mapPath));
        // 已保存显示名优先，空名称才使用本次解析后的谱面元数据名称。
        std::string displayName = state.m_displayName.empty()
                                      ? map->m_baseMapMetadata.name
                                      : state.m_displayName;
        int32_t     index       = createSession(map,
                                      displayName,
                                      false,
                                      state.m_cameraId,
                                      !state.m_cameraId.empty());
        fallbackActiveIndex     = index;
        // fallback 始终指向最后成功创建项，活动路径丢失时仍给用户可用画布。

        // 注册表访问只在锁内进行；后续设置相机与投递跳转持有独立生命周期。
        std::shared_ptr<BeatmapSession> restoredSession;
        std::string                     restoredCameraId;
        {
            // createSession 返回后重新在锁内读取实际条目，因为重复打开可能返回
            // 已有会话，preferredCameraId 也可能因冲突改由注册表分配新值。
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
                auto& context = restoredSession->getContextMutable();
                // 相机尚未由 Resize 建立时创建单位视口占位，保存比例仍可还原。
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
            // 路径比较使用工作区持久化文本，避免本机规范化差异影响活动项选择。
            restoredActiveIndex = index;
        }
    }

    if ( restoredActiveIndex < 0 ) {
        // 指定活动项缺失或加载失败时选择最后一个可用会话，不保留无效索引。
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
/// @details
/// 文件输入以父目录作为项目根校验，但原文件路径仍交给控制器选择目标谱面。
/// 当前项目目录的重复打开按文件系统身份判断并作为幂等完成返回，不关闭会话。
/// 真正切换严格按照发布开始进度、关闭旧项目、控制器打开、完成逻辑副作用、
/// 发布交互结果的顺序执行。旧项目保存失败时新项目绝不会继续接管运行状态。
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
        // 直接拖入谱面文件时项目根是父目录，但 targetBeatmapPath 仍保留原文件。
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
        // 普通打开必须指向现有目录；新建入口允许控制器随后创建目标目录。
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

    // 开始事件早于任何破坏性切换，UI 可建立进度界面并禁止重复提交。
    publishProjectOpenStarted(projectPath, false);
    publishProjectOpenProgress(Event::ProjectOpenProgressStage::Validating,
                               0.02F,
                               projectOpenProgressPathDetail(projectPath));
    publishProjectOpenProgress(
        Event::ProjectOpenProgressStage::ClosingCurrentProject,
        0.08F,
        projectOpenProgressPathDetail(projectPath));
    if ( !closeProject() ) {
        // closeProject 已保留旧项目所有权；这里只发布新请求失败，不清空会话。
        publishProjectOpenFailed(
            projectPath, "当前项目的元数据保存失败，已取消项目切换", false);
        return;
    }

    // 旧项目成功关闭后才建立新项目；打开失败不会在这里恢复旧项目。
    /// @brief 项目控制器打开项目后的结果。
    auto openResult =
        ProjectController::instance().openProject(projectPath, creationOptions);
    if ( !openResult.m_opened ) {
        // 控制器负责发布具体打开失败信息；旧项目已关闭，此处不伪造完成事件。
        return;
    }
    const bool beatmapOpened = finishOpenProject(openResult);
    // 交互完成与 ProjectLoaded 分开：前者供演练和输入入口确认本次操作归因。
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
/// @details
/// 谱面包先在旧项目仍完整可用时验证和解压，准备失败不会中断当前工作区。
/// 缓存准备成功后才关闭旧项目；关闭或控制器接管失败时由本入口回收缓存目录。
/// 打开完成事件保留用户选择的源包路径，内部缓存路径只随临时项目生命周期
/// 存在。编辑命令由 pushCommand 的只读门禁拒绝，另存命令可迁移到正式目录。
/// @warning 包解压和旧项目关闭属于低频阻塞工作，不得放入连续播放更新。
void EditorEngine::openTemporaryProjectPackage(
    const std::filesystem::path& packagePath, Event::ProjectOpenOrigin origin)
{
    // 临时包入口先发布解压阶段，UI 可与普通项目验证阶段显示不同说明。
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
        // prepare 失败时控制器未接管缓存，错误信息直接关联用户选择的源包。
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
        // 新缓存尚未进入控制器生命周期，由准备入口的调用者负责显式回收。
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
        // 接管失败同样回收缓存；成功后则由 TemporaryProjectInfo
        // 随关闭流程管理。
        std::error_code filesystemError;
        std::filesystem::remove_all(prepared.m_temporaryInfo.m_cacheProjectPath,
                                    filesystemError);
        return;
    }
    const bool beatmapOpened = finishOpenProject(openResult);
    // 临时项目已经完整建立后再报告只读交互完成，演练可据此自动标记步骤。
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
/// @details
/// 本函数恢复项目级自动备份、音频画笔、编辑工具和工具栏偏好，并登记全部
/// Effect 资源。显式谱面路径只创建目标会话；没有显式目标时恢复整个工作区。
/// ProjectLoadedEvent 在资源登记和会话恢复请求全部建立后发布，接收方可读取
/// 完整项目模型，但排队的 Session 命令仍由逻辑循环后续 update 完成处理。
/// @pre 控制器已成功打开新项目，调用者负责此前旧项目的关闭。
/// @warning 打开项目低频路径：登记音效、加载谱面和恢复画布可能分配或访问磁盘。
bool EditorEngine::finishOpenProject(const OpenProjectResult& openResult)
{
    // 旧工作区的延后切换请求不能应用到新项目的会话索引；先清除再创建任何
    // 新会话，防止逻辑循环稍后把同一个整数解释成新注册表条目。
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
            // 保存的 ID 必须仍存在于当前资源表；删除或损坏引用按无选择恢复。
            const auto resourceIterator = std::find_if(
                project->m_audioResources.begin(),
                project->m_audioResources.end(),
                [&](const AudioResource& resource) {
                    return resource.m_id ==
                           workspace.m_projectAudioToolSelectedResourceId;
                });
            if ( resourceIterator != project->m_audioResources.end() ) {
                // 类型从资源表权威值取得，避免持久化冗余字段在重命名后过期。
                m_brushAudioResourceId = resourceIterator->m_id;
                m_brushAudioTrackType  = resourceIterator->m_type;
            }
        }
        m_currentTool.store(workspaceNameToEditTool(workspace.m_activeEditTool),
                            std::memory_order_relaxed);
        // 已保存的项目工具栏覆盖当前配置；旧项目保留全局偏好，仅重置同步开关。
        if ( workspace.m_toolbarState.m_valid ) {
            // 旧项目没有 valid 工具栏快照时保持全局配置，并恢复默认同步开关。
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
        // Effect 分配固定进度区间，每项按原登记列表顺序推进，空列表不除零。
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

    // 返回值只表示显式目标谱面是否成功开图，不能以项目本身加载成功代替。
    bool beatmapOpened = false;
    // 显式目标由拖入谱面等入口提供，优先于项目旧工作区的打开列表。
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
            // shared_ptr 覆盖命令队列异步载图期间的谱面生命周期。
            auto map = std::make_shared<BeatMap>(std::move(loadedMap));
            beatmapOpened =
                createSession(map, map->m_baseMapMetadata.name) >= 0;
        }
    } else {
        // 没有显式目标才恢复整组标签，避免用户请求被旧活动谱面抢占。
        restoreProjectWorkspace(openResult.m_targetBeatmapPath);
    }

    publishProjectOpenProgress(Event::ProjectOpenProgressStage::Finalizing,
                               0.98F,
                               openResult.m_projectTitle);

    // 音频登记和谱面会话或工作区恢复请求完成后，UI 才能安全读取已就绪模型。
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
/// @details
/// 关闭采用可取消的前置保存阶段：先刷新待处理元数据，再捕获工作区并保存
/// 项目设置。只有这些操作全部成功，控制器才移出项目所有权。随后停止全局
/// transport、清空调度、卸载项目 Effect，最后释放项目对象和无引用音轨缓存。
/// 这种顺序保证失败时编辑状态仍保持打开，成功时音频线程不再引用项目资源。
/// @note 会话关闭由外围流程管理，本函数不遍历删除所有编辑画布。
/// @post 成功时 ProjectController 不再持有项目，AudioManager 不再播放其时间线。
/// @warning 项目切换及退出低频路径：保存文件、停止音频和回收堆页可能阻塞。
bool EditorEngine::closeProject()
{
    // 无项目关闭是幂等成功，调用者可统一执行“先关闭再打开”流程。
    if ( !ProjectController::instance().currentProject() ) return true;
    // 先落盘待保存的谱面元数据，再捕获工作区；失败时保留当前项目供用户处理。
    if ( !flushPendingMetadataAutoSaves() ) {
        // 元数据保存失败时不捕获或覆盖工作区，当前会话保持原状供用户修复。
        XERROR(
            "EditorEngine: pending metadata save failed; project close "
            "cancelled");
        return false;
    }
    captureProjectWorkspaceState();
    if ( !ProjectController::instance().saveProject() ) {
        // 项目配置保存失败同样取消所有资源卸载，避免磁盘状态未知时丢失会话。
        XERROR(
            "EditorEngine: project configuration save failed; project "
            "close cancelled");
        return false;
    }

    // 控制器移出项目所有权后，结果暂时保留资源表用于逐项卸载。
    /// @brief 项目控制器关闭当前项目后的结果。
    auto closeResult = ProjectController::instance().closeProject();
    if ( !closeResult.m_closed || !closeResult.m_project ) {
        // 控制器没有返回已移出的项目所有权时，不能安全枚举和卸载其资源。
        return false;
    }
    setProjectAutoBackupOverride(std::nullopt);

    // 先停止播放、清空调度和时间轴，再卸载 Effect，避免继续引用旧资源。
    auto& audio = Audio::AudioManager::instance();
    // 停止顺序先于 unload，音频线程不会在资源表释放过程中继续消费旧描述符。
    audio.stop();
    audio.clearAllScheduledSoundEffects();
    audio.unloadAudioTimeline();

    for ( const auto& res : closeResult.m_project->m_audioResources ) {
        if ( res.m_type == AudioTrackType::Effect ) {
            // Main 与 Sample 已随组合时间线卸载，独立 Effect 仍需按 ID
            // 逐项移除。
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
/// @details
/// 每次有效启动都重新读取 AppConfig，发布运行标记后向全局线程池提交 loop。
/// 若线程池尚未初始化则撤回标记，不保留一个永远不会执行的 future。重复启动
/// 是幂等操作，不会创建第二条逻辑循环。
/// @pre 启停由运行生命周期入口串行调用，原子标记不替代并发 start 的互斥。
/// @warning 启动低频路径；运行标记由启停入口写入、逻辑循环读取，
/// release/acquire 用于跨线程生命周期可见性，配置本体仍由配置锁保护。
void EditorEngine::start()
{
    // 重复 start 不提交第二个长驻任务，running 是生命周期状态而非引用计数。
    if ( m_running.load(std::memory_order_acquire) ) {
        return;
    }

    // 启动时重新读取 AppConfig，覆盖构造后设置页或启动流程可能产生的更新。
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
    // 不在此创建独立 std::thread，统一沿用应用线程池的异常和退出管理策略。
    m_loopFuture = appThreadPool->enqueue([this]() { loop(); });
    XINFO("EditorEngine logic thread started.");
}

/// @brief 停止目录监视并请求逻辑循环退出，等待其不再访问引擎成员。
/// @details
/// 停止顺序先撤销目录事件来源，再由 running 原子请求 loop 退出，最后等待
/// 线程池任务完成并清空 future。只有首次把运行状态从 true 改为 false 的调用
/// 承担等待责任，析构和显式关闭可以安全重复调用。
/// @warning 退出低频路径：监视器停止与 future::wait 可能阻塞；不得从逻辑
/// 循环自身调用而等待自己。启停入口写运行原子、逻辑循环读，exchange
/// 使用 acq_rel 取得本次停止责任并发布退出请求，不提供超时或任务强制取消。
void EditorEngine::stop()
{
    // 先切断外部目录事件来源，再退出逻辑消费端，避免关闭期间继续引入变化。
    ProjectController::instance().stopDirectoryWatcher();

    if ( m_running.exchange(false, std::memory_order_acq_rel) ) {
        // 只有成功把 true 改为 false 的调用者承担等待责任，重复 stop 保持幂等。
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
/// @details
/// ProjectController 负责创建文件和追加项目谱面条目；引擎只在控制器返回有效
/// BeatMap 后保存项目并创建编辑会话。控制器失败时不产生空标签，也不修改
/// 当前活动会话。新会话沿用 createSession 的查重、Logo 复用和初始化顺序。
/// @warning 新建命令低频路径：持有会话递归锁调用项目保存与会话创建。
void EditorEngine::handleCreateBeatmap(const CmdCreateBeatmap& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    /// @brief 项目命令服务的新建谱面处理结果。
    auto result = ProjectController::instance().createBeatmap(cmd);
    if ( !result.m_created || !result.m_beatmap ) {
        // created 与对象指针同时成立才算完整结果，防止部分失败进入保存流程。
        return;
    }

    // 控制器返回有效谱面后才推进项目设置保存和开图，避免产生空会话。
    // 先把新增谱面条目落入项目配置，再建立可由 UI 操作的新会话。
    saveProject();

    // createSession 接管 shared_ptr 生命周期，displayName
    // 使用控制器规范化结果。
    createSession(result.m_beatmap, result.m_displayName);
}

/// @brief 处理导入音频指令并执行音效登记和项目保存副作用。
/// @param cmd 导入来源及资源选项。
/// @details
/// 控制器完成文件复制、资源 ID 分配和项目资源表更新，并可返回需要登记的
/// Effect 描述。Main 或 Sample 导入没有独立音效登记，但仍必须保存项目清单。
/// 导入失败时控制器负责错误反馈，本函数不保存半完成项目状态。
/// @warning 用户导入低频路径：可能复制文件、修改资源表并保存项目。
void EditorEngine::handleImportAudio(const CmdImportAudio& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    /// @brief 项目命令服务的导入音频处理结果。
    auto result = ProjectController::instance().importAudio(cmd);
    if ( !result.m_imported ) {
        // 未导入不触碰 AudioManager，避免按失败结果登记不存在的目标文件。
        return;
    }

    // 不需要独立音效登记的导入仍需保存项目，只跳过此可选副作用。
    if ( result.m_effectRegistration ) {
        // 仅 Effect 进入按键音效池；绝对路径由控制器在复制成功后给出。
        Audio::AudioManager::instance().registerSoundEffect(
            result.m_effectRegistration->m_resource.m_id,
            Config::pathToUtf8(result.m_effectRegistration->m_absolutePath),
            result.m_effectRegistration->m_resource.m_config);
    }

    // 运行时登记完成后再保存资源表，项目重开时可按相同 ID 恢复音效。
    saveProject();
}

/// @brief 重新登记当前项目中按需加载的 Effect 音频资源。
/// @details
/// 皮肤切换等流程可能重置 AudioManager 的音效池，但项目模型和会话仍有效。
/// 本函数按项目资源表恢复 Effect 描述，保持 Main 与 Sample 由组合时间线管理。
/// 仅登记路径和配置，不主动解码，从而避免资源重载入口因大量音效产生长停顿。
/// @warning 低频资源重载路径：皮肤热切换清空音效池后调用；只访问项目
/// 资源表并更新内存描述，不执行音频解码。
void EditorEngine::registerCurrentProjectEffectSoundEffects()
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    const auto* currentProject = ProjectController::instance().currentProject();
    if ( !currentProject ) {
        // 没有项目时保持音效池现状，调用方的全局皮肤音效由其他服务恢复。
        return;
    }

    for ( const auto& res : currentProject->m_audioResources ) {
        if ( res.m_type != AudioTrackType::Effect ) {
            // Main 和 Sample 不注册为按键音效，防止重复解码与错误触发。
            continue;
        }

        const auto absolutePath =
            currentProject->m_projectRoot / Config::utf8ToPath(res.m_path);
        // 项目持久化相对路径在登记边界解析为绝对 UTF-8 路径。
        Audio::AudioManager::instance().registerSoundEffect(
            res.m_id, Config::pathToUtf8(absolutePath), res.m_config);
    }
}

/// @brief 更新编辑器级剪贴板。
/// @param items 玩家物件剪贴板载荷。
/// @param sourceContext 来源会话身份，用于跨会话剪切消费。
/// @param isCut 是否保留来源删除语义。
/// @note items 按值转移到 ClipboardManager，调用后来源容器内容不再保证保留。
/// @warning 用户复制或剪切低频入口，不应在每帧选择状态查询中重复构造载荷。
void EditorEngine::setClipboard(std::vector<ClipboardItem> items,
                                const SessionContext* sourceContext, bool isCut)
{
    // ClipboardManager 负责生成系统文本载荷和维护跨会话来源身份。
    m_clipboard.set(std::move(items), sourceContext, isCut);
}

/// @brief 更新编辑器级混合谱面物件剪贴板。
/// @param notes 玩家物件载荷。
/// @param samples 自动采样载荷。
/// @param sourceContext 来源会话身份。
/// @param isCut 是否为等待粘贴成功后删除来源的剪切操作。
/// @note 两类载荷共享同一个来源与剪切身份，粘贴成功后作为一次操作消费。
/// @warning 用户复制或剪切低频入口，两个容器均按值转移给管理器。
void EditorEngine::setChartObjectClipboard(
    std::vector<ClipboardItem> notes, std::vector<SampleClipboardItem> samples,
    const SessionContext* sourceContext, bool isCut)
{
    // 混合载荷一次提交，避免 Note 和 Sample 分别更新时暴露半份剪贴板。
    m_clipboard.setChartObjects(
        std::move(notes), std::move(samples), sourceContext, isCut);
}

/// @brief 更新编辑器级 Timeline 剪贴板。
/// @param items 时间线事件载荷。
/// @param sourceContext 来源会话身份。
/// @param isCut 是否保留来源剪切身份。
/// @note Timeline 载荷与谱面物件载荷互斥，由 ClipboardManager 维护当前种类。
/// @warning 用户复制或剪切低频入口，解析和序列化不应由 UI 每帧触发。
void EditorEngine::setTimelineClipboard(
    std::vector<TimelineClipboardItem> items,
    const SessionContext* sourceContext, bool isCut)
{
    // 时间线载荷覆盖此前剪贴板种类，来源身份随同一次调用更新。
    m_clipboard.setTimelines(std::move(items), sourceContext, isCut);
}

/// @brief 获取编辑器级剪贴板副本。
/// @param targetContext 目标会话，用于应用剪贴板隔离策略。
/// @return 可粘贴到目标会话的玩家物件副本。
/// @note 返回副本避免粘贴命令持有管理器内部容器引用。
std::vector<ClipboardItem> EditorEngine::getClipboard(
    const SessionContext* targetContext) const
{
    return m_clipboard.get(targetContext);
}

/// @brief 获取编辑器级自动采样剪贴板副本。
/// @param targetContext 目标会话，用于应用剪贴板隔离策略。
/// @return 可粘贴到目标会话的自动采样副本。
/// @note 隔离会话不接受其他会话来源，具体过滤由 ClipboardManager 统一执行。
std::vector<SampleClipboardItem> EditorEngine::getSampleClipboard(
    const SessionContext* targetContext) const
{
    return m_clipboard.getSamples(targetContext);
}

/// @brief 获取编辑器级 Timeline 剪贴板副本。
/// @param targetContext 目标会话，用于应用剪贴板隔离策略。
/// @return 可粘贴到目标会话的时间线事件副本。
/// @note 返回载荷保持原相对时间信息，目标会话在实际粘贴时决定定位基准。
std::vector<TimelineClipboardItem> EditorEngine::getTimelineClipboard(
    const SessionContext* targetContext) const
{
    return m_clipboard.getTimelines(targetContext);
}

/// @brief 判断当前剪贴板是否为指定会话的剪切内容。
/// @param context 待比较的会话身份。
/// @return 当前载荷来自该会话且仍保留剪切语义时返回 true。
/// @note 仅比较来源身份和消费状态，不验证源实体当前是否仍存在。
bool EditorEngine::isClipboardCutFrom(const SessionContext* context) const
{
    return m_clipboard.isCutFrom(context);
}

/// @brief 若剪贴板为其他会话剪切内容，则删除源会话原物件。
/// @param pasteContext 粘贴目标上下文，用于来源判断和目标编辑能力过滤。
/// @details
/// 跨会话剪切采用“先复制、粘贴成功后删除来源”的两阶段语义。本函数只在
/// 目标粘贴已经提交后执行第二阶段，通过保存的 SessionContext 身份查找仍然
/// 存活的源会话。玩家物件和自动采样分别构造批量撤销动作，混合载荷组合成
/// 单次 CompositeEditorAction，使源会话可以一次撤销恢复全部被删除对象。
/// Polyline 根物件删除时还需收集派生子实体；编辑能力过滤只保留不允许移动
/// 到目标格式的来源物件。无论源会话是否仍存在，剪切身份最终都只消费一次。
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
            // 空注册表槽没有可比较上下文，继续寻找仍存活的来源会话。
            continue;
        }

        auto& sourceCtx = entry.session->getContextMutable();
        // 仅比较上下文身份；找到仍注册的会话后，才通过该会话访问源数据。
        if ( &sourceCtx != sourceContext ) {
            continue;
        }

        // 玩家物件动作记录删除前完整组件和值选择状态，供撤销原位恢复。
        std::vector<BatchNoteAction::Entry> noteEntries;
        // 根物件与子物件可能同时带剪切标记，按实体去重避免重复生成删除动作。
        std::unordered_set<entt::entity> collectedNoteEntities;
        auto noteView = sourceCtx.noteRegistry.view<InteractionComponent>();
        for ( auto entity : noteView ) {
            auto& ic = sourceCtx.noteRegistry.get<InteractionComponent>(entity);
            if ( !ic.isCut ||
                 !sourceCtx.noteRegistry.all_of<NoteComponent>(entity) ) {
                // 只处理本次剪切标记且具有实体数据的根或子音符。
                continue;
            }
            if ( !collectedNoteEntities.insert(entity).second ) continue;

            // 撤销快照在删除前按值保存；目标编辑开关不允许的物件保留在源会话。
            auto oldNote = sourceCtx.noteRegistry.get<NoteComponent>(entity);
            if ( pasteContext &&
                 !SessionUtils::isNoteEditable(
                     oldNote, pasteContext->lastConfig.settings) ) {
                // 目标禁用的物件不会被粘贴成功，因此也不能从来源会话删除。
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
        // 自动采样位于独立 Registry，只处理同时存在交互和采样组件的实体。
        auto sampleView = sourceCtx.sampleRegistry
                              .view<InteractionComponent, SampleComponent>();
        for ( auto entity : sampleView ) {
            const auto& interaction =
                sampleView.get<InteractionComponent>(entity);
            if ( !interaction.isCut ) continue;
            // Sample 动作按值保存原组件和选择状态，不与 Note 实体 ID 混用。
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
            // 批量动作内部维持父子删除顺序，调用方只提供完整条目集合。
            actions.push_back(std::make_unique<BatchNoteAction>(
                std::move(noteEntries), "Cut Across Canvas"));
        }
        if ( !sampleEntries.empty() ) {
            // 自动采样单独成批，纯 Sample 剪切不需要额外 Composite 包装。
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
                // 单一数据域直接压入原动作，避免没有语义价值的组合层。
                action = std::move(actions.front());
            } else {
                // 两个 Registry 的删除合并为一次用户操作，撤销栈保持原子体验。
                action = std::make_unique<CompositeEditorAction>(
                    std::move(actions), "跨画布剪切谱面物件");
            }
            sourceCtx.actionStack.pushAndExecute(std::move(action), sourceCtx);
        }
        // 动作执行结束后才消费剪切标记，失败前不会提前丢失来源语义。
        markCutClipboardConsumed();
        return;
    }

    // 源会话已关闭也结束剪切身份，避免后续粘贴继续删除不存在的源对象。
    markCutClipboardConsumed();
}

/// @brief 将当前剪切剪贴板标记为已消费。
/// @note 只清除待删除来源语义，已复制载荷仍可供后续普通粘贴使用。
void EditorEngine::markCutClipboardConsumed()
{
    // 具体载荷继续保留，使首次移动后仍可再次执行普通复制式粘贴。
    m_clipboard.markCutConsumed();
}

/// @brief 消费需要由 UI 线程发布到系统剪贴板的文本载荷。
/// @return 有待发布文本时返回并清除一次性载荷，否则返回空。
std::optional<std::string> EditorEngine::consumePendingSystemClipboardText()
{
    // 管理器以移动方式返回一次性文本，重复消费不会再次写入系统剪贴板。
    return m_clipboard.consumePendingSystemText();
}

/// @brief 从系统剪贴板文本导入 MMM 剪贴板载荷。
/// @param text 系统剪贴板提供的文本，交由 MMM 协议解析。
/// @return 隔离会话拒绝导入或协议不被接受时返回 false。
/// @details
/// 当前活动会话启用协作剪贴板隔离时，系统文本在解析前即被拒绝。普通会话则
/// 委托 ClipboardManager 验证协议版本和载荷种类，并以一次完整操作替换内部
/// 剪贴板。该入口只响应显式粘贴，不主动轮询系统剪贴板。
/// @warning 用户粘贴低频路径：取得活动会话的共享句柄以跨越注册表解锁，
/// 保证权限检查期间对象存活；文本解析和分配不能用于每帧探测剪贴板。
bool EditorEngine::importSystemClipboardText(std::string_view text)
{
    auto activeSession = getActiveSession();
    // 隔离策略在协议解析之前执行，禁止系统载荷进入当前协作剪贴板范围。
    if ( activeSession && activeSession->isCollaborationClipboardIsolated() ) {
        // 外部文本不会短暂写入共享管理器后再清理，隔离边界保持不可观察。
        return false;
    }
    return m_clipboard.importSystemText(text);
}

/// @brief 清除指定来源会话持有的编辑器剪贴板内容。
/// @param context 来源身份；清理其他会话时不应误删当前来源的载荷。
/// @note 会话关闭时调用，防止剪贴板保留悬空 SessionContext 身份。
/// @warning 只清理由 context 创建的载荷，不影响其他仍打开会话的剪贴板。
/// @post 若当前载荷来自 context，则其系统文本和跨会话剪切身份一并失效。
void EditorEngine::clearClipboardForContext(const SessionContext* context)
{
    // 管理器按来源身份条件清除，不影响其他会话最近复制的有效载荷。
    m_clipboard.clearForContext(context);
}

/// @brief 同步单个谱面文件到项目配置并在发生变化时保存。
/// @param mapPath 新建、另存或更新后需要同步的谱面文件路径。
/// @details
/// 控制器负责新增或更新项目清单，本函数随后重算所有打开会话的稳定路径键，
/// 因为另存可能改变内存 BeatMap 路径而项目条目本身未发生结构变化。最后刷新
/// 音频时间线指纹，使相对资源解析和同主音轨配对使用新的谱面目录身份。
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
/// @details
/// 指令分为临时项目另存、受只读门禁约束的写操作、项目资源事务、全局广播、
/// cameraId 定向输入和活动会话普通命令。分类顺序不可随意交换，因为同一命令
/// 可能既修改项目又携带会话数据；必须由最外层职责完整处理其所有副作用。
/// 连续鼠标命令只执行常量级类型判断和索引查找，最终通过 Session 队列异步
/// 应用。项目资源命令是显式低频操作，可在本入口同步执行文件系统事务。
/// @note 名称表示统一入口，并非所有命令都异步入队；项目操作在此直接调用。
/// @warning 鼠标等连续交互可每帧进入，路由仅检查命令类型和会话列表，
/// 不得为等待焦点、视口或同步数据而休眠；文件操作只属于显式低频命令。
void EditorEngine::pushCommand(LogicCommand&& cmd)
{
    // 路由顺序本身属于权限契约：临时项目另存必须先于写命令门禁，项目级
    // 命令必须先于活动会话查找，全局状态命令则需要广播全部会话。
    if ( std::holds_alternative<CmdSaveTemporaryProject>(cmd) ) {
        handleSaveTemporaryProject(std::get<CmdSaveTemporaryProject>(cmd));
        return;
    }

    // 临时项目以只读方式查看；具有潜在写能力的命令即使最终可能无操作，
    // 也必须在进入任何控制器或会话队列前拒绝。
    if ( ProjectController::instance().isCurrentProjectTemporary() &&
         isTemporaryProjectMutationCommand(cmd) ) {
        const auto info =
            ProjectController::instance().currentTemporaryProjectInfo();
        Event::TemporaryProjectEditBlockedEvent event;
        // 同时提供源包与缓存路径，UI 能解释只读来源并引导保存到正式目录。
        event.m_sourcePackagePath =
            Config::pathToUtf8(info.m_sourcePackagePath);
        event.m_cacheProjectPath = Config::pathToUtf8(info.m_cacheProjectPath);
        Event::EventBus::instance().publish(event);
        return;
    }

    // 新建谱面同时修改项目清单和会话集合，不能只投递到某个活动会话。
    if ( std::holds_alternative<CmdCreateBeatmap>(cmd) ) {
        handleCreateBeatmap(std::get<CmdCreateBeatmap>(cmd));
        return;
    }

    // 音频资源命令按各自事务入口处理；这些入口负责引用校验、运行时登记、
    // 会话派生状态失效和项目保存，不能退化为普通 Session 命令。
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

    // 导入会复制外部文件并追加项目资源，同样属于项目级低频事务。
    if ( std::holds_alternative<CmdImportAudio>(cmd) ) {
        handleImportAudio(std::get<CmdImportAudio>(cmd));
        return;
    }

    // 编辑工具是编辑器级选择：立即发布查询值，并给全部现有会话排队切换。
    if ( std::holds_alternative<CmdChangeTool>(cmd) ) {
        auto tool = std::get<CmdChangeTool>(cmd).tool;
        // 枚举立即供 UI 查询，命令再让每个会话清理旧工具状态并接收新工具。
        // relaxed 只发布工具选择，不表示会话已经处理完命令。
        m_currentTool.store(tool, std::memory_order_relaxed);
        if ( auto* project = ProjectController::instance().currentProject() ) {
            // 活动工具随项目工作区持久化，项目切回时恢复用户上次创作上下文。
            project->m_settings.m_workspace.m_activeEditTool =
                editToolToWorkspaceName(tool);
        }

        std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
        /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
        auto& sessions = m_sessionRegistry.entriesUnsafe();
        for ( auto& entry : sessions ) {
            if ( entry.session ) {
                // 每个会话自行在 update 边界结束旧工具手势并采用新工具。
                entry.session->pushCommand(LogicCommand(CmdChangeTool{ tool }));
            }
        }
        return;
    }

    // 项目音频画笔同样跨画布共享，资源、类型和音量作为不可拆分状态传播。
    if ( const auto* audioResource =
             std::get_if<CmdSetBrushAudioResource>(&cmd) ) {
        m_brushAudioResourceId = audioResource->audioResourceId;
        m_brushAudioTrackType  = audioResource->audioTrackType;
        // 与工作区恢复采用相同音量边界，非有限输入回退默认增益。
        m_brushAudioVolume = std::isfinite(audioResource->volume)
                                 ? std::max(0.0F, audioResource->volume)
                                 : 1.0F;
        if ( auto* project = ProjectController::instance().currentProject() ) {
            // 工作区仅保存可跨启动恢复的资源 ID 和增益，轨道类型从资源表重建。
            auto& workspace = project->m_settings.m_workspace;
            workspace.m_projectAudioToolSelectedResourceId =
                m_brushAudioResourceId;
            workspace.m_projectAudioToolBrushVolume = m_brushAudioVolume;
        }

        std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
        auto& sessions = m_sessionRegistry.entriesUnsafe();
        for ( auto& entry : sessions ) {
            if ( entry.session ) {
                // Logo
                // 会话也接收画笔状态，后续原地复用时无需等待下一次选择事件。
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

    // 视口尺寸先写渲染同步缓存，未来创建或切换的会话也能取得最新值；
    // 当前目标仍需继续接收命令，所以这里不提前返回。
    if ( std::holds_alternative<CmdUpdateViewport>(cmd) ) {
        const auto& v = std::get<CmdUpdateViewport>(cmd);
        m_renderSyncRegistry.cacheViewportSize(v.cameraId,
                                               { v.width, v.height });
    }

    // 主画布悬停坐标按 cameraId 精确路由，使后台 Session 能独立生成
    // NearCursor 分拍线；坐标只影响视觉悬停，不改变活动会话身份。
    if ( std::holds_alternative<CmdSetMousePosition>(cmd) ) {
        const auto& mouse = std::get<CmdSetMousePosition>(cmd);
        // 主画布目标缺失时丢弃，不能回退到活动会话造成幽灵悬浮。
        // Preview、Timeline 等辅助相机不在此命中，继续走末尾活动会话路由。
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
            // variant 已转移给唯一目标，不允许再落入通用活动会话分支。
            return;
        }
    }

    // 主画布滚轮按 cameraId 精确路由，确保焦点切换帧的滚动命令进入悬停画布，
    // 而不是依赖上一帧仍未完成切换的活动索引。
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
            // 后台滚轮只允许与活动会话共享有效 Main 指纹的画布，保证时间滚动
            // 仍对应当前 transport；无关谱面必须先获得活动焦点。
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
    // 平移不改变播放时间，因此无需要求目标与活动会话共享主音轨。
    if ( std::holds_alternative<CmdPanCanvas>(cmd) ) {
        const auto& pan = std::get<CmdPanCanvas>(cmd);
        // 每次增量立即进入目标队列，不做固定窗口合并，连续拖动保持本地响应。
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

    // 剩余命令属于活动编辑上下文；取得锁后再读取活动索引，避免路由中途切换。
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    // 颜色选择虽然仅由活动会话执行，但引擎保留全局副本供新建与切换会话恢复。
    if ( const auto* palette = std::get_if<CmdSetBrushNotePalette>(&cmd) ) {
        for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
            // 调色板命令是全量快照，逐槽覆盖后再标记缓存已经初始化。
            m_brushNoteColors[i] = palette->colors[i];
        }
        m_brushNoteColorsInitialized = true;
    } else if ( const auto* color = std::get_if<CmdSetBrushNoteColor>(&cmd) ) {
        const auto colorIndex = static_cast<std::size_t>(color->slot);
        if ( colorIndex < m_brushNoteColors.size() ) {
            // 单槽命令只更新命中位置，越界枚举不污染缓存也不阻止会话自行校验。
            m_brushNoteColors[colorIndex] = color->color;
            m_brushNoteColorsInitialized  = true;
        }
    }

    auto& sessions = m_sessionRegistry.entriesUnsafe();
    // 无活动索引时不构造替代会话；已缓存的全局画笔选择仍供下次创建恢复。
    int32_t idx = m_sessionRegistry.activeIndex();
    if ( idx >= 0 && idx < static_cast<int32_t>(sessions.size()) ) {
        // 只有最终通用分支转移 variant，前面的检查在未返回时均保持载荷有效。
        sessions[idx].session->pushCommand(std::move(cmd));
    }
}

/// @brief 将已初始化的全局颜色逐槽投递给指定会话。
/// @param session 正在创建、复用或切换的目标会话。
/// @details
/// 引擎保存最近一次完整或单槽调色板选择。会话创建和切换时通过标准命令入口
/// 重放这些值，而不直接修改 SessionContext，从而复用会话侧的校验与刷新逻辑。
/// @pre 调用者持有会话注册表锁，保证画笔缓存读取一致。
/// @note 未初始化时保留目标会话默认颜色，不能用空缓存覆盖皮肤配置。
void EditorEngine::restoreBrushNoteColorsUnsafe(BeatmapSession& session) const
{
    // 未收到过用户或皮肤调色板时保留 BeatmapSession 自身初始化值。
    if ( !m_brushNoteColorsInitialized ) return;

    for ( std::size_t i = 0; i < NOTE_COLOR_SLOT_COUNT; ++i ) {
        // 使用单槽命令复用会话既有校验和派生状态更新，不直接访问其上下文。
        session.pushCommand(CmdSetBrushNoteColor{ static_cast<NoteColorSlot>(i),
                                                  m_brushNoteColors[i] });
    }
}

/// @brief 将全局音频画笔选择、类型和增益作为一个命令恢复到会话。
/// @param session 接收当前画笔状态的会话。
/// @details
/// 三个字段构成一次完整选择快照，空资源 ID 也具有清理旧选择的明确语义。
/// 统一命令可保证复用 Logo、创建新标签和切换活动标签采用相同行为。
/// @pre 调用者持有会话注册表锁；空资源 ID 也需投递以清理目标旧选择。
void EditorEngine::restoreBrushAudioResourceUnsafe(
    BeatmapSession& session) const
{
    // 空 ID 也是完整状态，用来清除复用会话可能保留的旧项目资源选择。
    session.pushCommand(LogicCommand(CmdSetBrushAudioResource{
        m_brushAudioResourceId,
        m_brushAudioTrackType,
        m_brushAudioVolume,
    }));
}

/// @brief 查询是否有会话撤销栈处于未保存状态。
/// @return 任一有效会话 actionStack 为脏时返回 true。
/// @details
/// 查询遍历所有打开会话而非仅活动标签，关闭项目提示必须覆盖后台谱面修改。
/// Logo 和空会话没有脏撤销栈，自然被空指针与栈状态检查排除。
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
/// @param cameraId 目标画布稳定相机 ID。
/// @return 可跨 UI 和逻辑线程保活的同步缓冲共享句柄。
/// @note 同一 cameraId 重复调用返回同一注册项，关闭会话时须同步 eraseCamera。
/// @warning 逻辑热路径/共享指针：返回 shared_ptr
/// 用于保证本次快照发布期间缓冲区不被 UI 关闭路径释放。
std::shared_ptr<BeatmapSyncBuffer> EditorEngine::getSyncBuffer(
    const std::string& cameraId)
{
    // 注册表按 cameraId 返回或惰性创建缓冲，调用方共享所有权覆盖本次使用。
    return m_renderSyncRegistry.getSyncBuffer(cameraId);
}

/// @brief 获取指定画布当前发布的纹理图集 UV 映射。
/// @param cameraId 目标画布稳定相机 ID。
/// @return 不可变映射共享快照；画布尚未发布时由注册表返回空或默认结果。
/// @note 映射按修订整体替换，读取方不得通过 const_cast 修改共享内容。
/// @warning 渲染热路径共享指针：返回所有权保证本帧读取期间旧映射不被替换释放。
std::shared_ptr<const std::unordered_map<uint32_t, glm::vec4>>
EditorEngine::getAtlasUVMap(const std::string& cameraId) const
{
    return m_renderSyncRegistry.getAtlasUVMap(cameraId);
}

/// @brief 按修订号将指定画布的图集 UV 映射同步到快照缓存。
/// @param cameraId 目标画布稳定相机 ID。
/// @param target 快照持有的 UV 映射，修订变化时被更新。
/// @param targetRevision 调用方已应用的图集修订号。
/// @param targetAsciiFontAtlasMetrics ASCII 字体图集度量输出。
/// @param targetUnicodeFontMetrics Unicode 字体度量输出。
/// @details
/// RenderSyncRegistry 比较目标修订与当前发布修订，只在变化时复制 UV 表和两类
/// 字体度量。调用方应让这些输出与同一修订号共同存在，不能单独缓存其中一项。
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
/// @note 有当前项目时相对输入按项目根解释，无项目时按进程绝对路径解释。
/// @warning 路径规范化可能访问文件系统，只能用于打开、查重和文件操作低频路径。
/// @warning
/// 低频路径：可能访问文件系统解析规范路径，只能在文件选择、打开或打包流程调用。
std::string EditorEngine::makeBeatmapPathKeyForPath(
    const std::filesystem::path& beatmapPath) const
{
    return makeBeatmapPathKey(getCurrentProject(), beatmapPath);
}

/// @brief 更新指定主画布窗口在 UI 中的可见状态。
/// @param cameraId 主画布的稳定相机 ID。
/// @param isVisible 当前窗口是否参与可见后台会话更新。
/// @note 可见性只影响后台更新预算，不改变活动索引、停靠状态或谱面打开状态。
/// @post 状态变化时发布新的 SessionRegistry 快照，未变化时不产生分配。
/// @warning UI 热路径：Basic2DCanvas 每帧写入；只修改注册表中的布尔状态。
void EditorEngine::setSessionCanvasVisible(const std::string& cameraId,
                                           bool               isVisible)
{
    if ( !SessionUtils::isMainCanvasCameraId(cameraId) ) {
        // 共享辅助相机不对应独立 SessionEntry，其可见性由所属窗口自行管理。
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    for ( auto& entry : sessions ) {
        if ( entry.cameraId == cameraId ) {
            if ( entry.isCanvasVisible == isVisible ) {
                // 状态未变时不发布新快照，避免 UI 每帧制造 shared_ptr 替换。
                return;
            }
            entry.isCanvasVisible = isVisible;
            // 逻辑循环从已发布快照读取可见性，写入后必须显式发布新版本。
            m_sessionRegistry.publishSnapshotUnsafe();
            return;
        }
    }
}

/// @brief 获取当前工具类型。
/// @return 跨画布共享的编辑工具枚举。
/// @note 该值表示最近发布选择，不保证各会话已经消费对应 CmdChangeTool。
/// @warning 逻辑/UI 热路径原子：只读取工具枚举状态，使用 relaxed。
EditTool EditorEngine::getCurrentTool() const
{
    return m_currentTool.load(std::memory_order_relaxed);
}

/// @brief 查询当前活动会话是否处于实际播放状态。
/// @return 活动会话存在且持有播放状态时返回 true。
/// @note 同步 follower 始终保持 isPlaying=false，不会把后台视觉跟随误报为播放。
/// @warning UI 热路径：短暂持有 SessionRegistry 递归锁，只读取常量布尔值。
bool EditorEngine::isPlaybackPlaying() const
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    const auto& sessions = m_sessionRegistry.entriesUnsafe();
    /// @brief 当前活跃 Session 索引快照。
    int32_t idx = m_sessionRegistry.activeIndex();
    if ( idx >= 0 && idx < static_cast<int32_t>(sessions.size()) ) {
        // isPlaying 只在活动会话代表全局 transport；后台 follower 不会返回
        // true。
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
        // Note 与 Sample 使用独立选择索引，任一非空都允许通用删除、剪切等操作。
        return !ctx.selectedNoteEntities.empty() ||
               !ctx.selectedSampleEntities.empty();
    }
    return false;
}

/// @brief 判断当前活跃 Session 是否正在拖拽框选区域。
/// @return 活动会话处于 Marquee 工具且选择手势已开始时返回 true。
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
        // isSelecting 也可能被其他工具用于内部状态，必须与当前工具联合判断。
        return ctx.currentTool == EditTool::Marquee && ctx.isSelecting;
    }
    return false;
}

/// @brief 判断当前活跃 Session 是否正在拖拽物件。
/// @return Move 工具存在有效拖拽实体且手势进行中时返回 true。
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
        // draggedEntity 哨兵可排除仅开始空白区域手势但未命中对象的状态。
        return ctx.currentTool == EditTool::Move && ctx.isDragging &&
               ctx.draggedEntity != entt::null;
    }
    return false;
}

/// @brief 判断当前活跃 Session 是否正在使用画笔绘制。
/// @return Draw 工具的画笔状态已激活时返回 true。
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
        // 工具与 brushState 双重判断避免切换工具后的尾随状态短暂误报。
        return ctx.currentTool == EditTool::Draw && ctx.brushState.isActive;
    }
    return false;
}

/// @brief 设置同主音轨多画布时间同步开关。
/// @param enabled 是否允许相同非空主音轨指纹的后台画布跟随活动项。
/// @warning 逻辑/UI 热路径原子：只写入同步开关状态，使用 relaxed。
void EditorEngine::setSyncSameMainAudioCanvases(bool enabled)
{
    m_syncSameMainAudioCanvases.store(enabled, std::memory_order_relaxed);
    if ( !enabled ) {
        // 关闭时立即清除所有 follower 身份，后续 update 恢复各会话独立时间轴。
        std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
        auto& sessions = m_sessionRegistry.entriesUnsafe();
        for ( auto& entry : sessions ) {
            if ( entry.session ) {
                auto& ctx = entry.session->getContextMutable();
                // 来源指纹与布尔身份必须成对清除，不能留下可被误复用的旧来源。
                ctx.isAudioTimelineSyncFollower = false;
                ctx.m_audioTimelineSyncSourceFingerprint.clear();
            }
        }
    }
    if ( auto* project = ProjectController::instance().currentProject() ) {
        // 开关属于项目工具栏工作区偏好，不覆盖其他项目或软件全局配置。
        const auto editorConfig = getEditorConfig();
        captureToolbarWorkspaceState(
            project->m_settings.m_workspace, editorConfig, enabled);
    }
    if ( enabled ) {
        // 开启后立即对齐一次，用户无需等待下一次播放或时间变化才看到同步。
        syncSameMainAudioCanvases();
    }
}

/// @brief 刷新已打开 Session 的主音轨同步路径键。
/// @details
/// 该入口从每个 SessionContext 的 AudioTimelineDescriptor 提取完整与主音轨
/// 两类指纹，随后重算同步候选并发布注册表快照。调用者无需自行持锁。
/// @warning 低频资源或会话变化入口：持有注册表锁并发布新的会话快照。
void EditorEngine::refreshAudioTimelineFingerprints()
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    refreshAudioTimelineFingerprintsUnsafe();
}

/// @brief 发布已打开 Session 的复合时间线指纹，调用者必须持有注册表锁。
/// @pre 调用者持有 SessionRegistry 递归锁。
/// @warning 低频描述符变化路径：遍历全部会话并发布快照，不得每帧无条件调用。
void EditorEngine::refreshAudioTimelineFingerprintsUnsafe()
{
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    for ( auto& entry : sessions ) {
        if ( entry.isLogoPlaceholder || !entry.session ) {
            // 占位和空会话明确清空两类身份，不能沿用此前复用谱面的指纹。
            entry.audioTimelineFingerprint.clear();
            entry.mainAudioSyncFingerprint.clear();
            continue;
        }

        const auto& ctx = entry.session->getContext();
        // 完整指纹控制 transport 复用，主音轨指纹控制多画布时间同步资格。
        entry.audioTimelineFingerprint =
            ctx.audioTimelineDescriptor.m_fingerprint;
        entry.mainAudioSyncFingerprint =
            ctx.audioTimelineDescriptor.m_mainAudioSyncFingerprint;
    }
    refreshMainAudioSyncPeerStateUnsafe();
    // 条目字段改变后一次性发布，UI 和逻辑下轮均观察到同一组配对状态。
    m_sessionRegistry.publishSnapshotUnsafe();
    // 活动源身份可能改变，强制下一轮重新传播当前时间到所有匹配 follower。
    m_lastMainAudioSyncActiveIndex = -1;
}

/// @brief 标记全部或引用指定资源的已打开谱面描述符需要低频重建。
/// @param resourceId 目标资源 ID；为空时标记全部有效谱面会话。
/// @details
/// 描述符脏标记表示结构需要重新计算，激活待处理标记表示活动 transport 需要
/// 在安全更新点采用新结构。两个标记同时设置，但实际重建可由后台节流延后。
/// @pre 调用者持有 SessionRegistry 递归锁。
void EditorEngine::markAudioTimelineDescriptorsDirtyUnsafe(
    std::string_view resourceId)
{
    // 空 ID 表示项目级资源布局整体变化，所有非占位会话都必须重建描述符。
    // 非空 ID 则把工作限制到已经引用该资源或此前已经标脏的会话。
    for ( auto& entry : m_sessionRegistry.entriesUnsafe() ) {
        if ( entry.isLogoPlaceholder || !entry.session ) continue;
        auto& ctx = entry.session->getContextMutable();
        // 已经标脏的会话不能因旧描述符暂时查不到引用而被跳过；其内容可能
        // 正等待上一项资源操作的重建，旧索引已不再能完整代表当前 ECS。
        if ( !resourceId.empty() && !ctx.isAudioTimelineDescriptorDirty &&
             !audioTimelineDescriptorReferencesResource(
                 ctx.audioTimelineDescriptor, resourceId) ) {
            continue;
        }
        // 描述符重建与 transport 激活分开标记：前者更新资源结构，后者保证
        // 活动会话在下一次安全更新点重新装载组合时间线。
        ctx.isAudioTimelineDescriptorDirty   = true;
        ctx.isAudioTimelineActivationPending = true;
    }
}

/// @brief 刷新同主音轨候选并清理与活动源不兼容的 follower。
/// @details
/// 第一遍根据当前活动源撤销失效 follower，第二遍寻找任意具有相同非空主音轨
/// 指纹的会话对并发布快速提示。具体同步不缓存配对，执行时仍重新校验快照。
/// @pre 调用者持有 SessionRegistry 递归锁。
/// @warning 逻辑热路径的状态维护分支：按已打开会话数量做两层候选检查，
/// 仅在会话列表或音频指纹发生变化后调用，不得提升为每帧无条件扫描。
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

    // follower 身份只对“当前活动源 + 相同非空主音轨”成立。活动项改变后，
    // 旧 follower 必须立即解除，不能继续沿用上一谱面的视觉时钟来源。
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

    // 原子提示只回答是否存在任意可同步对，供常见无候选路径快速返回。
    // 这里不缓存具体配对关系，因为关闭或重载任一会话都会改变有效身份。
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
                // relaxed 发布的是性能提示而非会话数据；真正同步仍在锁内
                // 重新校验来源和目标指纹，因此不承担对象生命周期同步职责。
                m_hasMainAudioSyncPeers.store(true, std::memory_order_relaxed);
                return;
            }
        }
    }

    m_hasMainAudioSyncPeers.store(false, std::memory_order_relaxed);
}

/// @brief 将使用同一主音轨的非活跃会话同步到当前活跃会话时间。
/// @details
/// 便捷入口只选择注册表当前活动索引，全部资格、时钟和 follower 更新由
/// syncSameMainAudioCanvasesFromIndex 统一实现，避免两套同步规则发生偏差。
/// @note 无活动索引、无候选或开关关闭时由下层入口快速返回。
/// @warning 逻辑热路径/原子：每次 Session update 后可能执行；开关读取使用
/// relaxed，后续短暂持有 SessionRegistry 递归锁并遍历已打开 Session 列表。
void EditorEngine::syncSameMainAudioCanvases()
{
    syncSameMainAudioCanvasesFromIndex(m_sessionRegistry.activeIndex());
}

/// @brief 从指定源 Session 同步同主音轨的其他画布时间。
/// @param sourceIndex 作为时间、缩放与播放状态来源的注册表稳定索引。
/// @warning 逻辑热路径/原子：每次 Session update 后可能执行；开关读取使用
/// relaxed，并短暂持有 SessionRegistry 递归锁以串行化 SessionContext 访问。
void EditorEngine::syncSameMainAudioCanvasesFromIndex(int32_t sourceIndex)
{
    // 配置按值读取，视觉偏移和缩放参数在本轮所有 follower 间保持一致。
    const auto editorConfig = getEditorConfig();
    if ( !m_syncSameMainAudioCanvases.load(std::memory_order_relaxed) ) {
        // 用户关闭同步后不进入注册表扫描，follower 身份已由设置入口主动清理。
        return;
    }
    if ( !m_hasMainAudioSyncPeers.load(std::memory_order_relaxed) ) {
        // 候选提示由会话或指纹变化时更新；假阴性只会推迟到下次刷新，
        // 真正同步仍会在锁内逐项验证，不依赖该原子保证正确性。
        return;
    }

    {
        // 快照用于稳定枚举会话及其 shared_ptr 生命周期，注册表锁则串行化
        // 各 SessionContext 的读取和原地更新；两者承担不同的一致性职责。
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
            // 来源索引可能在快照发布后被关闭，找不到时本轮同步安全作废。
            return;
        }

        // 完整时间线指纹标识具体混音结构，主音轨指纹只决定画布能否跟随。
        // 任一来源身份为空都不能建立可恢复的同步关系。
        auto&       sourceCtx = sourceEntry->session->getContext();
        const auto& sourceKey = sourceEntry->mainAudioSyncFingerprint;
        const auto& sourceTimelineFingerprint =
            sourceEntry->audioTimelineFingerprint;
        if ( sourceKey.empty() || sourceTimelineFingerprint.empty() ) {
            // 未完成载图或描述符尚未发布的来源没有稳定同步身份。
            return;
        }
        if ( sourceCtx.isAudioTimelineSyncFollower && !sourceCtx.isPlaying ) {
            // 后台 follower 不能继续作为二级来源广播，避免形成级联和时钟漂移。
            return;
        }

        // 本轮所有 follower 共用一次单调时钟采样，避免循环先后产生视觉偏移。
        // 若来源视觉时钟刚刚解析过，则优先沿用其精确解析时刻作为 rebase 基准。
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
                // 只接受不晚于当前采样的有限解析时刻，拒绝损坏或未来时间基线。
                sourceClockSteadyTime = resolvedSteadyTime;
            }
        }
        const double playbackRate =
            Audio::AudioManager::instance().getPlaybackSpeed();

        for ( const auto& entry : sessions ) {
            if ( entry.index == sourceIndex || !entry.session ) {
                // 来源自身和空槽不参与 follower 更新，也不会改变其身份。
                continue;
            }

            auto& ctx = entry.session->getContextMutable();
            // 指纹不再相同的会话必须清除 follower 来源，不能只跳过本轮写入；
            // 否则其后续 update 仍会把自身误判为外部 transport 的跟随者。
            if ( entry.mainAudioSyncFingerprint != sourceKey ) {
                ctx.isAudioTimelineSyncFollower = false;
                ctx.m_audioTimelineSyncSourceFingerprint.clear();
                continue;
            }

            const double sourceAnimateTarget =
                sourceCtx.currentTime +
                editorConfig.visual.getEffectiveVisualOffset();
            // 播放中以来源实际动画时间判断特效重置，暂停时以目标视觉偏移位置判断。
            const double sourceResetTime     = sourceCtx.isPlaying
                                                   ? sourceCtx.animateTime
                                                   : sourceAnimateTarget;
            const bool   wasFollowing        = ctx.isAudioTimelineSyncFollower;
            const double previousAnimateTime = ctx.animateTime;
            // 播放状态变化、明显回跳或大跨度定位都会使活动特效的增量游标失效。
            // 小幅正向推进仍走增量消费，避免每轮重建 Hold 特效。
            const bool shouldClearHitEffects =
                wasFollowing != sourceCtx.isPlaying ||
                sourceResetTime + MAIN_AUDIO_SYNC_BACKWARD_RESET_EPSILON <
                    previousAnimateTime ||
                std::abs(sourceResetTime - previousAnimateTime) > 0.2;

            ctx.currentTime = sourceCtx.currentTime;
            if ( sourceCtx.isPlaying ) {
                // 播放中复制来源已经插值后的动画状态，多个画布保持同一视觉相位。
                ctx.animateTime       = sourceCtx.animateTime;
                ctx.animateTimeTarget = sourceCtx.animateTimeTarget;
                ctx.animateTimeAnimationActive =
                    sourceCtx.animateTimeAnimationActive;
            } else if ( std::isfinite(ctx.animateTime) &&
                        std::isfinite(sourceAnimateTarget) ) {
                // 暂停时保留目标会话的当前插值起点，仅把目标推向来源位置；
                // 这让手动跳转保持既有平滑效果，而不是瞬间硬切。
                ctx.animateTimeTarget = sourceAnimateTarget;
                ctx.animateTimeAnimationActive =
                    std::abs(ctx.animateTimeTarget - ctx.animateTime) >
                    MAIN_AUDIO_SYNC_TIME_EPSILON;
            } else {
                // 任一动画时间异常时无法安全插值，直接在有效目标上重新建基线。
                ctx.animateTime                = sourceAnimateTarget;
                ctx.animateTimeTarget          = sourceAnimateTarget;
                ctx.animateTimeAnimationActive = false;
            }
            ctx.animatedTimelineZoom = sourceCtx.animatedTimelineZoom;
            // 缩放补间三元组整体复制，不能只复制当前值而让目标继续旧动画。
            ctx.animatedTimelineZoomTarget =
                sourceCtx.animatedTimelineZoomTarget;
            ctx.animatedTimelineZoomAnimationActive =
                sourceCtx.animatedTimelineZoomAnimationActive;
            // 只有真正控制全局音频的活动会话持有 isPlaying；后台画布通过
            // follower 标记推进视觉，不得重复向 AudioManager 发播放命令。
            ctx.isPlaying                   = false;
            ctx.isAudioTimelineSyncFollower = sourceCtx.isPlaying;
            if ( ctx.isAudioTimelineSyncFollower ) {
                if ( ctx.m_audioTimelineSyncSourceFingerprint !=
                     sourceTimelineFingerprint ) {
                    // 来源完整时间线变化时更新身份，后续会话可判断是否需重新激活。
                    ctx.m_audioTimelineSyncSourceFingerprint =
                        sourceTimelineFingerprint;
                }
            } else {
                // 暂停同步只对齐位置，不让后台会话保留运行中的 follower 标记。
                ctx.m_audioTimelineSyncSourceFingerprint.clear();
            }
            ctx.playbackVisualClock.rebase(sourceCtx.currentTime,
                                           sourceClockSteadyTime,
                                           playbackRate,
                                           sourceCtx.isPlaying);
            // 清理发生在恢复持续 Hold 之前，确保不会保留旧位置残留的特效。
            if ( shouldClearHitEffects ) {
                ctx.hitFXSystem.clearActiveEffects();
            }
            if ( ctx.isAudioTimelineSyncFollower && entry.isCanvasVisible ) {
                // 隐藏会话无需维护纯视觉特效；重新显示后由重置路径恢复必要状态。
                updateFollowerHitEffects(
                    ctx, previousAnimateTime, shouldClearHitEffects);
            }
        }
        // 所有匹配会话在同一锁作用域内更新完成，调用方不会观察到半批 follower。
        return;
    }
}

/// @brief 创建谱面会话，或复用符合条件的 Logo 占位会话。
/// @param beatmap 初始谱面；为空时创建未载图画布。
/// @param displayName 标签展示名；为空时从谱面元数据或默认名称推导。
/// @param isLogoPlaceholder 新会话是否仅作为欢迎 Logo 占位。
/// @param preferredCameraId 工作区恢复期希望复用的稳定画布 ID。
/// @param restoreDockFromWorkspace 是否由 UI 按已保存停靠身份恢复窗口。
/// @return 已存在、复用或新建会话的注册表索引；重复谱面返回原索引。
/// @warning 打开谱面的低频路径：持有 SessionRegistry 递归锁，可能规范化
/// 文件路径、分配 shared_ptr 并向命令队列投递载入任务，不得从每帧路径调用。
int32_t EditorEngine::createSession(std::shared_ptr<MMM::BeatMap> beatmap,
                                    const std::string&            displayName,
                                    bool               isLogoPlaceholder,
                                    const std::string& preferredCameraId,
                                    bool               restoreDockFromWorkspace)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    const auto                            editorConfig = getEditorConfig();
    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    // 工作区保存的 cameraId 只有在当前列表未占用时才能复用；注册表最终仍
    // 负责预留 ID，lambda 只用于本次创建前的冲突判断。
    auto cameraIdInUse = [&](const std::string& cameraId) {
        return std::any_of(
            sessions.begin(), sessions.end(), [&](const SessionEntry& entry) {
                return entry.cameraId == cameraId;
            });
    };

    /// @brief 当前项目指针快照，用于规范化新会话谱面中的项目相对路径。
    const auto* currentProject = ProjectController::instance().currentProject();
    if ( currentProject && beatmap ) {
        // 会话接管谱面前统一长期路径表示，后续保存、查重和资源解析共享同一基准。
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
    // 新会话的指纹必须由其处理 CmdLoadBeatmap 后从实际描述符发布；这里保持空，
    // 不能仅凭元数据路径提前推测包含自动采样的完整时间线身份。

    if ( !isLogoPlaceholder && beatmap ) {
        // 只有具有稳定路径身份的真实谱面参与查重。未保存的新谱面可能路径
        // 为空，不能把多个空路径会话错误合并成同一个标签。
        if ( !requestedBeatmapKey.empty() ) {
            for ( int32_t i = 0; i < static_cast<int32_t>(sessions.size());
                  ++i ) {
                const auto& entry = sessions[static_cast<size_t>(i)];
                if ( entry.isLogoPlaceholder || !entry.session ) {
                    continue;
                }

                const auto& ctx = entry.session->getContext();
                // 优先使用条目已缓存键；兼容旧会话时才从当前 BeatMap 路径补算。
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

                // 重复打开不重新载入、不替换内存谱面，只请求 UI 聚焦原会话。
                // 这样可保留未保存编辑、撤销栈与当前播放位置。
                requestSessionFocus(i);
                XINFO("Beatmap already open. Focusing Session #{} cameraId={}",
                      i,
                      entry.cameraId);
                return i;
            }
        }
    }

    // 真实谱面优先复用 Logo 占位，保持启动布局中的首个画布和停靠节点稳定。
    if ( !isLogoPlaceholder && beatmap ) {
        for ( int32_t i = 0; i < static_cast<int32_t>(sessions.size()); ++i ) {
            if ( sessions[i].isLogoPlaceholder ) {
                if ( !preferredCameraId.empty() &&
                     sessions[i].cameraId != preferredCameraId ) {
                    continue;
                }
                // 条目身份先从占位切为谱面，再按配置、工具、画笔、载图的
                // 顺序投递初始化命令，保证载图处理看到完整编辑环境。
                sessions[i].isLogoPlaceholder        = false;
                sessions[i].restoreDockFromWorkspace = restoreDockFromWorkspace;
                sessions[i].displayName              = displayName.empty()
                                                           ? beatmap->m_baseMapMetadata.name
                                                           : displayName;
                sessions[i].beatmapPathKey           = requestedBeatmapKey;
                // 占位条目的旧音频身份必须覆盖为空，等待载图命令生成新描述符。
                sessions[i].audioTimelineFingerprint =
                    requestedAudioTimelineFingerprint;
                sessions[i].mainAudioSyncFingerprint =
                    requestedMainAudioSyncFingerprint;
                if ( !preferredCameraId.empty() ) {
                    // 显式恢复的 ID 即使来自现有占位，也要同步注册表分配器，
                    // 防止后续自动编号重新生成同名相机。
                    m_sessionRegistry.reserveCameraId(preferredCameraId);
                }
                sessions[i].session->pushCommand(
                    LogicCommand(CmdUpdateEditorConfig{ editorConfig }));
                // 初始化命令全部进入原 Session 队列，复用保留了其 cameraId 和
                // RenderSyncBuffer，但载图处理会重置原占位上下文的谱面派生状态。
                sessions[i].session->pushCommand(LogicCommand(CmdChangeTool{
                    m_currentTool.load(std::memory_order_relaxed) }));
                restoreBrushNoteColorsUnsafe(*sessions[i].session);
                restoreBrushAudioResourceUnsafe(*sessions[i].session);
                sessions[i].session->pushCommand(
                    LogicCommand(CmdLoadBeatmap{ beatmap }));
                /// @brief 复用占位画布前的活动 Session，用于补交谱面切换事件。
                const int32_t previousIndex = m_sessionRegistry.activeIndex();
                // 离开旧活动谱面前请求切换触发的自动保存；复用同一活动槽时
                // 没有谱面切换边界，不产生多余保存任务。
                if ( previousIndex >= 0 && previousIndex != i &&
                     previousIndex < static_cast<int32_t>(sessions.size()) &&
                     sessions[static_cast<size_t>(previousIndex)].session ) {
                    sessions[static_cast<size_t>(previousIndex)]
                        .session->requestAutoSave(
                            AutoSaveTrigger::BeatmapSwitch);
                }
                m_sessionRegistry.setActiveIndex(i);
                // 会话条目身份变化后立即刷新候选并发布快照，UI 不能继续看到
                // 已经装载谱面的画布仍标记为 Logo。
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

    // 优先尊重工作区保存的相机身份；发生冲突时分配全新 ID，绝不抢占
    // 已打开窗口的渲染同步缓冲区。
    /// @brief 新 Session 对应的唯一画布 cameraId。
    std::string cameraId;
    if ( !preferredCameraId.empty() && !cameraIdInUse(preferredCameraId) ) {
        // 工作区恢复的稳定 ID 未冲突时直接保留，停靠 ini 可重新关联原窗口。
        cameraId = preferredCameraId;
        m_sessionRegistry.reserveCameraId(cameraId);
    } else {
        // 冲突或普通新建统一使用注册表分配器，保证跨关闭空洞仍不重复 ID。
        cameraId = m_sessionRegistry.createNextCameraId();
    }

    // 新会话的 shared_ptr 同时由注册表快照和 UI 查询短暂持有，保证关闭与
    // 渲染读取交错时对象生命周期覆盖当前操作。
    /// @brief 新创建的谱面逻辑 Session。
    auto newSession = std::make_shared<BeatmapSession>();

    // Preview、Timeline 等共享视口可能早于谱面会话创建，先补发缓存尺寸，
    // 使第一次 update 就能生成正确投影而不必等待下一次窗口 Resize。
    /// @brief 当前共享视口尺寸快照，用于初始化新 Session 的共享视口。
    auto sharedViewportSizes = m_renderSyncRegistry.getSharedViewportSizes();
    for ( const auto& [cid, size] : sharedViewportSizes ) {
        {
            // 保留原有局部作用域；当前没有临时资源需要提前析构。
            // 命令按缓存枚举顺序投递，不同共享视口之间没有顺序依赖。
        }
        newSession->pushCommand(CmdUpdateViewport{ cid, size.x, size.y });
    }

    // 预注册主画布缓冲区，使 UI 在会话快照发布后可以立即取得同步目标。
    getSyncBuffer(cameraId);

    // 初始命令顺序是会话契约：先配置，再工具与画笔，最后才加载谱面。
    // 载图后的派生系统因此不需要处理未初始化的主题或交互偏好。
    newSession->pushCommand(
        LogicCommand(CmdUpdateEditorConfig{ editorConfig }));
    newSession->pushCommand(LogicCommand(
        CmdChangeTool{ m_currentTool.load(std::memory_order_relaxed) }));
    restoreBrushNoteColorsUnsafe(*newSession);
    restoreBrushAudioResourceUnsafe(*newSession);

    // 空 beatmap 用于 Logo 或空画布，不向会话伪造载图命令。
    if ( beatmap ) {
        // shared_ptr 随命令复制到队列，在调用方释放局部引用后仍覆盖异步处理。
        newSession->pushCommand(LogicCommand(CmdLoadBeatmap{ beatmap }));
    }

    // 所有条目元数据在 append 前一次性准备，发布快照不会观察到半初始化项。
    /// @brief 即将注册到会话列表的新 Session 条目。
    SessionEntry entry;
    entry.session  = newSession;
    entry.cameraId = cameraId;
    entry.displayName =
        displayName.empty()
            ? (beatmap ? beatmap->m_baseMapMetadata.name : "New Canvas")
            : displayName;
    // displayName 对 UI 可变，beatmapPathKey 才是防止重复打开的稳定身份。
    entry.beatmapPathKey           = requestedBeatmapKey;
    entry.audioTimelineFingerprint = requestedAudioTimelineFingerprint;
    entry.mainAudioSyncFingerprint = requestedMainAudioSyncFingerprint;
    entry.isLogoPlaceholder        = isLogoPlaceholder;
    entry.restoreDockFromWorkspace = restoreDockFromWorkspace;
    /// @brief 新 Session 在注册表中的索引。
    int32_t newIndex = m_sessionRegistry.append(std::move(entry));
    // append 已负责发布会话列表；同步候选另依赖新条目的音频身份，需随后刷新。
    refreshMainAudioSyncPeerStateUnsafe();
    m_lastMainAudioSyncActiveIndex = -1;

    XINFO("Created Session #{} cameraId={} name={} (logo={})",
          newIndex,
          cameraId,
          sessions[newIndex].displayName,
          isLogoPlaceholder);

    return newIndex;
}

/// @brief 关闭指定会话并释放其主画布渲染同步状态。
/// @param index 待关闭会话的注册表索引。
/// @param updateWorkspace 是否立即用剩余会话刷新项目工作区快照。
/// @post 成功关闭后 cameraId 对应的 RenderSyncBuffer 已从注册表移除。
/// @warning 标签关闭低频路径：持有 SessionRegistry 递归锁并可能执行工作区
/// 路径规范化；调用返回后旧索引位置可能已被后续条目占用。
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

    // 先从注册表移除会话并发布新列表，后续同步计算不能再把它作为候选。
    // cameraId 已按值保存，因此对象析构后仍可精确清理对应渲染缓存。
    m_sessionRegistry.erase(index);
    refreshMainAudioSyncPeerStateUnsafe();
    m_lastMainAudioSyncActiveIndex = -1;

    // 主画布缓冲区不能跨同名会话复用，其中可能仍保留旧谱面的快照与图集。
    m_renderSyncRegistry.eraseCamera(cameraId);

    if ( updateWorkspace ) {
        // 批量关闭或项目切换可关闭逐项更新，由上层在最终状态统一捕获。
        captureProjectWorkspaceState();
    }
}

/// @brief 将指定 Session 原地重置为 Logo 占位画布。
/// @param index 需要保留画布身份的会话索引。
/// @param displayName 新占位标签名；为空时使用 Welcome。
/// @param updateWorkspace 是否立即移除工作区中的原谱面打开状态。
/// @post 原 cameraId 和注册表索引保持不变，谱面路径及音频指纹被清空。
/// @warning 项目关闭低频路径：分配替换会话并投递初始化命令，原 Session
/// 可能继续由已发布快照短暂持有，但不会再出现在新的注册表快照中。
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
    // 共享视口尺寸和本画布主视口分别来自不同缓存，必须都恢复到替换会话。
    // 缺少主视口缓存时等待下一次 Resize，不用虚构像素尺寸。
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
    // 占位会话仍可能被随后直接复用，提前恢复画笔状态可保持初始化顺序一致。
    restoreBrushNoteColorsUnsafe(*newSession);
    restoreBrushAudioResourceUnsafe(*newSession);

    entry.session     = std::move(newSession);
    entry.displayName = displayName.empty() ? "Welcome" : displayName;
    entry.beatmapPathKey.clear();
    entry.audioTimelineFingerprint.clear();
    entry.mainAudioSyncFingerprint.clear();
    // 原谱面身份全部清除后再发布快照，避免 UI 把占位标签当作仍可保存的谱面。
    entry.isLogoPlaceholder        = true;
    entry.restoreDockFromWorkspace = false;

    m_sessionRegistry.setActiveIndex(index);
    refreshMainAudioSyncPeerStateUnsafe();
    m_sessionRegistry.publishSnapshotUnsafe();
    m_lastMainAudioSyncActiveIndex = -1;

    if ( updateWorkspace ) {
        // 项目切换批量重置时由调用方统一保存，普通关图则立即同步打开列表。
        captureProjectWorkspaceState();
    }
}

/// @brief 设置当前活跃 Session，并切换全局复合音频 transport 所属谱面。
/// @param index 目标 Session 索引。
/// @details
/// 切换先结算旧活动会话的连续播放位置和自动保存触发，再把目标会话设置为
/// 唯一活动项。目标的脏音频描述符按需重建，SessionUtils 依据新旧主音轨指纹、
/// 当前时间和停止播放偏好决定是否继承播放。全局 transport 成功激活后才 seek
/// 并恢复播放；视觉时间、缩放和打击特效随后在目标位置建立新基线。
/// @warning 低频视图切换路径：持有 SessionRegistry 递归锁，可能解析并解码
/// 完整自动采样时间线；切换期间不得由 UI 直接并发修改 SessionContext。
/// @post 有效目标成为唯一活动上下文，并取得最新共享视口尺寸命令。
void EditorEngine::setActiveSessionIndex(int32_t index)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    const auto                            editorConfig = getEditorConfig();
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    if ( index < 0 || index >= static_cast<int32_t>(sessions.size()) ) {
        return;
    }

    // 切换只采样一次 steady_clock，使旧会话的连续播放位置与新会话的
    // transport 定位建立在同一时刻，不受中间资源激活耗时影响。
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
            // 自动保存请求在改变活动索引前投递，触发原因仍归属于离开的谱面。
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
            // 仅当前 transport 确实属于旧会话时外推播放位置；指纹不符说明
            // 音频已被其他加载流程替换，不能用它推算本会话时间。
            previousCtx.currentTime =
                resolveContinuousSessionTime(previousCtx, sessionSwitchTime);
            previousWasPlaying = true;
        }
        previousTime                            = previousCtx.currentTime;
        previousCtx.isPlaying                   = false;
        previousCtx.isAudioTimelineSyncFollower = false;
        previousCtx.m_audioTimelineSyncSourceFingerprint.clear();
        // 活跃标记先从旧上下文撤除，后续快照不会出现两个活动会话。
        previousCtx.isActiveSession = false;
    }

    m_sessionRegistry.setActiveIndex(index);
    auto& activeSession = sessions[index].session;
    if ( !activeSession ) {
        // 空槽无法接管 transport，卸载旧时间线防止 UI 状态与实际播放来源分离。
        audio.unloadAudioTimeline();
        return;
    }

    // 全局画笔状态在任何会话自身命令之前恢复，切换后第一次交互即可一致。
    restoreBrushNoteColorsUnsafe(*activeSession);
    restoreBrushAudioResourceUnsafe(*activeSession);
    auto& ctx                       = activeSession->getContextMutable();
    ctx.isActiveSession             = true;
    ctx.isPlaying                   = false;
    ctx.isAudioTimelineSyncFollower = false;
    ctx.m_audioTimelineSyncSourceFingerprint.clear();
    if ( ctx.isAudioTimelineDescriptorDirty ) {
        // 资源变更期间后台会话只被标脏；成为活动项时才支付完整重建成本。
        SessionUtils::rebuildAudioTimelineDescriptor(ctx, getCurrentProject());
    }
    sessions[index].audioTimelineFingerprint =
        ctx.audioTimelineDescriptor.m_fingerprint;
    sessions[index].mainAudioSyncFingerprint =
        ctx.audioTimelineDescriptor.m_mainAudioSyncFingerprint;
    ctx.isAudioTimelineFingerprintPublishPending = false;

    // 切换决策集中处理同主音轨时间继承和停止播放偏好；本函数只负责应用，
    // 避免在资源激活前后分别推导出不一致的目标时间。
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
    // 允许负视觉前置时间，但不能越过有效总时长；视觉偏移过大时上下界合并。
    double totalTime = SessionUtils::getEffectiveTotalTimeSeconds(ctx);
    double minTime   = -editorConfig.visual.getEffectiveVisualOffset();
    if ( minTime > totalTime ) minTime = totalTime;
    ctx.currentTime = std::clamp(ctx.currentTime, minTime, totalTime);
    if ( timelineReady ) {
        // transport 先定位再决定是否恢复播放，避免切换后短暂从旧位置出声。
        audio.seek(ctx.currentTime);
        if ( transferPlayback ) {
            audio.play();
            ctx.isPlaying = true;
        }
    } else {
        // Logo、空谱面或资源激活失败都必须卸载全局时间线；续播意图作废。
        audio.unloadAudioTimeline();
        transferPlayback = false;
    }

    ctx.animateTime =
        ctx.currentTime + editorConfig.visual.getEffectiveVisualOffset();
    // 切换后的动画状态直接对齐目标，避免上一会话的补间参数泄漏到新画布。
    ctx.animateTimeTarget                   = ctx.animateTime;
    ctx.animateTimeAnimationActive          = false;
    ctx.animatedTimelineZoom                = editorConfig.visual.timelineZoom;
    ctx.animatedTimelineZoomTarget          = ctx.animatedTimelineZoom;
    ctx.animatedTimelineZoomAnimationActive = false;
    ctx.currentTool = m_currentTool.load(std::memory_order_relaxed);
    // 打击特效属于会话时间位置的派生视觉状态，切换后由后续播放重新触发。
    ctx.hitFXSystem.clearActiveEffects();

    // 会话身份和音频指纹均已稳定后再发布；同步候选需基于新活动源重算。
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
    // 辅助视口可能在后台会话期间发生尺寸变化，切为活动项时补发最新快照。
    for ( const auto& [cameraId, size] : sharedViewportSizes ) {
        activeSession->pushCommand(
            CmdUpdateViewport{ cameraId, size.x, size.y });
    }
}

/// @brief 为当前活动谱面提交一次跨线程自动保存事件。
/// @param trigger 自动保存触发原因，供会话选择保存范围和反馈文案。
/// @note 请求仅进入会话队列或保存状态机，调用本身不在 UI 线程写文件。
/// @warning UI 到逻辑的低频入口：短暂持有注册表锁，只向活动会话发布请求。
void EditorEngine::requestAutoSaveForActiveSession(AutoSaveTrigger trigger)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    auto&         sessions    = m_sessionRegistry.entriesUnsafe();
    const int32_t activeIndex = m_sessionRegistry.activeIndex();
    if ( activeIndex < 0 ||
         activeIndex >= static_cast<int32_t>(sessions.size()) ||
         !sessions[static_cast<size_t>(activeIndex)].session ) {
        // 没有可保存活动谱面时忽略请求，不为 Logo 或空槽创建保存任务。
        return;
    }
    sessions[static_cast<size_t>(activeIndex)].session->requestAutoSave(
        trigger);
}

/// @brief 请求 UI 线程将指定 Session 对应的画布窗口聚焦到前台。
/// @param index 注册表中的目标会话索引。
/// @note 新请求覆盖尚未消费的旧请求，焦点只需反映最新用户打开目标。
/// @warning 可由逻辑打开路径写入、UI 每帧消费；relaxed 原子只传递一次性索引。
void EditorEngine::requestSessionFocus(int32_t index)
{
    m_pendingFocusSessionIndex.store(index, std::memory_order_relaxed);
}

/// @brief 消费一次待聚焦 Session 请求。
/// @return 待聚焦索引；没有请求时返回 -1，并原子清除已返回请求。
/// @warning UI 每帧可调用；exchange 不等待逻辑线程，也不验证索引仍然存在。
int32_t EditorEngine::consumePendingFocusSessionIndex()
{
    return m_pendingFocusSessionIndex.exchange(-1, std::memory_order_relaxed);
}

/// @brief 获取当前编辑器配置的线程安全值快照。
/// @return 配置按值副本，调用返回后不再受引擎内部更新影响。
/// @note 项目级自动备份覆盖不在此副本中组合，只供逻辑线程刷新入口应用。
/// @warning UI 热路径：只在调用期间持有配置互斥锁；调用者应在本帧复用副本。
Config::EditorConfig EditorEngine::getEditorConfig() const
{
    std::lock_guard<std::mutex> lock(m_editorConfigMutex);
    return m_editorConfig;
}

/// @brief 按修订号刷新逻辑线程持有的编辑器配置快照。
/// @param target 发生变化时接收最新逻辑线程配置。
/// @param targetRevision 调用方已知修订号，更新后写回当前修订。
/// @return 修订发生变化并复制配置时返回 true。
/// @note 返回 false 时 target 与 targetRevision
/// 保持调用前内容，便于循环直接复用。
/// @warning 逻辑热路径：未变化时仅执行一次 acquire
/// 原子读取，配置变化时才加锁复制。
bool EditorEngine::refreshEditorConfigSnapshot(
    Config::EditorConfig& target, std::uint64_t& targetRevision) const
{
    // 修订号先行读取让未变化的绝大多数逻辑轮次完全避开配置互斥锁。
    // acquire 与发布方的 release 配对，确认变化后才进入受锁保护的值复制。
    const std::uint64_t publishedRevision =
        m_editorConfigRevision.load(std::memory_order_acquire);
    if ( publishedRevision == targetRevision ) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_editorConfigMutex);
    // 加锁后复制最新值，并在逻辑线程局部应用项目覆盖；持久化的全局配置
    // 仍保持用户的软件级默认值，不被当前项目的覆盖反向改写。
    target = m_editorConfig;
    if ( m_projectAutoBackupOverride ) {
        target.settings.autoBackup = *m_projectAutoBackupOverride;
    }
    targetRevision = m_editorConfigRevision.load(std::memory_order_relaxed);
    return true;
}

/// @brief 合并软件级配置、项目工具栏状态并广播给全部会话。
/// @param config 调用方提交的编辑器配置候选值。
/// @warning 设置变更低频路径：依次持有配置更新锁、配置值锁和会话注册表锁，
/// 并发布事件；调用者不得在持有上述锁时重入此入口。
void EditorEngine::setEditorConfig(const Config::EditorConfig& config)
{
    // 外层递归锁串行化完整发布事务，防止两个设置来源交错覆盖 AppConfig、
    // 引擎缓存和会话命令队列中的不同版本。
    std::lock_guard<std::recursive_mutex> updateLock(m_editorConfigUpdateMutex);

    // 输入可能源于项目工作区恢复，只包含项目级工具栏偏好；近期项目、调色板
    // 等软件级状态必须从权威 AppConfig 回填，不能被旧工作区快照覆盖。
    const auto& globalConfig = Config::AppConfig::instance().getEditorConfig();
    const auto& globalRecent = globalConfig.recentProjects;
    const auto  globalColorPalettes = globalConfig.settings.colorPalettes;
    const auto  globalDefaultColorPalette =
        globalConfig.settings.defaultColorPaletteSchemeName;

    Config::EditorConfig updatedConfig = config;
    // 最近项目和调色板由全局设置页维护，不参与每项目工作区隔离。
    updatedConfig.recentProjects         = globalRecent;
    updatedConfig.settings.colorPalettes = globalColorPalettes;
    updatedConfig.settings.defaultColorPaletteSchemeName =
        globalDefaultColorPalette;
    auto& sfxConfig = updatedConfig.settings.sfxConfig;
    // 持久化或旧版本输入可能包含非有限增益，进入实时原子控制字前统一清洗。
    sfxConfig.unboundHitSfxGain =
        Config::sanitizeHitSfxGain(sfxConfig.unboundHitSfxGain);
    sfxConfig.boundHitSfxGain =
        Config::sanitizeHitSfxGain(sfxConfig.boundHitSfxGain);
    preserveGlobalAppManagedSettings(updatedConfig, globalConfig);
    // 限频偏好单独发布给逻辑循环的快速路径；完整配置仍通过修订快照传播。
    m_frameLimitPreference.store(updatedConfig.settings.frameLimit,
                                 std::memory_order_relaxed);
    if ( auto* project = ProjectController::instance().currentProject() ) {
        // 当前项目立即记录本次工具栏状态，后续项目切换可恢复独立偏好。
        // 这里只修改内存设置，实际项目文件保存仍由保存或关闭入口负责。
        captureToolbarWorkspaceState(
            project->m_settings.m_workspace,
            updatedConfig,
            m_syncSameMainAudioCanvases.load(std::memory_order_relaxed));
    }

    Config::EditorConfig sessionConfig = updatedConfig;
    {
        std::lock_guard<std::mutex> lock(m_editorConfigMutex);
        // 引擎缓存保存未应用项目覆盖的全局配置，供设置 UI 回读一致值。
        m_editorConfig = updatedConfig;
        if ( m_projectAutoBackupOverride ) {
            // 会话实际执行配置可接受项目级备份覆盖，但不能污染全局默认值。
            sessionConfig.settings.autoBackup = *m_projectAutoBackupOverride;
        }
        // 先完成值写入再 release 增加修订，使逻辑线程的 acquire 可见完整副本。
        m_editorConfigRevision.fetch_add(1, std::memory_order_release);
    }

    // AppConfig 接收合并后的软件级值；音效控制紧随其后更新实时播放行为。
    Config::AppConfig::instance().getEditorConfig() = updatedConfig;
    syncKeySoundControls(updatedConfig.settings.sfxConfig);

    // 每个会话通过自己的命令队列在 update 边界采用新配置，避免此线程直接
    // 改写其派生系统；广播使用已应用项目备份覆盖的 sessionConfig。
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

    // 会话命令全部排队后再通知 UI，订阅方读取引擎缓存时能看到同一版本。
    Event::EventBus::instance().publish(
        Event::EditorConfigChangedEvent{ updatedConfig });
}

/// @brief 更新当前项目对软件级谱面自动备份配置的可选覆盖。
/// @param config 项目覆盖；空值表示恢复软件级默认配置。
/// @warning 项目打开、关闭低频路径：只更新受锁配置派生状态和修订号。
/// @note 覆盖只影响逻辑线程下次取得的 sessionConfig，不写回 AppConfig。
void EditorEngine::setProjectAutoBackupOverride(
    std::optional<Config::AutoBackupConfig> config)
{
    std::lock_guard<std::mutex> lock(m_editorConfigMutex);
    // 覆盖不直接改写 m_editorConfig，refresh 快照时才组合到逻辑线程视图。
    m_projectAutoBackupOverride = std::move(config);
    m_editorConfigRevision.fetch_add(1, std::memory_order_release);
}

/// @brief 捕获当前画布工作区并保存项目配置。
/// @details
/// 工作区捕获与控制器保存处于同一个会话锁作用域，标签列表不会在两步之间
/// 改变。该便捷入口用于不需要向上传递失败的普通资源和工作区更新路径。
/// @warning 显式保存低频路径：持有会话递归锁并执行文件系统写入；控制器
/// 返回值当前不向上传递，要求需要失败处理的关闭流程直接调用控制器入口。
void EditorEngine::saveProject()
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    // 工作区必须先于项目序列化捕获，确保打开标签、活动时间和工具栏同批落盘。
    captureProjectWorkspaceState();
    ProjectController::instance().saveProject();
}

/// @brief 立即完成全部已打开会话中等待空闲期的元数据自动保存。
/// @return 所有有效会话均成功刷新元数据时返回 true。
/// @note 即使某个会话失败也继续处理其余会话，返回值是完整批次的合取结果。
/// @warning 低频阻塞路径：仅允许逻辑线程在打包或关闭项目前调用；会持有
/// Session 注册表锁并可能同步写入多个谱面。
bool EditorEngine::flushPendingMetadataAutoSaves()
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    /// @brief 需要检查尾随元数据保存的当前会话列表。
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    bool  success  = true;
    for ( auto& entry : sessions ) {
        // 逐会话继续尝试，单项失败不会阻止其他谱面的待保存元数据落盘。
        // 最终合并结果让关闭流程决定是否保留整个项目。
        if ( entry.session && !entry.session->flushPendingMetadataAutoSave() ) {
            success = false;
        }
    }
    return success;
}

/// @brief 在打包前保存所有已打开会话的完整未落盘谱面修改。
/// @return 所有有效会话均成功保存完整谱面时返回 true。
/// @note 与元数据刷新不同，该入口要求把撤销栈中的全部正文修改同步并写盘。
/// @warning
/// 低频阻塞路径：仅允许逻辑线程在打包前调用；会持有 Session
/// 注册表锁并同步写入多个谱面。
bool EditorEngine::saveDirtyBeatmapsForPackaging()
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    bool  success  = true;
    for ( auto& entry : sessions ) {
        // 打包必须看到所有内存编辑，而不只是元数据尾随保存；仍采用尽力保存，
        // 汇总失败后由打包入口整体取消，避免生成部分新旧内容混合的包。
        if ( entry.session && !entry.session->saveDirtyBeatmapForPackaging() ) {
            success = false;
        }
    }
    return success;
}

/// @brief 逻辑线程的主循环。
/// @details
/// 循环首先执行用户配置的 UPS 限频并累计真实 dt，然后在 Session 锁外消费
/// 项目切换。会话更新使用已发布快照稳定对象生命周期，按活动、可见实时需求、
/// 待命令和自动保存到期状态决定是否更新后台会话。Unlimited 模式通过空闲门控
/// 合并无工作轮次，但命令、播放和保存维护会立即唤醒。所有会话更新完成后，
/// 再统一传播同主音轨状态和光标视觉参数，最后处理非阻塞目录变更去抖。
/// @warning 逻辑热路径：按配置 UPS 频率执行；每个实际 Session update 会持有
/// SessionRegistry 递归锁，以与 UI 的低频 SessionContext/ECS 读取串行化；锁内
/// 普通路径禁止等待、文件系统操作、额外完整 entt 遍历、完整排序、try/catch
/// 和可避免的 shared_ptr 拷贝。Unlimited 无播放命令的视觉维护轮次必须在锁
/// 外合并，播放时钟和待处理命令不得受门控限制。自动保存、自动备份与元数据
/// 尾随保存仅允许在既有低频到期或事件分支阻塞。
void EditorEngine::loop()
{
    // deadline 独立于上一轮实际结束时刻推进，稳定固定 UPS 的长期节拍；
    // lastTime 只用于计算传给会话的真实经过时间。
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
        // 限频偏好允许 UI 即时修改；设备刷新率参与 VSync 倍率到 UPS 的换算。
        // 本轮读取值固定使用到下个循环，避免同轮截止时间采用两套目标。
        Config::FrameLimitPreference frameLimit =
            m_frameLimitPreference.load(std::memory_order_relaxed);
        const int refreshRate =
            Config::AppConfig::instance().getDeviceRefreshRate();
        const double targetDt =
            Config::frameLimitTargetInterval(frameLimit, refreshRate);

        // currentTime 在等待前后各采样一次；后续 dt 使用等待完成后的真实时刻。
        auto currentTime = FrameLimitClock::now();

        if ( targetDt > 0.0 ) {
            // duration 转换到 steady_clock 原生精度，避免用整数毫秒截断高 UPS。
            const auto targetDuration =
                std::chrono::duration_cast<FrameLimitClock::duration>(
                    std::chrono::duration<double>(targetDt));

            if ( targetDt != lastTargetDt ) {
                // 配置变化时从当前时刻重新建截止线，不追赶旧频率遗留的相位。
                nextDeadline = currentTime + targetDuration;
                lastTargetDt = targetDt;
            }

            if ( currentTime < nextDeadline ) {
                // 仅帧率限制允许等待；业务状态、命令和同步均不得使用此机制。
                sleepUntilFrameDeadline(nextDeadline);
                currentTime = FrameLimitClock::now();
            }

            if ( currentTime - nextDeadline > targetDuration ) {
                // 落后超过一个完整周期时丢弃欠下的节拍，避免连续零间隔追帧。
                nextDeadline = currentTime + targetDuration;
            } else {
                // 正常轻微超时时仍沿原时间线推进，减少调度抖动造成的漂移。
                nextDeadline += targetDuration;
            }
        } else {
            // Unlimited 不设未来截止线，由空闲门控决定无工作时的最低维护频率。
            nextDeadline = currentTime;
            lastTargetDt = targetDt;
        }

        std::chrono::duration<double> passed = currentTime - lastTime;
        // steady_clock 保证非负单调间隔；不使用系统墙钟，时间校准不会跳动画。
        lastTime  = currentTime;
        double dt = passed.count();
        // 门控期间累计真实时间，下一次实际 poll 一次性消费，时间轴不会变慢。
        unlimitedIdleUpdateGate.accumulateElapsedSeconds(dt);

        // 半秒窗口平衡统计响应速度与抖动，结果仅供快照预算回压和性能展示。
        m_logicUpdateCount++;
        std::chrono::duration<double> upsElapsed = currentTime - m_lastUpsTime;
        if ( upsElapsed.count() >= 0.5 ) {
            // relaxed 原子只传播近似统计值，不作为任何会话状态的发布屏障。
            m_logicUps.store(
                static_cast<float>(m_logicUpdateCount / upsElapsed.count()),
                std::memory_order_relaxed);
            m_logicUpdateCount = 0;
            m_lastUpsTime      = currentTime;
        }

        // 项目切换在会话轮询锁外执行，避免文件 IO 与 EventBus
        // 回调占用热路径锁。
        // 控制器将多来源请求合并成本轮唯一动作，由这里按关闭再打开的顺序执行。
        /// @brief 项目控制器消费出的本轮项目打开或关闭动作。
        ProjectController::PendingProjectAction projectAction;
        if ( projectController.hasPendingProjectAction() ) {
            // 先读轻量提示再消费，避免无项目动作时每轮构造控制器内部事务。
            projectAction = projectController.consumePendingProjectAction(
                needsCanvasCloseBeforeProjectOpen());
        }
        bool projectCloseSucceeded = true;
        if ( projectAction.m_closeProject ) {
            // 旧项目保存失败时本轮不得继续打开新目标，当前编辑状态保持可恢复。
            projectCloseSucceeded = closeProject();
        }
        if ( projectCloseSucceeded &&
             !projectAction.m_projectPathToOpen.empty() ) {
            if ( projectAction.m_projectOpenMode ==
                 ProjectController::ProjectOpenMode::TemporaryPackage ) {
                // 临时谱包使用独立准备和只读生命周期，不能走普通目录打开入口。
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
        // 配置刷新位于项目动作之后，新项目自动备份覆盖可在同轮进入 Session
        // 更新。

        // 项目动作可能恢复工作区并改变会话集合，因此在其完成后才获取发布快照。
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
            // 命令存在时 Unlimited
            // 门控必须立刻放行；检查只读取各队列的轻量提示， 实际命令仍由对应
            // Session::update 在注册表锁内消费。
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
                // 从上次实际 poll 起累计的时间只消费一次，跳过的空闲轮次不丢
                // dt。
                const double sessionDt =
                    unlimitedIdleUpdateGate.consumeElapsedSeconds();
                int32_t activeIndex     = m_sessionRegistry.activeIndex();
                int32_t maxSessionIndex = -1;
                // 快照索引可能因关闭留下空洞，后台时间表按最大稳定索引扩展，
                // 不能假设向量位置与注册表索引连续相等。
                for ( const auto& entry : sessionUpdateSnapshot ) {
                    maxSessionIndex = std::max(maxSessionIndex, entry.index);
                }
                if ( maxSessionIndex >= 0 &&
                     m_backgroundSessionUpdateTimes.size() <=
                         static_cast<size_t>(maxSessionIndex) ) {
                    // 新槽默认 time_point
                    // 表示从未更新，首次后台轮询会立即执行。
                    m_backgroundSessionUpdateTimes.resize(
                        static_cast<size_t>(maxSessionIndex) + 1);
                }

                const auto backgroundInterval =
                    backgroundSessionUpdateInterval(refreshRate);
                // 后台间隔只节流隐藏或无实时需求会话，不限制活动和可见动画。
                /// @brief 本轮由指令驱动发生时间变化的
                /// Session，用于同主音轨同步。
                int32_t commandSyncSourceIndex = -1;
                /// @brief 本轮结束后是否仍有播放时钟需逐 update 推进。
                bool hasUnlimitedSessionWork = false;
                for ( const auto& entry : sessionUpdateSnapshot ) {
                    // shared_ptr 由发布快照保活，注册表锁只保护 SessionContext
                    // 与 UI 低频读取的串行访问，不用于对象生命周期管理。
                    std::lock_guard<std::recursive_mutex> sessionLock(
                        m_sessionRegistry.mutex());
                    const bool isActiveSession = entry.index == activeIndex;
                    // 活动会话即使窗口暂时隐藏也必须推进 transport 和命令队列。
                    const bool isVisibleSession =
                        isActiveSession || entry.isCanvasVisible;
                    const bool hadPendingCommands =
                        entry.session->hasPendingCommands();
                    const bool hasPendingMetadataAutoSave =
                        entry.session->hasPendingMetadataAutoSave();
                    // 元数据尾随保存和完整自动保存使用不同提示，任一到期均需轮询。
                    const bool needsAutoSavePolling =
                        entry.session->needsAutoSavePolling(
                            editorConfigSnapshot.settings.autoSave,
                            editorConfigSnapshot.settings.autoBackup);
                    bool shouldUpdateSession = isActiveSession;
                    if ( !shouldUpdateSession ) {
                        // 活动会话始终更新；后台可见动画、待命令和到期保存各自
                        // 能独立唤醒，完全隐藏且无维护任务的会话直接跳过。
                        const bool needsRealtimeUpdate =
                            entry.session->needsRealtimeUpdate();
                        if ( isVisibleSession && needsRealtimeUpdate ) {
                            // 可见后台画布的动画与同步视觉必须跟随本轮逻辑节拍。
                            shouldUpdateSession = true;
                        } else if ( hadPendingCommands ) {
                            // 命令不能等待后台节流周期，否则滚轮和外部操作会延迟。
                            shouldUpdateSession = true;
                        } else if ( !isVisibleSession &&
                                    !hasPendingMetadataAutoSave &&
                                    !needsAutoSavePolling ) {
                            shouldUpdateSession = false;
                        } else {
                            // 元数据尾随保存或自动备份需要低频轮询，按设备刷新率
                            // 限制维护频率，既保证到期又避免隐藏会话持续满速运行。
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
                        // 更新前保存播放位置，只在活动会话确由命令改变时间时，
                        // 才把它登记为本轮同主音轨同步来源。
                        const double previousCurrentTime =
                            entry.session->getContext().currentTime;
                        entry.session->update(
                            sessionDt, editorConfigSnapshot, isActiveSession);
                        // update 可能处理
                        // seek、载图与播放命令，随后读取的是最终位置。
                        if ( isActiveSession && hadPendingCommands &&
                             std::abs(entry.session->getContext().currentTime -
                                      previousCurrentTime) >
                                 MAIN_AUDIO_SYNC_TIME_EPSILON ) {
                            commandSyncSourceIndex = entry.index;
                        }
                        if ( entry.index != activeIndex ) {
                            // 只有实际执行 update
                            // 才刷新节流时间，跳过不会推迟下次机会。
                            m_backgroundSessionUpdateTimes[static_cast<size_t>(
                                entry.index)] = currentTime;
                        }
                    }
                }
                if ( m_pendingWorkspaceActiveIndex >= 0 ) {
                    // 工作区恢复先让各新会话消费
                    // Load/Seek，再切活动项并装载音频，
                    // 防止目标会话仍处于默认时间时接管全局 transport。
                    int32_t requestedActiveIndex =
                        m_pendingWorkspaceActiveIndex;
                    m_pendingWorkspaceActiveIndex = -1;
                    setActiveSessionIndex(requestedActiveIndex);
                    activeIndex = m_sessionRegistry.activeIndex();
                }

                if ( commandSyncSourceIndex >= 0 ) {
                    // 命令驱动的 seek
                    // 在同轮立即传播，避免等待常规活动源检测一轮。
                    syncSameMainAudioCanvasesFromIndex(commandSyncSourceIndex);
                }

                bool  shouldSyncMainAudioCanvases = false;
                float cursorSmokeLifeOverride     = -1.0F;
                {
                    // 第二段锁内读取发生在所有会话 update
                    // 之后，得到本轮最终状态； 不在第一段循环内同步，避免
                    // follower 先后顺序影响结果。
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

                        // 播放中每轮传播连续时间；暂停时只有身份或位置变化才传播。
                        const auto& activeCtx = entry.session->getContext();
                        shouldSyncMainAudioCanvases =
                            activeCtx.isPlaying ||
                            activeIndex != m_lastMainAudioSyncActiveIndex ||
                            std::abs(activeCtx.currentTime -
                                     m_lastMainAudioSyncTime) >
                                MAIN_AUDIO_SYNC_TIME_EPSILON;
                        if ( shouldSyncMainAudioCanvases ) {
                            // 记录最近已传播的活动身份和时间，暂停且未移动时跳过扫描。
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
                    // 在读取锁作用域结束后调用同步入口，由其自行取得所需锁。
                    syncSameMainAudioCanvases();
                }
                // 烟雾寿命是 UI 视觉提示的近似值，不承担谱面 BPM 状态同步。
                m_cursorSmokeLifeOverride.store(cursorSmokeLifeOverride,
                                                std::memory_order_relaxed);
                // 门控在全部派生状态发布后记录本轮完成时刻，避免把处理耗时算作空闲。
                unlimitedIdleUpdateGate.completePoll(FrameLimitClock::now(),
                                                     hasUnlimitedSessionWork);
            }
        } else {
            // 没有会话时丢弃累计 dt；未来新会话不能继承空窗期的巨大更新时间。
            unlimitedIdleUpdateGate.requestPoll();
            unlimitedIdleUpdateGate.discardElapsedSeconds();
            m_cursorSmokeLifeOverride.store(-1.0f, std::memory_order_relaxed);
            // 空会话是唯一固定短睡路径，只降低空转 CPU，不等待任何业务状态。
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // 目录监视器只发布轻量待处理标记；真正扫描留在逻辑线程低频尾部。
        // 静态状态属于唯一 EditorEngine 循环实例，并随线程重启保留最近默认值。
        static auto lastChangeTime   = FrameLimitClock::now();
        static bool hasPendingChange = false;

        if ( projectController.consumeDirectoryChangePending() ) {
            // 连续事件只覆盖最后时间，不排队每个中间状态，批量复制最终只扫一次。
            hasPendingChange = true;
            lastChangeTime   = FrameLimitClock::now();
        }

        if ( hasPendingChange ) {
            // hasPendingChange 跨轮保留，不用 sleep
            // 等待静止窗口，也不阻塞播放更新。
            auto now = FrameLimitClock::now();
            // 200ms
            // 为非阻塞去抖：循环和会话仍正常推进，仅推迟低频完整目录扫描。
            // 每次新事件覆盖
            // lastChangeTime，扫描看到的是批量写入静止后的最终状态。
            if ( std::chrono::duration<double>(now - lastChangeTime).count() >=
                 0.2 ) {
                XINFO(
                    "Directory Watcher: filesystem changes settled, rescanning "
                    "directory...");
                scanProjectDirectory();
                // 扫描完成后才清除标记；扫描期间到达的新通知会在下一轮重新置位。
                hasPendingChange = false;
            }
        }
    }
}

/// @brief 处理音频资源更新指令并执行音效登记和项目保存副作用。
/// @param cmd 目标资源 ID 与新的轨道类型或属性。
/// @details
/// 更新前同步全部打开会话的内存引用，使 Effect 转 Main 等受限变化不会绕过
/// 尚未保存的玩家绑定。控制器完成项目模型修改后，本函数更新 AudioManager
/// 的 Effect 登记、当前音频画笔类型，并标记组合时间线描述符等待重建。
/// 成功与失败均通过结构化事件返回，失败时包含所有阻塞谱面路径。
/// @warning 低频项目资源事务：持有 SessionRegistry 递归锁，可能扫描打开会话、
/// 保存项目并更新音频运行时，不得从每帧属性预览调用。
/// @post 成功时项目资源表、音频画笔类型和会话描述符脏状态保持一致。
void EditorEngine::handleUpdateAudioResource(const CmdUpdateAudioResource& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    auto* project = ProjectController::instance().currentProject();
    if ( !project ) {
        // 没有项目时资源 ID 没有解释域，直接发布失败而不访问会话或音频池。
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::UpdateType,
            cmd.id,
            false,
            {},
            "当前没有可更新音频资源的项目");
        return;
    }

    auto& sessions = m_sessionRegistry.entriesUnsafe();
    // 控制器的引用门禁同时使用磁盘谱面扫描和这里提供的未保存内存引用。
    const auto openBeatmapReferences =
        collectOpenSessionAudioReferencesUnsafe(*project, sessions);

    /// @brief 项目命令服务的音频资源更新结果。
    auto result = ProjectController::instance().updateAudioResource(
        cmd, openBeatmapReferences);
    if ( !result.m_updated ) {
        // 有阻塞路径时失败来自玩家物件绑定门禁，否则按目标资源不存在解释。
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
        // Effect 改为其他类型时先移除独立音效登记，防止旧键继续可播放。
        Audio::AudioManager::instance().unloadSoundEffect(
            *result.m_effectResourceIdToUnload);
    }
    if ( result.m_effectRegistration ) {
        // 新变为 Effect 或属性变化的资源用控制器解析后的绝对路径重新登记。
        Audio::AudioManager::instance().registerSoundEffect(
            result.m_effectRegistration->m_resource.m_id,
            Config::pathToUtf8(result.m_effectRegistration->m_absolutePath),
            result.m_effectRegistration->m_resource.m_config);
    }
    if ( m_brushAudioResourceId == cmd.id ) {
        // 当前画笔只缓存 ID、类型和音量；资源类型变化后必须刷新类型语义。
        const auto resourceIterator =
            std::find_if(project->m_audioResources.begin(),
                         project->m_audioResources.end(),
                         [&](const AudioResource& resource) {
                             return resource.m_id == cmd.id;
                         });
        if ( resourceIterator != project->m_audioResources.end() ) {
            m_brushAudioTrackType = resourceIterator->m_type;
            // 所有现有会话立即接收相同画笔状态；未来会话从引擎缓存恢复。
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
    // 轨道类型变化可改变资源在 Main、Sample 或 Effect 管线中的角色，
    // 使用全量标脏而不是仅按旧描述符引用筛选。
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
/// @details
/// 重命名同时涉及物理文件、项目资源表、磁盘谱面、打开会话 ECS、工作区引用、
/// 音效池和当前音频画笔。流程先完成所有无副作用校验，再改名文件、迁移路径，
/// 最后以资源服务事务更新磁盘谱面 ID。磁盘谱面失败时按相反方向恢复文件与
/// 路径状态；只有这一阶段成功才提交其余内存身份并保存项目。
/// @warning 低频项目资源路径：执行文件系统改名、谱面引用事务写回和
/// 已打开会话增量同步，禁止从每帧热路径调用。
/// @post 成功时旧资源 ID 不再出现在项目工作区、打开 ECS 或 Effect 登记中。
void EditorEngine::handleRenameAudioResource(const CmdRenameAudioResource& cmd)
{
    // 文件、项目模型、打开会话和 AudioManager 必须作为一个串行事务更新；
    // 注册表递归锁也允许内部 saveProject 复用工作区捕获入口。
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
        // 在接触文件系统前验证资源身份，避免陈旧 UI 指令误改同名外部文件。
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Rename,
            cmd.id,
            false,
            {},
            "未找到要重命名的音频资源");
        return;
    }
    if ( !isValidAudioResourceFileName(cmd.newFileName) ) {
        // 输入仅允许文件名，目录移动由专门的文件系统重映射入口处理。
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Rename,
            cmd.id,
            false,
            {},
            "文件名不能为空、点目录或包含路径分隔符");
        return;
    }

    // 保留移动前完整快照，后续谱面兼容匹配和失败回滚仍需要旧 ID 与旧路径。
    const AudioResource previousResource = *resourceIterator;
    const auto storedPath = Config::utf8ToPath(resourceIterator->m_path);
    const auto oldPath =
        (storedPath.is_absolute() ? storedPath
                                  : project->m_projectRoot / storedPath)
            .lexically_normal();
    auto requestedFileName = Config::utf8ToPath(cmd.newFileName);
    if ( requestedFileName.extension().empty() ) {
        // UI 可只提交基础名称；未显式给扩展名时继承源格式，不触发转码语义。
        requestedFileName += oldPath.extension();
    }
    if ( requestedFileName.extension() != oldPath.extension() ) {
        // 重命名不等于格式转换，扩展名变化会使内容与解码器选择不一致。
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
    // 资源 ID 与实际文件名保持一致，统一供谱面引用、工作区和音效池查找。
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
        // 完全相同的目标视为幂等成功，不产生磁盘写入和会话脏标记。
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
        // 即使文件系统目标不存在，项目级 ID 也必须唯一，否则绑定解析有歧义。
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
    // 所有文件系统查询使用 error_code，权限或编码错误按普通失败事件返回。
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
        // 不覆盖用户已有文件；存在性检查本身失败也按不安全目标处理。
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
        // 先校验隐式路径引用，确保文件改名后所有受支持谱面格式仍可表达资源。
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
        // 文件改名是第一个实际副作用；失败时内存模型尚未改变，无需回滚。
        publishAudioResourceMutationResult(
            Event::AudioResourceMutationOperation::Rename,
            cmd.id,
            false,
            {},
            "重命名音频文件失败：" + filesystemError.message());
        return;
    }

    std::string pathRemapError;
    // 先把项目资源路径从旧文件位置重映射到新位置；成功数量应恰为目标资源。
    // 该步骤同步打开会话中的路径兼容引用，但资源 ID 尚保持旧值。
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
        // 谱面文件中的稳定 ID 事务失败后，尽力按相反方向恢复物理文件和路径表。
        // 回滚诊断必须区分文件名失败、路径状态失败和完整成功三种情况。
        filesystemError.clear();
        std::filesystem::rename(newPath, oldPath, filesystemError);
        std::string rollbackError;
        if ( !filesystemError ) {
            (void)remapAudioResourcePathsAfterMove(
                newPath, oldPath, &rollbackError);
        }
        std::string errorMessage = beatmapIdRemap.m_errorMessage;
        if ( filesystemError ) {
            // 物理文件留在新位置，错误消息必须提示需要用户手工处理。
            errorMessage +=
                "；文件名自动回滚失败：" + filesystemError.message();
        } else if ( !rollbackError.empty() ) {
            // 文件已恢复但项目内路径表可能不一致，保留路径回滚的具体原因。
            errorMessage += "；路径状态回滚失败：" + rollbackError;
        } else {
            // 完整回滚后仍报告原事务失败，调用者可安全重试或更换名称。
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
    // 磁盘谱面引用事务成功后才提交项目资源主键，避免中途暴露新旧混合身份。
    resourceIterator->m_id = newResourceId;

    // 项目目录条目保存 Main 轨道身份，必须跟随资源主键一并迁移。
    for ( auto& beatmapEntry : project->m_beatmaps ) {
        if ( beatmapEntry.m_audioTrackId == cmd.id ) {
            beatmapEntry.m_audioTrackId = newResourceId;
        }
    }
    auto& workspace = project->m_settings.m_workspace;
    // 工作区中的选择、测量工具、控制器和放置记录都属于持久化引用，
    // 任一遗漏都会在重启后重新引入不存在的旧资源 ID。
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
        // 磁盘谱面已由服务事务更新，打开会话还持有独立内存域和 ECS 副本，
        // 必须原地迁移以免下一次保存把旧 ID 写回文件。
        if ( entry.isLogoPlaceholder || !entry.session ) continue;
        auto& ctx = entry.session->getContextMutable();
        if ( !ctx.currentBeatmap ) continue;

        const auto domainChanged =
            ProjectResourceService::remapBeatmapAudioResourceId(
                *ctx.currentBeatmap, cmd.id, newResourceId);
        // ECS 中玩家绑定与自动采样分别计数，后续只重建实际受影响的派生缓存。
        const auto ecsChanged =
            remapSessionEcsAudioResourceId(ctx, cmd.id, newResourceId);
        if ( ecsChanged.m_changedNoteBindingCount > 0U ) {
            // 玩家绑定变化影响命中事件缓存，即使音符几何本身没有变化也需重建。
            ctx.m_needsNotesSync = true;
            SessionUtils::markHitEventsDirty(ctx);
        }
        if ( ecsChanged.m_changedAudioSampleCount > 0U ) {
            // 自动采样 Registry 需要独立同步回 BeatMap 的时间线数据域。
            ctx.m_needsSamplesSync = true;
        }
        if ( ecsChanged.m_changedNoteBindingCount > 0U ||
             ecsChanged.m_changedAudioSampleCount > 0U ) {
            // 所有 ECS 标志设置完成后只做一次聚合同步，避免分别序列化两个域。
            SessionUtils::syncBeatmap(ctx);
        }
        if ( domainChanged > 0U ||
             ecsChanged.m_audioSampleReferenceCount > 0U ) {
            // 歌曲提示或自动采样引用变化会改变组合时间线，延后到安全更新点激活。
            ctx.isAudioTimelineDescriptorDirty   = true;
            ctx.isAudioTimelineActivationPending = true;
        }
    }

    auto& audio = Audio::AudioManager::instance();
    if ( renamedType == AudioTrackType::Effect ) {
        // 音效池以资源 ID 为键，先卸载旧键再用新路径登记相同配置。
        audio.unloadSoundEffect(cmd.id);
        audio.registerSoundEffect(
            newResourceId, Config::pathToUtf8(newPath), renamedConfig);
    }
    if ( m_brushAudioResourceId == cmd.id ) {
        // 全局画笔选择和每会话缓存都要切换到新 ID，保持下一笔放置可解析。
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
    // 通用脏标记覆盖主音轨、歌曲提示等未由局部计数识别的项目级变化。
    markAudioTimelineDescriptorsDirtyUnsafe();
    // 所有内存域、音效池和文件事务成功后才保存项目并发布成功结果。
    saveProject();
    publishAudioResourceMutationResult(
        Event::AudioResourceMutationOperation::Rename,
        newResourceId,
        true,
        {},
        {});
}

/// @brief 更新音频资源配置并使所有引用它的已打开时间线失效。
/// @param cmd 目标资源 ID 与新的解码、增益等配置。
/// @warning 低频资源设置路径：持有 SessionRegistry 递归锁，可能重新登记音效
/// 并保存项目；时间线重建延后到各会话安全更新点。
/// @post 成功时资源配置已保存，引用该资源的会话至少带有描述符脏标记。
void EditorEngine::handleUpdateAudioResourceConfig(
    const CmdUpdateAudioResourceConfig& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    auto* project = ProjectController::instance().currentProject();
    if ( !project ) {
        // 配置命令没有独立结果事件，使用日志记录陈旧或无项目调用。
        XWARN("Cannot update audio resource config without an open project");
        return;
    }

    const auto resourceIterator = std::find_if(
        project->m_audioResources.begin(),
        project->m_audioResources.end(),
        [&](const AudioResource& resource) { return resource.m_id == cmd.id; });
    if ( resourceIterator == project->m_audioResources.end() ) {
        // UI 可能持有目录刷新前的旧 ID；不创建新资源替代缺失目标。
        XWARN("Cannot update missing audio resource config: {}", cmd.id);
        return;
    }

    // 先提交项目模型；Effect 的运行时描述随后以同一配置覆盖登记。
    resourceIterator->m_config = cmd.config;
    if ( resourceIterator->m_type == AudioTrackType::Effect ) {
        // registerSoundEffect 对现有 ID 执行配置刷新，不在此强制预解码音频。
        const auto absolutePath = project->m_projectRoot /
                                  Config::utf8ToPath(resourceIterator->m_path);
        Audio::AudioManager::instance().registerSoundEffect(
            cmd.id, Config::pathToUtf8(absolutePath), cmd.config);
    }

    // 只标脏引用目标资源的时间线，未使用该资源的后台谱面无需重建。
    markAudioTimelineDescriptorsDirtyUnsafe(cmd.id);
    saveProject();
}

/// @brief 处理删除音频资源指令并执行音效卸载和项目保存副作用。
/// @param cmd 待删除资源 ID。
/// @details
/// 删除门禁使用磁盘谱面与打开会话的合并引用，任何玩家绑定、自动采样或格式
/// 隐式引用都可阻止资源移除。控制器成功删除项目模型和文件后，本函数卸载
/// Effect、清除当前画笔选择、标脏所有组合时间线并保存项目。失败不修改运行时
/// 选择，结构化结果事件会携带服务返回的具体阻塞谱面路径。
/// @warning 低频项目资源路径：持有 SessionRegistry 递归锁，可能同步脏会话、
/// 扫描磁盘谱面引用、删除文件并保存项目。
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

    auto& sessions = m_sessionRegistry.entriesUnsafe();
    // 磁盘扫描前先同步打开会话的 ECS，删除门禁必须覆盖尚未保存的新引用。
    const auto openBeatmapReferences =
        collectOpenSessionAudioReferencesUnsafe(*project, sessions);

    /// @brief 项目命令服务的音频资源删除结果。
    auto result = ProjectController::instance().removeAudioResource(
        cmd, openBeatmapReferences);
    if ( !result.m_removed ) {
        // 优先使用服务层具体失败原因；没有错误文本时再按引用结果区分提示。
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
        // 只有实际 Effect 删除结果才触发运行时卸载，Main 轨道由描述符重建处理。
        Audio::AudioManager::instance().unloadSoundEffect(
            *result.m_effectResourceIdToUnload);
    }
    if ( m_brushAudioResourceId == cmd.id ) {
        // 删除当前画笔资源后恢复无选择默认状态，并同步清理工作区持久化选择。
        m_brushAudioResourceId.clear();
        m_brushAudioTrackType = AudioTrackType::Effect;
        m_brushAudioVolume    = 1.0F;
        project->m_settings.m_workspace.m_projectAudioToolSelectedResourceId
            .clear();
        for ( auto& entry : sessions ) {
            if ( entry.session ) {
                // 空资源命令显式清除会话内部选择，不能仅修改引擎缓存等待重建。
                entry.session->pushCommand(
                    LogicCommand(CmdSetBrushAudioResource{
                        {},
                        AudioTrackType::Effect,
                        1.0F,
                    }));
            }
        }
    }
    // 删除可能改变 Main 或自动采样组合，所有会话在下一安全点重新验证描述符。
    markAudioTimelineDescriptorsDirtyUnsafe();
    // 控制器删除、运行时卸载与画笔清理全部完成后才持久化成功状态。
    saveProject();
    publishAudioResourceMutationResult(
        Event::AudioResourceMutationOperation::Remove, cmd.id, true, {}, {});
}

/// @brief 处理删除谱面指令并在项目发生变化时保存。
/// @param cmd 项目中的谱面路径及删除选项。
/// @details
/// 控制器确认项目清单或磁盘发生变化后，本函数用删除前计算的稳定路径键寻找
/// 所有已打开副本。命中的会话按索引逆序关闭，最终只捕获一次工作区，避免
/// erase 引起的索引移动和多次项目写入。找不到打开副本仍需保存项目删除结果。
/// @warning 低频项目操作路径：可能删除谱面文件、关闭多个会话并保存工作区。
/// @post 成功变化后所有命中删除路径的打开会话均已关闭。
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
        // 控制器未改变项目时保持所有打开会话，不根据请求路径自行猜测删除结果。
        return;
    }

    /// @brief 需要同步关闭的已打开谱面 Session 索引。
    std::vector<int32_t> sessionsToClose;
    if ( !removedBeatmapKey.empty() ) {
        // 路径键优先使用条目缓存；旧会话缺少缓存时才从内存谱面补算兼容值。
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
                // 先收集索引再统一关闭，避免遍历中 erase 改变后续条目位置。
                sessionsToClose.push_back(i);
            }
        }
    }

    // 逆序关闭保持较小索引在删除前仍指向原会话；逐项关闭不重复捕获工作区。
    for ( auto it = sessionsToClose.rbegin(); it != sessionsToClose.rend();
          ++it ) {
        closeSession(*it, false);
    }
    if ( !sessionsToClose.empty() ) {
        // 最终会话集合稳定后只捕获一次工作区，移除所有已删除谱面的标签状态。
        captureProjectWorkspaceState();
    }

    saveProject();
}

/// @brief 将当前临时项目保存到正式项目目录。
/// @param cmd 保存临时项目指令。
/// @details
/// 保存前把所有打开谱面元数据规范化到临时项目根并捕获完整工作区，控制器
/// 随后复制项目并切换 currentProject。成功后再次以正式目录为根规范化内存
/// 谱面、重建稳定路径键与音频指纹，并把更新后的工作区写入正式项目配置。
/// 失败时 currentProject 仍保持临时身份。结果事件在注册表解锁后统一发布。
/// @warning 用户另存低频路径：复制项目树并更新全部路径身份，持有注册表锁期间
/// 可能执行大量文件系统操作；结果事件在解锁后发布以避免回调重入。
void EditorEngine::handleSaveTemporaryProject(
    const CmdSaveTemporaryProject& cmd)
{
    ProjectController::SaveTemporaryProjectResult result;
    {
        // 事务内保持会话和当前项目身份稳定，控制器完成切换前 UI 不会看到
        // 一半仍指向临时缓存、一半已经指向目标目录的路径。
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
                    // 复制前统一为当前临时项目相对路径，目标目录可整体搬迁资源。
                    normalizeBeatmapMetadataPathsForProject(*ctx.currentBeatmap,
                                                            *currentProject);
                }
            }
        }

        // 工作区与谱面元数据一起成为复制源，另存目标能恢复当前标签和位置。
        captureProjectWorkspaceState();
        result = ProjectController::instance().saveTemporaryProjectTo(
            Config::utf8ToPath(cmd.destinationPath));

        if ( result.m_success ) {
            // 控制器成功后 currentProject
            // 已切到正式目录，需要按新根重建会话键。
            if ( const auto* currentProject =
                     ProjectController::instance().currentProject() ) {
                auto& sessions = m_sessionRegistry.entriesUnsafe();
                for ( auto& entry : sessions ) {
                    if ( entry.isLogoPlaceholder || !entry.session ) {
                        // Logo 不关联任何谱面文件，确保不会遗留临时缓存路径键。
                        entry.beatmapPathKey.clear();
                        continue;
                    }

                    const auto& ctx = entry.session->getContext();
                    if ( !ctx.currentBeatmap ) {
                        // 异常空会话同样清除键，避免查重把它当作已打开谱面。
                        entry.beatmapPathKey.clear();
                        continue;
                    }

                    normalizeBeatmapMetadataPathsForProject(*ctx.currentBeatmap,
                                                            *currentProject);
                    // 新键以正式项目根解释；旧临时缓存即使尚未清理也不再参与身份。
                    entry.beatmapPathKey = makeBeatmapPathKey(
                        currentProject,
                        ctx.currentBeatmap->m_baseMapMetadata.map_path);
                }
            }
            // 项目根改变后音频路径指纹、同步候选和工作区路径都必须重新发布。
            refreshAudioTimelineFingerprintsUnsafe();
            captureProjectWorkspaceState();
            ProjectController::instance().saveProject();
        }
    }

    // 解锁后发布最终结果，UI 回调可安全查询新项目或继续处理失败的临时项目。
    Event::TemporaryProjectSaveResultEvent event;
    event.m_success          = result.m_success;
    event.m_savedProjectPath = Config::pathToUtf8(result.m_savedProjectPath);
    event.m_errorMessage     = result.m_errorMessage;

    Event::EventBus::instance().publish(event);
}

/// @brief 更新项目内谱面文件路径关联并在项目发生变化时保存。
/// @param oldPath 文件系统移动前的谱面路径。
/// @param newPath 文件系统移动后的谱面路径。
/// @details
/// 项目控制器更新持久化谱面条目后，运行时会话仍可能保存旧的查重键。本函数
/// 精确替换命中旧键的条目，并无条件刷新音频时间线指纹，因为谱面目录变化可
/// 改变相对音频引用解析，即使项目清单判断为没有结构变化也不能跳过。
/// @warning 低频目录变更路径：可能保存项目并刷新会话音频同步身份。
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
        // 先保存项目条目的新路径；会话键随后只影响运行时查重和工作区捕获。
        saveProject();
    }

    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    if ( !oldBeatmapKey.empty() && !newBeatmapKey.empty() ) {
        // 仅精确命中旧键的打开会话随文件移动，其他同名谱面不受影响。
        for ( auto& entry : sessions ) {
            if ( entry.beatmapPathKey == oldBeatmapKey ) {
                entry.beatmapPathKey = newBeatmapKey;
            }
        }
    }
    // 谱面目录变化可能改变相对音频解析结果，因此即使条目未变也刷新指纹。
    refreshAudioTimelineFingerprintsUnsafe();
}

/// @brief 在文件系统操作前验证外部谱面的音频引用可安全保持。
/// @param oldPath 计划移动的文件或目录路径。
/// @param newPath 计划移动到的文件或目录路径。
/// @return 允许移动时为空；否则返回可直接展示的阻止原因。
/// @details
/// 校验覆盖项目资源路径、osu! 等外部格式的相对音频引用，以及 RM/IMD 的隐式
/// 目录关联。它不执行移动或重映射，文件浏览器必须在收到空错误后自行移动，
/// 再调用 remapAudioResourcePathsAfterMove 提交项目运行时状态。
/// @warning 低频文件操作路径：会读取项目中的 osu! 谱面并检查全部
/// RM/IMD 隐式音频关联。
std::string EditorEngine::validateAudioResourceMove(
    const std::filesystem::path& oldPath, const std::filesystem::path& newPath)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    const auto* project = ProjectController::instance().currentProject();
    // 未打开项目时文件移动不涉及项目音频引用，由普通文件浏览操作自行处理。
    if ( !project ) return {};
    // 只做校验不改变项目或会话状态，调用者在实际 move 后再调用 remap 入口。
    return ProjectResourceService::validateAudioResourceMove(
        *project, oldPath, newPath);
}

/// @brief 文件或目录移动后同步项目音频路径和已打开会话引用。
/// @param oldPath 移动前的文件或目录路径。
/// @param newPath 移动后的文件或目录路径。
/// @param errorMessage 失败时接收面向用户的错误和回滚状态。
/// @return 路径发生变化的项目音频资源数量。
/// @details
/// 控制器先按旧、新路径更新项目资源表与磁盘谱面；本函数通过前后资源快照
/// 确定真实变化集合。每个打开会话分别迁移 BeatMap 域与 ECS 域引用，并按
/// 变化种类设置 Note、Sample 和音频描述符派生状态。Effect 在 AudioManager
/// 中按新绝对路径重新登记，全部运行时状态一致后才保存项目。
/// @warning 低频文件操作路径：会同步全部打开会话并扫描项目谱面文件。
/// @post 返回非零时项目配置已保存，相关 Effect 已按移动后的路径重新登记。
std::size_t EditorEngine::remapAudioResourcePathsAfterMove(
    const std::filesystem::path& oldPath, const std::filesystem::path& newPath,
    std::string* errorMessage)
{
    // 输出参数始终先清空，成功返回不能携带调用方上一次操作的陈旧错误。
    if ( errorMessage ) errorMessage->clear();
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    auto* project = ProjectController::instance().currentProject();
    // 无项目意味着没有受管资源路径；外部文件移动不产生同步数量。
    if ( !project ) return 0U;

    auto& sessions = m_sessionRegistry.entriesUnsafe();
    // 资源服务扫描磁盘前把所有打开会话同步进 BeatMap，纳入未保存引用。
    (void)collectOpenSessionAudioReferencesUnsafe(*project, sessions);

    /// @brief 移动前资源表快照，用于识别路径变化并匹配旧引用。
    const auto resourcesBeforeMove = project->m_audioResources;
    const auto changedCount =
        ProjectResourceService::remapAudioResourcePathsAfterMove(
            *project, oldPath, newPath, errorMessage);
    // 零变化也可能带有校验错误，保持服务写入的 errorMessage 原样返回调用方。
    if ( changedCount == 0U ) return 0U;

    /// @brief 单个路径发生变化的资源前后快照。
    struct ChangedAudioResourcePath {
        /// @brief 移动前资源状态。
        AudioResource m_before;

        /// @brief 移动后资源状态。
        AudioResource m_after;
    };
    std::vector<ChangedAudioResourcePath> changedResources;
    // 服务返回数量作为容量提示，实际前后快照比较仍是后续处理的权威集合。
    changedResources.reserve(changedCount);
    for ( const auto& resourceAfter : project->m_audioResources ) {
        // 资源 ID 在路径移动中保持稳定，因此可作为前后快照的唯一连接键。
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
        // 按值保留前后资源，后续会话匹配必须使用移动前路径，重新登记使用新路径。
        changedResources.push_back(
            ChangedAudioResourcePath{ *resourceBefore, resourceAfter });
    }

    for ( auto& entry : sessions ) {
        if ( entry.isLogoPlaceholder || !entry.session ) continue;

        auto& ctx = entry.session->getContextMutable();
        if ( !ctx.currentBeatmap ) continue;

        const auto beatmapPath =
            getOpenSessionBeatmapDiagnosticPath(*project, entry, ctx);
        // 三个标记跨多个移动资源累积，循环结束后每个派生域至多重建一次。
        bool noteEcsChanged                  = false;
        bool sampleEcsChanged                = false;
        bool referencesMovedTimelineResource = false;
        for ( const auto& changedResource : changedResources ) {
            // BeatMap 域覆盖元数据和序列化对象，ECS
            // 域覆盖当前编辑中的实时组件。
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
            // 玩家采样绑定改变会影响 HitEvent 派生缓存，但不改变音符几何索引。
            ctx.m_needsNotesSync = true;
            SessionUtils::markHitEventsDirty(ctx);
        }
        if ( sampleEcsChanged ) {
            // 自动采样独立于 Note Registry，同步标志不能与音符变化合并省略。
            ctx.m_needsSamplesSync = true;
        }
        if ( noteEcsChanged || sampleEcsChanged ) {
            // 两类 ECS 修改合并为一次 BeatMap
            // 同步，避免中间状态被后续保存观察。
            SessionUtils::syncBeatmap(ctx);
        }
        if ( referencesMovedTimelineResource ) {
            // 只有时间线资源引用受影响时才要求重新激活，纯 Note Effect 绑定
            // 由命中音效池按稳定 ID 解析，不需要重载主 transport。
            ctx.isAudioTimelineDescriptorDirty   = true;
            ctx.isAudioTimelineActivationPending = true;
        }
    }

    auto& audio = Audio::AudioManager::instance();
    for ( const auto& changedResource : changedResources ) {
        // Effect 注册表按新路径刷新；Main 和 Sample
        // 的加载由会话描述符延后处理。
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

    // 所有资源运行时登记完成后一次性保存项目，返回服务确认的变化数量。
    saveProject();
    return changedCount;
}

/// @brief 扫描当前项目目录并把新增、删除或变更资源同步到运行时。
/// @details
/// ProjectController 负责目录枚举、资源清单差异和删除判断；EditorEngine 只
/// 补充跨模块副作用，包括登记新增 Effect、使会话音频描述符失效、保存项目
/// 配置并向 UI 发布刷新事件。扫描失败不发布成功事件，也不清空现有资源展示。
/// @warning 目录监视去抖后的低频路径：持有 SessionRegistry 递归锁，执行完整
/// 项目目录扫描、音效登记和可选项目保存，不得从每帧无条件调用。
/// @post 扫描成功时发布 ProjectDirectoryRefreshedEvent 表明资源是否实际变化。
void EditorEngine::scanProjectDirectory()
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    /// @brief 当前项目指针，仅用于解析音效资源绝对路径。
    const auto* currentProject = ProjectController::instance().currentProject();
    // 项目关闭后的迟到监视通知无须处理，下一项目会建立独立监视状态。
    if ( !currentProject ) return;

    /// @brief 当前目录资源同步结果。
    auto syncResult = ProjectController::instance().scanProjectDirectory();
    // 控制器结果已经完成项目资源表差异计算，本层只处理跨模块运行时副作用。

    // 只登记控制器确认新增或更新的 Effect 描述；首次显式播放时才按需解码。
    for ( const auto& res : syncResult.m_effectResourcesToRegister ) {
        /// @brief 新音效资源的项目内绝对路径。
        auto absAudioPath =
            currentProject->m_projectRoot / Config::utf8ToPath(res.m_path);
        // 控制器输出保持项目相对路径，跨入 AudioManager 前才解析为绝对路径。
        Audio::AudioManager::instance().registerSoundEffect(
            res.m_id, Config::pathToUtf8(absAudioPath), res.m_config);
    }

    // 任一目录变化都可能改变组合音频路径，先标脏会话再保存新的项目清单。
    if ( syncResult.m_changed ) {
        markAudioTimelineDescriptorsDirtyUnsafe();
        saveProject();
    }

    if ( syncResult.m_scanSucceeded ) {
        // 只有完整扫描成功才通知 UI
        // 刷新；失败时保留现有展示，等待下次通知重试。
        Event::EventBus::instance().publish(
            Event::ProjectDirectoryRefreshedEvent{
                // UI 可据 changed 区分只完成扫描与确有资源列表变化。
                .m_resourcesChanged = syncResult.m_changed,
            });
    }
}

}  // namespace MMM::Logic
