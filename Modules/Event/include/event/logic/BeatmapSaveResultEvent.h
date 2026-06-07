#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"
#include <string>

namespace MMM::Event
{

/// @brief 谱面保存或导出完成后的结果事件。
struct BeatmapSaveResultEvent : public BaseEvent {
    /// @brief 目标文件路径，使用 UTF-8 字符串。
    std::string path;
    /// @brief 是否成功写出文件。
    bool success{ false };
    /// @brief 是否来自另存为/导出流程。
    bool isExport{ false };
};

}  // namespace MMM::Event

EVENT_REGISTER_PARENTS(MMM::Event::BeatmapSaveResultEvent,
                       MMM::Event::BaseEvent)
