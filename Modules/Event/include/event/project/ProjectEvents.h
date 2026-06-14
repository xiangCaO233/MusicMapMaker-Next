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

/// @brief 请求显示临时项目关闭确认弹窗。
struct TemporaryProjectClosePromptRequestedEvent : public ProjectRequestEvent {
};

/// @brief 新建项目请求事件，由项目控制器创建目录并排队打开。
struct ProjectCreateRequestedEvent : public ProjectRequestEvent {
    /// @brief 项目根目录路径。
    std::filesystem::path m_projectPath;

    /// @brief 项目显示标题。
    std::string m_title;

    /// @brief 项目曲作者或艺术家。
    std::string m_artist;

    /// @brief 项目谱师。
    std::string m_mapper;

    /// @brief 项目默认物件调色方案；空字符串表示继承软件默认。
    std::string m_noteColorPaletteSchemeName;

    /// @brief 新项目首次打开时的侧边栏页签名称。
    std::string m_sidebarActiveTab;
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

/// @brief 临时项目只读编辑被拦截事件。
struct TemporaryProjectEditBlockedEvent : public ProjectLifecycleEvent {
    /// @brief 用户拖拽打开的原始包文件路径。
    std::string m_sourcePackagePath;

    /// @brief 当前解压出来的临时项目缓存目录。
    std::string m_cacheProjectPath;
};

/// @brief 临时项目保存到正式目录后的结果事件。
struct TemporaryProjectSaveResultEvent : public ProjectLifecycleEvent {
    /// @brief 是否保存成功。
    bool m_success{ false };

    /// @brief 保存成功后的正式项目目录。
    std::string m_savedProjectPath;

    /// @brief 失败时的错误信息。
    std::string m_errorMessage;
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
EVENT_REGISTER_PARENTS(MMM::Event::TemporaryProjectClosePromptRequestedEvent,
                       MMM::Event::ProjectRequestEvent);
EVENT_REGISTER_PARENTS(MMM::Event::ProjectCreateRequestedEvent,
                       MMM::Event::ProjectRequestEvent);
EVENT_REGISTER_PARENTS(MMM::Event::ProjectSwitchNeedsCanvasCloseEvent,
                       MMM::Event::ProjectSwitchEvent);
EVENT_REGISTER_PARENTS(MMM::Event::ProjectSwitchCompletedEvent,
                       MMM::Event::ProjectSwitchEvent);
EVENT_REGISTER_PARENTS(MMM::Event::ProjectSwitchCancelledEvent,
                       MMM::Event::ProjectSwitchEvent);
EVENT_REGISTER_PARENTS(MMM::Event::ProjectClosedEvent,
                       MMM::Event::ProjectLifecycleEvent);
EVENT_REGISTER_PARENTS(MMM::Event::TemporaryProjectEditBlockedEvent,
                       MMM::Event::ProjectLifecycleEvent);
EVENT_REGISTER_PARENTS(MMM::Event::TemporaryProjectSaveResultEvent,
                       MMM::Event::ProjectLifecycleEvent);
