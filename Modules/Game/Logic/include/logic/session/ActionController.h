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

    /// @brief 处理镜像选中实体的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdMirrorSelected& cmd);

    /// @brief 处理对齐选中物件至常用分拍的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdAlignSelectedToCommonBeats& cmd);

    /// @brief 处理将自定义颜色应用到选中音符的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdApplyNoteColorToSelection& cmd);

    /// @brief 处理将完整调色盘应用到选中音符的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdApplyNotePaletteToSelection& cmd);

    /// @brief 处理将当前画笔调色盘应用到单个音符的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdApplyBrushPaletteToEntity& cmd);

    /// @brief 处理清除单个音符自定义配色的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdClearNoteColorOverrides& cmd);

    /// @brief 处理粘贴实体的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdPaste& cmd);

    /// @brief 处理更新时间轴事件(如BPM)的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdUpdateTimelineEvent& cmd);

    /// @brief 处理批量更新时间轴事件的命令。
    /// @param cmd 命令数据。
    void handleCommand(const CmdUpdateTimelineEvents& cmd);

    /// @brief 处理删除时间轴事件的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdDeleteTimelineEvent& cmd);

    /// @brief 处理创建时间轴事件的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdCreateTimelineEvent& cmd);

    /// @brief 处理批量创建时间轴事件的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdCreateTimelineEvents& cmd);

    /// @brief 处理批量替换谱面 Timing 列表的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdReplaceBeatmapTimings& cmd);

    /// @brief 处理从其他谱面替换当前谱面数据的命令。
    /// @param cmd 命令数据。
    void handleCommand(const CmdReplaceBeatmapData& cmd);

    /// @brief 强制同步当前的谱面数据(通常在批量编辑后调用)
    void syncBeatmap();

private:
    SessionContext& m_ctx;  ///< 全局会话上下文引用
};

}  // namespace MMM::Logic
