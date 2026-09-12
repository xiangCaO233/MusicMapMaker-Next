#pragma once
#include "event/ui/UIEvent.h"

namespace MMM::Event
{

/// @brief 请求指定浮窗管理器显示或隐藏一个子视图。
struct UISubViewToggleEvent : public UIEvent {
    /// @brief 目标浮窗管理器的名称 (例如 "LeftPanel", "RightPanel")
    std::string targetFloatManagerName;

    /// @brief 要切换到的子视图 ID (例如 "FileManager", "AudioManager")
    std::string subViewId;

    /// @brief 是否要显示子视图
    bool showSubView{ false };
};

}  // namespace MMM::Event

// 注册父类关系，使通用 UI 监听器也能接收子视图切换请求。
EVENT_REGISTER_PARENTS(MMM::Event::UISubViewToggleEvent, MMM::Event::UIEvent);
