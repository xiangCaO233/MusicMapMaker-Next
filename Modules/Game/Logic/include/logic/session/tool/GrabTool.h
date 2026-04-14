#pragma once

#include "logic/session/tool/IEditTool.h"

namespace MMM::Logic
{

/// @brief 抓取工具 (MoveTool)，负责移动选中的音符。
class GrabTool : public IEditTool
{
public:
    void handleStartDrag(SessionContext& ctx, const CmdStartDrag& cmd) override;
    void handleUpdateDrag(SessionContext&      ctx,
                          const CmdUpdateDrag& cmd) override;
    void handleEndDrag(SessionContext& ctx, const CmdEndDrag& cmd) override;
};

}  // namespace MMM::Logic
