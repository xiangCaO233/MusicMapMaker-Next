#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"
#include <array>

namespace MMM::Event
{

struct ClearColorUpdateEvent : public BaseEvent {
    std::array<float, 4> clear_color_value;
};

}  // namespace MMM::Event

EVENT_REGISTER_PARENTS(ClearColorUpdateEvent, BaseEvent)
