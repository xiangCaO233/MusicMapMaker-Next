#pragma once

#include "event/EventDef.h"
#include "event/core/BaseEvent.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace MMM::Event
{

/// @brief 所有项目相关事件的基础事件。
struct ProjectEvent : public BaseEvent {
};

/// @brief 项目请求类事件，表示 UI 或外部系统提交的项目操作意图。
struct ProjectRequestEvent : public ProjectEvent {
};

/// @brief 项目生命周期事件，表示项目打开、关闭或迁移过程中的状态变更。
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

    /// @brief 项目默认调色方案；空字符串表示继承软件默认。
    std::string m_colorPaletteSchemeName;

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

/// @brief 项目打开流程中可展示给 UI 的加载阶段。
enum class ProjectOpenProgressStage : std::uint8_t {
    Validating,              ///< 校验目标路径和项目类型。
    ExtractingPackage,       ///< 解压临时谱面包。
    ClosingCurrentProject,   ///< 保存并关闭当前项目。
    ScanningDirectory,       ///< 扫描项目目录中的谱面和音频。
    BuildingResources,       ///< 根据扫描结果构建项目资源表。
    LoadingConfiguration,    ///< 读取并合并项目配置。
    MigratingConfiguration,  ///< 迁移旧版项目字段和谱面引用。
    SavingConfiguration,     ///< 写入当前分片项目配置。
    PreparingAudio,          ///< 登记项目音效资源。
    LoadingBeatmaps,         ///< 解析并恢复项目谱面会话。
    Finalizing,              ///< 应用工作区并发布项目状态。
};

/// @brief 项目开始切换事件，通知 UI 保留切换中的工作区状态。
struct ProjectOpenStartedEvent : public ProjectLifecycleEvent {
    /// @brief 正在打开的项目目录、谱面文件或谱面包路径。
    std::string m_projectPath;

    /// @brief 当前是否正在打开临时谱面包。
    bool m_isPackage{ false };
};

/// @brief 项目打开阶段进度事件，由逻辑线程投递给 UI 状态栏。
struct ProjectOpenProgressEvent : public ProjectLifecycleEvent {
    /// @brief 当前加载阶段。
    ProjectOpenProgressStage m_stage{ ProjectOpenProgressStage::Validating };

    /// @brief 当前总进度，范围为 0 到 1。
    float m_fraction{ 0.0F };

    /// @brief 当前处理的项目、谱面或资源名称；没有具体对象时为空。
    std::string m_detail;
};

/// @brief 项目关闭完成事件。
struct ProjectClosedEvent : public ProjectLifecycleEvent {
    /// @brief 被关闭项目的标题。
    std::string m_projectTitle;

    /// @brief 被关闭项目的根目录路径。
    std::filesystem::path m_projectPath;
};

/// @brief 项目或谱面包打开失败事件。
struct ProjectOpenFailedEvent : public ProjectLifecycleEvent {
    /// @brief 尝试打开的项目目录、谱面文件或谱面包路径。
    std::string m_projectPath;

    /// @brief 失败原因。
    std::string m_errorMessage;

    /// @brief 是否是打开谱面包为临时项目时失败。
    bool m_isPackage{ false };
};

/// @brief 协作访客在线期间打开本机项目的请求被拦截事件。
struct CollaborationProjectOpenBlockedEvent : public ProjectLifecycleEvent {
};

/// @brief 已离线的协作房间谱面收到编辑命令时的拦截事件。
struct CollaborationOfflineEditBlockedEvent : public ProjectLifecycleEvent {
};

/// @brief 协作访客尝试修改房主未授权的数据类别时的拦截事件。
struct CollaborationPermissionEditBlockedEvent : public ProjectLifecycleEvent {
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

/// @brief 音频资源变更操作类型。
enum class AudioResourceMutationOperation {
    UpdateType,
    Rename,
    Remove,
    MovePath,
};

/// @brief 音频资源变更完成或被引用保护拦截后的结果事件。
struct AudioResourceMutationResultEvent : public ProjectLifecycleEvent {
    /// @brief 本次变更的操作类型。
    AudioResourceMutationOperation m_operation{
        AudioResourceMutationOperation::UpdateType
    };

    /// @brief 操作目标的稳定音频资源 ID。
    std::string m_resourceId;

    /// @brief 操作是否成功完成。
    bool m_success{ false };

    /// @brief 阻止操作的具体谱面路径；成功或非引用错误时为空。
    std::vector<std::string> m_blockingBeatmapPaths;

    /// @brief 失败原因；成功时为空。
    std::string m_errorMessage;
};

/// @brief 当前项目描述文件保存完成事件。
struct ProjectSavedEvent : public ProjectLifecycleEvent {
    /// @brief 保存成功的项目描述文件路径，使用 UTF-8 字符串。
    std::string m_projectFilePath;
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
EVENT_REGISTER_PARENTS(MMM::Event::ProjectOpenStartedEvent,
                       MMM::Event::ProjectLifecycleEvent);
EVENT_REGISTER_PARENTS(MMM::Event::ProjectOpenProgressEvent,
                       MMM::Event::ProjectLifecycleEvent);
EVENT_REGISTER_PARENTS(MMM::Event::ProjectClosedEvent,
                       MMM::Event::ProjectLifecycleEvent);
EVENT_REGISTER_PARENTS(MMM::Event::ProjectOpenFailedEvent,
                       MMM::Event::ProjectLifecycleEvent);
EVENT_REGISTER_PARENTS(MMM::Event::CollaborationProjectOpenBlockedEvent,
                       MMM::Event::ProjectLifecycleEvent);
EVENT_REGISTER_PARENTS(MMM::Event::CollaborationOfflineEditBlockedEvent,
                       MMM::Event::ProjectLifecycleEvent);
EVENT_REGISTER_PARENTS(MMM::Event::CollaborationPermissionEditBlockedEvent,
                       MMM::Event::ProjectLifecycleEvent);
EVENT_REGISTER_PARENTS(MMM::Event::TemporaryProjectEditBlockedEvent,
                       MMM::Event::ProjectLifecycleEvent);
EVENT_REGISTER_PARENTS(MMM::Event::TemporaryProjectSaveResultEvent,
                       MMM::Event::ProjectLifecycleEvent);
EVENT_REGISTER_PARENTS(MMM::Event::AudioResourceMutationResultEvent,
                       MMM::Event::ProjectLifecycleEvent);
EVENT_REGISTER_PARENTS(MMM::Event::ProjectSavedEvent,
                       MMM::Event::ProjectLifecycleEvent);
