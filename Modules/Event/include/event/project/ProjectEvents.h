#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"

#include <filesystem>
#include <string>

namespace MMM::Event
{

/// @brief 所有项目相关事件的基础事件。
struct ProjectEvent : public BaseEvent {
};

/// @brief 项目请求类事件，表示 UI 或外部系统提交的项目操作意图。
struct ProjectRequestEvent : public ProjectEvent {
};

/// @brief 项目生命周期事件，表示项目实例已经完成打开、关闭等状态变更。
struct ProjectLifecycleEvent : public ProjectEvent {
};

/// @brief 项目切换流程事件，表示旧画布关闭确认的流程状态。
struct ProjectSwitchEvent : public ProjectEvent {
};

/// @brief 关闭当前项目请求事件，由项目控制器排队到逻辑线程处理。
struct ProjectCloseRequestedEvent : public ProjectRequestEvent {
};

/// @brief 项目切换需要先关闭旧谱面画布事件。
struct ProjectSwitchNeedsCanvasCloseEvent : public ProjectSwitchEvent {
    /// @brief 旧画布关闭后需要打开的项目路径；仅关闭当前项目时为空。
    std::filesystem::path m_projectPathToOpen;

    /// @brief 是否只关闭当前项目而不继续打开新项目。
    bool m_closeOnly{ false };
};

/// @brief UI 已完成旧谱面画布关闭确认的项目切换事件。
struct ProjectSwitchCompletedEvent : public ProjectSwitchEvent {
};

/// @brief UI 取消旧谱面画布关闭确认的项目切换事件。
struct ProjectSwitchCancelledEvent : public ProjectSwitchEvent {
};

/// @brief 项目关闭完成事件。
struct ProjectClosedEvent : public ProjectLifecycleEvent {
    /// @brief 被关闭项目的标题。
    std::string m_projectTitle;

    /// @brief 被关闭项目的根目录路径。
    std::filesystem::path m_projectPath;
};

}  // namespace MMM::Event

EVENT_REGISTER_PARENTS(MMM::Event::ProjectEvent, MMM::Event::BaseEvent);
EVENT_REGISTER_PARENTS(MMM::Event::ProjectRequestEvent,
                       MMM::Event::ProjectEvent);
EVENT_REGISTER_PARENTS(MMM::Event::ProjectLifecycleEvent,
                       MMM::Event::ProjectEvent);
EVENT_REGISTER_PARENTS(MMM::Event::ProjectSwitchEvent,
                       MMM::Event::ProjectEvent);
EVENT_REGISTER_PARENTS(MMM::Event::ProjectCloseRequestedEvent,
                       MMM::Event::ProjectRequestEvent);
EVENT_REGISTER_PARENTS(MMM::Event::ProjectSwitchNeedsCanvasCloseEvent,
                       MMM::Event::ProjectSwitchEvent);
EVENT_REGISTER_PARENTS(MMM::Event::ProjectSwitchCompletedEvent,
                       MMM::Event::ProjectSwitchEvent);
EVENT_REGISTER_PARENTS(MMM::Event::ProjectSwitchCancelledEvent,
                       MMM::Event::ProjectSwitchEvent);
EVENT_REGISTER_PARENTS(MMM::Event::ProjectClosedEvent,
                       MMM::Event::ProjectLifecycleEvent);
