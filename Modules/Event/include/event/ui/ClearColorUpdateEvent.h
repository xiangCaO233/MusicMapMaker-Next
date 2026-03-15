#pragma once

#include <array>

namespace MMM::Event
{

struct ClearColorUpdateEvent {
    std::array<float, 4> clear_color_value;
};

}  // namespace MMM::Event
