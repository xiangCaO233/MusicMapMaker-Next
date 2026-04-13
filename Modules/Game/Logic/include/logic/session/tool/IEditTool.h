#pragma once

#include "common/LogicCommands.h"

namespace MMM::Logic
{
class BeatmapSession;

/**
 * @brief Base interface for editor tools.
 * Tools encapsulate behavior for dragging, selection, creation, and
 * modification of notes.
 */
class IEditTool
{
public:
    virtual ~IEditTool() = default;

    virtual void onSelected(BeatmapSession& session) {}
    virtual void onDeselected(BeatmapSession& session) {}

    virtual void handleStartDrag(BeatmapSession&     session,
                                 const CmdStartDrag& cmd)
    {
    }
    virtual void handleUpdateDrag(BeatmapSession&      session,
                                  const CmdUpdateDrag& cmd)
    {
    }
    virtual void handleEndDrag(BeatmapSession& session, const CmdEndDrag& cmd)
    {
    }

    virtual void handleStartMarquee(BeatmapSession&        session,
                                    const CmdStartMarquee& cmd)
    {
    }
    virtual void handleUpdateMarquee(BeatmapSession&         session,
                                     const CmdUpdateMarquee& cmd)
    {
    }
    virtual void handleEndMarquee(BeatmapSession&      session,
                                  const CmdEndMarquee& cmd)
    {
    }
    virtual void handleRemoveMarqueeAt(BeatmapSession&           session,
                                       const CmdRemoveMarqueeAt& cmd)
    {
    }
};

}  // namespace MMM::Logic
