#include "event/core/EventBus.h"

namespace MMM::Event
{
EventBus& EventBus::instance()
{
    static EventBus evtBus;
    return evtBus;
}
}  // namespace MMM::Event
