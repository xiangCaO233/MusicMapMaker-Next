#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"
#include <string>

namespace MMM::Event
{

/// @brief 谱面保存前发现目标文件可能已被外部修改时触发的确认事件。
struct BeatmapSaveConflictEvent : public BaseEvent {
    /// @brief 存在覆盖风险的目标文件路径，使用 UTF-8 字符串。
    std::string path;
};

}  // namespace MMM::Event

EVENT_REGISTER_PARENTS(MMM::Event::BeatmapSaveConflictEvent,
                       MMM::Event::BaseEvent)
