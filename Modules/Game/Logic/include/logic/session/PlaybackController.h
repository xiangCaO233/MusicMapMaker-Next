#pragma once

#include "common/LogicCommands.h"

namespace MMM::Logic
{
struct SessionContext;

/// @brief 播放控制器，负责处理音频播放、时间轴滚动以及视听同步相关逻辑。
class PlaybackController
{
public:
    /// @brief 构造函数
    /// @param ctx 会话上下文引用
    PlaybackController(SessionContext& ctx) : m_ctx(ctx) {}

    /// @brief 处理设置播放状态的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdSetPlayState& cmd);

    /// @brief 处理时间轴跳转的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdSeek& cmd);

    /// @brief 处理设置播放速度的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdSetPlaybackSpeed& cmd);

    /// @brief 处理鼠标滚轮滚动的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdScroll& cmd);

    /// @brief 同步当前的打击索引，用于打击特效和音效判断
    void syncHitIndex();

    /// @brief 重新构建所有的打击事件列表
    void rebuildHitEvents();

    /// @brief 每帧更新逻辑
    /// @param dt 帧间隔时间 (秒)
    void onUpdate(double dt);

private:
    SessionContext& m_ctx;  ///< 全局会话上下文引用
};

}  // namespace MMM::Logic
