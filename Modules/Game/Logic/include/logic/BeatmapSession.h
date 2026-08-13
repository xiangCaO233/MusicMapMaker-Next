#pragma once

#include "common/LogicCommands.h"
#include "mmm/beatmap/BeatmapMutationObserver.h"
#include <atomic>
#include <concurrentqueue.h>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace MMM::Config
{
struct AutoSaveConfig;
struct EditorConfig;
}  // namespace MMM::Config
namespace MMM::Logic
{

struct SessionContext;
class PlaybackController;
class InteractionController;
class ActionController;

/// @brief 谱面逻辑会话核心 (Facade / Controller Manager)
/// 持有各领域子控制器和共享上下文，处理逻辑线程中的所有业务更新。
class BeatmapSession
{
public:
    /// @brief 构造函数，初始化上下文与各控制器
    BeatmapSession();
    ~BeatmapSession();

    // 禁用拷贝与移动
    BeatmapSession(BeatmapSession&&)                 = delete;
    BeatmapSession(const BeatmapSession&)            = delete;
    BeatmapSession& operator=(BeatmapSession&&)      = delete;
    BeatmapSession& operator=(const BeatmapSession&) = delete;

    /// @brief 推送指令到无锁队列（跨线程安全，由 UI 线程或事件系统调用）
    /// @param cmd 指令对象
    void pushCommand(LogicCommand&& cmd);

    /// @brief 设置协作访客谱面是否因房间离线而只读。
    /// @param readOnly 离线时为 true，重新连接到权威房间时为 false。
    /// @warning UI/逻辑跨线程低频调用；原子标志在命令入队入口读取，状态变化时
    /// 额外排队一次取消交互命令。
    void setCollaborationOfflineReadOnly(bool readOnly);

    /// @brief 查询协作访客谱面是否处于离线只读状态。
    /// @return 离线只读时返回 true。
    [[nodiscard]] bool isCollaborationOfflineReadOnly() const;

    /// @brief 设置谱面剪贴板是否限制在当前协作 Session 内。
    /// @param isolated 加入协作房间时为 true，退出房间时为 false。
    /// @warning UI/逻辑跨线程低频调用；原子状态用于在系统剪贴板导入前即时
    /// 拦截，具体范围切换仍由逻辑命令按序完成。
    void setCollaborationClipboardIsolated(bool isolated);

    /// @brief 查询当前会话是否启用了协作剪贴板隔离。
    /// @return 禁止系统剪贴板交换时返回 true。
    [[nodiscard]] bool isCollaborationClipboardIsolated() const;

    /// @brief 设置当前协作身份可修改的谱面数据类别。
    /// @param allowedFlags 房主下发权限映射得到的类别位；离线会话传 All。
    /// @warning UI/逻辑跨线程低频调用；命令入队热路径读取单个原子字节，权限
    /// 收紧时额外排队一次交互取消命令。
    void setCollaborationAllowedMutationFlags(
        ::MMM::BeatmapMutationFlags allowedFlags);

    /// @brief 查询当前协作身份可修改的谱面数据类别。
    [[nodiscard]] ::MMM::BeatmapMutationFlags
    collaborationAllowedMutationFlags() const;

    /// @brief 设置低频谱面领域变化观察者。
    /// @param observer 新观察者；为空时恢复纯离线会话。
    /// @param publishCurrentSnapshot 是否在下一次逻辑更新发布当前完整谱面；
    /// 访客从房主快照创建会话时应传 false，避免把刚收到的快照回传为本地编辑。
    /// @warning 跨 UI/逻辑线程低频调用；原子 shared_ptr
    /// 用于保证协作房间断开时回调对象仍存活，不得在普通 update 中复制。
    void setMutationObserver(
        std::shared_ptr<::MMM::IBeatmapMutationObserver> observer,
        bool publishCurrentSnapshot = true);

    /// @brief 会话逻辑每帧更新（由 Logic 线程主循环调用）
    /// @warning 逻辑热路径：每个逻辑 update
    /// 执行；禁止文件系统访问、完整排序、try/catch 和可避免的 shared_ptr 拷贝。
    /// 元数据尾随保存只允许在空闲超时分支触发，不得扩散到普通更新帧。
    /// @param dt 帧间隔时间 (秒)
    /// @param config 全局编辑器配置
    /// @param isActiveSession 当前会话是否是前台活跃会话。
    void update(double dt, const Config::EditorConfig& config,
                bool isActiveSession);

    /// @brief 获取共享上下文的只读引用（通常供 UI 渲染层读取状态）
    const SessionContext& getContext() const { return *m_ctx; }

    /// @brief 获取共享上下文的可变引用
    SessionContext& getContextMutable() { return *m_ctx; }

    /// @brief 判断会话是否存在等待逻辑线程消费的指令。
    /// @warning 逻辑热路径：后台 Session
    /// 限频判断会调用；只读取无锁队列近似长度。
    bool hasPendingCommands() const;

    /// @brief 判断会话是否需要跳过后台限频并立即更新。
    /// @warning 逻辑热路径：每个 Session update
    /// 调度前调用；只读取会话热状态和队列近似长度。
    bool needsRealtimeUpdate() const;

    /// @brief 判断会话是否需要在 Unlimited 模式下逐逻辑轮次推进。
    /// @return 播放时钟或同音轨播放跟随正在推进时返回 true。
    /// @warning 逻辑热路径：持有 SessionRegistry 锁时调用；只读取播放状态。
    /// 交互命令由无锁命令队列立即唤醒，视觉动画按既有维护间隔推进，不得在此
    /// 扩大为全部 needsRealtimeUpdate 状态以免重新引入 UI 锁饥饿。
    bool needsUnlimitedPolling() const;

    /// @brief 跨线程请求一次由指定编辑器事件触发的自动保存。
    /// @param trigger 触发自动保存的编辑器事件。
    /// @warning UI 线程或 Session 切换路径低频写入，逻辑线程每次 update
    /// 只交换一个原子位掩码；用于避免在 UI/GLFW 回调中执行文件 I/O。
    void requestAutoSave(AutoSaveTrigger trigger);

    /// @brief 判断后台会话是否仍需轮询自动保存期限或事件请求。
    /// @param config 当前软件全局自动保存配置。
    /// @return 存在事件请求，或定时模式下谱面未保存时返回 true。
    /// @warning 逻辑调度热路径：仅读取原子位、布尔状态和撤销栈脏标记，
    /// 不访问文件系统或遍历 ECS。
    [[nodiscard]] bool needsAutoSavePolling(
        const Config::AutoSaveConfig& config) const;

    /// @brief 判断会话是否仍需低频轮询元数据尾随保存。
    /// @return 存在等待空闲期的元数据保存时返回 true。
    /// @warning 逻辑线程调度路径：只读取逻辑线程维护的布尔状态。
    bool hasPendingMetadataAutoSave() const
    {
        return m_metadataAutoSavePending;
    }

    /// @brief 立即落盘尚在等待空闲期的元数据自动保存。
    /// @return 没有待保存内容或保存成功时返回 true。
    /// @warning 低频阻塞路径：仅允许逻辑线程在打包、项目关闭或尾随自动保存
    /// 超时时调用；可能同步谱面数据、访问文件系统并保存项目配置。
    bool flushPendingMetadataAutoSave();

    /// @brief 为打包流程立即保存当前会话中的全部未落盘修改。
    /// @return 没有待保存内容或谱面完整保存成功时返回 true。
    /// @warning
    /// 低频阻塞路径：仅允许逻辑线程在打包前调用；会同步完整谱面并访问文件系统。
    bool saveDirtyBeatmapForPackaging();

private:
    /// @brief 在用户停止 note 编辑一段时间后同步 BeatMap 数据。
    /// @param currentSysTime 当前单调系统时间（秒）。
    /// @param processed 本轮是否处理了用户/逻辑命令。
    /// @param isBusy 当前会话是否处于交互、播放或命令堆积状态。
    /// @warning 逻辑热路径：每个 Session update
    /// 调用；普通路径只做常量级状态判断，只有空闲超时脏分支允许全量同步
    /// BeatMap。
    void flushDeferredBeatmapSync(double currentSysTime, bool processed,
                                  bool isBusy);

    /// @brief 在元数据停止变化且会话空闲后执行一次尾随自动保存。
    /// @param currentSysTime 当前单调系统时间（秒）。
    /// @param isEditingBusy 当前会话是否仍处于编辑或命令堆积状态。
    /// @warning 逻辑热路径：普通帧只做常量级状态判断；仅空闲超时分支允许
    /// 调用同步文件保存流程。
    void flushDeferredMetadataAutoSave(double currentSysTime,
                                       bool   isEditingBusy);

    /// @brief 消费全局配置允许的事件请求并推进定时/事件自动保存。
    /// @param currentSysTime 当前单调系统时间（秒）。
    /// @param isEditingBusy 当前会话是否仍处于连续交互或命令堆积状态。
    /// @param config 当前软件全局自动保存配置。
    /// @warning 逻辑热路径：普通帧只做常量级状态判断；仅到期且存在未保存
    /// 修改的低频分支允许同步谱面并访问文件系统。
    void flushConfiguredAutoSave(double currentSysTime, bool isEditingBusy,
                                 const Config::AutoSaveConfig& config);

    /// @brief 消费并路由指令队列中的所有命令
    /// @return 如果处理了至少一个指令，则返回 true
    /// @warning 逻辑热路径：每个 Session update 调用；普通帧只检查空队列，
    /// 仅低频元数据命令允许同步自动采样领域数据，纯物件协作同步期间可复制一次
    /// 活跃画笔草稿以保持未结束手势。
    bool processCommands();

    /// @brief 在入队与消费边界统一拦截离线房间谱面的编辑命令。
    /// @param cmd 待检查命令。
    /// @return 命令已被拦截时返回 true。
    /// @warning 命令热路径：只读取原子门闩、执行 variant 类型分派，并在每个
    /// 离线周期首次拦截时发布一次事件。
    [[nodiscard]] bool blockCollaborationOfflineEdit(const LogicCommand& cmd);

    /// @brief 在入队与消费边界按房主权限拦截本地谱面变更命令。
    /// @param cmd 待检查命令。
    /// @return 命令需要至少一个未授权数据类别时返回 true。
    /// @warning 命令热路径：只读取一个原子字节并执行 variant 类型分派；每个
    /// 连续拒绝周期最多发布一次提示事件。
    [[nodiscard]] bool blockCollaborationUnauthorizedEdit(
        const LogicCommand& cmd, bool inspectSessionState = false);

    /// @brief 发布跨线程请求的完整谱面快照。
    /// @warning 逻辑热路径：每 update 仅检查一个 relaxed 原子标志；只有新观察者
    /// 绑定后的单次分支会同步完整 BeatMap 并回调。
    void publishRequestedMutationSnapshot();

    /// @brief 判断本轮是否需要生成并发布渲染快照。
    /// @param currentSysTime 当前单调系统时间（秒）。
    /// @param forceImmediate 是否因命令、跳转、交互或脏缓存强制立即发布。
    /// @param config 当前编辑器配置。
    /// @return 需要发布渲染快照时返回 true。
    /// @warning 逻辑热路径：每个 Session update 调用；只做常量时间背压判断，
    /// 禁止访问 ECS 或同步缓冲区。
    bool shouldUpdateRenderSnapshot(double currentSysTime, bool forceImmediate,
                                    const Config::EditorConfig& config) const;

    /// @brief 更新 ECS 状态并为所有活跃视口生成渲染快照
    /// @param config 全局编辑器配置。
    /// @param isActiveSession 当前会话是否是前台活跃会话。
    /// @warning 逻辑/渲染热路径：每个 Session update 执行；完整排序和完整 entt
    /// 遍历只能在脏标记分支内发生。
    void updateECSAndRender(const Config::EditorConfig& config,
                            bool                        isActiveSession);

    /// @brief 根据逻辑时间刷新动画渲染时间。
    /// @param dt 帧间隔时间，单位秒。
    /// @param config 全局编辑器配置。
    /// @param forceImmediate 是否强制跳过暂停态滚动动画。
    /// @warning 逻辑热路径：每个 Session update 执行；只允许常量级数学运算。
    void updateAnimateTime(double dt, const Config::EditorConfig& config,
                           bool forceImmediate);

    /// @brief 刷新渲染使用的动画时间线缩放倍率。
    /// @param dt 帧间隔时间，单位秒。
    /// @param config 全局编辑器配置。
    /// @warning 逻辑热路径：每个 Session update 执行；只允许常量级数学运算。
    void updateAnimatedTimelineZoom(double                      dt,
                                    const Config::EditorConfig& config);

    // --- 内部指令处理器 (由 Session 自身处理的元命令) ---
    void handleCommand(const CmdUpdateEditorConfig& cmd);
    void handleCommand(const CmdUpdateViewport& cmd);
    void handleCommand(const CmdLoadBeatmap& cmd);
    void handleCommand(const CmdSetCollaborationResources& cmd);
    void handleCommand(const CmdSetCollaborationOfflineReadOnly& cmd);
    void handleCommand(const CmdSetCollaborationClipboardIsolation& cmd);
    void handleCommand(const CmdSaveBeatmap& cmd);
    void handleCommand(const CmdSaveBeatmapAs& cmd);
    void handleCommand(const CmdPackBeatmap& cmd);
    /// @brief 更新谱面元数据，并在主音轨提示变化时同步首个 Main BGM 采样。
    /// @param cmd 新的谱面基础元数据。
    /// @return 本次元数据更新额外产生的谱面变化类型。
    /// @warning 低频 UI 命令路径；主音轨实际变化时会同步完整自动采样列表。
    ::MMM::BeatmapMutationFlags handleCommand(
        const CmdUpdateBeatmapMetadata& cmd);
    void handleCommand(const CmdMarkBeatmapMetadataDirty& cmd);

    std::unique_ptr<SessionContext>        m_ctx;          ///< 共享上下文状态
    std::unique_ptr<PlaybackController>    m_playback;     ///< 播放控制器
    std::unique_ptr<InteractionController> m_interaction;  ///< 交互控制器
    std::unique_ptr<ActionController>      m_actions;  ///< 动作/历史记录控制器

    moodycamel::ConcurrentQueue<LogicCommand>
        m_commandQueue;  ///< 跨线程无锁指令队列

    /// @brief 当前低频谱面变化观察者。
    /// @warning 跨线程 shared_ptr 原子：只在谱面发生实际变化或首次绑定时加载，
    /// 用于避免观察者在逻辑回调期间被 UI 线程销毁。
    std::shared_ptr<::MMM::IBeatmapMutationObserver> m_mutationObserver;

    /// @brief 新观察者绑定后请求逻辑线程发布一次完整谱面快照。
    /// @warning UI 线程写、逻辑线程每 update 读，使用 relaxed
    /// 即可，因为观察者指针自身通过 acquire/release 原子传递。
    std::atomic_bool m_mutationSnapshotRequested{ false };

    /// @brief 协作访客谱面断线后的入队级只读门闩。
    /// @warning UI 线程写、所有命令生产线程读；只在协作连接状态变化时写入，
    /// 入队热路径使用 acquire 读取以确保及时拦截编辑命令。
    std::atomic_bool m_collaborationOfflineReadOnly{ false };

    /// @brief 系统剪贴板桥接读取的协作隔离门闩。
    /// @warning UI 线程低频写、UI 剪贴板路径读；使用 acquire/release 保证房间
    /// 状态切换后不再导入系统剪贴板。
    std::atomic_bool m_collaborationClipboardIsolated{ false };

    /// @brief 最近分配给该 Session 的协作剪贴板范围标识。
    /// @warning UI 线程仅在协作绑定切换时更新，逻辑线程通过排队命令接收副本。
    std::atomic_uint64_t m_collaborationClipboardScopeId{ 0 };

    /// @brief 当前协作身份允许修改的谱面类别位。
    /// @warning UI 线程低频写、所有命令生产线程和逻辑线程读；使用单字节原子
    /// 避免在输入热路径复制网络层权限状态。
    std::atomic_uint8_t m_collaborationAllowedMutationFlags{
        static_cast<std::uint8_t>(::MMM::BeatmapMutationFlags::All)
    };

    /// @brief UI 与谱面切换路径提交、逻辑线程消费的自动保存事件位。
    /// @warning 多个低频生产者执行 relaxed fetch_or，逻辑线程每 update
    /// 执行一次 relaxed exchange；事件仅用于唤醒保存调度，不承载其它数据。
    std::atomic_uint8_t m_requestedAutoSaveTriggers{ 0U };

    /// @brief 当前离线周期是否已经发布过编辑拦截提示。
    /// @warning 多命令生产线程写入；用于把连续鼠标命令合并为一次 UI 提示。
    std::atomic_bool m_offlineEditBlockedNotificationSent{ false };

    bool   m_wasPlaying{ false };                   ///< 上一帧是否正在播放
    bool   m_hasDeferredBeatmapSyncTimer{ false };  ///< 是否已有延迟同步计时点
    double m_lastDeferredBeatmapSyncTime{
        0.0
    };  ///< 最近一次刷新延迟同步的时间

    /// @brief 是否有元数据编辑正在等待尾随自动保存。
    bool m_metadataAutoSavePending{ false };

    /// @brief 下一次更新是否需要重置元数据自动保存空闲计时点。
    bool m_metadataAutoSaveTimerNeedsReset{ false };

    /// @brief 最近一次元数据编辑后的自动保存计时点（秒）。
    double m_lastMetadataUpdateTime{ 0.0 };

    /// @brief 是否有已启用的编辑器事件正在等待会话空闲后自动保存。
    bool m_triggeredAutoSavePending{ false };

    /// @brief 当前定时自动保存周期的下一次截止时间（单调秒）。
    double m_timedAutoSaveDeadline{ 0.0 };

    /// @brief 最近一次用于生成截止时间的定时间隔秒数。
    double m_timedAutoSaveIntervalSeconds{ 0.0 };

    /// @brief 最近一次生成渲染快照的单调系统时间（秒）。
    /// @warning 逻辑热路径：每 update 读取，只有发布渲染快照后写入；用于给
    /// RenderSnapshot 生成施加背压，避免压低逻辑 UPS。
    double m_lastRenderSnapshotTime{ 0.0 };

    /// @brief 当前会话已加载或成功保存过的谱面文件哈希，键为规范化路径。
    std::unordered_map<std::string, std::uint64_t> m_savedBeatmapFileHashes;
};

}  // namespace MMM::Logic
