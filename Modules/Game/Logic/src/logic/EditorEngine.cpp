#include "logic/EditorEngine.h"
#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "event/logic/EditorConfigChangedEvent.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "event/ui/menu/ProjectLoadedEvent.h"
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
#include <fstream>
#include <iomanip>

#include <unordered_set>

namespace MMM::Logic
{

namespace
{
/// @brief 获取会话当前谱面主音轨的稳定比较键。
/// @brief Normalize a project-relative UTF-8 path for stable exclusion checks.
std::string normalizeProjectRelativePath(const std::string& path)
{
    if ( path.empty() ) return "";
    return Config::pathToUtf8(Config::utf8ToPath(path).lexically_normal());
}

/// @brief Return true if the normalized path exists in an exclusion list.
bool containsExcludedPath(const std::vector<std::string>& excludedPaths,
                          const std::string&              path)
{
    std::string normalized = normalizeProjectRelativePath(path);
    return std::any_of(excludedPaths.begin(),
                       excludedPaths.end(),
                       [&](const std::string& excluded) {
                           return normalizeProjectRelativePath(excluded) ==
                                  normalized;
                       });
}

/// @brief Add a normalized path to an exclusion list if it is absent.
void addExcludedPath(std::vector<std::string>& excludedPaths,
                     const std::string&        path)
{
    std::string normalized = normalizeProjectRelativePath(path);
    if ( normalized.empty() ||
         containsExcludedPath(excludedPaths, normalized) ) {
        return;
    }
    excludedPaths.push_back(normalized);
}

/// @brief Remove a normalized path from an exclusion list.
void removeExcludedPath(std::vector<std::string>& excludedPaths,
                        const std::string&        path)
{
    std::string normalized = normalizeProjectRelativePath(path);
    excludedPaths.erase(std::remove_if(excludedPaths.begin(),
                                       excludedPaths.end(),
                                       [&](const std::string& excluded) {
                                           return normalizeProjectRelativePath(
                                                      excluded) == normalized;
                                       }),
                        excludedPaths.end());
}

/// @brief Resolve a stored project-relative path to a filesystem path.
std::filesystem::path resolveProjectPath(const Project&               project,
                                         const std::filesystem::path& path)
{
    if ( path.empty() || path.is_absolute() ) {
        return path.lexically_normal();
    }
    return (project.m_projectRoot / path).lexically_normal();
}

/// @brief Convert a filesystem path into a stable project-relative path.
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

// clang-format off
/// @brief Resolve a metadata resource path before storing it relative to a project.
// clang-format on
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

// clang-format off
/// @brief Normalize long-lived beatmap metadata paths to project-relative paths.
// clang-format on
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

    // 订阅打开项目事件
    // 重要：不在回调里直接调用 openProject！
    // 原因：EventBus::publish 持有 shared_lock，而 openProject 内部会创建
    // BeatmapSession， 其构造函数又会调用 EventBus::subscribe（需
    // unique_lock）。 同一线程无法将 shared_lock 升级为 unique_lock →
    // 永久卡死。 正确做法：将路径存入队列，在逻辑线程 loop() 里境外处理。
    Event::EventBus::instance().subscribe<Event::OpenProjectEvent>(
        [this](const Event::OpenProjectEvent& e) {
            requestOpenProject(e.m_projectPath);
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

void EditorEngine::requestOpenProject(const std::filesystem::path& projectPath)
{
    if ( projectPath.empty() ) {
        return;
    }

    std::lock_guard<std::mutex> lk(m_pendingMutex);
    m_pendingProjectPath.clear();
    m_requestedProjectClose = false;
    m_pendingProjectClose   = false;
    m_projectCloseReady     = false;
    if ( !m_pendingProjectSwitchPath.empty() ) {
        m_requestedProjectPath.clear();
        m_pendingProjectSwitchPath = projectPath;
    } else {
        m_requestedProjectPath = projectPath;
    }
}

void EditorEngine::requestCloseProject()
{
    std::lock_guard<std::mutex> lk(m_pendingMutex);
    m_pendingProjectPath.clear();
    m_requestedProjectPath.clear();
    m_pendingProjectSwitchPath.clear();
    m_requestedProjectClose = true;
    m_pendingProjectClose   = false;
    m_projectCloseReady     = false;
}

bool EditorEngine::hasPendingProjectSwitch() const
{
    std::lock_guard<std::mutex> lk(m_pendingMutex);
    return !m_pendingProjectSwitchPath.empty() || m_pendingProjectClose;
}

void EditorEngine::completePendingProjectSwitch()
{
    std::lock_guard<std::mutex> lk(m_pendingMutex);
    if ( m_pendingProjectClose ) {
        m_pendingProjectClose = false;
        m_projectCloseReady   = true;
        return;
    }

    if ( m_pendingProjectSwitchPath.empty() ) return;

    m_pendingProjectPath = m_pendingProjectSwitchPath;
    m_pendingProjectSwitchPath.clear();
}

void EditorEngine::cancelPendingProjectSwitch()
{
    std::lock_guard<std::mutex> lk(m_pendingMutex);
    m_pendingProjectPath.clear();
    m_requestedProjectPath.clear();
    m_pendingProjectSwitchPath.clear();
    m_requestedProjectClose = false;
    m_pendingProjectClose   = false;
    m_projectCloseReady     = false;
}

bool EditorEngine::needsCanvasCloseBeforeProjectOpen() const
{
    return m_sessionRegistry.hasNonLogoSession();
}

void EditorEngine::openProject(const std::filesystem::path& projectPath)
{
    std::filesystem::path actualProjectPath = projectPath;
    std::filesystem::path targetBeatmapPath = "";

    // 如果传入的是文件，则将其父目录作为项目目录，并记录该文件为目标加载谱面
    if ( std::filesystem::exists(projectPath) &&
         std::filesystem::is_regular_file(projectPath) ) {
        actualProjectPath = projectPath.parent_path();
        targetBeatmapPath = projectPath;
    }

    if ( !std::filesystem::exists(actualProjectPath) ||
         !std::filesystem::is_directory(actualProjectPath) ) {
        XERROR(
            "Failed to open project: Path does not exist or is not a "
            "directory: {}",
            Config::pathToUtf8(actualProjectPath));
        return;
    }

    XINFO("Opening project at: {}", Config::pathToUtf8(actualProjectPath));

    closeProject();

    auto newProject                = std::make_unique<Project>();
    newProject->m_projectRoot      = actualProjectPath;
    newProject->m_metadata.m_title = Config::pathToUtf8(
        actualProjectPath.filename());  // 默认标题为目录名（UTF-8）

    // 扫描文件系统
    try {
        auto directoryScan = m_projectDirectoryScanner.scan(actualProjectPath);
        if ( !directoryScan.m_success ) {
            XERROR("Error while scanning project directory: {}",
                   Config::pathToUtf8(actualProjectPath));
        }

        const auto& mapFiles   = directoryScan.m_beatmapFiles;
        const auto& audioFiles = directoryScan.m_audioFiles;

        // 记录哪些音频被识别为主音轨
        std::unordered_set<std::string> mainAudioPaths;

        // 1. 处理谱面并识别主音轨
        for ( const auto& mapPath : mapFiles ) {
            auto relMapPath = Config::pathToUtf8(
                std::filesystem::relative(mapPath, actualProjectPath));
            auto filename = Config::pathToUtf8(mapPath.filename());

            Project::BeatmapEntry mapEntry;
            mapEntry.m_name     = filename;
            mapEntry.m_filePath = relMapPath;

            // 预加载谱面以获取其定义的主音频路径
            try {
                auto tempMap = BeatMap::loadFromFile(mapPath);
                normalizeBeatmapMetadataPathsForProject(tempMap, *newProject);
                if ( !tempMap.m_baseMapMetadata.main_audio_path.empty() ) {
                    // 获取相对于项目根目录的音频路径
                    auto absAudioPath =
                        newProject->m_projectRoot /
                        tempMap.m_baseMapMetadata.main_audio_path;
                    auto relAudioPath = Config::pathToUtf8(
                        tempMap.m_baseMapMetadata.main_audio_path);

                    mapEntry.m_audioTrackId =
                        Config::pathToUtf8(absAudioPath.filename());
                    mainAudioPaths.insert(relAudioPath);
                }
            } catch ( ... ) {
                XWARN("Failed to probe main audio for beatmap: {}", filename);
            }

            newProject->m_beatmaps.push_back(mapEntry);
            XINFO("Found beatmap: {}", filename);
        }

        // 2. 处理所有音频资源
        for ( const auto& audioPath : audioFiles ) {
            auto relAudioPath = Config::pathToUtf8(
                std::filesystem::relative(audioPath, actualProjectPath));
            auto filename = Config::pathToUtf8(audioPath.filename());

            AudioResource res;
            res.m_id   = filename;
            res.m_path = relAudioPath;
            // 如果该音频在任意一个谱面中被引用为主音轨，则标记为 Main，否则为
            // Effect
            res.m_type          = (mainAudioPaths.count(relAudioPath) > 0)
                                      ? AudioTrackType::Main
                                      : AudioTrackType::Effect;
            res.m_config.volume = 0.5f;
            res.m_config.playbackSpeed = 1.0f;
            res.m_config.playbackPitch = 0.0f;
            res.m_config.muted         = false;
            res.m_config.eqEnabled     = false;
            res.m_config.eqPreset      = 0;

            newProject->m_audioResources.push_back(res);
            XINFO("Found {} audio resource: {}",
                  (res.m_type == AudioTrackType::Main ? "Main" : "Effect"),
                  filename);
        }

        // 3. 兜底逻辑：如果没有任何 Main
        // 音轨但有音频，且有谱面没关联音轨，关联第一个
        if ( mainAudioPaths.empty() && !newProject->m_audioResources.empty() ) {
            newProject->m_audioResources.front().m_type = AudioTrackType::Main;
            for ( auto& map : newProject->m_beatmaps ) {
                if ( map.m_audioTrackId.empty() ) {
                    map.m_audioTrackId =
                        newProject->m_audioResources.front().m_id;
                }
            }
        }

    } catch ( const std::exception& e ) {
        XERROR("Error while scanning project directory: {}", e.what());
    }

    // 检查是否有项目描述文件
    std::filesystem::path projectFile = actualProjectPath / "mmm_project.json";
    if ( std::filesystem::exists(projectFile) ) {
        try {
            std::ifstream  file(projectFile);
            nlohmann::json j;
            file >> j;
            Project loadedProject  = j.get<Project>();
            newProject->m_metadata = loadedProject.m_metadata;
            newProject->m_settings = loadedProject.m_settings;
            newProject->m_excludedBeatmapPaths =
                loadedProject.m_excludedBeatmapPaths;
            newProject->m_excludedAudioPaths =
                loadedProject.m_excludedAudioPaths;

            // 合并资源配置并应用到音频引擎
            for ( auto& res : newProject->m_audioResources ) {
                for ( const auto& loadedRes : loadedProject.m_audioResources ) {
                    if ( res.m_id == loadedRes.m_id ) {
                        res.m_type   = loadedRes.m_type;
                        res.m_config = loadedRes.m_config;

                        // 如果是项目音效，应用音量
                        if ( res.m_type == AudioTrackType::Effect ) {
                            Audio::AudioManager::instance().setSFXPoolVolume(
                                res.m_id, res.m_config.volume, false);
                        }
                        break;
                    }
                }
            }

            XINFO("Project configuration loaded from mmm_project.json");
        } catch ( ... ) {
            XWARN(
                "Failed to load existing mmm_project.json, using scanned "
                "results.");
        }
    }

    // 自动持久化扫描结果 (标记此目录为项目)
    newProject->m_beatmaps.erase(
        std::remove_if(newProject->m_beatmaps.begin(),
                       newProject->m_beatmaps.end(),
                       [&](const Project::BeatmapEntry& entry) {
                           return containsExcludedPath(
                               newProject->m_excludedBeatmapPaths,
                               entry.m_filePath);
                       }),
        newProject->m_beatmaps.end());
    newProject->m_audioResources.erase(
        std::remove_if(newProject->m_audioResources.begin(),
                       newProject->m_audioResources.end(),
                       [&](const AudioResource& res) {
                           return containsExcludedPath(
                               newProject->m_excludedAudioPaths, res.m_path);
                       }),
        newProject->m_audioResources.end());

    try {
        std::ofstream  file(projectFile);
        nlohmann::json j = *newProject;
        file << std::setw(4) << j << std::endl;
    } catch ( ... ) {
    }

    // 预加载所有项目内的音效资源
    for ( const auto& res : newProject->m_audioResources ) {
        if ( res.m_type == AudioTrackType::Effect ) {
            auto absPath = actualProjectPath / Config::utf8ToPath(res.m_path);
            if ( std::filesystem::exists(absPath) ) {
                Audio::AudioManager::instance().preloadSoundEffect(
                    res.m_id, Config::pathToUtf8(absPath), res.m_config.volume);
            }
        }
    }

    // 更新当前项目单例状态
    {
        std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
        m_currentProject = std::move(newProject);
    }

    // 发布加载成功事件
    Event::ProjectLoadedEvent loadedEv;
    loadedEv.m_projectTitle = m_currentProject->m_metadata.m_title;
    loadedEv.m_projectPath  = Config::pathToUtf8(actualProjectPath);
    loadedEv.m_beatmapCount = m_currentProject->m_beatmaps.size();
    Event::EventBus::instance().publish(loadedEv);

    XINFO("Project '{}' loaded successfully with {} beatmaps.",
          loadedEv.m_projectTitle,
          loadedEv.m_beatmapCount);

    // 启动文件夹监听器实时监控项目目录
    startDirectoryWatcher(actualProjectPath);

    // 记录到最近打开列表
    Config::AppConfig::instance().addRecentProject(
        Config::pathToUtf8(actualProjectPath));

    // 如果指定了谱面路径，则通过 createSession 加载它
    if ( !targetBeatmapPath.empty() ) {
        XINFO("Auto loading beatmap: {}",
              Config::pathToUtf8(targetBeatmapPath));
        try {
            auto map = std::make_shared<BeatMap>(
                BeatMap::loadFromFile(targetBeatmapPath));
            createSession(map, map->m_baseMapMetadata.name);
        } catch ( const std::exception& e ) {
            XERROR("Failed to auto load beatmap {}: {}",
                   Config::pathToUtf8(targetBeatmapPath),
                   e.what());
        }
    }
}

void EditorEngine::closeProject()
{
    std::unique_ptr<Project> closedProject;
    std::string              projectTitle;
    {
        std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
        if ( !m_currentProject ) {
            return;
        }
        projectTitle  = m_currentProject->m_metadata.m_title;
        closedProject = std::move(m_currentProject);
    }

    stopDirectoryWatcher();

    auto& audio = Audio::AudioManager::instance();
    audio.stop();
    audio.clearAllScheduledSoundEffects();
    audio.unloadBGM();

    for ( const auto& res : closedProject->m_audioResources ) {
        if ( res.m_type == AudioTrackType::Effect ) {
            audio.unloadSoundEffect(res.m_id);
        }
    }

    XINFO("Project '{}' closed.", projectTitle);
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
    stopDirectoryWatcher();

    if ( m_running.exchange(false, std::memory_order_acq_rel) ) {
        if ( m_thread.joinable() ) {
            m_thread.join();
        }
        XINFO("EditorEngine logic thread stopped.");
    }
}

void EditorEngine::handleCreateBeatmap(const CmdCreateBeatmap& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    if ( !m_currentProject ) {
        XERROR("Cannot create beatmap: No project opened.");
        return;
    }

    auto meta = cmd.baseMeta;
    XINFO("Creating new beatmap: {} (Title: {})", meta.name, meta.title);

    // 1. 确定文件保存路径 (默认在项目根目录下，以 name.imd 命名)
    std::string safeFilename = meta.name;
    // 替换非法字符
    std::replace_if(
        safeFilename.begin(),
        safeFilename.end(),
        [](char c) {
            return c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                   c == '"' || c == '<' || c == '>' || c == '|';
        },
        '_');

    std::filesystem::path mapPath = m_currentProject->m_projectRoot /
                                    Config::utf8ToPath(safeFilename + ".mmm");

    // 如果文件已存在，增加后缀
    int suffix = 1;
    while ( std::filesystem::exists(mapPath) ) {
        mapPath = m_currentProject->m_projectRoot /
                  Config::utf8ToPath(safeFilename + "_" +
                                     std::to_string(suffix++) + ".mmm");
    }

    meta.map_path = makeProjectRelativePath(*m_currentProject, mapPath);

    // 处理音频资源路径 (如果是绝对路径，尝试转为相对路径)
    meta.main_audio_path =
        makeProjectRelativePath(*m_currentProject, meta.main_audio_path);
    meta.main_cover_path =
        makeProjectRelativePath(*m_currentProject, meta.main_cover_path);
    meta.cover_path =
        makeProjectRelativePath(*m_currentProject, meta.cover_path);

    // 2. 创建 BeatMap 对象
    auto newBeatmap               = std::make_shared<MMM::BeatMap>();
    newBeatmap->m_baseMapMetadata = meta;

    // 3. 保存文件
    try {
        newBeatmap->saveToFile(mapPath);
        XINFO("Beatmap saved to: {}", Config::pathToUtf8(mapPath));
    } catch ( const std::exception& e ) {
        XERROR("Failed to save new beatmap: {}", e.what());
        return;
    }

    // 4. 更新项目列表
    Project::BeatmapEntry entry;
    entry.m_name     = meta.name;
    entry.m_filePath = Config::pathToUtf8(
        std::filesystem::relative(mapPath, m_currentProject->m_projectRoot));
    entry.m_audioTrackId = Config::pathToUtf8(meta.main_audio_path.filename());
    removeExcludedPath(m_currentProject->m_excludedBeatmapPaths,
                       entry.m_filePath);
    m_currentProject->m_beatmaps.push_back(entry);

    // 5. 如果音频资源不在列表中，添加进去
    bool audioExists = false;
    for ( const auto& res : m_currentProject->m_audioResources ) {
        if ( res.m_path == Config::pathToUtf8(meta.main_audio_path) ) {
            audioExists = true;
            break;
        }
    }
    if ( !audioExists && !meta.main_audio_path.empty() ) {
        AudioResource res;
        res.m_id   = Config::pathToUtf8(meta.main_audio_path.filename());
        res.m_path = Config::pathToUtf8(meta.main_audio_path);
        res.m_type = AudioTrackType::Main;
        res.m_config.volume = 0.5f;
        removeExcludedPath(m_currentProject->m_excludedAudioPaths, res.m_path);
        m_currentProject->m_audioResources.push_back(res);
    }

    // 保存项目文件
    saveProject();

    // 6. 立即在新画布中加载这个新谱面
    createSession(newBeatmap, meta.name);
}

void EditorEngine::handleImportAudio(const CmdImportAudio& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    if ( !m_currentProject ) {
        XERROR("Cannot import audio: No project opened.");
        return;
    }

    std::filesystem::path audioPath = Config::utf8ToPath(cmd.path);
    if ( !std::filesystem::exists(audioPath) ) {
        XERROR("Cannot import audio: File does not exist: {}", cmd.path);
        return;
    }

    XINFO("Importing audio: {}", cmd.path);

    // 1. 确定在项目中的相对路径
    std::filesystem::path finalPath = audioPath;
    bool                  needsCopy = false;

    try {
        // 检查是否已经在项目目录下
        auto absAudioPath = std::filesystem::absolute(audioPath);
        auto absRoot =
            std::filesystem::absolute(m_currentProject->m_projectRoot);

        auto [rootIt, pathIt] = std::mismatch(absRoot.begin(),
                                              absRoot.end(),
                                              absAudioPath.begin(),
                                              absAudioPath.end());

        if ( rootIt != absRoot.end() ) {
            // 在项目外，标记需要复制到项目根目录
            needsCopy = true;
            finalPath = m_currentProject->m_projectRoot / audioPath.filename();
            // 如果文件名冲突，加后缀
            int suffix = 1;
            while ( std::filesystem::exists(finalPath) ) {
                finalPath = m_currentProject->m_projectRoot /
                            Config::utf8ToPath(
                                Config::pathToUtf8(audioPath.stem()) + "_" +
                                std::to_string(suffix++) +
                                Config::pathToUtf8(audioPath.extension()));
            }
        } else {
            // 已在项目内，转为相对路径
            finalPath = std::filesystem::relative(absAudioPath, absRoot);
        }
    } catch ( ... ) {
        needsCopy = true;
        finalPath = m_currentProject->m_projectRoot / audioPath.filename();
    }

    // 2. 如果需要，执行物理复制
    if ( needsCopy ) {
        try {
            std::filesystem::copy_file(audioPath, finalPath);
            XINFO("Copied external audio to project: {}",
                  Config::pathToUtf8(finalPath));
            finalPath = std::filesystem::relative(
                finalPath, m_currentProject->m_projectRoot);
        } catch ( const std::exception& e ) {
            XERROR("Failed to copy audio file: {}", e.what());
            return;
        }
    }

    // 3. 检查是否已经在列表中
    std::string relPathUtf8 = Config::pathToUtf8(finalPath);
    removeExcludedPath(m_currentProject->m_excludedAudioPaths, relPathUtf8);
    for ( const auto& res : m_currentProject->m_audioResources ) {
        if ( res.m_path == relPathUtf8 ) {
            XWARN("Audio already exists in project: {}", relPathUtf8);
            return;
        }
    }

    // 4. 添加到资源列表
    AudioResource res;
    res.m_id                   = Config::pathToUtf8(finalPath.filename());
    res.m_path                 = relPathUtf8;
    res.m_type                 = cmd.trackType;
    res.m_config.volume        = 0.5f;
    res.m_config.playbackSpeed = 1.0f;
    res.m_config.playbackPitch = 0.0f;
    res.m_config.muted         = false;

    m_currentProject->m_audioResources.push_back(res);

    // 5. 立即预加载音效资源
    if ( res.m_type == AudioTrackType::Effect ) {
        auto absFinalPath = m_currentProject->m_projectRoot / finalPath;
        Audio::AudioManager::instance().preloadSoundEffect(
            res.m_id, Config::pathToUtf8(absFinalPath), res.m_config.volume);
    }

    // 6. 保存项目配置
    saveProject();

    XINFO("Successfully imported audio: {} as ID: {}", relPathUtf8, res.m_id);
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

void EditorEngine::syncProjectWithFile(const std::filesystem::path& mapPath)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    if ( !m_currentProject ) return;

    // 检查路径是否在项目根目录下
    auto absMapPath = std::filesystem::absolute(mapPath);
    auto absRoot = std::filesystem::absolute(m_currentProject->m_projectRoot);

    auto [rootIt, pathIt] = std::mismatch(
        absRoot.begin(), absRoot.end(), absMapPath.begin(), absMapPath.end());

    if ( rootIt != absRoot.end() ) {
        // 不在项目根目录下，忽略
        return;
    }

    std::string relMapPath =
        Config::pathToUtf8(std::filesystem::relative(absMapPath, absRoot));
    removeExcludedPath(m_currentProject->m_excludedBeatmapPaths, relMapPath);

    // 检查是否已经存在
    for ( const auto& entry : m_currentProject->m_beatmaps ) {
        auto entryPath = absRoot / Config::utf8ToPath(entry.m_filePath);
        if ( std::filesystem::exists(entryPath) &&
             std::filesystem::equivalent(entryPath, absMapPath) ) {
            return;  // 已存在
        }
    }

    // 添加到项目列表
    try {
        auto map = BeatMap::loadFromFile(absMapPath);
        normalizeBeatmapMetadataPathsForProject(map, *m_currentProject);

        Project::BeatmapEntry entry;
        entry.m_name = map.m_baseMapMetadata.version;
        if ( entry.m_name.empty() )
            entry.m_name = Config::pathToUtf8(absMapPath.filename());

        entry.m_filePath = relMapPath;

        if ( !map.m_baseMapMetadata.main_audio_path.empty() ) {
            entry.m_audioTrackId = Config::pathToUtf8(
                map.m_baseMapMetadata.main_audio_path.filename());
        }

        m_currentProject->m_beatmaps.push_back(entry);
        XINFO("EditorEngine: Discovered new beatmap for project: {}",
              entry.m_name);

        saveProject();
    } catch ( const std::exception& e ) {
        XWARN("EditorEngine: Failed to sync new beatmap {}: {}",
              Config::pathToUtf8(absMapPath),
              e.what());
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
    // 回退到默认图集 (Basic2DCanvas)
    // 该回退逻辑已收敛到 RenderSyncRegistry::getAtlasUVMap()。
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

    auto&      activeCtx = sessions[activeIndex].session->getContext();
    const auto activeKey =
        getMainAudioSyncKey(activeCtx, m_currentProject.get());
    if ( activeKey.empty() ) {
        return;
    }

    for ( int32_t i = 0; i < static_cast<int32_t>(sessions.size()); ++i ) {
        if ( i == activeIndex || !sessions[i].session ) {
            continue;
        }

        auto& ctx = sessions[i].session->getContextMutable();
        if ( getMainAudioSyncKey(ctx, m_currentProject.get()) != activeKey ) {
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

    if ( m_currentProject && beatmap ) {
        normalizeBeatmapMetadataPathsForProject(*beatmap, *m_currentProject);
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

    // 修正活跃索引
    // 该职责已收敛到 SessionRegistry::erase()。
    {
        // 关闭的是当前活跃的，切换到前一个或第一个
        // 关闭的在活跃的前面，索引需要减一
    }
    // 关闭的是当前活跃的，切换到前一个或第一个
    // 该分支已收敛到 SessionRegistry::normalizeActiveIndexAfterErase()。
    // 关闭的在活跃的前面，索引需要减一
    // 该分支已收敛到 SessionRegistry::normalizeActiveIndexAfterErase()。
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
                auto& oldCtx = oldSession->getContextMutable();
                oldMainAudioKey =
                    getMainAudioSyncKey(oldCtx, m_currentProject.get());
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
                getMainAudioSyncKey(targetCtx, m_currentProject.get()) ==
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
    if ( !m_currentProject ) return;

    std::filesystem::path projectFile =
        m_currentProject->m_projectRoot / "mmm_project.json";
    XINFO("Saving project to {}", Config::pathToUtf8(projectFile));

    try {
        std::ofstream  file(projectFile);
        nlohmann::json j = *m_currentProject;
        file << std::setw(4) << j << std::endl;
        XINFO("Project saved successfully.");
    } catch ( const std::exception& e ) {
        XERROR("Failed to save project: {}", e.what());
    }
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
        std::filesystem::path pendingPath;
        std::filesystem::path requestedPath;
        bool                  requestedClose = false;
        bool                  closeReady     = false;
        {
            std::lock_guard<std::mutex> lk(m_pendingMutex);
            if ( m_requestedProjectClose ) {
                requestedClose          = true;
                m_requestedProjectClose = false;
            }
            if ( !m_requestedProjectPath.empty() ) {
                requestedPath = m_requestedProjectPath;
                m_requestedProjectPath.clear();
            }
        }
        if ( requestedClose ) {
            if ( needsCanvasCloseBeforeProjectOpen() ) {
                std::lock_guard<std::mutex> lk(m_pendingMutex);
                m_pendingProjectPath.clear();
                m_pendingProjectSwitchPath.clear();
                m_pendingProjectClose = true;
                XINFO(
                    "Project close deferred until current beatmap canvases "
                    "close.");
            } else {
                closeReady = true;
            }
        }
        if ( !requestedPath.empty() ) {
            if ( needsCanvasCloseBeforeProjectOpen() ) {
                std::lock_guard<std::mutex> lk(m_pendingMutex);
                m_pendingProjectPath.clear();
                m_pendingProjectClose      = false;
                m_pendingProjectSwitchPath = requestedPath;
                XINFO(
                    "Project open deferred until current beatmap canvases "
                    "close: {}",
                    Config::pathToUtf8(requestedPath));
            } else {
                pendingPath = requestedPath;
            }
        }
        {
            std::lock_guard<std::mutex> lk(m_pendingMutex);
            if ( m_projectCloseReady ) {
                closeReady          = true;
                m_projectCloseReady = false;
            }
            if ( pendingPath.empty() && !m_pendingProjectPath.empty() ) {
                pendingPath = m_pendingProjectPath;
                m_pendingProjectPath.clear();
            }
        }
        if ( closeReady ) {
            closeProject();
        }
        if ( !pendingPath.empty() ) {
            openProject(pendingPath);
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

        if ( m_projectDirectoryWatcher.consumeChangePending() ) {
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

void EditorEngine::handleUpdateAudioResource(const CmdUpdateAudioResource& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    if ( !m_currentProject ) return;

    XINFO("Updating audio resource type: {} -> {}",
          cmd.id,
          (cmd.newType == AudioTrackType::Main ? "Main" : "Effect"));

    for ( auto& res : m_currentProject->m_audioResources ) {
        if ( res.m_id == cmd.id ) {
            res.m_type = cmd.newType;
            // 如果切为音效，确保加载到池中
            if ( res.m_type == AudioTrackType::Effect ) {
                auto absPath = m_currentProject->m_projectRoot /
                               Config::utf8ToPath(res.m_path);
                if ( std::filesystem::exists(absPath) ) {
                    Audio::AudioManager::instance().preloadSoundEffect(
                        res.m_id,
                        Config::pathToUtf8(absPath),
                        res.m_config.volume);
                }
            }
            break;
        }
    }

    saveProject();
}

void EditorEngine::handleRemoveAudioResource(const CmdRemoveAudioResource& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    if ( !m_currentProject ) return;

    XINFO("Removing audio resource from project: {}", cmd.id);

    std::string removedPath;
    auto&       resources = m_currentProject->m_audioResources;
    resources.erase(
        std::remove_if(
            resources.begin(),
            resources.end(),
            [&](const AudioResource& res) {
                if ( res.m_id != cmd.id ) {
                    return false;
                }
                removedPath = res.m_path;
                if ( res.m_type == AudioTrackType::Effect ) {
                    Audio::AudioManager::instance().unloadSoundEffect(res.m_id);
                }
                return true;
            }),
        resources.end());
    addExcludedPath(m_currentProject->m_excludedAudioPaths, removedPath);

    // 同时清理谱面对该音轨的引用
    for ( auto& map : m_currentProject->m_beatmaps ) {
        if ( map.m_audioTrackId == cmd.id ) {
            map.m_audioTrackId = "";
        }
    }

    saveProject();
}

void EditorEngine::handleRemoveBeatmap(const CmdRemoveBeatmap& cmd)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    if ( !m_currentProject ) return;

    XINFO("Removing beatmap from project list: {}", cmd.filePath);

    addExcludedPath(m_currentProject->m_excludedBeatmapPaths, cmd.filePath);

    auto& maps = m_currentProject->m_beatmaps;
    maps.erase(std::remove_if(maps.begin(),
                              maps.end(),
                              [&](const Project::BeatmapEntry& e) {
                                  return e.m_filePath == cmd.filePath;
                              }),
               maps.end());

    saveProject();
}

void EditorEngine::updateBeatmapFilePathInProject(
    const std::filesystem::path& oldPath, const std::filesystem::path& newPath)
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    if ( !m_currentProject ) return;

    auto absRoot = std::filesystem::absolute(m_currentProject->m_projectRoot);
    auto absOld  = resolveProjectPath(*m_currentProject, oldPath);
    auto absNew  = resolveProjectPath(*m_currentProject, newPath);

    std::error_code oldEc;
    std::error_code newEc;
    auto relOldPath = std::filesystem::relative(absOld, absRoot, oldEc);
    auto relNewPath = std::filesystem::relative(absNew, absRoot, newEc);

    std::string relOld =
        (oldEc || relOldPath.empty()) ? "" : Config::pathToUtf8(relOldPath);
    std::string relNew =
        (newEc || relNewPath.empty()) ? "" : Config::pathToUtf8(relNewPath);

    bool found = false;
    for ( auto& entry : m_currentProject->m_beatmaps ) {
        if ( entry.m_filePath == relOld ) {
            entry.m_filePath = relNew;
            try {
                auto map = BeatMap::loadFromFile(absNew);
                normalizeBeatmapMetadataPathsForProject(map, *m_currentProject);
                entry.m_name = map.m_baseMapMetadata.version;
                if ( entry.m_name.empty() )
                    entry.m_name = Config::pathToUtf8(absNew.filename());
                if ( !map.m_baseMapMetadata.main_audio_path.empty() ) {
                    entry.m_audioTrackId = Config::pathToUtf8(
                        map.m_baseMapMetadata.main_audio_path.filename());
                }
            } catch ( ... ) {
            }
            found = true;
            break;
        }
    }

    if ( !found ) {
        syncProjectWithFile(newPath);
    } else {
        saveProject();
    }
}

void EditorEngine::scanProjectDirectory()
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionRegistry.mutex());
    if ( !m_currentProject ) return;

    auto actualProjectPath = m_currentProject->m_projectRoot;

    // 收集当前目录下的所有有效文件
    std::vector<std::filesystem::path> mapFiles;
    std::vector<std::filesystem::path> audioFiles;

    try {
        auto directoryScan = m_projectDirectoryScanner.scan(actualProjectPath);
        if ( !directoryScan.m_success ) {
            return;  // 防御可能的文件系统并发读写冲突
        }
        mapFiles   = std::move(directoryScan.m_beatmapFiles);
        audioFiles = std::move(directoryScan.m_audioFiles);
    } catch ( ... ) {
        return;  // 防御可能的文件系统并发读写冲突
    }

    bool changed = false;

    // 1. 同步谱面文件列表
    std::vector<Project::BeatmapEntry> newBeatmaps;
    std::unordered_set<std::string>    mainAudioPaths;

    for ( const auto& mapPath : mapFiles ) {
        auto relMapPath = Config::pathToUtf8(
            std::filesystem::relative(mapPath, actualProjectPath));
        auto filename = Config::pathToUtf8(mapPath.filename());
        if ( containsExcludedPath(m_currentProject->m_excludedBeatmapPaths,
                                  relMapPath) ) {
            continue;
        }

        // 查找是否已经存在该谱面 entry
        Project::BeatmapEntry mapEntry;
        bool                  exists = false;
        for ( const auto& entry : m_currentProject->m_beatmaps ) {
            if ( entry.m_filePath == relMapPath ) {
                mapEntry = entry;
                exists   = true;
                break;
            }
        }

        if ( !exists ) {
            // 新发现的谱面
            mapEntry.m_name     = filename;
            mapEntry.m_filePath = relMapPath;
            try {
                auto tempMap = BeatMap::loadFromFile(mapPath);
                normalizeBeatmapMetadataPathsForProject(tempMap,
                                                        *m_currentProject);
                if ( !tempMap.m_baseMapMetadata.main_audio_path.empty() ) {
                    auto absAudioPath =
                        m_currentProject->m_projectRoot /
                        tempMap.m_baseMapMetadata.main_audio_path;
                    auto relAudioPath = Config::pathToUtf8(
                        tempMap.m_baseMapMetadata.main_audio_path);
                    mapEntry.m_audioTrackId =
                        Config::pathToUtf8(absAudioPath.filename());
                    mainAudioPaths.insert(relAudioPath);
                }
            } catch ( ... ) {
            }
            changed = true;
            XINFO("Directory Listener: Discovered new beatmap: {}", filename);
        } else {
            // 已有谱面，收集其主音轨引用
            if ( !mapEntry.m_audioTrackId.empty() ) {
                try {
                    auto tempMap = BeatMap::loadFromFile(mapPath);
                    normalizeBeatmapMetadataPathsForProject(tempMap,
                                                            *m_currentProject);
                    if ( !tempMap.m_baseMapMetadata.main_audio_path.empty() ) {
                        auto absAudioPath =
                            m_currentProject->m_projectRoot /
                            tempMap.m_baseMapMetadata.main_audio_path;
                        auto relAudioPath = Config::pathToUtf8(
                            tempMap.m_baseMapMetadata.main_audio_path);
                        mainAudioPaths.insert(relAudioPath);
                    }
                } catch ( ... ) {
                }
            }
        }
        newBeatmaps.push_back(mapEntry);
    }

    // 如果有任何谱面被删除了
    if ( newBeatmaps.size() != m_currentProject->m_beatmaps.size() ) {
        changed = true;
        XINFO(
            "Directory Listener: Some beatmaps were removed from the "
            "directory.");
    }
    m_currentProject->m_beatmaps = newBeatmaps;

    // 2. 同步音频资源列表
    std::vector<AudioResource> newAudioResources;
    for ( const auto& audioPath : audioFiles ) {
        auto relAudioPath = Config::pathToUtf8(
            std::filesystem::relative(audioPath, actualProjectPath));
        auto filename = Config::pathToUtf8(audioPath.filename());
        if ( containsExcludedPath(m_currentProject->m_excludedAudioPaths,
                                  relAudioPath) ) {
            continue;
        }

        // 查找是否已经存在该音频资源
        AudioResource res;
        bool          exists = false;
        for ( const auto& existingRes : m_currentProject->m_audioResources ) {
            if ( existingRes.m_path == relAudioPath ) {
                res    = existingRes;
                exists = true;
                break;
            }
        }

        if ( !exists ) {
            // 新加入的音频
            res.m_id            = filename;
            res.m_path          = relAudioPath;
            res.m_type          = (mainAudioPaths.count(relAudioPath) > 0)
                                      ? AudioTrackType::Main
                                      : AudioTrackType::Effect;
            res.m_config.volume = 0.5f;
            res.m_config.playbackSpeed = 1.0f;
            res.m_config.playbackPitch = 0.0f;
            res.m_config.muted         = false;
            res.m_config.eqEnabled     = false;
            res.m_config.eqPreset      = 0;

            // 自动加载音效
            if ( res.m_type == AudioTrackType::Effect ) {
                Audio::AudioManager::instance().preloadSoundEffect(
                    res.m_id,
                    Config::pathToUtf8(audioPath),
                    res.m_config.volume);
            }
            changed = true;
            XINFO("Directory Listener: Discovered new audio file: {}",
                  filename);
        } else {
            // 已有音频，如果被谱面引用为主音轨，则强制设为主音轨
            // 如果未被引用，保持其原有的类型（尊重用户的显式配置）
            if ( mainAudioPaths.count(relAudioPath) > 0 ) {
                if ( res.m_type != AudioTrackType::Main ) {
                    res.m_type = AudioTrackType::Main;
                    changed    = true;
                }
            }
        }
        newAudioResources.push_back(res);
    }

    // 如果有任何音频被删除了
    if ( newAudioResources.size() !=
         m_currentProject->m_audioResources.size() ) {
        changed = true;
        XINFO(
            "Directory Listener: Some audio files were removed from the "
            "directory.");
    }
    m_currentProject->m_audioResources = newAudioResources;

    // 如果有任何文件发现/删除/更新，保存项目配置
    if ( changed ) {
        saveProject();
    }
}

/// @brief 启动文件夹监听器。
/// @param path 需要递归监听的项目目录路径。
void EditorEngine::startDirectoryWatcher(const std::filesystem::path& path)
{
    m_projectDirectoryWatcher.start(path);
}

/// @brief 停止文件夹监听器。
void EditorEngine::stopDirectoryWatcher()
{
    {
        {
            // 触发退出事件，使 ReadDirectoryChangesW 阻塞立刻解除并退出
        }
    }
    m_projectDirectoryWatcher.stop();

    // 在线程完全退出并 Join 之后，再安全地在主线程清理句柄，防止重叠 I/O
    // 并发冲突
}

/// @brief 文件夹监听线程的主循环。
/// @param watchPath 需要递归监听的项目目录路径。
void EditorEngine::watcherThreadLoop(std::filesystem::path watchPath)
{
    /// @brief ProjectDirectoryWatcher
    /// 已接管实际线程循环，此处仅保留迁移前的行为注释。
    (void)watchPath;

    // 使用重叠 I/O 开启非阻塞模式
    // 创建重叠 I/O 事件
    // 递归监听子目录
    {
        // 等待退出事件或变更事件
        {
            // 收到退出事件，主动取消挂起的 I/O 并退出循环
            // 成功监听到文件系统事件，通知主逻辑线程挂起变更
            // 发生错误
        }
    }
    // 等待退出事件或变更事件
    // 收到退出事件，主动取消挂起的 I/O 并退出循环
    // 成功监听到文件系统事件，通知主逻辑线程挂起变更
    // 发生错误
    // 非 Windows 平台的模拟实现或备用监听
}

}  // namespace MMM::Logic
