#pragma once
#include "event/project/ProjectEvents.h"
#include <string>

namespace MMM::Event
{

/// @brief 项目加载就绪事件：音频预加载和谱面会话或工作区恢复均已完成。
/// @note 该事件表示项目可供 UI 使用，不等同于已打开某一张谱面。
struct ProjectLoadedEvent : public ProjectLifecycleEvent {
    /// @brief 项目标题
    std::string m_projectTitle;
    /// @brief 项目根目录
    std::string m_projectPath;
    /// @brief 包含的谱面数量
    size_t m_beatmapCount{ 0 };
};

}  // namespace MMM::Event

// 注册到项目生命周期分发链，供通用项目状态监听器接收。
EVENT_REGISTER_PARENTS(MMM::Event::ProjectLoadedEvent,
                       MMM::Event::ProjectLifecycleEvent);
