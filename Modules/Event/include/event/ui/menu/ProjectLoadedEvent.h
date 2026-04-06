#pragma once
#include "event/core/BaseEvent.h"
#include <string>

namespace MMM::Event
{

/// @brief 项目加载完成事件：UI 层可以监听此事件以刷新文件列表、标题等
struct ProjectLoadedEvent : public BaseEvent {
    /// @brief 项目标题
    std::string m_projectTitle;
    /// @brief 项目根目录
    std::string m_projectPath;
    /// @brief 包含的谱面数量
    size_t m_beatmapCount{ 0 };
};

}  // namespace MMM::Event
