#pragma once

#include "common/LogicCommands.h"
#include "logic/BeatmapSession.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorClipboard.h"
#include "logic/RenderSyncRegistry.h"
#include "logic/SessionRegistry.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/project/Project.h"
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace MMM::Logic
{

/// @brief 多画布 Session 条目，绑定 Session 与其对应画布的 cameraId
// clang-format off
    /// @brief 逻辑会话
    /// @brief 该画布对应的主 cameraId（如 "Canvas_0", "Canvas_1"...）
    /// @brief 显示名称（谱面名或默认标签）
    /// @brief 是否为初始 Logo 占位画布（尚未加载谱面）
// clang-format on
/// @brief 该类型定义已迁移到 SessionRegistry.h，由 SessionRegistry 统一管理。
/// @brief 逻辑会话
/// @brief 该字段现在由 SessionEntry::session 在 SessionRegistry.h 中定义。
/// @brief 该画布对应的主 cameraId（如 "Canvas_0", "Canvas_1"...）
/// @brief 该字段现在由 SessionEntry::cameraId 在 SessionRegistry.h 中定义。
/// @brief 显示名称（谱面名或默认标签）
/// @brief 该字段现在由 SessionEntry::displayName 在 SessionRegistry.h 中定义。
/// @brief 是否为初始 Logo 占位画布（尚未加载谱面）
/// @brief 该字段现在由 SessionEntry::isLogoPlaceholder 在 SessionRegistry.h
/// 中定义。

/**
 * @brief 编辑器逻辑引擎 (全局单例)
 *
 * 负责管理后台逻辑线程的生命周期，以及分发指令给当前活动的 BeatmapSession。
 * 支持多画布并行编辑，每个画布对应一个独立的 SessionEntry。
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

    /// @brief 请求打开项目，必要时先等待 UI 关闭当前谱面画布
    void requestOpenProject(const std::filesystem::path& projectPath);

    /// @brief Request closing the current project after dirty canvases confirm
    /// saving.
    void requestCloseProject();

    /// @brief 是否存在等待旧谱面画布关闭后的项目打开或关闭流程。
    bool hasPendingProjectSwitch() const;

    /// @brief 完成旧谱面画布关闭流程，并排队执行挂起的项目打开或关闭。
    void completePendingProjectSwitch();

    /// @brief 取消挂起的项目打开或关闭请求。
    void cancelPendingProjectSwitch();

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

    /// @brief 更新编辑器级剪贴板。
    void setClipboard(std::vector<ClipboardItem> items,
                      const SessionContext* sourceContext, bool isCut);

    /// @brief 获取编辑器级剪贴板副本。
    std::vector<ClipboardItem> getClipboard() const;

    /// @brief 判断当前剪贴板是否为指定会话的剪切内容。
    bool isClipboardCutFrom(const SessionContext* context) const;

    /// @brief 若剪贴板为其他会话剪切内容，则删除源会话原物件。
    void consumeCrossSessionCutClipboard(const SessionContext* pasteContext);

    /// @brief 将当前剪切剪贴板标记为已消费。
    void markCutClipboardConsumed();

    // ========== 多 Session 管理 API ==========

    // clang-format off
    /**
     * @brief 创建新的画布 Session
     * @param beatmap 要加载的谱面（可为 nullptr 表示空白占位）
     * @param displayName 显示名称
     * @param isLogoPlaceholder 是否为初始 Logo 画布
     * @return 新 Session 在 m_sessions 中的索引
     * @note
     * m_sessions 已迁移到 SessionRegistry 内部列表。
     */
    // clang-format on
    int32_t createSession(std::shared_ptr<MMM::BeatMap> beatmap     = nullptr,
                          const std::string&            displayName = "",
                          bool isLogoPlaceholder                    = false);

    /**
     * @brief 关闭指定索引的画布 Session
     * @param index 要关闭的 Session 索引
     */
    void closeSession(int32_t index);

    /**
     * @brief 设置当前活跃（前台）的 Session 索引
     * @param index 目标 Session 索引
     */
    void setActiveSessionIndex(int32_t index);

    /**
     * @brief 获取当前活跃 Session 索引
     */
    int32_t getActiveSessionIndex() const
    {
        return m_sessionRegistry.activeIndex();
    }

    /**
     * @brief 获取 Session 总数
     */
    int32_t getSessionCount() const { return m_sessionRegistry.count(); }

    /**
     * @brief 获取指定索引的 SessionEntry (只读)
     */
    const SessionEntry* getSessionEntry(int32_t index) const
    {
        return m_sessionRegistry.entry(index);
    }

    /**
     * @brief 获取所有 Session 条目的只读快照
     */
    std::vector<SessionEntry> getSessionEntries() const
    {
        return m_sessionRegistry.entries();
    }

    /**
     * @brief 获取当前激活的谱面会话 (兼容旧接口)
     */
    std::shared_ptr<BeatmapSession> getActiveSession()
    {
        return m_sessionRegistry.activeSession();
    }

    /**
     * @brief 获取当前激活画布的 cameraId
     */
    std::string getActiveCameraId() const
    {
        return m_sessionRegistry.activeCameraId();
    }

    /**
     * @brief 获取会话保护递归锁，以允许 UI 线程安全同步访问会话内部状态

     */
    std::recursive_mutex& getSessionMutex() const
    {
        return m_sessionRegistry.mutex();
    }


    /**
     * @brief 获取指定摄像机/画布的同步缓冲区
     * @warning
     * 逻辑热路径/共享指针：返回 shared_ptr
     * 是为了保证画布关闭并擦除注册表时，本次快照发布仍持有缓冲区生命周期。

     */
    std::shared_ptr<BeatmapSyncBuffer> getSyncBuffer(
        const std::string& cameraId);

    /**
     * @brief 设置全局图集 UV 映射 (由 UI 线程在构建图集后调用)
     */
    void setAtlasUVMap(const std::string&                             cameraId,
                       const std::unordered_map<uint32_t, glm::vec4>& uvMap)
    {
        m_renderSyncRegistry.setAtlasUVMap(cameraId, uvMap);
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
     * @warning 逻辑/UI
     * 热路径原子：只读取工具枚举状态，使用 relaxed。
     */
    EditTool getCurrentTool() const;

    /**
     * @brief 获取当前播放状态
     */
    bool isPlaybackPlaying() const;

    /**
     * @brief 获取逻辑线程实时刷新率 (UPS - Updates Per Second)

     */
    /// @warning UI 热路径/原子：状态栏可每帧读取；只用于统计展示，使用
    /// relaxed。
    float getLogicUps() const
    {
        return m_logicUps.load(std::memory_order_relaxed);
    }

    /// @brief 设置同主音轨多画布时间同步开关。
    /// @warning 逻辑/UI 热路径原子：只写入同步开关状态，使用 relaxed。
    void setSyncSameMainAudioCanvases(bool enabled);

    /// @brief 获取同主音轨多画布时间同步开关。
    /// @warning 逻辑热路径/原子：逻辑循环同步判断读取；只表示开关状态，使用
    /// relaxed。
    bool isSyncSameMainAudioCanvasesEnabled() const
    {
        return m_syncSameMainAudioCanvases.load(std::memory_order_relaxed);
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
     * @warning
     * 逻辑热路径：独立逻辑线程按 UPS 频率执行；禁止每 update 文件系统操作、完整
     * entt 遍历、完整排序、try/catch 和可避免的 shared_ptr 拷贝。

     */
    void loop();

    /**
     * @brief 定期扫描项目目录变更（实现实时目录监听与资源同步）
     */
    void scanProjectDirectory();

    /// @brief Clear the currently opened project and unload project audio
    /// state.
    void closeProject();

    /// @brief 将使用同一主音轨的非活跃会话同步到当前活跃会话时间。
    /// @warning 逻辑热路径/原子：每次 Session update
    /// 后可能执行；只读取同步开关并遍历当前会话列表。
    void syncSameMainAudioCanvases();

    /// @brief 判断打开新项目前是否需要先关闭当前谱面画布。
    bool needsCanvasCloseBeforeProjectOpen() const;

    /// @brief 逻辑线程
    std::thread m_thread;

    /// @brief 线程运行标志
    /// @warning 逻辑热路径/原子：loop 每次迭代读取、stop
    /// 写入；需要跨线程停止信号，使用 acquire/release。
    std::atomic<bool> m_running{ false };

    /// @brief 所有打开的画布 Session 列表
    /// @brief 该职责已收敛到 SessionRegistry 内部列表。

    /// @brief 当前活跃（前台）的 Session 索引 (-1 表示无)
    /// @brief 该职责已收敛到 SessionRegistry 内部活跃索引。

    /// @brief 全局递增的画布 ID 计数器，用于生成唯一 cameraId
    /// @brief 该职责已收敛到 SessionRegistry 内部画布 ID 计数器。

    /// @brief 多画布会话注册表，封装 Session 列表、活跃索引和 cameraId 分配。
    SessionRegistry m_sessionRegistry;

    /// @brief 当前打开的项目
    std::unique_ptr<Project> m_currentProject;

    /// @brief 所有的同步缓冲区 (Key 为 CameraID)
    /// @brief 该职责已收敛到 RenderSyncRegistry 内部同步缓冲区表。

    /// @brief 保护会话和缓冲区的递归锁
    /// @brief 该职责已拆分：会话由 SessionRegistry::mutex() 保护，缓冲区由
    /// m_buffersMutex 保护。

    /// @brief 编辑器配置
    Config::EditorConfig m_editorConfig;

    /// @brief 当前全局编辑工具。
    /// @warning 逻辑/UI 热路径/原子：UI 命令写入、会话 update
    /// 读取；只传递枚举状态，使用 relaxed。
    std::atomic<EditTool> m_currentTool{ EditTool::Move };

    /// @brief 逻辑线程用于节流判断的帧率限制模式缓存。
    /// @warning 逻辑热路径/原子：loop 每次迭代读取；只缓存配置枚举，使用
    /// relaxed 降低栅栏成本。
    std::atomic<Config::FrameLimitPreference> m_frameLimitPreference{
        Config::FrameLimitPreference::Refresh2x
    };

    /// @brief 各摄像机独立的图集 UV 映射表 (受 m_buffersMutex 保护)
    /// @brief 该职责已收敛到 RenderSyncRegistry 内部图集 UV 映射表。

    /// @brief 保护 m_syncBuffers 和 m_cameraUVMaps 的独立锁（与 m_sessionMutex
    /// 无交叉，防止死锁）
    /// @brief m_sessionMutex 已迁移为 SessionRegistry::mutex()。
    /// @brief 该职责已收敛到 RenderSyncRegistry 内部共享锁。

    /// @brief 渲染同步注册表，封装同步缓冲区、图集 UV 映射和视口尺寸缓存。
    RenderSyncRegistry m_renderSyncRegistry;

    /// @brief 已确认可由逻辑线程直接打开的项目路径
    std::filesystem::path m_pendingProjectPath;

    /// @brief 等待逻辑线程判定是否需要先关闭旧画布的项目路径
    std::filesystem::path m_requestedProjectPath;

    /// @brief 等待 UI 逐个关闭旧谱面画布后再处理的项目路径
    std::filesystem::path m_pendingProjectSwitchPath;

    /// @brief Whether a project close was requested and waits for logic-thread
    /// routing.
    bool m_requestedProjectClose{ false };

    /// @brief Whether the UI is closing old beatmap canvases before closing
    /// project.
    bool m_pendingProjectClose{ false };

    /// @brief Whether all dirty-close prompts finished and project cleanup can
    /// run.
    bool m_projectCloseReady{ false };

    /// @brief 保护项目打开请求路径的轻量级锁
    mutable std::mutex m_pendingMutex;

    /// @brief 保护编辑器级剪贴板的轻量级锁。
    /// @brief 该职责已收敛到 EditorClipboard 内部锁。

    /// @brief 编辑器级共享剪贴板。
    /// @brief 该职责已收敛到 EditorClipboard 内部条目列表。

    /// @brief 当前剪贴板是否来自剪切操作。
    /// @brief 该职责已收敛到 EditorClipboard 内部剪切状态。

    /// @brief 剪切来源会话上下文，仅用于身份比较，不负责生命周期。
    /// @brief 该职责已收敛到 EditorClipboard 内部来源上下文。
    /// @brief 编辑器级剪贴板组件，封装剪贴板内容、来源 Session 和剪切状态。
    EditorClipboard m_clipboard;

    /// @brief 缓存各摄像机的最后已知视口尺寸 (受 m_buffersMutex 保护)
    /// @brief 该职责已收敛到 RenderSyncRegistry 内部视口尺寸缓存。

    /// @brief 逻辑线程实时刷新率 (UPS)
    /// @warning 逻辑/UI 热路径/原子：逻辑线程低频写入、UI
    /// 可每帧读取；仅用于展示，使用 relaxed。
    std::atomic<float> m_logicUps{ 0.0f };

    /// @brief 是否强制同步使用同一主音轨的画布时间。
    /// @warning 逻辑热路径/原子：多会话同步分支读取；只传递开关状态，使用
    /// relaxed。
    std::atomic<bool> m_syncSameMainAudioCanvases{ false };

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
    /// @warning 文件监听线程原子：仅用于启动/停止监听线程，不属于渲染热路径。
    std::atomic<bool> m_watcherRunning{ false };

    /// @brief 是否有未处理的文件系统变更挂起
    /// @warning 逻辑热路径/原子：loop 每次迭代 exchange；不可避免，用于把
    /// watcher 线程的文件系统事件去抖后转入低频扫描。
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
