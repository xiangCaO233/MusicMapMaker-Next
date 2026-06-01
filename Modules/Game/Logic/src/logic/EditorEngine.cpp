#include "logic/EditorEngine.h"
#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "event/logic/EditorConfigChangedEvent.h"
#include "event/logic/LogicCommandEvent.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/session/NoteAction.h"
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
#include <thread>

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

/// @brief 同主音轨画布同步的逻辑时间变化阈值。
constexpr double MAIN_AUDIO_SYNC_TIME_EPSILON = 1e-6;

/// @brief 为同主音轨后台跟随谱面推进视觉打击特效事件。
/// @warning 逻辑热路径：同主音轨同步时调用；只线性消费已排序 hitEvents
/// 的新增区间，禁止文件系统访问或重新构建事件序列。
void updateFollowerHitEffects(SessionContext& ctx, double previousVisualTime,
                              const Config::EditorConfig& config,
                              bool                        resetHitIndex)
{
    if ( resetHitIndex ) {
        SessionUtils::syncHitIndex(ctx);
        return;
    }

    std::vector<System::HitFXSystem::HitEvent> triggeredEvents;
    while ( ctx.nextHitIndex < ctx.hitEvents.size() &&
            ctx.hitEvents[ctx.nextHitIndex].timestamp <= ctx.visualTime ) {
        const auto& ev = ctx.hitEvents[ctx.nextHitIndex];
        if ( ev.timestamp > previousVisualTime ) {
            triggeredEvents.push_back(ev);
        }
        ctx.nextHitIndex++;
    }

    ctx.hitFXSystem.update(ctx.visualTime, triggeredEvents, config);
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

    const auto& activeKey =
        sessions[static_cast<size_t>(activeIndex)].mainAudioSyncKey;
    const auto& targetKey =
        sessions[static_cast<size_t>(targetIndex)].mainAudioSyncKey;
    return !activeKey.empty() && activeKey == targetKey;
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
    normalizeResourcePath(meta.main_cover_path);
    normalizeResourcePath(meta.cover_path);
}

/// @brief 生成用于判断多个谱面是否使用同一主音轨的稳定路径键。
/// @param project 当前项目；存在时相对路径按项目根目录解析。
/// @param mapPath 谱面文件路径；无项目时用于解析相对音频路径。
/// @param audioPath 主音轨路径。
/// @return 规范化后的 UTF-8 路径键，空音频路径返回空字符串。
std::string makeMainAudioSyncKey(const Project*               project,
                                 const std::filesystem::path& mapPath,
                                 const std::filesystem::path& audioPath)
{
    if ( audioPath.empty() ) {
        return "";
    }

    std::filesystem::path keyPath = audioPath;
    if ( project && keyPath.is_relative() ) {
        keyPath = project->m_projectRoot / keyPath;
    } else if ( !project && keyPath.is_relative() ) {
        keyPath = mapPath.parent_path() / keyPath;
    }

    std::error_code ec;
    auto canonicalPath = std::filesystem::weakly_canonical(keyPath, ec);
    if ( !ec ) {
        keyPath = canonicalPath;
    }

    return Config::pathToUtf8(keyPath.lexically_normal());
}

/// @brief 根据 Session 上下文生成主音轨同步路径键。
/// @param ctx 当前 Session 上下文。
/// @param project 当前项目；存在时相对路径按项目根目录解析。
/// @return 规范化后的 UTF-8 路径键，缺少谱面或音频路径时返回空字符串。
std::string getMainAudioSyncKey(const SessionContext& ctx,
                                const Project*        project)
{
    if ( !ctx.currentBeatmap ) {
        return "";
    }

    const auto& meta = ctx.currentBeatmap->m_baseMapMetadata;
    return makeMainAudioSyncKey(project, meta.map_path, meta.main_audio_path);
}

/// @brief 将编辑工具枚举转换为项目工作区中的稳定文本。
std::string editToolToWorkspaceName(EditTool tool)
{
    switch ( tool ) {
    case EditTool::Marquee: return "Marquee";
    case EditTool::Draw: return "Draw";
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
    return EditTool::Move;
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
    toolbarState.m_snapFloor     = editorConfig.settings.snapFloor;
    toolbarState.m_enableLinearScrollMapping =
        editorConfig.visual.enableLinearScrollMapping;
    toolbarState.m_drawBeatLines = editorConfig.visual.drawBeatLines;
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
    editorConfig.settings.snapFloor     = toolbarState.m_snapFloor;
    editorConfig.visual.enableLinearScrollMapping =
        toolbarState.m_enableLinearScrollMapping;
    editorConfig.visual.drawBeatLines = toolbarState.m_drawBeatLines;
    editorConfig.settings.stopPlaybackOnScroll =
        toolbarState.m_stopPlaybackOnScroll;
    editorConfig.visual.enableHitEffects = toolbarState.m_enableHitEffects;
    editorConfig.settings.beatDivisor =
        std::clamp(toolbarState.m_beatDivisor, 1, 64);
    editorConfig.visual.timelineZoom =
        std::clamp(toolbarState.m_timelineZoom, 0.1f, 10.0f);
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
    m_editorConfig = Config::AppConfig::instance().getEditorConfig();
    m_frameLimitPreference.store(m_editorConfig.settings.frameLimit,
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
            } else if ( std::holds_alternative<CmdCreateBeatmap>(e.command) ) {
                // 将创建谱面指令拦截，交由 EditorEngine 引擎级别处理
                handleCreateBeatmap(std::get<CmdCreateBeatmap>(e.command));
            } else {
                pushCommand(MMM::Logic::LogicCommand(e.command));
            }
        });
}

EditorEngine::~EditorEngine()
{
    stop();
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
    captureToolbarWorkspaceState(
        workspace,
        m_editorConfig,
        m_syncSameMainAudioCanvases.load(std::memory_order_relaxed));

    /// @brief 保护工作区状态捕获期间的会话列表访问。
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    const auto& sessions    = m_sessionRegistry.entriesUnsafe();
    const auto  activeIndex = m_sessionRegistry.activeIndex();

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
        if ( i == activeIndex && ctx.isPlaying ) {
            beatmapState.m_playbackTime =
                Audio::AudioManager::instance().getCurrentTime();
        }
        workspace.m_openBeatmaps.push_back(beatmapState);

        if ( i == activeIndex ) {
            workspace.m_activeBeatmapPath  = beatmapState.m_filePath;
            workspace.m_activePlaybackTime = beatmapState.m_playbackTime;
            project->m_settings.m_lastOpenedBeatmap = entry.displayName;

            auto audioPath =
                ctx.currentBeatmap->m_baseMapMetadata.main_audio_path;
            auto audioPathUtf8 = Config::pathToUtf8(audioPath);
            auto audioIdUtf8   = Config::pathToUtf8(audioPath.filename());
            for ( auto& resource : project->m_audioResources ) {
                if ( resource.m_path != audioPathUtf8 &&
                     resource.m_id != audioIdUtf8 ) {
                    continue;
                }

                auto& audio              = Audio::AudioManager::instance();
                resource.m_config.volume = audio.getMainTrackVolume();
                resource.m_config.muted  = audio.isMainTrackMuted();
                resource.m_config.playbackSpeed =
                    static_cast<float>(audio.getPlaybackSpeed());
                resource.m_config.playbackPitch =
                    static_cast<float>(audio.getPlaybackPitch());
                resource.m_config.eqEnabled = audio.isMainTrackEQEnabled();
                resource.m_config.eqPreset =
                    static_cast<int>(audio.getMainTrackEQPreset());
                resource.m_config.eqBandGains.clear();
                resource.m_config.eqBandQs.clear();

                const size_t bandCount = audio.getMainTrackEQBandCount();
                resource.m_config.eqBandGains.reserve(bandCount);
                resource.m_config.eqBandQs.reserve(bandCount);
                for ( size_t band = 0; band < bandCount; ++band ) {
                    resource.m_config.eqBandGains.push_back(
                        audio.getMainTrackEQBandGain(band));
                    resource.m_config.eqBandQs.push_back(
                        audio.getMainTrackEQBandQ(band));
                }
                break;
            }
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
        {
            std::lock_guard<std::recursive_mutex> lock(
                m_sessionRegistry.mutex());
            auto& sessions = m_sessionRegistry.entriesUnsafe();
            if ( index >= 0 && index < static_cast<int32_t>(sessions.size()) ) {
                restoredSession = sessions[index].session;
            }
        }

        if ( restoredSession ) {
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

void EditorEngine::openProject(const std::filesystem::path& projectPath)
{
    /// @brief 实际打开前用于保持旧行为的项目目录校验路径。
    std::filesystem::path actualProjectPath = projectPath;
    if ( std::filesystem::exists(projectPath) &&
         std::filesystem::is_regular_file(projectPath) ) {
        actualProjectPath = projectPath.parent_path();
    }

    if ( !std::filesystem::exists(actualProjectPath) ||
         !std::filesystem::is_directory(actualProjectPath) ) {
        XERROR(
            "Failed to open project: Path does not exist or is not a "
            "directory: {}",
            Config::pathToUtf8(actualProjectPath));
        return;
    }

    closeProject();

    /// @brief 项目控制器打开项目后的结果。
    auto openResult = ProjectController::instance().openProject(projectPath);
    if ( !openResult.m_opened ) {
        return;
    }
    m_pendingWorkspaceActiveIndex = -1;
    if ( auto* project = ProjectController::instance().currentProject() ) {
        const auto& workspace = project->m_settings.m_workspace;
        m_currentTool.store(workspaceNameToEditTool(workspace.m_activeEditTool),
                            std::memory_order_relaxed);
        if ( workspace.m_toolbarState.m_valid ) {
            auto restoredConfig = m_editorConfig;
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

    for ( const auto& preload : openResult.m_effectPreloads ) {
        Audio::AudioManager::instance().preloadSoundEffect(
            preload.m_resource.m_id,
            Config::pathToUtf8(preload.m_absolutePath),
            preload.m_resource.m_config.volume);
    }

    XINFO("Project '{}' loaded successfully with {} beatmaps.",
          openResult.m_projectTitle,
          openResult.m_beatmapCount);

    // 如果指定了谱面路径，则通过 createSession 加载它
    if ( !openResult.m_targetBeatmapPath.empty() ) {
        XINFO("Auto loading beatmap: {}",
              Config::pathToUtf8(openResult.m_targetBeatmapPath));
        try {
            auto map = std::make_shared<BeatMap>(
                BeatMap::loadFromFile(openResult.m_targetBeatmapPath));
            createSession(map, map->m_baseMapMetadata.name);
        } catch ( const std::exception& e ) {
            XERROR("Failed to auto load beatmap {}: {}",
                   Config::pathToUtf8(openResult.m_targetBeatmapPath),
                   e.what());
        }
    } else {
        restoreProjectWorkspace(openResult.m_targetBeatmapPath);
    }
}

void EditorEngine::closeProject()
{
    ProjectController::instance().saveProject();

    /// @brief 项目控制器关闭当前项目后的结果。
    auto closeResult = ProjectController::instance().closeProject();
    if ( !closeResult.m_closed || !closeResult.m_project ) {
        return;
    }

    auto& audio = Audio::AudioManager::instance();
    audio.stop();
    audio.clearAllScheduledSoundEffects();
    audio.unloadBGM();

    for ( const auto& res : closeResult.m_project->m_audioResources ) {
        if ( res.m_type == AudioTrackType::Effect ) {
            audio.unloadSoundEffect(res.m_id);
        }
    }

    XINFO("Project '{}' closed.", closeResult.m_projectTitle);
}

void EditorEngine::start()
{
    if ( m_running.load(std::memory_order_acquire) ) {
        return;
    }

    // 从全局配置同步到本地缓存
    m_editorConfig = Config::AppConfig::instance().getEditorConfig();
    m_frameLimitPreference.store(m_editorConfig.settings.frameLimit,
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

/// @brief 处理导入音频指令并执行音效预加载和项目保存副作用。
void EditorEngine::handleImportAudio(const CmdImportAudio& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    /// @brief 项目命令服务的导入音频处理结果。
    auto result = ProjectController::instance().importAudio(cmd);
    if ( !result.m_imported ) {
        return;
    }

    if ( result.m_effectPreload ) {
        Audio::AudioManager::instance().preloadSoundEffect(
            result.m_effectPreload->m_resource.m_id,
            Config::pathToUtf8(result.m_effectPreload->m_absolutePath),
            result.m_effectPreload->m_resource.m_config.volume);
    }

    saveProject();
}

/// @brief 更新编辑器级剪贴板。
void EditorEngine::setClipboard(std::vector<ClipboardItem> items,
                                const SessionContext* sourceContext, bool isCut)
{
    m_clipboard.set(std::move(items), sourceContext, isCut);
}

/// @brief 获取编辑器级剪贴板副本。
std::vector<ClipboardItem> EditorEngine::getClipboard() const
{
    return m_clipboard.get();
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

        std::vector<BatchNoteAction::Entry> entries;
        auto view = sourceCtx.noteRegistry.view<InteractionComponent>();
        for ( auto entity : view ) {
            auto& ic = sourceCtx.noteRegistry.get<InteractionComponent>(entity);
            if ( !ic.isCut ||
                 !sourceCtx.noteRegistry.all_of<NoteComponent>(entity) ) {
                continue;
            }

            auto oldNote = sourceCtx.noteRegistry.get<NoteComponent>(entity);
            entries.push_back({ entity, oldNote, std::nullopt });

            if ( oldNote.m_type == ::MMM::NoteType::POLYLINE &&
                 !oldNote.m_subNotes.empty() ) {
                for ( auto subEnt :
                      sourceCtx.noteRegistry.view<NoteComponent>() ) {
                    const auto& subNC =
                        sourceCtx.noteRegistry.get<NoteComponent>(subEnt);
                    if ( subNC.m_isSubNote &&
                         subNC.m_parentPolyline == entity ) {
                        entries.push_back({ subEnt, subNC, std::nullopt });
                    }
                }
            }
        }

        if ( !entries.empty() ) {
            auto action = std::make_unique<BatchNoteAction>(
                std::move(entries), "Cut Across Canvas");
            sourceCtx.actionStack.pushAndExecute(std::move(action), sourceCtx);
        }

        for ( auto entity : view ) {
            sourceCtx.noteRegistry.get<InteractionComponent>(entity).isCut =
                false;
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
    refreshMainAudioSyncKeysUnsafe();
}

void EditorEngine::pushCommand(LogicCommand&& cmd)
{
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

    // 拦截视口更新指令，缓存最新的尺寸
    if ( std::holds_alternative<CmdUpdateViewport>(cmd) ) {
        const auto& v = std::get<CmdUpdateViewport>(cmd);
        m_renderSyncRegistry.cacheViewportSize(v.cameraId,
                                               { v.width, v.height });
    }

    // 主画布滚轮按 cameraId 路由，以允许同主音轨后台画布在 hover
    // 状态下接收滚动，但不同主音轨画布不会被滚轮同步或切换。
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

            if ( targetIndex != activeIndex ) {
                setActiveSessionIndex(targetIndex);
            }
            sessions[static_cast<size_t>(targetIndex)].session->pushCommand(
                std::move(cmd));
            return;
        }
    }

    // 分发到当前活跃 Session
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    /// @brief 当前活跃 Session 索引快照。
    int32_t idx = m_sessionRegistry.activeIndex();
    if ( idx >= 0 && idx < static_cast<int32_t>(sessions.size()) ) {
        sessions[idx].session->pushCommand(std::move(cmd));
    }
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

const std::unordered_map<uint32_t, glm::vec4>& EditorEngine::getAtlasUVMap(
    const std::string& cameraId) const
{
    return m_renderSyncRegistry.getAtlasUVMap(cameraId);
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
    /// @brief hover 目标主画布对应的 Session 索引。
    const int32_t targetIndex =
        findSessionIndexByCameraIdUnsafe(sessions, cameraId);
    return canUseHoverScrollTargetUnsafe(sessions, activeIndex, targetIndex);
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
                entry.session->getContextMutable().isMainAudioSyncFollower =
                    false;
            }
        }
    }
    if ( auto* project = ProjectController::instance().currentProject() ) {
        captureToolbarWorkspaceState(
            project->m_settings.m_workspace, m_editorConfig, enabled);
    }
    if ( enabled ) {
        syncSameMainAudioCanvases();
    }
}

/// @brief 刷新已打开 Session 的主音轨同步路径键。
void EditorEngine::refreshMainAudioSyncKeys()
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    refreshMainAudioSyncKeysUnsafe();
}

/// @brief 刷新已打开 Session 的主音轨同步路径键，调用者必须持有注册表锁。
void EditorEngine::refreshMainAudioSyncKeysUnsafe()
{
    const auto* currentProject = ProjectController::instance().currentProject();
    auto&       sessions       = m_sessionRegistry.entriesUnsafe();
    for ( auto& entry : sessions ) {
        if ( entry.isLogoPlaceholder || !entry.session ) {
            entry.mainAudioSyncKey.clear();
            continue;
        }

        const auto& ctx        = entry.session->getContext();
        entry.mainAudioSyncKey = getMainAudioSyncKey(ctx, currentProject);
    }
    refreshMainAudioSyncPeerStateUnsafe();
    m_lastMainAudioSyncActiveIndex = -1;
}

/// @brief 刷新是否存在同主音轨同步候选，调用者必须持有注册表锁。
void EditorEngine::refreshMainAudioSyncPeerStateUnsafe()
{
    const auto& sessions = m_sessionRegistry.entriesUnsafe();
    for ( size_t i = 0; i < sessions.size(); ++i ) {
        const auto& key = sessions[i].mainAudioSyncKey;
        if ( key.empty() || sessions[i].isLogoPlaceholder ||
             !sessions[i].session ) {
            continue;
        }

        for ( size_t j = i + 1; j < sessions.size(); ++j ) {
            if ( sessions[j].isLogoPlaceholder || !sessions[j].session ) {
                continue;
            }
            if ( sessions[j].mainAudioSyncKey == key ) {
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
    if ( !m_syncSameMainAudioCanvases.load(std::memory_order_relaxed) ) {
        return;
    }
    if ( !m_hasMainAudioSyncPeers.load(std::memory_order_relaxed) ) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    if ( sourceIndex < 0 ||
         sourceIndex >= static_cast<int32_t>(sessions.size()) ||
         !sessions[static_cast<size_t>(sourceIndex)].session ) {
        return;
    }

    auto& sourceCtx =
        sessions[static_cast<size_t>(sourceIndex)].session->getContext();
    const auto& sourceKey =
        sessions[static_cast<size_t>(sourceIndex)].mainAudioSyncKey;
    if ( sourceKey.empty() ) {
        return;
    }
    // 被动 follower 的时间变化来自本地视觉插值，不能反向覆盖真正的播放源。
    if ( sourceCtx.isMainAudioSyncFollower && !sourceCtx.isPlaying ) {
        return;
    }

    for ( int32_t i = 0; i < static_cast<int32_t>(sessions.size()); ++i ) {
        if ( i == sourceIndex || !sessions[static_cast<size_t>(i)].session ) {
            continue;
        }

        auto& ctx =
            sessions[static_cast<size_t>(i)].session->getContextMutable();
        if ( sessions[static_cast<size_t>(i)].mainAudioSyncKey != sourceKey ) {
            continue;
        }

        const bool   wasFollowing       = ctx.isMainAudioSyncFollower;
        const double previousVisualTime = ctx.visualTime;
        const bool   shouldClearHitEffects =
            wasFollowing != sourceCtx.isPlaying ||
            sourceCtx.visualTime < previousVisualTime ||
            std::abs(sourceCtx.visualTime - previousVisualTime) > 0.2;

        ctx.currentTime             = sourceCtx.currentTime;
        ctx.visualTime              = sourceCtx.visualTime;
        ctx.isPlaying               = false;
        ctx.isMainAudioSyncFollower = sourceCtx.isPlaying;
        ctx.lastAudioPos            = 0.0;
        ctx.lastAudioSysTime        = 0.0;
        ctx.hasInitialAudioOffset   = false;
        ctx.playStartSysTime =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        ctx.playStartVisualTime = ctx.currentTime;
        ctx.syncClock.reset(ctx.currentTime);
        if ( shouldClearHitEffects ) {
            ctx.hitFXSystem.clearActiveEffects();
        }
        if ( ctx.isMainAudioSyncFollower ) {
            updateFollowerHitEffects(
                ctx, previousVisualTime, m_editorConfig, shouldClearHitEffects);
        }
    }
}

int32_t EditorEngine::createSession(std::shared_ptr<MMM::BeatMap> beatmap,
                                    const std::string&            displayName,
                                    bool               isLogoPlaceholder,
                                    const std::string& preferredCameraId,
                                    bool               restoreDockFromWorkspace)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
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
    /// @brief 本次请求打开的主音轨稳定路径键。
    const std::string requestedMainAudioSyncKey =
        beatmap
            ? makeMainAudioSyncKey(currentProject,
                                   beatmap->m_baseMapMetadata.map_path,
                                   beatmap->m_baseMapMetadata.main_audio_path)
            : std::string{};

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
                sessions[i].beatmapPathKey   = requestedBeatmapKey;
                sessions[i].mainAudioSyncKey = requestedMainAudioSyncKey;
                if ( !preferredCameraId.empty() ) {
                    m_sessionRegistry.reserveCameraId(preferredCameraId);
                }
                sessions[i].session->pushCommand(
                    LogicCommand(CmdUpdateEditorConfig{ m_editorConfig }));
                sessions[i].session->pushCommand(LogicCommand(CmdChangeTool{
                    m_currentTool.load(std::memory_order_relaxed) }));
                sessions[i].session->pushCommand(
                    LogicCommand(CmdLoadBeatmap{ beatmap }));
                m_sessionRegistry.setActiveIndex(i);
                refreshMainAudioSyncPeerStateUnsafe();
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
        LogicCommand(CmdUpdateEditorConfig{ m_editorConfig }));
    newSession->pushCommand(LogicCommand(
        CmdChangeTool{ m_currentTool.load(std::memory_order_relaxed) }));

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
    entry.mainAudioSyncKey         = requestedMainAudioSyncKey;
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

void EditorEngine::setActiveSessionIndex(int32_t index)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    if ( index >= 0 && index < static_cast<int32_t>(sessions.size()) ) {
        // 1. 如果旧会话正在播放，先暂停它
        /// @brief 切换前的活跃 Session 索引快照。
        int32_t     currentActive        = m_sessionRegistry.activeIndex();
        double      syncedCurrentTime    = 0.0;
        bool        shouldSyncTargetTime = false;
        std::string oldMainAudioKey;
        if ( currentActive >= 0 &&
             currentActive < static_cast<int32_t>(sessions.size()) ) {
            auto& oldSession = sessions[currentActive].session;
            if ( oldSession ) {
                auto& oldCtx    = oldSession->getContextMutable();
                oldMainAudioKey = sessions[currentActive].mainAudioSyncKey;
                if ( oldCtx.isPlaying ) {
                    oldCtx.currentTime =
                        Audio::AudioManager::instance().getCurrentTime();
                    oldCtx.visualTime =
                        oldCtx.currentTime +
                        m_editorConfig.visual.getEffectiveVisualOffset();
                    oldCtx.isPlaying               = false;
                    oldCtx.isMainAudioSyncFollower = false;
                }
                syncedCurrentTime = oldCtx.currentTime;
            }
        }

        if ( m_syncSameMainAudioCanvases.load(std::memory_order_relaxed) &&
             !oldMainAudioKey.empty() && sessions[index].session ) {
            shouldSyncTargetTime =
                sessions[index].mainAudioSyncKey == oldMainAudioKey;
        }

        m_sessionRegistry.setActiveIndex(index);
        XINFO("Switched active session to #{} cameraId={}",
              index,
              sessions[index].cameraId);

        // 2. 加载新激活会话的音频资源并同步播放进度
        auto& activeSession = sessions[index].session;
        if ( activeSession ) {
            // 停止当前所有播放
            Audio::AudioManager::instance().stop();

            auto& ctx                   = activeSession->getContextMutable();
            ctx.isMainAudioSyncFollower = false;
            if ( shouldSyncTargetTime ) {
                ctx.currentTime = syncedCurrentTime;
            }
            if ( ctx.currentBeatmap && !sessions[index].isLogoPlaceholder ) {
                auto*                 project = getCurrentProject();
                std::filesystem::path audioPath;
                if ( project ) {
                    audioPath =
                        project->m_projectRoot /
                        ctx.currentBeatmap->m_baseMapMetadata.main_audio_path;
                } else {
                    audioPath =
                        ctx.currentBeatmap->m_baseMapMetadata.map_path
                            .parent_path() /
                        ctx.currentBeatmap->m_baseMapMetadata.main_audio_path;
                }

                if ( !ctx.currentBeatmap->m_baseMapMetadata.main_audio_path
                          .empty() &&
                     std::filesystem::exists(audioPath) ) {
                    AudioTrackConfig config;
                    if ( project ) {
                        for ( const auto& res : project->m_audioResources ) {
                            if ( res.m_id ==
                                     Config::pathToUtf8(
                                         ctx.currentBeatmap->m_baseMapMetadata
                                             .main_audio_path.filename()) ||
                                 res.m_path ==
                                     Config::pathToUtf8(
                                         ctx.currentBeatmap->m_baseMapMetadata
                                             .main_audio_path) ) {
                                config = res.m_config;
                                break;
                            }
                        }
                    }
                    Audio::AudioManager::instance().loadBGM(
                        Config::pathToUtf8(audioPath), config);
                }
            }

            double totalTime = Audio::AudioManager::instance().getTotalTime();
            double minTime = -m_editorConfig.visual.getEffectiveVisualOffset();
            if ( minTime > totalTime ) {
                minTime = totalTime;
            }
            ctx.currentTime = std::clamp(ctx.currentTime, minTime, totalTime);
            ctx.visualTime  = ctx.currentTime +
                              m_editorConfig.visual.getEffectiveVisualOffset();
            ctx.currentTool = m_currentTool.load(std::memory_order_relaxed);
            ctx.isPlaying   = false;
            ctx.isMainAudioSyncFollower = false;
            ctx.lastAudioPos            = 0.0;
            ctx.lastAudioSysTime        = 0.0;
            ctx.hasInitialAudioOffset   = false;
            ctx.playStartSysTime =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            ctx.playStartVisualTime = ctx.currentTime;
            ctx.syncClock.reset(ctx.currentTime);
            ctx.hitFXSystem.clearActiveEffects();

            // 同步进度到音频管理器。这里必须使用逻辑播放时间，
            // visualTime 包含视觉偏移，会导致切换画布时跳到错误位置。
            Audio::AudioManager::instance().seek(ctx.currentTime);

            /// @brief 当前共享视口尺寸快照，用于刷新切换后的活跃 Session。
            auto sharedViewportSizes =
                m_renderSyncRegistry.getSharedViewportSizes();
            for ( const auto& [cid, size] : sharedViewportSizes ) {
                activeSession->pushCommand(
                    CmdUpdateViewport{ cid, size.x, size.y });
            }
        }
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

void EditorEngine::setEditorConfig(const Config::EditorConfig& config)
{
    // 关键修复：从全局 AppConfig 中同步最新的最近项目列表，防止被 UI 设置覆盖
    auto& globalRecent =
        Config::AppConfig::instance().getEditorConfig().recentProjects;

    m_editorConfig                = config;
    m_editorConfig.recentProjects = globalRecent;
    m_frameLimitPreference.store(m_editorConfig.settings.frameLimit,
                                 std::memory_order_relaxed);
    if ( auto* project = ProjectController::instance().currentProject() ) {
        captureToolbarWorkspaceState(
            project->m_settings.m_workspace,
            m_editorConfig,
            m_syncSameMainAudioCanvases.load(std::memory_order_relaxed));
    }

    // 同步回全局 AppConfig 实例
    Config::AppConfig::instance().getEditorConfig() = m_editorConfig;

    // 向所有 Session 广播配置变更
    {
        std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
        /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
        auto& sessions = m_sessionRegistry.entriesUnsafe();
        for ( auto& entry : sessions ) {
            if ( entry.session ) {
                entry.session->pushCommand(
                    LogicCommand(CmdUpdateEditorConfig{ m_editorConfig }));
            }
        }
    }

    const char* limitNames[] = { "VSync",
                                 "2x Refresh Rate",
                                 "4x Refresh Rate",
                                 "8x Refresh Rate",
                                 "Unlimited" };
    XINFO("EditorEngine: Updated config. Frame Limit: {}",
          limitNames[static_cast<int>(m_editorConfig.settings.frameLimit)]);

    // 发布配置更新事件，供 UI 层订阅
    Event::EventBus::instance().publish(
        Event::EditorConfigChangedEvent{ m_editorConfig });
}

void EditorEngine::saveProject()
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    captureProjectWorkspaceState();
    ProjectController::instance().saveProject();
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

    while ( m_running.load(std::memory_order_acquire) ) {
        // 动态获取当前的延迟目标
        double targetDt = 0.0;
        int refreshRate = Config::AppConfig::instance().getDeviceRefreshRate();
        if ( refreshRate <= 0 ) refreshRate = 60;  // 兜底

        Config::FrameLimitPreference frameLimit =
            m_frameLimitPreference.load(std::memory_order_relaxed);
        switch ( frameLimit ) {
        case Config::FrameLimitPreference::VSync:
            targetDt = 1.0 / static_cast<double>(refreshRate);
            break;
        case Config::FrameLimitPreference::Refresh2x:
            targetDt = 1.0 / static_cast<double>(refreshRate * 2);
            break;
        case Config::FrameLimitPreference::Refresh4x:
            targetDt = 1.0 / static_cast<double>(refreshRate * 4);
            break;
        case Config::FrameLimitPreference::Refresh8x:
            targetDt = 1.0 / static_cast<double>(refreshRate * 8);
            break;
        case Config::FrameLimitPreference::Unlimited:
        default: targetDt = 0.0; break;
        }

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

        /// @brief 释放上一轮锁外 update 持有的 Session 引用，并保留 vector
        /// 容量供本轮复用。
        m_sessionUpdateSnapshot.clear();

        // 关键修复：使用 shared_ptr 在锁内获取引用。
        // 这样即使 UI 线程在此时 closeSession 销毁了某个 Session，
        // 逻辑线程持有的共享引用也能保证 session 在 update
        // 期间一直有效。
        // 如果有待处理的项目路径，在锁外处理（避免 EventBus 锁内与 subscribe
        // 交叉）
        /// @brief 项目控制器消费出的本轮项目打开或关闭动作。
        auto projectAction = projectController.consumePendingProjectAction(
            needsCanvasCloseBeforeProjectOpen());
        if ( projectAction.m_closeProject ) {
            closeProject();
        }
        if ( !projectAction.m_projectPathToOpen.empty() ) {
            openProject(projectAction.m_projectPathToOpen);
        }

        // 多 Session 轮询更新
        /// @brief 当前所有有效 Session 指针快照，避免更新时持有注册表锁。
        /// @warning 逻辑热路径/共享指针：这里的 shared_ptr
        /// 拷贝用于保证会话在锁外 update 期间不被 UI 线程关闭释放。
        m_sessionRegistry.fillIndexedSessionSnapshot(m_sessionUpdateSnapshot);

        if ( !m_sessionUpdateSnapshot.empty() ) {
            int32_t activeIndex     = m_sessionRegistry.activeIndex();
            int32_t maxSessionIndex = -1;
            for ( const auto& entry : m_sessionUpdateSnapshot ) {
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
            for ( auto& entry : m_sessionUpdateSnapshot ) {
                bool shouldUpdateSession = entry.index == activeIndex;
                if ( !shouldUpdateSession ) {
                    const bool needsRealtimeUpdate =
                        entry.session->needsRealtimeUpdate();
                    if ( needsRealtimeUpdate ) {
                        shouldUpdateSession = true;
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

                const bool hadPendingCommands =
                    entry.session->hasPendingCommands();
                const double previousCurrentTime =
                    entry.session->getContext().currentTime;
                entry.session->update(
                    dt, m_editorConfig, entry.index == activeIndex);
                if ( hadPendingCommands &&
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
            for ( const auto& entry : m_sessionUpdateSnapshot ) {
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
                    m_sessionUpdateSnapshot, m_sessionRegistry.activeIndex()),
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

/// @brief 处理音频资源更新指令并执行音效预加载和项目保存副作用。
void EditorEngine::handleUpdateAudioResource(const CmdUpdateAudioResource& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    /// @brief 项目命令服务的音频资源更新结果。
    auto result = ProjectController::instance().updateAudioResource(cmd);
    if ( !result.m_updated ) {
        return;
    }

    if ( result.m_effectPreload ) {
        Audio::AudioManager::instance().preloadSoundEffect(
            result.m_effectPreload->m_resource.m_id,
            Config::pathToUtf8(result.m_effectPreload->m_absolutePath),
            result.m_effectPreload->m_resource.m_config.volume);
    }
    saveProject();
}

/// @brief 处理删除音频资源指令并执行音效卸载和项目保存副作用。
void EditorEngine::handleRemoveAudioResource(const CmdRemoveAudioResource& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    /// @brief 项目命令服务的音频资源删除结果。
    auto result = ProjectController::instance().removeAudioResource(cmd);
    if ( !result.m_removed ) {
        return;
    }

    if ( result.m_effectResourceIdToUnload ) {
        Audio::AudioManager::instance().unloadSoundEffect(
            *result.m_effectResourceIdToUnload);
    }
    saveProject();
}

/// @brief 处理删除谱面指令并在项目发生变化时保存。
void EditorEngine::handleRemoveBeatmap(const CmdRemoveBeatmap& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());

    /// @brief 项目命令服务的谱面删除结果。
    auto result = ProjectController::instance().removeBeatmap(cmd);
    if ( result.m_changed ) {
        saveProject();
    }
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
    refreshMainAudioSyncKeysUnsafe();
}

void EditorEngine::scanProjectDirectory()
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    /// @brief 当前项目指针，仅用于解析音效资源绝对路径。
    const auto* currentProject = ProjectController::instance().currentProject();
    if ( !currentProject ) return;

    /// @brief 当前目录资源同步结果。
    auto syncResult = ProjectController::instance().scanProjectDirectory();

    // 自动加载音效
    for ( const auto& res : syncResult.m_effectResourcesToPreload ) {
        /// @brief 新音效资源的项目内绝对路径。
        auto absAudioPath =
            currentProject->m_projectRoot / Config::utf8ToPath(res.m_path);
        Audio::AudioManager::instance().preloadSoundEffect(
            res.m_id, Config::pathToUtf8(absAudioPath), res.m_config.volume);
    }

    // 如果有任何文件发现/删除/更新，保存项目配置
    if ( syncResult.m_changed ) {
        saveProject();
    }
}

}  // namespace MMM::Logic
