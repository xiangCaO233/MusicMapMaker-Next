#include "logic/BeatmapSession.h"
#include "logic/ecs/components/TimelineComponent.h"

namespace MMM::Logic
{

// --- Editing Handlers ---

void BeatmapSession::handleCommand(const CmdUndo& cmd) {}
void BeatmapSession::handleCommand(const CmdRedo& cmd) {}
void BeatmapSession::handleCommand(const CmdCopy& cmd) {}
void BeatmapSession::handleCommand(const CmdPaste& cmd) {}
void BeatmapSession::handleCommand(const CmdCut& cmd) {}

// --- Timeline Handlers ---

void BeatmapSession::handleCommand(const CmdUpdateTimelineEvent& cmd)
{
    if ( m_timelineRegistry.valid(cmd.entity) ) {
        auto& tl = m_timelineRegistry.get<TimelineComponent>(cmd.entity);
        tl.m_timestamp = cmd.newTime;
        tl.m_value     = cmd.newValue;
        m_timelineRegistry.patch<TimelineComponent>(cmd.entity);
    }
}

void BeatmapSession::handleCommand(const CmdDeleteTimelineEvent& cmd)
{
    if ( m_timelineRegistry.valid(cmd.entity) ) {
        m_timelineRegistry.destroy(cmd.entity);
    }
}

void BeatmapSession::handleCommand(const CmdCreateTimelineEvent& cmd)
{
    auto entity = m_timelineRegistry.create();
    m_timelineRegistry.emplace<TimelineComponent>(entity,
                                                  cmd.time,
                                                  cmd.type,
                                                  cmd.value);
    m_timelineRegistry.patch<TimelineComponent>(entity);
}

} // namespace MMM::Logic
