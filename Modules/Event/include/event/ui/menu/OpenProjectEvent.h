#pragma once
#include "event/core/BaseEvent.h"
#include <filesystem>

namespace MMM::Event
{

/// @brief 打开项目事件：指示逻辑层加载指定目录下的项目资源
struct OpenProjectEvent : public BaseEvent {
    /// @brief 项目所在的根目录路径
    std::filesystem::path m_projectPath;
};

}  // namespace MMM::Event
