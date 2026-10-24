#pragma once

#include "event/core/SubscriptionID.h"
#include "logic/ProjectCommandService.h"
#include "logic/ProjectController.h"
#include "logic/ProjectDirectoryScanner.h"
#include "logic/ProjectResourceService.h"
#include "logic/ProjectStorage.h"
#include <mutex>
#include <type_traits>

namespace MMM::Logic
{
/// @brief 一次构造且地址稳定的低频项目服务、订阅和请求状态。
/// @details 请求字段仍仅在控制器原有 m_pendingMutex
/// 范围内读写，不替换状态对象。
/// 监听器由控制器直接持有，析构体先退订和停止监听，再销毁本状态。
struct ProjectController::PrivateState {
    /// @brief 打开项目请求事件订阅 ID。
    Event::SubscriptionID m_openProjectSubscription{ 0 };

    /// @brief 关闭项目请求事件订阅 ID。
    Event::SubscriptionID m_closeProjectSubscription{ 0 };

    /// @brief 新建项目请求事件订阅 ID。
    Event::SubscriptionID m_createProjectSubscription{ 0 };

    /// @brief 打开临时谱面包请求事件订阅 ID。
    Event::SubscriptionID m_openTemporaryProjectSubscription{ 0 };

    /// @brief 项目切换完成事件订阅 ID。
    Event::SubscriptionID m_projectSwitchCompletedSubscription{ 0 };

    /// @brief 项目切换取消事件订阅 ID。
    Event::SubscriptionID m_projectSwitchCancelledSubscription{ 0 };

    /// @brief 扫描当前项目目录中的谱面和音频文件。
    ProjectDirectoryScanner m_projectDirectoryScanner;

    /// @brief 读写隐藏目录中的项目配置分片并迁移旧单文件。
    ProjectStorage m_projectStorage;

    /// @brief 根据项目目录扫描结果构建和同步项目资源。
    ProjectResourceService m_projectResourceService;

    /// @brief 处理项目资源命令并返回调用方需要执行的副作用请求。
    ProjectCommandService m_projectCommandService;

    /// @brief 已确认可由逻辑线程直接打开的项目路径。
    std::filesystem::path m_pendingProjectPath;
    /// @brief 已确认请求的用户入口。
    Event::ProjectOpenOrigin m_pendingOrigin{
        Event::ProjectOpenOrigin::Unknown
    };
    /// @brief 待消费请求的用户入口。
    Event::ProjectOpenOrigin m_requestedOrigin{
        Event::ProjectOpenOrigin::Unknown
    };
    /// @brief 等待旧画布关闭的请求入口。
    Event::ProjectOpenOrigin m_switchOrigin{
        Event::ProjectOpenOrigin::Unknown
    };

    /// @brief 已确认可由逻辑线程直接打开的项目模式。
    ProjectOpenMode m_pendingProjectOpenMode{ ProjectOpenMode::Normal };

    /// @brief 已确认可由逻辑线程直接创建并打开的项目初始设置。
    std::optional<ProjectCreationOptions> m_pendingProjectCreationOptions;

    /// @brief 等待逻辑线程判定是否需要先关闭旧画布的项目路径。
    std::filesystem::path m_requestedProjectPath;

    /// @brief 等待逻辑线程判定的项目打开模式。
    ProjectOpenMode m_requestedProjectOpenMode{ ProjectOpenMode::Normal };

    /// @brief 等待逻辑线程判定的项目创建初始设置。
    std::optional<ProjectCreationOptions> m_requestedProjectCreationOptions;

    /// @brief 等待 UI 关闭旧谱面画布后的项目打开模式。
    ProjectOpenMode m_pendingProjectSwitchOpenMode{ ProjectOpenMode::Normal };

    /// @brief 等待 UI 关闭旧谱面画布后的项目创建初始设置。
    std::optional<ProjectCreationOptions> m_pendingProjectSwitchCreationOptions;

    /// @brief 是否已有项目关闭请求正在等待逻辑线程分发。
    bool m_requestedProjectClose{ false };

    /// @brief 所有脏谱面关闭提示是否已完成，项目清理是否可以执行。
    bool m_projectCloseReady{ false };
};

// 服务当前不拥有需与监听器协调销毁的资源；后续增加状态须重新审查停止顺序。
static_assert(std::is_trivially_destructible_v<ProjectDirectoryScanner>);
static_assert(std::is_trivially_destructible_v<ProjectStorage>);
static_assert(std::is_trivially_destructible_v<ProjectResourceService>);
static_assert(std::is_trivially_destructible_v<ProjectCommandService>);
}  // namespace MMM::Logic
