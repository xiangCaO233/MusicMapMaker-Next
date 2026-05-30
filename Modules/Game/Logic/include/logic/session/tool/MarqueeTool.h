#pragma once

#include "logic/session/tool/IEditTool.h"

namespace MMM::Logic
{

/// @brief 框选工具，负责创建、更新和删除矩形框选区域。
class MarqueeTool : public IEditTool
{
public:
    /// @brief 开始创建框选区域，并在非加选模式下清空旧选择。
    /// @param ctx 会话上下文
    /// @param cmd 开始框选指令
    void handleStartMarquee(SessionContext&        ctx,
                            const CmdStartMarquee& cmd) override;

    /// @brief 更新当前框选区域，并标记选择结果需要重算。
    /// @param ctx 会话上下文
    /// @param cmd 框选更新指令
    void handleUpdateMarquee(SessionContext&         ctx,
                             const CmdUpdateMarquee& cmd) override;

    /// @brief 结束框选区域创建，并处理无效的小尺寸框选。
    /// @param ctx 会话上下文
    /// @param cmd 结束框选指令
    void handleEndMarquee(SessionContext&      ctx,
                          const CmdEndMarquee& cmd) override;

    /// @brief 移除鼠标位置命中的框选区域，并标记选择结果需要重算。
    /// @param ctx 会话上下文
    /// @param cmd 移除框选框指令
    void handleRemoveMarqueeAt(SessionContext&           ctx,
                               const CmdRemoveMarqueeAt& cmd) override;
};

}  // namespace MMM::Logic
