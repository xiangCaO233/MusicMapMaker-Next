#pragma once

#include "logic/session/tool/IEditTool.h"

namespace MMM::Logic
{

class MarqueeTool : public IEditTool
{
public:
    void handleStartMarquee(BeatmapSession&        session,
                            const CmdStartMarquee& cmd) override;
    void handleUpdateMarquee(BeatmapSession&         session,
                             const CmdUpdateMarquee& cmd) override;
    void handleEndMarquee(BeatmapSession&      session,
                          const CmdEndMarquee& cmd) override;
    void handleRemoveMarqueeAt(BeatmapSession&           session,
                               const CmdRemoveMarqueeAt& cmd) override;
};

}  // namespace MMM::Logic
