#include "logic/BeatmapSettingsSnapshot.h"

#include "common/EditorPlaybackState.h"
#include "common/LogicCommands.h"
#include "config/Utf8Path.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectController.h"
#include "logic/ProjectUiState.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "runtime/AppThreadPool.h"

#include <ice/thread/ThreadPool.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace MMM::Logic
{
namespace
{

/// @brief 判断路径上次检查时是否存在；权限错误与不存在统一展示。
/// @param path 按调用方项目根解析后的路径，允许独立谱面的原始路径。
/// @return 只有探测成功且对象仍存在时才返回 true。
bool fileExists(const std::filesystem::path& path)
{
    // 空引用表示未绑定资源；不能将其展开为当前目录并当成有效文件。
    // 权限错误只影响本次展示，不清除持久化的媒体引用。
    std::error_code error;
    return !path.empty() && std::filesystem::exists(path, error) && !error;
}

/// @brief 去掉旧模型中重复的工程文件夹前缀。
/// @param root 本次复制的工程根，未打开工程时不能做兼容改写。
/// @param path 谱面的已保存引用，可能指向多级子目录。
/// @return 路径没有重复前缀时返回空；调用方仍需检查对应文件。
std::filesystem::path stripProjectPrefix(const std::filesystem::path& root,
                                         const std::filesystem::path& path)
{
    // 遗留格式会重复写入工程目录名。绝对路径有自己的根，不走兼容规则。
    if ( root.empty() || path.empty() || path.is_absolute() ) return {};
    auto segment = path.begin();
    // 只比较完整路径分量，不能把工程名 song 当成 song2 的合法前缀。
    if ( *segment != root.filename() ) return {};
    std::filesystem::path stripped;
    // 保留剩余的相对目录结构，使子目录中的同名资源仍可区分。
    for ( ++segment; segment != path.end(); ++segment ) stripped /= *segment;
    return stripped.lexically_normal();
}

/// @brief 生成文件显示和选择的兼容路径；相对路径只在目标存在时剥前缀。
/// @param root 捕获的项目根目录；独立谱面可能不带工程。
/// @param source 需要匹配的原元数据路径，空值代表当前没有绑定。
/// @warning 文件线程入口，绝对路径相对化可能访问磁盘。
BeatmapSettingsPath describePath(const std::filesystem::path& root,
                                 const std::filesystem::path& source)
{
    BeatmapSettingsPath result;
    result.source = source;
    if ( source.empty() ) return result;

    // 词法规范化不会访问文件；原值仍保留，供画面判断路径状态是否过期。
    // 即使新旧引用规范化后相同，外部改名也不能复用旧的存在性判断。
    const auto path     = source.lexically_normal();
    auto       resolved = root / path;
    result.exists       = fileExists(resolved);
    auto display        = path;
    // 展示用相对路径和实际打开的路径各自保留，避免兼容名改变访问目标。
    if ( path.is_relative() ) {
        const auto stripped = stripProjectPrefix(root, path);
        if ( !stripped.empty() && fileExists(root / stripped) ) {
            // 直接拼接路径存在时保留实际访问优先级；展示仍沿用兼容路径。
            display = stripped;
            if ( !result.exists ) {
                resolved      = root / stripped;
                result.exists = true;
            }
        }
    } else if ( !root.empty() ) {
        // 跨卷或权限失败时保留原路径，不能为了显示而丢失文件身份。
        std::error_code error;
        auto            relative = std::filesystem::relative(path, root, error);
        if ( !error && !relative.empty() ) display = std::move(relative);
    }
    // 实际访问与相对显示分开。后台同时转换这两种文本，避免路径提示出现时
    // UI 每帧重复做原生编码转换，尤其是 Windows 的宽字符路径。
    result.display         = Config::pathToUtf8(display);
    result.absoluteDisplay = Config::pathToUtf8(resolved);
    return result;
}

/// @brief 在同一次文件遍历中生成两类候选；不读取资源内容。
/// @param snapshot 本次待完成发布的文件快照，路径与候选对应同一工程。
/// @warning 递归目录操作可能被设备阻塞，只允许在独立文件池执行。
void scanProjectMedia(BeatmapSettingsSnapshot& snapshot)
{
    // 白名单只筛选候选，媒体解码仍独立检查文件内容。
    // 两类候选共享一轮遍历，切换背景类型无需启动第二次扫描。
    constexpr std::array<std::string_view, 4> imageExtensions{
        ".png", ".jpg", ".jpeg", ".bmp"
    };
    constexpr std::array<std::string_view, 7> videoExtensions{
        ".mp4", ".avi", ".mkv", ".webm", ".mov", ".flv", ".m4v"
    };
    // 空目录也是一份完成的结果，UI 必须能区分空列表与任务未就绪。
    snapshot.scanned = true;
    if ( snapshot.projectRoot.empty() ) return;

    // 不跟随目录符号链接，避免循环和对工程外目录的递归扫描。
    // 无权访问的子目录可以跳过，不应屏蔽其同级目录里的有效资源。
    std::error_code                               error;
    std::filesystem::recursive_directory_iterator it(
        snapshot.projectRoot,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    for ( ; !error && it != end; it.increment(error) ) {
        // 单项损坏或竞态删除不应破坏后继目录项的检查。
        std::error_code entryError;
        if ( !it->is_regular_file(entryError) ) continue;
        // 只检查普通文件；使用 unsigned char 逐字节转换 ASCII 扩展名。
        // UTF-8 文件名可原样保留，负的 char 值不能直接交给 ctype。
        auto extension = Config::pathToUtf8(it->path().extension());
        std::ranges::transform(
            extension, extension.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
        const bool image = std::ranges::find(imageExtensions, extension) !=
                           imageExtensions.end();
        const bool video = std::ranges::find(videoExtensions, extension) !=
                           videoExtensions.end();
        if ( !image && !video ) continue;
        // 相对路径可随工程一同搬迁。某项消失时跳过该项，保留其它候选。
        auto relative = std::filesystem::relative(
            it->path(), snapshot.projectRoot, entryError);
        if ( !entryError )
            (image ? snapshot.images : snapshot.videos)
                .push_back(Config::pathToUtf8(relative));
    }
}

// 普通刷新和目录扫描携带同样的载图身份，发布后都只允许读取。
using SnapshotResult = std::shared_ptr<const BeatmapSettingsSnapshot>;

/// @brief 设置页等待的单次读取；文件线程只收到已复制的字段。
struct SettingsRequest {
    /// @brief 请求是否包含递归资源枚举。
    bool scanResources;
    /// @brief UI 当前使用的结果，只在逻辑线程判断是否可以复用。
    SnapshotResult previous;
    /// @brief UI 持有 future，任务结束时不访问设置窗口对象。
    std::shared_ptr<std::promise<SnapshotResult>> completion;
};

/// @brief 在 UI 和逻辑线程间交接一个待处理请求。
struct SettingsRequestSlot {
    /// @brief 保护尚未由逻辑线程取走的请求。
    std::mutex mutex;
    /// @brief 请求槽只保存最新一项，重复请求不会累积过时工作。
    std::optional<SettingsRequest> request;
    /// @brief 逻辑循环空闲时判断是否需要进入请求锁。
    /// @warning UI 写入，逻辑线程每轮读取。仅作提示；槽本体由 mutex 发布。
    /// 因此使用 relaxed，真正取出请求后才清除标志。
    std::atomic<bool> pending{ false };
};

/// @brief 设置页全局唯一的有界请求槽。
/// @note 文件任务不回读此槽，慢盘不会阻塞新请求提交。
SettingsRequestSlot requestSlot;

/// @brief 在逻辑线程比较活动谱面和现有结果，仅捕获失效的快照。
/// @param request 当前请求及 UI 已显示的版本。
/// @param project 已发布的工程版本，和文件线程共享期间保持不可变。
/// @param directoryRevision 已处理的文件通知版本，与项目存储版本独立。
/// @param unchanged 为 true 时调用方直接交付旧版本，不访问文件池。
/// @return 新快照的待补全数据；无谱面或可复用旧结果时为空。
/// @return 没有完成加载的活动谱面时返回空，调用方无需排文件任务。
/// @warning 低频请求路径；项目切换在同一逻辑线程，不能挪到文件线程。
std::shared_ptr<BeatmapSettingsSnapshot> captureSettingsValues(
    const SettingsRequest&                       request,
    const std::shared_ptr<const ProjectUiState>& project,
    std::uint64_t directoryRevision, bool& unchanged)
{
    auto&                 engine = EditorEngine::instance();
    const std::lock_guard lock(engine.getSessionMutex());
    // 会话也可能从其它入口切换；在同一锁内读取代次和谱面控制块。
    const auto session = engine.getActiveSession();
    if ( !session || !session->getContext().currentBeatmap ) return {};
    const auto& current    = session->getContext().currentBeatmap;
    const auto  generation = engine.activePlaybackState().generation;
    const auto& previous   = request.previous;
    // 先检查轻量身份与字段。未变化时既不分配快照，也不遍历工程资源。
    // 无工程时两个空控制块同样可匹配；旧版本的弱引用不会保留工程资源数组。
    // 先验版本不同便不比较元数据，避免对刚换下的大段 UTF-8 字段做无谓比较。
    unchanged =
        !request.scanResources && previous &&
        previous->generation == generation &&
        !previous->beatmap.owner_before(current) &&
        !current.owner_before(previous->beatmap) &&
        !previous->projectRevision.owner_before(project) &&
        !project.owner_before(previous->projectRevision) &&
        previous->directoryRevision == directoryRevision &&
        std::chrono::steady_clock::now() < previous->externalPathExpiry &&
        previous->metadata == current->m_baseMapMetadata;
    if ( unchanged ) return {};
    // 目录通知和工程发布是不同信号：删除封面不一定改动清单，
    // 编辑清单内的引用也不一定导致媒体文件变动。任一变更都重新取值。
    // 显式扫描从上面绕过缓存，允许关闭后再次打开弹层时刷新候选。
    // 快照需要保留所见的数据，但不应为已经关闭的会话延长整个 ECS 生命周期。
    // 只把对象身份作为弱引用写入快照，再将可见字段复制成真正的值。
    auto result     = std::make_shared<BeatmapSettingsSnapshot>();
    result->session = session;
    result->beatmap = current;
    // 代次隔离活动标签，谱面控制块另外隔离同一标签内发生的重载。
    result->generation        = generation;
    result->metadata          = current->m_baseMapMetadata;
    result->projectRevision   = project;
    result->directoryRevision = directoryRevision;
    if ( project ) {
        // 独立谱面也有设置页。项目是否存在与资源是否可写分别表示，
        // 让临时工程显示资源而保持导入和重命名禁用。
        result->hasProject  = true;
        result->projectRoot = project->m_projectRoot;
        result->readOnly    = project->m_isTemporaryProject;
    }
    return result;
}

/// @brief 在文件线程补全显示路径与可选资源。
/// @param result 逻辑线程已移交的独占快照，完成后才向 UI 发布。
/// @param project 与 result 的项目版本一致；独立谱面允许为空。
/// @note 文件阶段不能回读当前工程，任务排队时可能已切到另一个根目录。
/// @param scanResources 关闭资源下拉时仅检查现有引用。
void completeSettingsFiles(BeatmapSettingsSnapshot& result,
                           const ProjectUiState* project, bool scanResources)
{
    // 慢盘、链接解析和目录遍历与逻辑线程不共享锁。
    const auto& meta = result.metadata;
    const auto& root = result.projectRoot;
    // 外部格式优先显示歌曲提示，仅缺失时回退到原生主音频路径。
    // 这里处理显示和引用，不影响正在播放的音频时间线。
    const auto& audio = meta.song_file_hint.empty() ? meta.main_audio_path
                                                    : meta.song_file_hint;
    // 将路径状态和产生状态的源元数据一起发布，避免跨快照复用错误结果。
    result.paths = { describePath(root, meta.map_path),
                     describePath(root, audio),
                     describePath(root, meta.cover_path),
                     describePath(root, meta.main_cover_path) };
    if ( project ) {
        // 发布模型已经过滤 Effect；仅在失效后于文件线程复制主音频候选。
        // 预留精确容量，避免大工程的路径字符串在构造期间反复随数组搬移。
        result.audio.reserve(project->m_audioResources.size());
        for ( const auto& entry : project->m_audioResources )
            result.audio.push_back({ entry.m_id, entry.m_path, {} });
    }
    for ( auto& entry : result.audio ) {
        // 只有剥前缀后目标确实存在时才写入兼容路径；未知文件保留原引用。
        auto       path     = Config::utf8ToPath(entry.path);
        const auto stripped = stripProjectPrefix(root, path);
        entry.selection     = !stripped.empty() && fileExists(root / stripped)
                                  ? stripped
                                  : path.lexically_normal();
    }
    // 独立谱面和工程目录之外的引用收不到工程监听通知。仅对此类引用
    // 保留低频兜底刷新，工程内的普通静置页始终以通知为准。
    for ( const auto& path : result.paths ) {
        if ( path.source.empty() ) continue;
        // 用路径分量判断词法归属，避免磁盘访问，也避免同名前缀误判为子目录。
        // 跨卷路径没有相对形式，也沿用低频检查。
        const auto relative = path.source.is_absolute()
                                  ? path.source.lexically_relative(root)
                                  : path.source.lexically_normal();
        if ( root.empty() || relative.empty() || *relative.begin() == ".." ) {
            result.externalPathExpiry =
                std::chrono::steady_clock::now() + std::chrono::seconds(3);
            break;
        }
    }
    // 只有打开候选下拉才需要递归枚举。
    if ( scanResources ) scanProjectMedia(result);
}

}  // namespace

/// @brief 从发布槽获取同一代次的设置目标与计数。
/// @warning 设置页每帧读取；读取不会延长会话或旧谱面的生命周期。
std::optional<ActiveBeatmapSettingsState> activeBeatmapSettingsState()
{
    // 轨道变更按逻辑更新节拍可见。慢磁盘不会让连续点击读到同一旧值。
    return EditorEngine::instance().activeBeatmapSettingsState();
}

/// @brief 将请求放到逻辑线程，UI 自身无需接触会话互斥量。
/// @param scanResources 源自候选打开边沿；普通字段刷新不走递归枚举。
/// @param previous 由渲染侧保存旧快照，关闭和重载后可以直接丢弃。
/// @return 有效 future 在后台阶段完成；空结果用于撤回已有表单。
std::future<SnapshotResult> requestBeatmapSettings(bool           scanResources,
                                                   SnapshotResult previous)
{
    // 完成权先交给逻辑线程，再交给文件线程。窗口可随时销毁自己的 future，
    // 两阶段都不借用窗口对象，也不在窗口关闭时 join 文件任务。
    auto completion = std::make_shared<std::promise<SnapshotResult>>();
    auto result     = completion->get_future();
    {
        const std::lock_guard lock(requestSlot.mutex);
        // 多个来源重复请求时为旧 future 明确返回空，避免破损 promise。
        if ( requestSlot.request )
            requestSlot.request->completion->set_value({});
        requestSlot.request = { scanResources,
                                std::move(previous),
                                std::move(completion) };
        // 写槽和置位共用临界区，逻辑线程处理旧任务时不能抹掉新唤醒。
        requestSlot.pending.store(true, std::memory_order_relaxed);
    }
    return result;
}

/// @brief 在项目切换和会话更新之后获取有一致身份的输入。
/// @warning 每轮逻辑循环最多读一次提示，只有检测到请求才加锁。
/// @note 文件任务持有独立值，下一轮逻辑仍可照常推进。
void processBeatmapSettingsRequest()
{
    // 空闲时逻辑线程每轮 relaxed 读一次；标志只提示是否进入短请求锁。
    if ( !requestSlot.pending.load(std::memory_order_relaxed) ) return;
    SettingsRequest request;
    {
        const std::lock_guard lock(requestSlot.mutex);
        // 消费后立即释放请求锁，再访问会话；界面提交从不与会话锁嵌套。
        // pending 的所有写入都在请求锁下，观察到 true 时槽中有值。
        request = std::move(*requestSlot.request);
        requestSlot.request.reset();
        requestSlot.pending.store(false, std::memory_order_relaxed);
    }
    // 发布模型只借用不可变版本，不需要在会话锁内复制整个音频表。
    auto& controller = ProjectController::instance();
    auto  project    = controller.uiSnapshot();
    bool  unchanged  = false;
    auto  snapshot   = captureSettingsValues(
        request, project, controller.directoryRevision(), unchanged);
    if ( unchanged ) {
        request.completion->set_value(std::move(request.previous));
        return;
    }
    auto* worker = Runtime::AppThreadPool::instance().getFileThreadPool();
    // 无可用谱面或线程池已停止时，UI 仍能消费正常的空结果。
    if ( !snapshot || !worker ) {
        request.completion->set_value({});
        return;
    }
    // 进入文件池时会话锁已经释放。未接收任务时闭包不会运行，
    // 因此仍由提交侧完成空结果，不能让 UI 访问失效的 future。
    const auto submitted = worker->enqueue([snapshot   = std::move(snapshot),
                                            project    = std::move(project),
                                            scan       = request.scanResources,
                                            completion = request.completion] {
        completeSettingsFiles(*snapshot, project.get(), scan);
        completion->set_value(snapshot);
    });
    // 结果要么由已接收的任务提交，要么在此明确返回空；二者不会同时执行。
    if ( !submitted.valid() ) request.completion->set_value({});
}

/// @brief 将设置动作绑定到快照的载图实例，保持会话权限和命令队列协议。
/// @param snapshot 产生交互时显示的设置，定义目标会话和载图身份。
/// @param command 元数据差量或持久化辅助轨数量，命令消费前不会修改谱面。
/// @warning 设置交互低频路径，UI 不获取会话锁，也不执行文件操作。
void submitBeatmapSettingsCommand(const BeatmapSettingsSnapshot& snapshot,
                                  LogicCommand                   command)
{
    // 只有交互时提升弱所有权，按原会话入队，切换标签不会重定向命令。
    auto session = snapshot.session.lock();
    if ( !session || snapshot.beatmap.expired() ) return;
    // 活动谱面可能在队列消费前重载，三类动作都携带原载图身份再次校验。
    if ( auto* edit = std::get_if<CmdUpdateBeatmapMetadata>(&command) )
        edit->settingsSource = snapshot.beatmap;
    else if ( auto* draft = std::get_if<CmdUpdateDraftTrackCount>(&command) )
        draft->settingsSource = snapshot.beatmap;
    else if ( auto* bgm = std::get_if<CmdUpdateBgmTrackCount>(&command) )
        bgm->settingsSource = snapshot.beatmap;
    else
        return;
    session->pushCommand(std::move(command));
}

}  // namespace MMM::Logic
