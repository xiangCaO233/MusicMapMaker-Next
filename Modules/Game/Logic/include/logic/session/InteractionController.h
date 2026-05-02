#pragma once

#include "common/LogicCommands.h"
#include "logic/session/tool/IEditTool.h"

namespace MMM::Logic
{
struct SessionContext;

/// @brief 交互控制器，负责处理鼠标悬停、选中、拖拽以及编辑工具的状态机逻辑。
class InteractionController
{
public:
    /// @brief 构造函数
    /// @param ctx 会话上下文引用
    InteractionController(SessionContext& ctx);

    /// @brief 处理设置悬停实体的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdSetHoveredEntity& cmd);

    /// @brief 处理选中实体的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdSelectEntity& cmd);

    /// @brief 处理全选命令
    /// @param cmd 命令数据
    void handleCommand(const CmdSelectAll& cmd);

    /// @brief 处理开始拖拽的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdStartDrag& cmd);

    /// @brief 处理拖拽更新的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdUpdateDrag& cmd);

    /// @brief 处理结束拖拽的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdEndDrag& cmd);

    /// @brief 处理切换工具的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdChangeTool& cmd);

    /// @brief 处理更新鼠标位置的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdSetMousePosition& cmd);

    /// @brief 处理开始框选的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdStartMarquee& cmd);

    /// @brief 处理框选更新的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdUpdateMarquee& cmd);

    /// @brief 处理结束框选的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdEndMarquee& cmd);

    /// @brief 处理移除指定框选框的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdRemoveMarqueeAt& cmd);

    /// @brief 处理开始画笔的命令
    void handleCommand(const CmdStartBrush& cmd);

    /// @brief 处理画笔更新的命令
    void handleCommand(const CmdUpdateBrush& cmd);

    /// @brief 处理结束画笔的命令
    void handleCommand(const CmdEndBrush& cmd);

    /// @brief 处理开始擦除的命令
    void handleCommand(const CmdStartErase& cmd);

    /// @brief 处理擦除更新的命令
    void handleCommand(const CmdUpdateErase& cmd);

    /// @brief 处理结束擦除的命令
    void handleCommand(const CmdEndErase& cmd);


    /// @brief 处理更新轨道数量的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdUpdateTrackCount& cmd);

    /// @brief 更新当前所有实体的框选选中状态
    /// @param forceFullSync 是否强制全量同步（忽略加选模式）
    void updateMarqueeSelection(bool forceFullSync = false);

private:
    SessionContext& m_ctx;  ///< 全局会话上下文引用
    std::unordered_map<EditTool, std::unique_ptr<IEditTool>>
        m_tools;  ///< 工具状态机字典
};

}  // namespace MMM::Logic
