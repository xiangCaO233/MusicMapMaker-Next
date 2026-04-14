#pragma once

#include "common/LogicCommands.h"

namespace MMM::Logic
{
struct SessionContext;

/// @brief 编辑工具基类接口。
/// 工具封装了针对音符的拖拽、选择、创建和修改的行为逻辑。
class IEditTool
{
public:
    virtual ~IEditTool() = default;

    /// @brief 工具被选中激活时的回调
    /// @param ctx 会话上下文
    virtual void onSelected(SessionContext& ctx) {}

    /// @brief 工具被取消选中时的回调
    /// @param ctx 会话上下文
    virtual void onDeselected(SessionContext& ctx) {}

    /// @brief 处理开始拖拽事件
    /// @param ctx 会话上下文
    /// @param cmd 开始拖拽指令
    virtual void handleStartDrag(SessionContext& ctx, const CmdStartDrag& cmd)
    {
    }

    /// @brief 处理拖拽更新事件
    /// @param ctx 会话上下文
    /// @param cmd 拖拽更新指令
    virtual void handleUpdateDrag(SessionContext& ctx, const CmdUpdateDrag& cmd)
    {
    }

    /// @brief 处理结束拖拽事件
    /// @param ctx 会话上下文
    /// @param cmd 结束拖拽指令
    virtual void handleEndDrag(SessionContext& ctx, const CmdEndDrag& cmd) {}

    /// @brief 处理开始框选事件
    /// @param ctx 会话上下文
    /// @param cmd 开始框选指令
    virtual void handleStartMarquee(SessionContext&        ctx,
                                    const CmdStartMarquee& cmd)
    {
    }

    /// @brief 处理框选更新事件
    /// @param ctx 会话上下文
    /// @param cmd 框选更新指令
    virtual void handleUpdateMarquee(SessionContext&         ctx,
                                     const CmdUpdateMarquee& cmd)
    {
    }

    /// @brief 处理结束框选事件
    /// @param ctx 会话上下文
    /// @param cmd 结束框选指令
    virtual void handleEndMarquee(SessionContext& ctx, const CmdEndMarquee& cmd)
    {
    }

    /// @brief 处理点击移除指定框选框的事件
    /// @param ctx 会话上下文
    /// @param cmd 移除框选框指令
    virtual void handleRemoveMarqueeAt(SessionContext&           ctx,
                                       const CmdRemoveMarqueeAt& cmd)
    {
    }
};

}  // namespace MMM::Logic
