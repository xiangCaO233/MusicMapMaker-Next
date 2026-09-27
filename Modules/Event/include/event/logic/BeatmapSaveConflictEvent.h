#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"
#include <string>

namespace MMM::Event
{

/// @brief 谱面保存前发现目标文件可能已被外部修改时触发的确认事件。
/// @note 该事件只报告冲突目标，是否继续覆盖由交互层另行决定。
struct BeatmapSaveConflictEvent : public BaseEvent {
    /// @brief 存在覆盖风险的目标文件路径，使用 UTF-8 字符串。
    /// @note 路径用于向用户定位目标，不代表文件在处理时仍然存在。
    std::string path;
};

}  // namespace MMM::Event

// 将保存冲突纳入基础事件分发链。
// 通用监听器可据此统一记录冲突发生时间，而无需了解保存交互细节。
EVENT_REGISTER_PARENTS(MMM::Event::BeatmapSaveConflictEvent,
                       MMM::Event::BaseEvent)
