#pragma once

#include "logic/session/tool/IEditTool.h"

namespace MMM::Logic
{

class GrabTool : public IEditTool
{
public:
    void handleStartDrag(BeatmapSession&     session,
                         const CmdStartDrag& cmd) override;
    void handleUpdateDrag(BeatmapSession&      session,
                          const CmdUpdateDrag& cmd) override;
    void handleEndDrag(BeatmapSession& session, const CmdEndDrag& cmd) override;
};

}  // namespace MMM::Logic
