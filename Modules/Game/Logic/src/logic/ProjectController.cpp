#include "logic/ProjectController.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <exception>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>

namespace MMM::Logic
{

/// @brief 获取当前项目。
/// @return 当前项目指针；未打开项目时返回 nullptr。
Project* ProjectController::currentProject()
{
    return m_currentProject.get();
}

/// @brief 获取当前项目。
/// @return 当前项目只读指针；未打开项目时返回 nullptr。
const Project* ProjectController::currentProject() const
{
    return m_currentProject.get();
}

/// @brief 请求打开项目，必要时等待 UI 完成旧画布关闭。
/// @param projectPath 要打开的项目目录或谱面文件路径。
void ProjectController::requestOpenProject(
    const std::filesystem::path& projectPath)
{
    if ( projectPath.empty() ) {
        return;
    }

    /// @brief 保护本次打开请求状态写入的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
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

/// @brief 请求关闭当前项目，必要时等待 UI 完成旧画布关闭。
void ProjectController::requestCloseProject()
{
    /// @brief 保护本次关闭请求状态写入的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingProjectPath.clear();
    m_requestedProjectPath.clear();
    m_pendingProjectSwitchPath.clear();
    m_requestedProjectClose = true;
    m_pendingProjectClose   = false;
    m_projectCloseReady     = false;
}

/// @brief 是否存在等待旧谱面画布关闭后的项目打开或关闭流程。
/// @return 有挂起项目切换流程时返回 true。
bool ProjectController::hasPendingProjectSwitch() const
{
    /// @brief 保护挂起项目切换状态读取的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    return !m_pendingProjectSwitchPath.empty() || m_pendingProjectClose;
}

/// @brief 完成 UI 侧逐个关闭旧谱面画布后的项目切换流程。
void ProjectController::completePendingProjectSwitch()
{
    /// @brief 保护挂起项目切换状态推进的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    if ( m_pendingProjectClose ) {
        m_pendingProjectClose = false;
        m_projectCloseReady   = true;
        return;
    }

    if ( m_pendingProjectSwitchPath.empty() ) return;

    m_pendingProjectPath = m_pendingProjectSwitchPath;
    m_pendingProjectSwitchPath.clear();
}

/// @brief 取消所有挂起项目切换流程。
void ProjectController::cancelPendingProjectSwitch()
{
    /// @brief 保护挂起项目切换状态清理的锁。
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingProjectPath.clear();
    m_requestedProjectPath.clear();
    m_pendingProjectSwitchPath.clear();
    m_requestedProjectClose = false;
    m_pendingProjectClose   = false;
    m_projectCloseReady     = false;
}

/// @brief 消费逻辑线程本轮需要处理的项目切换动作。
/// @param needsCanvasClose 当前是否需要先关闭旧谱面画布。
/// @return 本轮需要执行的关闭或打开动作。
ProjectController::PendingProjectAction
ProjectController::consumePendingProjectAction(bool needsCanvasClose)
{
    /// @brief 本轮要返回给逻辑线程的项目动作。
    PendingProjectAction action;
    /// @brief 本轮消费到的项目打开请求。
    std::filesystem::path requestedPath;
    /// @brief 本轮是否消费到项目关闭请求。
    bool requestedClose = false;

    {
        /// @brief 保护从待处理请求队列取出请求的锁。
        std::lock_guard<std::mutex> lock(m_pendingMutex);
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
        if ( needsCanvasClose ) {
            /// @brief 保护关闭请求转入 UI 等待状态的锁。
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pendingProjectPath.clear();
            m_pendingProjectSwitchPath.clear();
            m_pendingProjectClose = true;
            XINFO(
                "Project close deferred until current beatmap canvases close.");
        } else {
            action.m_closeProject = true;
        }
    }

    if ( !requestedPath.empty() ) {
        if ( needsCanvasClose ) {
            /// @brief 保护打开请求转入 UI 等待状态的锁。
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            m_pendingProjectPath.clear();
            m_pendingProjectClose      = false;
            m_pendingProjectSwitchPath = requestedPath;
            XINFO(
                "Project open deferred until current beatmap canvases close: "
                "{}",
                Config::pathToUtf8(requestedPath));
        } else {
            action.m_projectPathToOpen = requestedPath;
        }
    }

    {
        /// @brief 保护 UI 完成后的延迟动作消费锁。
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if ( m_projectCloseReady ) {
            action.m_closeProject = true;
            m_projectCloseReady   = false;
        }
        if ( action.m_projectPathToOpen.empty() &&
             !m_pendingProjectPath.empty() ) {
            action.m_projectPathToOpen = m_pendingProjectPath;
            m_pendingProjectPath.clear();
        }
    }

    return action;
}

/// @brief 打开项目并启动项目目录监听。
/// @param projectPath 要打开的项目目录或谱面文件路径。
/// @return 打开项目后的结果信息。
ProjectController::OpenProjectResult ProjectController::openProject(
    const std::filesystem::path& projectPath)
{
    /// @brief 本次打开项目的返回结果。
    OpenProjectResult result;
    /// @brief 实际打开的项目目录路径。
    std::filesystem::path actualProjectPath = projectPath;
    /// @brief 若传入谱面文件，则记录需要自动打开的谱面路径。
    std::filesystem::path targetBeatmapPath;

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
        return result;
    }

    XINFO("Opening project at: {}", Config::pathToUtf8(actualProjectPath));

    /// @brief 新创建并等待接管为当前项目的项目实例。
    auto newProject           = std::make_unique<Project>();
    newProject->m_projectRoot = actualProjectPath;
    newProject->m_metadata.m_title =
        Config::pathToUtf8(actualProjectPath.filename());

    try {
        /// @brief 当前项目目录扫描结果。
        auto directoryScan = m_projectDirectoryScanner.scan(actualProjectPath);
        if ( !directoryScan.m_success ) {
            XERROR("Error while scanning project directory: {}",
                   Config::pathToUtf8(actualProjectPath));
        }

        m_projectResourceService.buildInitialResources(*newProject,
                                                       directoryScan);
    } catch ( const std::exception& e ) {
        XERROR("Error while scanning project directory: {}", e.what());
    }

    /// @brief 项目描述文件路径。
    std::filesystem::path projectFile = actualProjectPath / "mmm_project.json";
    if ( std::filesystem::exists(projectFile) ) {
        try {
            /// @brief 项目描述文件输入流。
            std::ifstream file(projectFile);
            /// @brief 项目描述 JSON 数据。
            nlohmann::json jsonData;
            file >> jsonData;
            /// @brief 从项目描述文件反序列化出的项目配置。
            Project loadedProject  = jsonData.get<Project>();
            newProject->m_metadata = loadedProject.m_metadata;
            newProject->m_settings = loadedProject.m_settings;
            newProject->m_excludedBeatmapPaths =
                loadedProject.m_excludedBeatmapPaths;
            newProject->m_excludedAudioPaths =
                loadedProject.m_excludedAudioPaths;

            for ( auto& resource : newProject->m_audioResources ) {
                for ( const auto& loadedResource :
                      loadedProject.m_audioResources ) {
                    if ( resource.m_id == loadedResource.m_id ) {
                        resource.m_type   = loadedResource.m_type;
                        resource.m_config = loadedResource.m_config;
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

    m_projectResourceService.applyExcludedResources(*newProject);

    try {
        /// @brief 项目描述文件输出流。
        std::ofstream file(projectFile);
        /// @brief 即将写入的项目描述 JSON 数据。
        nlohmann::json jsonData = *newProject;
        file << std::setw(4) << jsonData << std::endl;
    } catch ( ... ) {
    }

    for ( const auto& resource : newProject->m_audioResources ) {
        if ( resource.m_type != AudioTrackType::Effect ) {
            continue;
        }

        /// @brief 音效资源在项目目录中的绝对路径。
        auto absolutePath =
            actualProjectPath / Config::utf8ToPath(resource.m_path);
        if ( !std::filesystem::exists(absolutePath) ) {
            continue;
        }

        /// @brief 需要调用方预加载的音效请求。
        ProjectCommandService::AudioPreloadRequest preloadRequest;
        preloadRequest.m_resource     = resource;
        preloadRequest.m_absolutePath = absolutePath;
        result.m_effectPreloads.push_back(preloadRequest);
    }

    m_currentProject = std::move(newProject);
    m_projectDirectoryWatcher.start(actualProjectPath);
    Config::AppConfig::instance().addRecentProject(
        Config::pathToUtf8(actualProjectPath));

    result.m_opened            = true;
    result.m_actualProjectPath = actualProjectPath;
    result.m_targetBeatmapPath = targetBeatmapPath;
    result.m_projectTitle      = m_currentProject->m_metadata.m_title;
    result.m_beatmapCount      = m_currentProject->m_beatmaps.size();
    return result;
}

/// @brief 关闭当前项目并停止项目目录监听。
/// @return 被关闭项目的信息。
ProjectController::CloseProjectResult ProjectController::closeProject()
{
    /// @brief 本次关闭项目的返回结果。
    CloseProjectResult result;
    if ( !m_currentProject ) {
        return result;
    }

    result.m_projectTitle = m_currentProject->m_metadata.m_title;
    result.m_project      = std::move(m_currentProject);
    result.m_closed       = true;
    m_projectDirectoryWatcher.stop();
    return result;
}

/// @brief 停止项目目录监听。
void ProjectController::stopDirectoryWatcher()
{
    m_projectDirectoryWatcher.stop();
}

/// @brief 消费项目目录监听器捕获到的变更标记。
/// @return 有待处理目录变更时返回 true。
bool ProjectController::consumeDirectoryChangePending()
{
    return m_projectDirectoryWatcher.consumeChangePending();
}

/// @brief 扫描当前项目目录并同步项目资源列表。
/// @return 目录同步结果，包含是否改变项目和需要预加载的音效。
ProjectResourceService::DirectorySyncResult
ProjectController::scanProjectDirectory()
{
    /// @brief 本次目录扫描同步结果。
    ProjectResourceService::DirectorySyncResult result;
    if ( !m_currentProject ) {
        return result;
    }

    /// @brief 当前项目根目录路径。
    auto actualProjectPath = m_currentProject->m_projectRoot;
    /// @brief 当前项目目录扫描结果。
    ProjectDirectoryScanner::ScanResult directoryScan;

    try {
        directoryScan = m_projectDirectoryScanner.scan(actualProjectPath);
        if ( !directoryScan.m_success ) {
            return result;
        }
    } catch ( ... ) {
        return result;
    }

    return m_projectResourceService.syncDirectoryResources(*m_currentProject,
                                                           directoryScan);
}

/// @brief 保存当前项目配置。
/// @return 保存成功时返回 true。
bool ProjectController::saveProject()
{
    if ( !m_currentProject ) return false;

    /// @brief 当前项目描述文件路径。
    std::filesystem::path projectFile =
        m_currentProject->m_projectRoot / "mmm_project.json";
    XINFO("Saving project to {}", Config::pathToUtf8(projectFile));

    try {
        /// @brief 项目描述文件输出流。
        std::ofstream file(projectFile);
        /// @brief 即将写入的项目描述 JSON 数据。
        nlohmann::json jsonData = *m_currentProject;
        file << std::setw(4) << jsonData << std::endl;
        XINFO("Project saved successfully.");
        return true;
    } catch ( const std::exception& e ) {
        XERROR("Failed to save project: {}", e.what());
    }

    return false;
}

/// @brief 创建谱面文件并登记到当前项目。
/// @param cmd 新建谱面命令。
/// @return 新建谱面的处理结果。
ProjectCommandService::CreateBeatmapResult ProjectController::createBeatmap(
    const CmdCreateBeatmap& cmd)
{
    if ( !m_currentProject ) {
        XERROR("Cannot create beatmap: No project opened.");
        return {};
    }
    return m_projectCommandService.createBeatmap(*m_currentProject, cmd);
}

/// @brief 导入音频文件并登记到当前项目。
/// @param cmd 导入音频命令。
/// @return 导入音频的处理结果。
ProjectCommandService::ImportAudioResult ProjectController::importAudio(
    const CmdImportAudio& cmd)
{
    if ( !m_currentProject ) {
        XERROR("Cannot import audio: No project opened.");
        return {};
    }
    return m_projectCommandService.importAudio(*m_currentProject, cmd);
}

/// @brief 将单个谱面文件同步到当前项目谱面列表。
/// @param mapPath 需要同步的谱面文件路径。
/// @return 当前项目是否发生变化。
ProjectCommandService::ProjectMutationResult
ProjectController::syncProjectWithFile(const std::filesystem::path& mapPath)
{
    if ( !m_currentProject ) return {};
    return m_projectCommandService.syncProjectWithFile(*m_currentProject,
                                                       mapPath);
}

/// @brief 更新当前项目内谱面条目的文件路径关联。
/// @param oldPath 旧谱面路径。
/// @param newPath 新谱面路径。
/// @return 当前项目是否发生变化。
ProjectCommandService::ProjectMutationResult
ProjectController::updateBeatmapFilePath(const std::filesystem::path& oldPath,
                                         const std::filesystem::path& newPath)
{
    if ( !m_currentProject ) return {};
    return m_projectCommandService.updateBeatmapFilePath(
        *m_currentProject, oldPath, newPath);
}

/// @brief 更新当前项目的音频资源类型。
/// @param cmd 更新音频资源命令。
/// @return 更新音频资源的处理结果。
ProjectCommandService::UpdateAudioResourceResult
ProjectController::updateAudioResource(const CmdUpdateAudioResource& cmd)
{
    if ( !m_currentProject ) return {};
    return m_projectCommandService.updateAudioResource(*m_currentProject, cmd);
}

/// @brief 从当前项目中删除音频资源。
/// @param cmd 删除音频资源命令。
/// @return 删除音频资源的处理结果。
ProjectCommandService::RemoveAudioResourceResult
ProjectController::removeAudioResource(const CmdRemoveAudioResource& cmd)
{
    if ( !m_currentProject ) return {};
    return m_projectCommandService.removeAudioResource(*m_currentProject, cmd);
}

/// @brief 从当前项目谱面列表中删除谱面。
/// @param cmd 删除谱面命令。
/// @return 当前项目是否发生变化。
ProjectCommandService::ProjectMutationResult ProjectController::removeBeatmap(
    const CmdRemoveBeatmap& cmd)
{
    if ( !m_currentProject ) return {};
    return m_projectCommandService.removeBeatmap(*m_currentProject, cmd);
}

}  // namespace MMM::Logic
