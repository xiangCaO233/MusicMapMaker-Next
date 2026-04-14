#pragma once

#include "logic/session/tool/IEditTool.h"

namespace MMM::Logic
{

/// @brief 框选工具，负责创建、更新和删除矩形框选区域。
class MarqueeTool : public IEditTool
{
public:
    void handleStartMarquee(SessionContext&        ctx,
                            const CmdStartMarquee& cmd) override;
    void handleUpdateMarquee(SessionContext&         ctx,
                             const CmdUpdateMarquee& cmd) override;
    void handleEndMarquee(SessionContext&      ctx,
                          const CmdEndMarquee& cmd) override;
    void handleRemoveMarqueeAt(SessionContext&           ctx,
                               const CmdRemoveMarqueeAt& cmd) override;
};

}  // namespace MMM::Logic
