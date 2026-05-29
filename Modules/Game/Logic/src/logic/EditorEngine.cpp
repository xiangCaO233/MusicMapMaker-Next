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
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>

namespace MMM::Logic
{

namespace
{
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

std::string getMainAudioSyncKey(const SessionContext& ctx,
                                const Project*        project)
{
    if ( !ctx.currentBeatmap ||
         ctx.currentBeatmap->m_baseMapMetadata.main_audio_path.empty() ) {
        return "";
    }

    std::filesystem::path audioPath =
        ctx.currentBeatmap->m_baseMapMetadata.main_audio_path;
    if ( project && audioPath.is_relative() ) {
        audioPath = project->m_projectRoot / audioPath;
    } else if ( !project && audioPath.is_relative() ) {
        audioPath =
            ctx.currentBeatmap->m_baseMapMetadata.map_path.parent_path() /
            audioPath;
    }

    return Config::pathToUtf8(audioPath.lexically_normal());
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
    }
}

void EditorEngine::closeProject()
{
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

    m_thread = std::thread(&EditorEngine::loop, this);
    XINFO("EditorEngine logic thread started.");
}

void EditorEngine::stop()
{
    ProjectController::instance().stopDirectoryWatcher();

    if ( m_running.exchange(false, std::memory_order_acq_rel) ) {
        if ( m_thread.joinable() ) {
            m_thread.join();
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
    if ( enabled ) {
        syncSameMainAudioCanvases();
    }
}

/// @brief 将使用同一主音轨的非活跃会话同步到当前活跃会话时间。
/// @warning 逻辑热路径/原子：每次 Session update 后可能执行；开关读取使用
/// relaxed，后续只遍历已打开 Session 列表。
void EditorEngine::syncSameMainAudioCanvases()
{
    if ( !m_syncSameMainAudioCanvases.load(std::memory_order_relaxed) ) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    auto& sessions = m_sessionRegistry.entriesUnsafe();
    /// @brief 当前活跃 Session 索引快照。
    int32_t activeIndex = m_sessionRegistry.activeIndex();
    if ( activeIndex < 0 ||
         activeIndex >= static_cast<int32_t>(sessions.size()) ||
         !sessions[activeIndex].session ) {
        return;
    }

    /// @brief 当前项目指针快照，仅用于解析主音轨同步键。
    const auto* currentProject = ProjectController::instance().currentProject();
    auto&       activeCtx      = sessions[activeIndex].session->getContext();
    const auto  activeKey      = getMainAudioSyncKey(activeCtx, currentProject);
    if ( activeKey.empty() ) {
        return;
    }

    for ( int32_t i = 0; i < static_cast<int32_t>(sessions.size()); ++i ) {
        if ( i == activeIndex || !sessions[i].session ) {
            continue;
        }

        auto& ctx = sessions[i].session->getContextMutable();
        if ( getMainAudioSyncKey(ctx, currentProject) != activeKey ) {
            continue;
        }

        ctx.currentTime           = activeCtx.currentTime;
        ctx.visualTime            = activeCtx.visualTime;
        ctx.isPlaying             = false;
        ctx.lastAudioPos          = 0.0;
        ctx.lastAudioSysTime      = 0.0;
        ctx.hasInitialAudioOffset = false;
        ctx.playStartSysTime =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        ctx.playStartVisualTime = ctx.currentTime;
        ctx.syncClock.reset(ctx.currentTime);
        ctx.hitFXSystem.clearActiveEffects();
    }
}

int32_t EditorEngine::createSession(std::shared_ptr<MMM::BeatMap> beatmap,
                                    const std::string&            displayName,
                                    bool isLogoPlaceholder)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    /// @brief 当前注册的 Session 列表，调用者已持有注册表锁。
    auto& sessions = m_sessionRegistry.entriesUnsafe();

    /// @brief 当前项目指针快照，用于规范化新会话谱面中的项目相对路径。
    const auto* currentProject = ProjectController::instance().currentProject();
    if ( currentProject && beatmap ) {
        normalizeBeatmapMetadataPathsForProject(*beatmap, *currentProject);
    }

    // 检查是否可以复用 Logo 占位画布
    if ( !isLogoPlaceholder && beatmap ) {
        for ( int32_t i = 0; i < static_cast<int32_t>(sessions.size()); ++i ) {
            if ( sessions[i].isLogoPlaceholder ) {
                // 复用此画布：加载谱面到它的 Session
                sessions[i].isLogoPlaceholder = false;
                sessions[i].displayName = displayName.empty()
                                              ? beatmap->m_baseMapMetadata.name
                                              : displayName;
                sessions[i].session->pushCommand(
                    LogicCommand(CmdUpdateEditorConfig{ m_editorConfig }));
                sessions[i].session->pushCommand(LogicCommand(CmdChangeTool{
                    m_currentTool.load(std::memory_order_relaxed) }));
                sessions[i].session->pushCommand(
                    LogicCommand(CmdLoadBeatmap{ beatmap }));
                m_sessionRegistry.setActiveIndex(i);

                XINFO("Reused Logo canvas {} for beatmap: {}",
                      sessions[i].cameraId,
                      sessions[i].displayName);
                return i;
            }
        }
    }

    // 生成唯一 cameraId
    /// @brief 新 Session 对应的唯一画布 cameraId。
    std::string cameraId = m_sessionRegistry.createNextCameraId();

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
    entry.isLogoPlaceholder = isLogoPlaceholder;
    /// @brief 新 Session 在注册表中的索引。
    int32_t newIndex = m_sessionRegistry.append(std::move(entry));

    XINFO("Created Session #{} cameraId={} name={} (logo={})",
          newIndex,
          cameraId,
          sessions[newIndex].displayName,
          isLogoPlaceholder);

    return newIndex;
}

void EditorEngine::closeSession(int32_t index)
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

    // 清理对应的 SyncBuffer
    m_renderSyncRegistry.eraseCamera(cameraId);
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
                oldMainAudioKey = getMainAudioSyncKey(
                    oldCtx, ProjectController::instance().currentProject());
                if ( oldCtx.isPlaying ) {
                    oldCtx.currentTime =
                        Audio::AudioManager::instance().getCurrentTime();
                    oldCtx.visualTime =
                        oldCtx.currentTime +
                        m_editorConfig.visual.getEffectiveVisualOffset();
                    oldCtx.isPlaying = false;
                }
                syncedCurrentTime = oldCtx.currentTime;
            }
        }

        if ( m_syncSameMainAudioCanvases.load(std::memory_order_relaxed) &&
             !oldMainAudioKey.empty() && sessions[index].session ) {
            const auto& targetCtx = sessions[index].session->getContext();
            shouldSyncTargetTime =
                getMainAudioSyncKey(
                    targetCtx,
                    ProjectController::instance().currentProject()) ==
                oldMainAudioKey;
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

            auto& ctx = activeSession->getContextMutable();
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
            ctx.currentTime  = std::clamp(ctx.currentTime, minTime, totalTime);
            ctx.visualTime   = ctx.currentTime +
                               m_editorConfig.visual.getEffectiveVisualOffset();
            ctx.currentTool  = m_currentTool.load(std::memory_order_relaxed);
            ctx.isPlaying    = false;
            ctx.lastAudioPos = 0.0;
            ctx.lastAudioSysTime      = 0.0;
            ctx.hasInitialAudioOffset = false;
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

void EditorEngine::setEditorConfig(const Config::EditorConfig& config)
{
    // 关键修复：从全局 AppConfig 中同步最新的最近项目列表，防止被 UI 设置覆盖
    auto& globalRecent =
        Config::AppConfig::instance().getEditorConfig().recentProjects;

    m_editorConfig                = config;
    m_editorConfig.recentProjects = globalRecent;
    m_frameLimitPreference.store(m_editorConfig.settings.frameLimit,
                                 std::memory_order_relaxed);

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
    ProjectController::instance().saveProject();
}

/// @brief 逻辑线程的主循环。
/// @warning 逻辑热路径：按配置 UPS 频率执行；禁止每 update 文件系统操作、完整
/// entt 遍历、完整排序、try/catch 和可避免的 shared_ptr 拷贝。
void EditorEngine::loop()
{
    auto lastTime      = std::chrono::high_resolution_clock::now();
    m_lastUpsTime      = lastTime;
    m_logicUpdateCount = 0;
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

        auto currentTime = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> passed = currentTime - lastTime;

        // 如果设置了帧率限制，并且距离上一帧还没有达到目标时间，就主动让出 CPU
        if ( passed.count() < targetDt ) {
            auto remaining = std::chrono::duration<double>(targetDt) - passed;
            if ( remaining.count() > 0.0015 ) {
                // 剩余时间较长，进行较粗粒度的睡眠（减去 1ms 预留以补偿精度）
                std::this_thread::sleep_for(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        remaining - std::chrono::milliseconds(1)));
            } else {
                // 剩余时间很少，微量让出 CPU 或进行非常短的睡眠
                std::this_thread::yield();
            }
            continue;
        }

        lastTime  = currentTime;
        double dt = passed.count();

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
        std::vector<std::shared_ptr<BeatmapSession>> sessionsSnapshot =
            m_sessionRegistry.sessionSnapshot();

        if ( !sessionsSnapshot.empty() ) {
            for ( auto& session : sessionsSnapshot ) {
                session->update(dt, m_editorConfig);
            }
            syncSameMainAudioCanvases();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // 检查文件夹监听器是否捕获到了任何文件系统变更事件
        static auto lastChangeTime = std::chrono::high_resolution_clock::now();
        static bool hasPendingChange = false;

        if ( projectController.consumeDirectoryChangePending() ) {
            hasPendingChange = true;
            lastChangeTime   = std::chrono::high_resolution_clock::now();
        }

        if ( hasPendingChange ) {
            auto now = std::chrono::high_resolution_clock::now();
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

    /// @brief 项目命令服务的谱面路径更新结果。
    auto result =
        ProjectController::instance().updateBeatmapFilePath(oldPath, newPath);
    if ( result.m_changed ) {
        saveProject();
    }
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
