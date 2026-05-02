#pragma once

#include "common/LogicCommands.h"

namespace MMM::Logic
{
struct SessionContext;

/// @brief 动作控制器，负责处理撤销/重做栈的维护以及各类实体的编辑指令操作。
class ActionController
{
public:
    /// @brief 构造函数
    /// @param ctx 会话上下文引用
    ActionController(SessionContext& ctx) : m_ctx(ctx) {}

    /// @brief 处理撤销命令
    /// @param cmd 命令数据
    void handleCommand(const CmdUndo& cmd);

    /// @brief 处理重做命令
    /// @param cmd 命令数据
    void handleCommand(const CmdRedo& cmd);

    /// @brief 处理复制选中实体的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdCopy& cmd);

    /// @brief 处理剪切选中实体的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdCut& cmd);

    /// @brief 处理删除选中实体的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdDeleteSelected& cmd);

    /// @brief 处理粘贴实体的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdPaste& cmd);

    /// @brief 处理更新时间轴事件(如BPM)的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdUpdateTimelineEvent& cmd);

    /// @brief 处理删除时间轴事件的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdDeleteTimelineEvent& cmd);

    /// @brief 处理创建时间轴事件的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdCreateTimelineEvent& cmd);

    /// @brief 强制同步当前的谱面数据(通常在批量编辑后调用)
    void syncBeatmap();

private:
    SessionContext& m_ctx;  ///< 全局会话上下文引用
};

}  // namespace MMM::Logic
