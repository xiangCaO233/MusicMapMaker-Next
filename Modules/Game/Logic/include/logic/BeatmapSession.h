#pragma once

#include "common/LogicCommands.h"
#include <concurrentqueue.h>
#include <memory>

namespace MMM::Config
{
struct EditorConfig;
}
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

    /// @brief 会话逻辑每帧更新（由 Logic 线程主循环调用）
    /// @param dt 帧间隔时间 (秒)
    /// @param config 全局编辑器配置
    void update(double dt, const Config::EditorConfig& config);

    /// @brief 获取共享上下文的只读引用（通常供 UI 渲染层读取状态）
    const SessionContext& getContext() const { return *m_ctx; }

    /// @brief 获取共享上下文的可变引用
    SessionContext& getContextMutable() { return *m_ctx; }

private:
    /// @brief 消费并路由指令队列中的所有命令
    void processCommands();

    /// @brief 更新 ECS 状态并为所有活跃视口生成渲染快照
    void updateECSAndRender(const Config::EditorConfig& config);

    // --- 内部指令处理器 (由 Session 自身处理的元命令) ---
    void handleCommand(const CmdUpdateEditorConfig& cmd);
    void handleCommand(const CmdUpdateViewport& cmd);
    void handleCommand(const CmdLoadBeatmap& cmd);
    void handleCommand(const CmdSaveBeatmap& cmd);
    void handleCommand(const CmdSaveBeatmapAs& cmd);
    void handleCommand(const CmdPackBeatmap& cmd);
    void handleCommand(const CmdUpdateBeatmapMetadata& cmd);

    std::unique_ptr<SessionContext>        m_ctx;          ///< 共享上下文状态
    std::unique_ptr<PlaybackController>    m_playback;     ///< 播放控制器
    std::unique_ptr<InteractionController> m_interaction;  ///< 交互控制器
    std::unique_ptr<ActionController>      m_actions;  ///< 动作/历史记录控制器

    moodycamel::ConcurrentQueue<LogicCommand>
        m_commandQueue;  ///< 跨线程无锁指令队列

    bool m_wasPlaying{ false };  ///< 上一帧是否正在播放
};

}  // namespace MMM::Logic
