#pragma once

#include "common/LogicCommands.h"
#include "logic/BeatmapSession.h"
#include "logic/BeatmapSyncBuffer.h"
#include "mmm/project/Project.h"
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace MMM::Logic
{

/**
 * @brief 编辑器逻辑引擎 (全局单例)
 *
 * 负责管理后台逻辑线程的生命周期，以及分发指令给当前活动的 BeatmapSession。
 */
class EditorEngine
{
public:
    static EditorEngine& instance();

    EditorEngine();
    ~EditorEngine();

    // 禁用拷贝与移动
    EditorEngine(EditorEngine&&)                 = delete;
    EditorEngine(const EditorEngine&)            = delete;
    EditorEngine& operator=(EditorEngine&&)      = delete;
    EditorEngine& operator=(const EditorEngine&) = delete;

    /**
     * @brief 启动逻辑线程
     */
    void start();

    /**
     * @brief 停止逻辑线程
     */
    void stop();

    /**
     * @brief 打开项目目录并加载其中的所有资源
     */
    void openProject(const std::filesystem::path& projectPath);

    /**
     * @brief 获取当前项目
     */
    Project*       getCurrentProject() { return m_currentProject.get(); }
    const Project* getCurrentProject() const { return m_currentProject.get(); }

    /**
     * @brief 向当前活动的 Session 推送指令
     */
    void pushCommand(LogicCommand&& cmd);

    /**
     * @brief 检查当前是否有未保存的修改
     */
    bool hasUnsavedChanges() const;

    /// @brief 处理新建谱面指令 (向导/项目管理)
    void handleCreateBeatmap(const CmdCreateBeatmap& cmd);

    /**
     * @brief 同步文件系统变更到当前项目 (如另存为后刷新列表)
     * @param mapPath 新保存的谱面文件路径
     */
    void syncProjectWithFile(const std::filesystem::path& mapPath);

    /**
     * @brief 更新项目内谱面的文件路径关联 (例如将 .imd 强制保存为 .mmm
     * 后更新关联)
     * @param oldPath 旧的谱面文件路径
     * @param newPath 新的谱面文件路径
     */
    void updateBeatmapFilePathInProject(const std::filesystem::path& oldPath,
                                        const std::filesystem::path& newPath);

    /// @brief 处理导入音频指令
    void handleImportAudio(const CmdImportAudio& cmd);

    /// @brief 更新音轨资源信息
    void handleUpdateAudioResource(const CmdUpdateAudioResource& cmd);

    /// @brief 移除项目音轨资源
    void handleRemoveAudioResource(const CmdRemoveAudioResource& cmd);

    /// @brief 从项目中移除谱面
    void handleRemoveBeatmap(const CmdRemoveBeatmap& cmd);

    /**
     * @brief 获取当前激活的谱面会话
     */
    std::shared_ptr<BeatmapSession> getActiveSession()
    {
        std::lock_guard<std::recursive_mutex> lock(m_sessionMutex);
        return m_activeSession;
    }

    /**
     * @brief 获取会话保护递归锁，以允许 UI 线程安全同步访问会话内部状态
     */
    std::recursive_mutex& getSessionMutex() const { return m_sessionMutex; }


    /**
     * @brief 获取指定摄像机/画布的同步缓冲区
     */
    std::shared_ptr<BeatmapSyncBuffer> getSyncBuffer(
        const std::string& cameraId);

    /**
     * @brief 设置全局图集 UV 映射 (由 UI 线程在构建图集后调用)
     */
    void setAtlasUVMap(const std::string&                             cameraId,
                       const std::unordered_map<uint32_t, glm::vec4>& uvMap)
    {
        std::unique_lock<std::shared_mutex> lock(m_buffersMutex);
        m_cameraUVMaps[cameraId] = uvMap;
    }

    /**
     * @brief 获取当前全局图集 UV 映射
     */
    const std::unordered_map<uint32_t, glm::vec4>& getAtlasUVMap(
        const std::string& cameraId) const;

    /**
     * @brief 获取当前编辑器配置
     */
    const Config::EditorConfig& getEditorConfig() const
    {
        return m_editorConfig;
    }

    /**
     * @brief 获取当前工具类型
     */
    EditTool getCurrentTool() const;

    /**
     * @brief 获取当前播放状态
     */
    bool isPlaybackPlaying() const;

    /**
     * @brief 获取逻辑线程实时刷新率 (UPS - Updates Per Second)
     */
    float getLogicUps() const
    {
        return m_logicUps.load(std::memory_order_relaxed);
    }


    /**
     * @brief 设置编辑器配置 (同时分发指令给 Session)
     */
    void setEditorConfig(const Config::EditorConfig& config);

    /**
     * @brief 持久化当前项目配置到 mmm_project.json
     */
    void saveProject();

private:
    /**
     * @brief 逻辑线程的主循环
     */
    void loop();

    /**
     * @brief 定期扫描项目目录变更（实现实时目录监听与资源同步）
     */
    void scanProjectDirectory();

    /// @brief 逻辑线程
    std::thread m_thread;

    /// @brief 线程运行标志
    std::atomic<bool> m_running{ false };

    /// @brief 当前激活的谱面会话 (ECS 核心)
    std::shared_ptr<BeatmapSession> m_activeSession;

    /// @brief 当前打开的项目
    std::unique_ptr<Project> m_currentProject;

    /// @brief 所有的同步缓冲区 (Key 为 CameraID)
    std::unordered_map<std::string, std::shared_ptr<BeatmapSyncBuffer>>
        m_syncBuffers;

    /// @brief 保护会话和缓冲区的递归锁
    mutable std::recursive_mutex m_sessionMutex;

    /// @brief 编辑器配置
    Config::EditorConfig m_editorConfig;

    /// @brief 各摄像机独立的图集 UV 映射表 (受 m_buffersMutex 保护)
    std::unordered_map<std::string, std::unordered_map<uint32_t, glm::vec4>>
        m_cameraUVMaps;

    /// @brief 保护 m_syncBuffers 和 m_cameraUVMaps 的独立锁（与 m_sessionMutex
    /// 无交叉，防止死锁）
    mutable std::shared_mutex m_buffersMutex;

    /// @brief 待处理的项目路径（由 EventBus 回调写入，由逻辑线程消费）
    std::filesystem::path m_pendingProjectPath;

    /// @brief 保护 m_pendingProjectPath 的轻量级锁
    mutable std::mutex m_pendingMutex;

    /// @brief 缓存各摄像机的最后已知视口尺寸 (受 m_buffersMutex 保护)
    std::unordered_map<std::string, glm::vec2> m_lastViewportSizes;

    /// @brief 逻辑线程实时刷新率 (UPS)
    std::atomic<float> m_logicUps{ 0.0f };

    /// @brief 逻辑线程更新计数器，用于 UPS 计算
    uint32_t m_logicUpdateCount{ 0 };

    /// @brief 上一次计算 UPS 的时间戳
    std::chrono::high_resolution_clock::time_point m_lastUpsTime;

    /**
     * @brief 启动文件夹监听器
     */
    void startDirectoryWatcher(const std::filesystem::path& path);

    /**
     * @brief 停止文件夹监听器
     */
    void stopDirectoryWatcher();

    /**
     * @brief 文件夹监听线程的主循环
     */
    void watcherThreadLoop(std::filesystem::path watchPath);

    /// @brief 文件夹监听线程
    std::thread m_watcherThread;

    /// @brief 监听线程运行标志
    std::atomic<bool> m_watcherRunning{ false };

    /// @brief 是否有未处理的文件系统变更挂起
    std::atomic<bool> m_directoryChangedPending{ false };

#ifdef _WIN32
    /// @brief Win32 目录句柄，用于取消阻塞的 ReadDirectoryChangesW
    void* m_watcherDirHandle{ nullptr };
    /// @brief Win32 退出事件句柄，用于安全退出监听线程
    void* m_watcherExitEvent{ nullptr };
#endif

    /// @brief 保护目录句柄的独立锁
    mutable std::mutex m_watcherMutex;
};

}  // namespace MMM::Logic
