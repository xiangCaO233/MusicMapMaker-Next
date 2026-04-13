#include "logic/EditorEngine.h"
#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "event/logic/EditorConfigChangedEvent.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "event/ui/menu/ProjectLoadedEvent.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "mmm/beatmap/BeatMap.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <unordered_set>

namespace MMM::Logic
{

EditorEngine& EditorEngine::instance()
{
    static EditorEngine instance;
    return instance;
}

EditorEngine::EditorEngine()
{
    // 从全局配置初始化本地缓存
    m_editorConfig = Config::AppConfig::instance().getEditorConfig();

    // 默认创建一个 Session
    m_activeSession = std::make_unique<BeatmapSession>();

    // 订阅画布尺寸改变事件
    Event::EventBus::instance().subscribe<Event::CanvasResizeEvent>(
        [this](const Event::CanvasResizeEvent& e) {
            pushCommand(CmdUpdateViewport{ e.canvasName,
                                           static_cast<float>(e.newSize.x),
                                           static_cast<float>(e.newSize.y) });
        });

    // 订阅打开项目事件
    // 重要：不在回调里直接调用 openProject！
    // 原因：EventBus::publish 持有 shared_lock，而 openProject 内部会创建
    // BeatmapSession， 其构造函数又会调用 EventBus::subscribe（需
    // unique_lock）。 同一线程无法将 shared_lock 升级为 unique_lock →
    // 永久卡死。 正确做法：将路径存入队列，在逻辑线程 loop() 里境外处理。
    Event::EventBus::instance().subscribe<Event::OpenProjectEvent>(
        [this](const Event::OpenProjectEvent& e) {
            std::lock_guard<std::mutex> lk(m_pendingMutex);
            m_pendingProjectPath = e.m_projectPath;
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

void EditorEngine::openProject(const std::filesystem::path& projectPath)
{
    if ( !std::filesystem::exists(projectPath) ||
         !std::filesystem::is_directory(projectPath) ) {
        XERROR(
            "Failed to open project: Path does not exist or is not a "
            "directory: {}",
            projectPath.string());
        return;
    }

    XINFO("Opening project at: {}", projectPath.string());

    auto newProject           = std::make_unique<Project>();
    newProject->m_projectRoot = projectPath;
    newProject->m_metadata.m_title =
        projectPath.filename().string();  // 默认标题为目录名

    // 扫描文件系统
    try {
        std::vector<std::filesystem::path> mapFiles;
        std::vector<std::filesystem::path> audioFiles;

        for ( const auto& entry :
              std::filesystem::recursive_directory_iterator(projectPath) ) {
            if ( !entry.is_regular_file() ) continue;

            auto ext = entry.path().extension().string();
            if ( ext == ".osu" || ext == ".imd" || ext == ".mc" ) {
                mapFiles.push_back(entry.path());
            } else if ( ext == ".mp3" || ext == ".ogg" || ext == ".wav" ||
                        ext == ".flac" ) {
                audioFiles.push_back(entry.path());
            }
        }

        // 记录哪些音频被识别为主音轨
        std::unordered_set<std::string> mainAudioPaths;

        // 1. 处理谱面并识别主音轨
        for ( const auto& mapPath : mapFiles ) {
            auto relMapPath =
                std::filesystem::relative(mapPath, projectPath).string();
            auto filename = mapPath.filename().string();

            Project::BeatmapEntry mapEntry;
            mapEntry.m_name     = filename;
            mapEntry.m_filePath = relMapPath;

            // 预加载谱面以获取其定义的主音频路径
            try {
                auto tempMap = BeatMap::loadFromFile(mapPath);
                if ( !tempMap.m_baseMapMetadata.main_audio_path.empty() ) {
                    // 获取相对于项目根目录的音频路径
                    auto absAudioPath =
                        mapPath.parent_path() /
                        tempMap.m_baseMapMetadata.main_audio_path;
                    auto relAudioPath =
                        std::filesystem::relative(absAudioPath, projectPath)
                            .string();

                    mapEntry.m_audioTrackId = absAudioPath.filename().string();
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
            auto relAudioPath =
                std::filesystem::relative(audioPath, projectPath).string();
            auto filename = audioPath.filename().string();

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
    std::filesystem::path projectFile = projectPath / "mmm_project.json";
    if ( std::filesystem::exists(projectFile) ) {
        try {
            std::ifstream  file(projectFile);
            nlohmann::json j;
            file >> j;
            Project loadedProject  = j.get<Project>();
            newProject->m_metadata = loadedProject.m_metadata;
            newProject->m_settings = loadedProject.m_settings;

            // 合并资源配置并应用到音频引擎
            for ( auto& res : newProject->m_audioResources ) {
                for ( const auto& loadedRes : loadedProject.m_audioResources ) {
                    if ( res.m_id == loadedRes.m_id ) {
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
    try {
        std::ofstream  file(projectFile);
        nlohmann::json j = *newProject;
        file << std::setw(4) << j << std::endl;
    } catch ( ... ) {
    }

    // 预加载所有项目内的音效资源
    for ( const auto& res : newProject->m_audioResources ) {
        if ( res.m_type == AudioTrackType::Effect ) {
            auto absPath = projectPath / res.m_path;
            if ( std::filesystem::exists(absPath) ) {
                Audio::AudioManager::instance().preloadSoundEffect(
                    res.m_id, absPath.string(), res.m_config.volume);
            }
        }
    }

    // 更新当前项目单例状态
    {
        std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
        m_currentProject = std::move(newProject);
    }

    // 发布加载成功事件
    Event::ProjectLoadedEvent loadedEv;
    loadedEv.m_projectTitle = m_currentProject->m_metadata.m_title;
    loadedEv.m_projectPath  = projectPath.string();
    loadedEv.m_beatmapCount = m_currentProject->m_beatmaps.size();
    Event::EventBus::instance().publish(loadedEv);

    XINFO("Project '{}' loaded successfully with {} beatmaps.",
          loadedEv.m_projectTitle,
          loadedEv.m_beatmapCount);

    // 关键修正：在将新 Session 设为激活前，先同步历史视口尺寸
    // 逻辑线程必须知道画布宽高，否则无法生成几何体
    auto newSession = std::make_shared<BeatmapSession>();
    {
        std::lock_guard<std::recursive_mutex> lock(m_buffersMutex);
        // 重要：不再调用 m_syncBuffers.clear()！
        // 核心原因是 UI 线程的组件（如 TimelineCanvas）持有这些 Buffer 的
        // shared_ptr。 如果清空并重新创建，UI 和逻辑线程将指向不同的 Buffer
        // 对象，导致画面永不更新。
        for ( const auto& [cid, size] : m_lastViewportSizes ) {
            newSession->pushCommand(CmdUpdateViewport{ cid, size.x, size.y });
        }
    }

    // 记录到最近打开列表
    Config::AppConfig::instance().addRecentProject(projectPath.string());

    {
        std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
        m_activeSession = newSession;
    }

    // 设置初始配置
    pushCommand(CmdUpdateEditorConfig{ m_editorConfig });

    // 新加载项目时，清空当前 Session 的所有谱面数据
    pushCommand(CmdLoadBeatmap{ nullptr });
}

void EditorEngine::start()
{
    if ( m_running ) {
        return;
    }

    // 从全局配置同步到本地缓存
    m_editorConfig = Config::AppConfig::instance().getEditorConfig();

    m_running = true;

    m_thread = std::thread(&EditorEngine::loop, this);
    XINFO("EditorEngine logic thread started.");
}

void EditorEngine::stop()
{
    if ( m_running ) {
        m_running = false;
        if ( m_thread.joinable() ) {
            m_thread.join();
        }
        XINFO("EditorEngine logic thread stopped.");
    }
}

void EditorEngine::pushCommand(LogicCommand&& cmd)
{
    // 拦截视口更新指令，缓存最新的尺寸
    if ( std::holds_alternative<CmdUpdateViewport>(cmd) ) {
        const auto&     v = std::get<CmdUpdateViewport>(cmd);
        std::lock_guard lk(m_buffersMutex);
        m_lastViewportSizes[v.cameraId] = { v.width, v.height };
    }

    std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if ( m_activeSession ) {
        m_activeSession->pushCommand(std::move(cmd));
    }
}

std::shared_ptr<BeatmapSyncBuffer> EditorEngine::getSyncBuffer(
    const std::string& cameraId)
{
    std::lock_guard<std::recursive_mutex> lock(m_buffersMutex);
    if ( m_syncBuffers.find(cameraId) == m_syncBuffers.end() ) {
        m_syncBuffers[cameraId] = std::make_shared<BeatmapSyncBuffer>();
    }
    return m_syncBuffers[cameraId];
}

EditTool EditorEngine::getCurrentTool() const
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if ( m_activeSession ) {
        return m_activeSession->getCurrentTool();
    }
    return EditTool::Move;
}

void EditorEngine::setEditorConfig(const Config::EditorConfig& config)
{
    // 关键修复：从全局 AppConfig 中同步最新的最近项目列表，防止被 UI 设置覆盖
    auto& globalRecent =
        Config::AppConfig::instance().getEditorConfig().recentProjects;

    m_editorConfig                = config;
    m_editorConfig.recentProjects = globalRecent;

    // 同步回全局 AppConfig 实例
    Config::AppConfig::instance().getEditorConfig() = m_editorConfig;

    pushCommand(CmdUpdateEditorConfig{ m_editorConfig });

    // 发布配置更新事件，供 UI 层订阅
    Event::EventBus::instance().publish(
        Event::EditorConfigChangedEvent{ m_editorConfig });
}

void EditorEngine::saveProject()
{
    std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
    if ( !m_currentProject ) return;

    std::filesystem::path projectFile =
        m_currentProject->m_projectRoot / "mmm_project.json";
    XINFO("Saving project to {}", projectFile.string());

    try {
        std::ofstream  file(projectFile);
        nlohmann::json j = *m_currentProject;
        file << std::setw(4) << j << std::endl;
        XINFO("Project saved successfully.");
    } catch ( const std::exception& e ) {
        XERROR("Failed to save project: {}", e.what());
    }
}

void EditorEngine::loop()
{
    auto lastTime = std::chrono::high_resolution_clock::now();

    while ( m_running ) {
        // 动态获取当前的延迟目标
        double targetDt = 0.0;
        if ( m_editorConfig.settings.vsync ) {
            int refreshRate =
                Config::AppConfig::instance().getDeviceRefreshRate();
            if ( refreshRate <= 0 ) refreshRate = 60;  // 兜底
            targetDt = 1.0 / static_cast<double>(refreshRate);
        }

        auto currentTime = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> passed = currentTime - lastTime;

        // 如果设置了帧率限制，并且距离上一帧还没有达到目标时间，就主动让出 CPU
        if ( targetDt > 0.0 && passed.count() < targetDt ) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }

        lastTime  = currentTime;
        double dt = passed.count();

        // 关键修复：使用 shared_ptr 在锁内获取引用。
        // 这样即使 UI 线程在此时 openProject 销毁了原有 Session，
        // 逻辑线程持有的这个共享引用也能保证 session 在 update
        // 期间一直有效，避免 Use-After-Free 导致的死锁或崩溃。
        // 如果有待处理的项目路径，在锁外处理（避免 EventBus 锁内与 subscribe
        // 交叉）
        std::filesystem::path pendingPath;
        {
            std::lock_guard<std::mutex> lk(m_pendingMutex);
            if ( !m_pendingProjectPath.empty() ) {
                pendingPath = m_pendingProjectPath;
                m_pendingProjectPath.clear();
            }
        }
        if ( !pendingPath.empty() ) {
            openProject(pendingPath);
        }

        std::shared_ptr<BeatmapSession> session;
        {
            std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
            session = m_activeSession;
        }

        if ( session ) {
            session->update(dt, m_editorConfig);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

}  // namespace MMM::Logic
