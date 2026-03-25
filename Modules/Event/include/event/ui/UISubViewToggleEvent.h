// event/ui/UISubViewToggleEvent.h
#pragma once
#include "event/ui/UIEvent.h"

namespace MMM::Event
{

struct UISubViewToggleEvent : public UIEvent {
    /// @brief 目标浮窗管理器的名称 (例如 "LeftPanel", "RightPanel")
    std::string targetFloatManagerName;

    /// @brief 要切换到的子视图 ID (例如 "FileManager", "AudioManager")
    std::string subViewId;
};

}  // namespace MMM::Event

// 注册父类关系，以便可以通过订阅 UIEvent 监听到它
EVENT_REGISTER_PARENTS(MMM::Event::UISubViewToggleEvent, MMM::Event::UIEvent);
