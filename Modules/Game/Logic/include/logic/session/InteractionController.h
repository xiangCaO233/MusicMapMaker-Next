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

    /// @brief 将项目音频资源放置为 BGM 区自动采样。
    /// @param cmd 项目资源 ID 与主画布放置坐标。
    void handleCommand(const CmdCreateAudioSample& cmd);

    /// @brief 原子更新一个自动采样的精确属性。
    /// @param cmd 实体与资源、BGM 相对轨、偏移和音量。
    void handleCommand(const CmdUpdateAudioSampleProperties& cmd);

    /// @brief 更新单个玩家绑定或自动采样的物件音量。
    /// @param cmd 带类型的实体、可选子物件索引与音量倍率。
    void handleCommand(const CmdUpdateObjectSampleVolume& cmd);

    /// @brief 处理切换工具的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdChangeTool& cmd);

    /// @brief 处理设置画笔自定义颜色的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdSetBrushNoteColor& cmd);

    /// @brief 处理设置画笔完整调色盘的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdSetBrushNotePalette& cmd);

    /// @brief 处理项目音频工具的画笔资源选择。
    /// @param cmd 稳定资源 ID 与当前资源类型。
    void handleCommand(const CmdSetBrushAudioResource& cmd);

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

    /// @brief 处理持久化 BGM 轨道数量更新。
    /// @param cmd 目标 BGM 轨道数量。
    void handleCommand(const CmdUpdateBgmTrackCount& cmd);

    /// @brief 更新当前所有实体的框选选中状态
    /// @warning
    /// 逻辑热路径：仅在框选脏标记置位时调用；优先按排序时间段缓存扫描候选，
    /// 禁止改为每帧无条件执行或完整扫描所有音符。
    /// @param forceFullSync 是否强制全量同步（忽略加选模式）
    void updateMarqueeSelection(bool forceFullSync = false);

private:
    SessionContext& m_ctx;  ///< 全局会话上下文引用
    std::unordered_map<EditTool, std::unique_ptr<IEditTool>>
        m_tools;  ///< 工具状态机字典
};

}  // namespace MMM::Logic
