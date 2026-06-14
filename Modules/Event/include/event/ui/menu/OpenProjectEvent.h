#pragma once
#include "event/project/ProjectEvents.h"
#include <filesystem>

namespace MMM::Event
{

/// @brief 打开项目事件：指示逻辑层加载指定目录下的项目资源
struct OpenProjectEvent : public ProjectRequestEvent {
    /// @brief 项目所在的根目录路径
    std::filesystem::path m_projectPath;
};

/// @brief 打开谱面包为临时项目事件。
struct OpenTemporaryProjectPackageEvent : public ProjectRequestEvent {
    /// @brief 需要解压并临时阅览的谱面包文件路径。
    std::filesystem::path m_packagePath;
};

}  // namespace MMM::Event

EVENT_REGISTER_PARENTS(MMM::Event::OpenProjectEvent,
                       MMM::Event::ProjectRequestEvent);
EVENT_REGISTER_PARENTS(MMM::Event::OpenTemporaryProjectPackageEvent,
                       MMM::Event::ProjectRequestEvent);
