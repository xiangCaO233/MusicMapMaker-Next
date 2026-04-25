#pragma once

#include "logic/session/tool/IEditTool.h"

namespace MMM::Logic
{

/// @brief 绘制工具，负责在谱面上放置/创建新的音符。
class DrawTool : public IEditTool
{
public:
    void handleStartBrush(SessionContext& ctx, const CmdStartBrush& cmd) override;
    void handleUpdateBrush(SessionContext& ctx, const CmdUpdateBrush& cmd) override;
    void handleEndBrush(SessionContext& ctx, const CmdEndBrush& cmd) override;
};

}  // namespace MMM::Logic
