#pragma once

#include "event/core/BaseEvent.h"
#include <cstddef>

namespace MMM::Event
{
///@brief 调试事件
struct DebugEvent : public BaseEvent {
    void*  mem{ nullptr };
    size_t memSize;
};
}  // namespace MMM::Event
