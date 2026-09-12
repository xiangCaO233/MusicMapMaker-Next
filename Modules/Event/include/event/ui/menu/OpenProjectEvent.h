#pragma once
#include "event/project/ProjectEvents.h"
#include "event/project/ProjectOpenInteractionEvent.h"
#include <filesystem>

namespace MMM::Event
{

/// @brief 指示逻辑层加载指定目录下的项目资源。
/// @note 路径由逻辑层校验，事件构造本身不访问文件系统。
struct OpenProjectEvent : public ProjectRequestEvent {
    /// @brief 项目所在的根目录路径
    std::filesystem::path m_projectPath;
    /// @brief 用户入口，传递至实际打开完成结果。
    ProjectOpenOrigin m_origin{ ProjectOpenOrigin::Unknown };
};

/// @brief 打开谱面包为临时项目事件。
/// @note 临时项目的解压目录和清理生命周期由项目服务管理。
struct OpenTemporaryProjectPackageEvent : public ProjectRequestEvent {
    /// @brief 需要解压并临时阅览的谱面包文件路径。
    std::filesystem::path m_packagePath;
    /// @brief 用户入口，传递至实际打开完成结果。
    ProjectOpenOrigin m_origin{ ProjectOpenOrigin::Unknown };
};

}  // namespace MMM::Event

// 两类打开请求均纳入项目请求分发链，便于统一校验当前项目状态。
EVENT_REGISTER_PARENTS(MMM::Event::OpenProjectEvent,
                       MMM::Event::ProjectRequestEvent);
EVENT_REGISTER_PARENTS(MMM::Event::OpenTemporaryProjectPackageEvent,
                       MMM::Event::ProjectRequestEvent);
