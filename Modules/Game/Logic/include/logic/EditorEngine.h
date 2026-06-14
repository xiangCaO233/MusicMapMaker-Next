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
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
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

    /// @brief 当前是否打开了临时只读项目。
    /// @return 当前项目为临时项目时返回 true。
    bool isTemporaryProjectOpen() const;

    /// @brief 获取当前临时项目的运行时路径信息。
    /// @return 当前临时项目源包与缓存目录；非临时项目时返回默认值。
    ProjectController::TemporaryProjectInfo currentTemporaryProjectInfo() const;

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

    /// @brief 重新预加载当前项目中的 Effect 音频资源。
    /// @warning 低频资源重载路径：皮肤热切换清空音效池后调用；会访问项目资源表
    /// 并触发音频解码缓存加载，禁止放入逻辑 update 热路径。
    void reloadCurrentProjectEffectSoundEffects();

    /// @brief 更新音轨资源信息
    void handleUpdateAudioResource(const CmdUpdateAudioResource& cmd);

    /// @brief 移除项目音轨资源
    void handleRemoveAudioResource(const CmdRemoveAudioResource& cmd);

    /// @brief 从项目中移除谱面
    void handleRemoveBeatmap(const CmdRemoveBeatmap& cmd);

    /// @brief 将当前临时项目保存到正式项目目录。
    /// @param cmd 保存临时项目指令。
    void handleSaveTemporaryProject(const CmdSaveTemporaryProject& cmd);

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

    /// @brief 将指定 Session 原地重置为 Logo 占位画布。
    /// @param index 要重置的 Session 索引。
    /// @param displayName Logo 占位画布的显示名称。
    /// @param updateWorkspace 是否在重置后刷新项目工作区中的打开谱面列表。
    void resetSessionToLogoPlaceholder(int32_t            index,
                                       const std::string& displayName,
                                       bool updateWorkspace = true);

    /**
     * @brief 设置当前活跃（前台）的 Session 索引
     * @param index 目标 Session 索引
     */
    void setActiveSessionIndex(int32_t index);

    /// @brief 请求 UI 线程将指定 Session 对应的画布窗口聚焦到前台。
    /// @param index 目标 Session 索引。
    /// @warning 逻辑/UI 热路径原子：低频写入，UI 同步阶段读取；只传递索引。
    void requestSessionFocus(int32_t index);

    /// @brief 消费一次待聚焦 Session 请求。
    /// @return 目标 Session 索引；无请求时返回 -1。
    /// @warning UI 热路径原子：CanvasTabManager
    /// 每帧读取；只承载一次性焦点请求。
    int32_t consumePendingFocusSessionIndex();

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

    /// @brief 判断指定主画布是否允许通过悬停滚轮接管滚动。
    /// @param cameraId 目标主画布 cameraId。
    /// @return 目标是当前活动画布，或与当前活动画布引用同一主音轨时返回 true。
    /// @warning UI 热路径辅助：只允许在滚轮输入分支调用；会短暂持有
    /// SessionRegistry 锁。
    bool canHoverScrollCamera(const std::string& cameraId) const;

    /// @brief 更新指定主画布窗口在 UI 中的可见状态。
    /// @param cameraId 目标主画布 cameraId。
    /// @param isVisible 当前 ImGui 窗口是否真实可见。
    /// @warning UI 热路径：Basic2DCanvas 每帧写入；只更新 SessionEntry
    /// 中的可见脏状态，供逻辑线程裁剪隐藏 tab 快照。
    void setSessionCanvasVisible(const std::string& cameraId, bool isVisible);

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

    /// @brief 按修订号将指定画布的图集 UV 映射同步到快照缓存。
    /// @param cameraId 目标画布 cameraId。
    /// @param target 目标快照中的 UV 映射表。
    /// @param targetRevision 目标快照当前持有的 UV 修订号。
    /// @warning 逻辑/渲染热路径：每个快照生成时调用；只有图集变化时才复制 UV
    /// 表，普通路径只做锁内查找和 revision 比较。
    void updateSnapshotAtlasUVMap(
        const std::string&                       cameraId,
        std::unordered_map<uint32_t, glm::vec4>& target,
        std::uint64_t&                           targetRevision) const;

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

    /// @brief 发布主渲染线程实时帧率。
    /// @param fps ImGui 或渲染循环统计出的当前 FPS。
    /// @warning UI/逻辑热路径原子：UI 线程每帧写入，逻辑线程每 update
    /// 读取；只承载自适应 RenderSnapshot 预算，使用 relaxed。
    void publishRenderFps(float fps);

    /// @brief 获取主渲染线程实时帧率。
    /// @return 最近发布的 FPS；尚未发布时返回 0。
    /// @warning
    /// UI/逻辑热路径原子：状态栏和逻辑线程可每帧读取；只用于统计和快照预算。
    float getRenderFps() const
    {
        return m_renderFps.load(std::memory_order_relaxed);
    }

    /// @brief 根据实时 UPS/FPS 计算 RenderSnapshot 最小生成间隔。
    /// @param config 当前编辑器配置。
    /// @param secondaryCamera 是否为 Preview/Timeline 等辅助视图。
    /// @return 当前建议的最小快照间隔，单位秒。
    /// @warning 逻辑热路径：每 update 或每辅助相机执行；仅读取 relaxed
    /// 原子并做常量级数学运算，禁止访问 ECS 或同步缓冲区。
    double adaptiveRenderSnapshotMinInterval(const Config::EditorConfig& config,
                                             bool secondaryCamera) const;

    /// @brief 获取逻辑线程发布的软件光标 BPM 同步烟雾寿命。
    /// @return 当前 BPM 对应的一拍时长；无有效谱面或 BPM 时返回 -1。
    /// @warning UI
    /// 热路径/原子：主渲染循环每帧读取；只承载逻辑线程发布的显示状态， 使用
    /// relaxed，避免渲染线程访问 Session 锁和谱面数据。
    float getCursorSmokeLifeOverride() const
    {
        return m_cursorSmokeLifeOverride.load(std::memory_order_relaxed);
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

    /// @brief 刷新已打开 Session 的主音轨同步路径键。
    /// @warning 低频路径：谱面元数据或项目路径发生变化时调用；会遍历当前
    /// Session 列表并执行路径规范化，禁止放入每帧或每 update 调用链。
    void refreshMainAudioSyncKeys();

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
    void openProject(
        const std::filesystem::path& projectPath,
        const std::optional<ProjectController::ProjectCreationOptions>&
            creationOptions = std::nullopt);

    /// @brief 打开谱面包为临时只读项目。
    /// @param packagePath 需要临时阅览的谱面包路径。
    void openTemporaryProjectPackage(const std::filesystem::path& packagePath);

    /// @brief 应用项目控制器打开项目后的逻辑副作用。
    /// @param openResult 项目控制器返回的打开结果。
    void finishOpenProject(
        const ProjectController::OpenProjectResult& openResult);

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

    /// @brief 从指定源 Session 同步同主音轨的其他画布时间。
    /// @param sourceIndex 源 Session 在注册表中的索引。
    /// @warning 逻辑热路径：每次 Session update 后可能执行；同步开关读取使用
    /// relaxed，并只同步 mainAudioSyncKey 相同的 Session。
    void syncSameMainAudioCanvasesFromIndex(int32_t sourceIndex);

    /// @brief 刷新已打开 Session 的主音轨同步路径键，调用者必须持有注册表锁。
    /// @warning 低频路径：会遍历当前 Session 列表并执行路径规范化。
    void refreshMainAudioSyncKeysUnsafe();

    /// @brief 刷新是否存在同主音轨同步候选，调用者必须持有注册表锁。
    /// @warning 低频路径：只在 Session 增删或主音轨路径变化后调用。
    void refreshMainAudioSyncPeerStateUnsafe();

    /// @brief 判断打开新项目前是否需要先关闭当前谱面画布。
    bool needsCanvasCloseBeforeProjectOpen() const;

    /// @brief 逻辑循环在线程池中的任务句柄。
    /// @warning 生命周期路径：仅在 start/stop
    /// 低频路径写入；逻辑循环本身长期占用一个共享线程池工作线程。
    std::future<void> m_loopFuture;

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

    /// @brief 非活跃 Session 最近一次生成渲染快照的时间点，按 Session
    /// 索引存储。
    /// @warning 逻辑热路径：每 update
    /// 读取和低频写入；用于限制后台画布快照频率。
    std::vector<std::chrono::steady_clock::time_point>
        m_backgroundSessionUpdateTimes;

    /// @brief 编辑器级剪贴板组件，封装剪贴板内容、来源 Session 和剪切状态。
    EditorClipboard m_clipboard;

    /// @brief 逻辑线程实时刷新率 (UPS)
    /// @warning 逻辑/UI 热路径/原子：逻辑线程低频写入、UI
    /// 可每帧读取；仅用于展示，使用 relaxed。
    std::atomic<float> m_logicUps{ 0.0f };

    /// @brief 主渲染线程实时刷新率 (FPS)。
    /// @warning UI/逻辑热路径/原子：UI 线程每帧写入、逻辑线程每 update
    /// 读取；仅用于展示和 RenderSnapshot 自适应预算，使用 relaxed。
    std::atomic<float> m_renderFps{ 0.0f };

    /// @brief 逻辑线程发布给渲染线程的软件光标烟雾寿命覆盖值。
    /// @warning 逻辑/UI 热路径/原子：逻辑线程每 update
    /// 写入，渲染线程每帧读取；只承载 当前 BPM 对应的一拍时长或 -1，使用
    /// relaxed 以避免主渲染线程锁竞争。
    std::atomic<float> m_cursorSmokeLifeOverride{ -1.0f };

    /// @brief 是否强制同步使用同一主音轨的画布时间。
    /// @warning 逻辑热路径/原子：多会话同步分支读取；只传递开关状态，使用
    /// relaxed。
    std::atomic<bool> m_syncSameMainAudioCanvases{ true };

    /// @brief 当前是否存在至少两个使用同一主音轨的非 Logo Session。
    /// @warning 逻辑热路径/原子：同步入口每次读取；低频路径写入，只承载
    /// 同步候选脏状态，使用 relaxed。
    std::atomic<bool> m_hasMainAudioSyncPeers{ false };

    /// @brief 等待 UI 线程聚焦的 Session 索引。
    /// @warning 逻辑/UI 热路径原子：createSession 低频写入，CanvasTabManager
    /// 每帧消费。
    std::atomic<int32_t> m_pendingFocusSessionIndex{ -1 };

    /// @brief 项目工作区恢复后待激活的 Session 索引，仅由逻辑线程读写。
    int32_t m_pendingWorkspaceActiveIndex{ -1 };

    /// @brief 上次执行同主音轨同步时的活跃 Session 索引。
    int32_t m_lastMainAudioSyncActiveIndex{ -1 };

    /// @brief 上次执行同主音轨同步时的活跃 Session 逻辑时间。
    double m_lastMainAudioSyncTime{ 0.0 };

    /// @brief 逻辑线程更新计数器，用于 UPS 计算
    uint32_t m_logicUpdateCount{ 0 };

    /// @brief 上次统计 UPS 的单调时钟时间点。
    std::chrono::steady_clock::time_point m_lastUpsTime;
};

}  // namespace MMM::Logic
