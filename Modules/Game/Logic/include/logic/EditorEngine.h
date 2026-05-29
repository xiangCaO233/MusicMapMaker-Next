#pragma once

#include "common/LogicCommands.h"
#include "logic/BeatmapSession.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorClipboard.h"
#include "logic/ProjectController.h"
#include "logic/RenderSyncRegistry.h"
#include "logic/SessionRegistry.h"
#include "logic/session/context/SessionContext.h"
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
     * @brief 获取当前项目
     */
    Project* getCurrentProject()
    {
        return ProjectController::instance().currentProject();
    }
    const Project* getCurrentProject() const
    {
        return ProjectController::instance().currentProject();
    }

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

    /**
     * @brief 创建新的画布 Session
     * @param beatmap
     * 要加载的谱面（可为 nullptr 表示空白占位）
     * @param displayName
     * 显示名称
     * @param isLogoPlaceholder 是否为初始 Logo 画布
     * @param preferredCameraId 工作区恢复时希望复用的稳定画布 ID。
     * @param restoreDockFromWorkspace 是否让 UI 使用项目 ini 中的原停靠状态。
     *
     * @return 新 Session 在会话注册表中的索引
     */
    int32_t createSession(std::shared_ptr<MMM::BeatMap> beatmap       = nullptr,
                          const std::string&            displayName   = "",
                          bool               isLogoPlaceholder        = false,
                          const std::string& preferredCameraId        = "",
                          bool               restoreDockFromWorkspace = false);

    /**
     * @brief 关闭指定索引的画布 Session
     * @param index 要关闭的 Session 索引
     * @param updateWorkspace 是否在关闭后刷新项目工作区中的打开谱面列表
     */
    void closeSession(int32_t index, bool updateWorkspace = true);

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
    /// @brief 捕获当前打开谱面、播放进度和主音轨运行时配置到项目设置。
    void captureProjectWorkspaceState();

    /// @brief 根据当前项目的工作区状态恢复上次打开的谱面和播放进度。
    /// @param explicitBeatmapPath 本次打开项目时用户显式指定的谱面路径。
    void restoreProjectWorkspace(
        const std::filesystem::path& explicitBeatmapPath);

    /**
     * @brief 逻辑线程的主循环
     * @warning
     * 逻辑热路径：独立逻辑线程按 UPS 频率执行；禁止每 update
     * 文件系统操作、完整
     * entt 遍历、完整排序、try/catch 和可避免的
     * shared_ptr 拷贝。
     */
    void loop();

    /// @brief 打开项目目录并加载其中的所有资源。
    /// @param projectPath 要打开的项目目录或谱面文件路径。
    void openProject(const std::filesystem::path& projectPath);

    /**
     * @brief 定期扫描项目目录变更（实现实时目录监听与资源同步）

     */
    void scanProjectDirectory();

    /// @brief 清理当前已打开的项目并卸载项目音频状态。
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

    /// @brief 多画布会话注册表，封装 Session 列表、活跃索引和 cameraId 分配。
    SessionRegistry m_sessionRegistry;

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

    /// @brief 渲染同步注册表，封装同步缓冲区、图集 UV 映射和视口尺寸缓存。
    RenderSyncRegistry m_renderSyncRegistry;

    /// @brief 编辑器级剪贴板组件，封装剪贴板内容、来源 Session 和剪切状态。
    EditorClipboard m_clipboard;

    /// @brief 逻辑线程实时刷新率 (UPS)
    /// @warning 逻辑/UI 热路径/原子：逻辑线程低频写入、UI
    /// 可每帧读取；仅用于展示，使用 relaxed。
    std::atomic<float> m_logicUps{ 0.0f };

    /// @brief 是否强制同步使用同一主音轨的画布时间。
    /// @warning 逻辑热路径/原子：多会话同步分支读取；只传递开关状态，使用
    /// relaxed。
    std::atomic<bool> m_syncSameMainAudioCanvases{ false };

    /// @brief 项目工作区恢复后待激活的 Session 索引，仅由逻辑线程读写。
    int32_t m_pendingWorkspaceActiveIndex{ -1 };

    /// @brief 逻辑线程更新计数器，用于 UPS 计算
    uint32_t m_logicUpdateCount{ 0 };

    /// @brief 上一次计算 UPS 的时间戳
    std::chrono::high_resolution_clock::time_point m_lastUpsTime;
};

}  // namespace MMM::Logic
