#pragma once

#include "common/LogicCommands.h"
#include "event/core/EventBus.h"
#include "logic/ProjectCommandService.h"
#include "logic/ProjectDirectoryScanner.h"
#include "logic/ProjectDirectoryWatcher.h"
#include "logic/ProjectResourceService.h"
#include "mmm/project/Project.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace MMM::Logic
{

/// @brief 统一管理项目实例、项目切换状态、目录监听和项目资源命令。
class ProjectController
{
public:
    /// @brief 获取项目控制器全局实例。
    /// @return 项目控制器全局实例引用。
    static ProjectController& instance();

    /// @brief 析构项目控制器并取消项目事件订阅。
    ~ProjectController();

    /// @brief 禁止移动构造，项目控制器必须保持单例地址稳定。
    ProjectController(ProjectController&&) = delete;

    /// @brief 禁止拷贝构造，避免复制项目实例和事件订阅状态。
    ProjectController(const ProjectController&) = delete;

    /// @brief 禁止移动赋值，项目控制器必须保持单例地址稳定。
    ProjectController& operator=(ProjectController&&) = delete;

    /// @brief 禁止拷贝赋值，避免复制项目实例和事件订阅状态。
    ProjectController& operator=(const ProjectController&) = delete;

    /// @brief 打开项目后的结果信息。
    struct OpenProjectResult {
        /// @brief 是否成功打开项目。
        bool m_opened{ false };

        /// @brief 实际打开的项目目录路径。
        std::filesystem::path m_actualProjectPath;

        /// @brief 打开项目时若传入谱面文件，则记录需要自动打开的谱面路径。
        std::filesystem::path m_targetBeatmapPath;

        /// @brief 项目显示标题。
        std::string m_projectTitle;

        /// @brief 项目内谱面数量。
        std::size_t m_beatmapCount{ 0 };

        /// @brief 打开项目后需要由音频引擎预加载的音效资源。
        std::vector<ProjectCommandService::AudioPreloadRequest>
            m_effectPreloads;
    };

    /// @brief 关闭项目后的结果信息。
    struct CloseProjectResult {
        /// @brief 是否实际关闭了项目。
        bool m_closed{ false };

        /// @brief 被关闭项目的标题。
        std::string m_projectTitle;

        /// @brief 被关闭项目实例，用于调用方执行音频卸载等副作用。
        std::unique_ptr<Project> m_project;
    };

    /// @brief 新建项目时需要写入项目描述文件的初始设置。
    struct ProjectCreationOptions {
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

    /// @brief 项目打开请求的来源模式。
    enum class ProjectOpenMode {
        Normal,           ///< 直接打开普通项目目录或谱面文件。
        TemporaryPackage  ///< 解压谱面包并作为临时只读项目打开。
    };

    /// @brief 当前临时项目的运行时信息。
    struct TemporaryProjectInfo {
        /// @brief 是否存在临时项目。
        bool m_isTemporary{ false };

        /// @brief 用户拖拽打开的原始谱面包路径。
        std::filesystem::path m_sourcePackagePath;

        /// @brief 当前临时项目缓存目录。
        std::filesystem::path m_cacheProjectPath;
    };

    /// @brief 临时项目保存为正式项目的结果。
    struct SaveTemporaryProjectResult {
        /// @brief 是否保存成功。
        bool m_success{ false };

        /// @brief 保存成功后的正式项目目录。
        std::filesystem::path m_savedProjectPath;

        /// @brief 失败时的错误信息。
        std::string m_errorMessage;
    };

    /// @brief 谱面包解压成临时项目目录的准备结果。
    struct PreparedTemporaryProjectResult {
        /// @brief 是否准备成功。
        bool m_success{ false };

        /// @brief 准备成功后的临时项目信息。
        TemporaryProjectInfo m_temporaryInfo;

        /// @brief 失败时的错误信息。
        std::string m_errorMessage;
    };

    /// @brief 逻辑线程本轮应执行的项目切换动作。
    struct PendingProjectAction {
        /// @brief 本轮是否需要关闭当前项目。
        bool m_closeProject{ false };

        /// @brief 本轮是否需要打开指定项目。
        std::filesystem::path m_projectPathToOpen;

        /// @brief 打开项目时需要应用的新建项目初始设置。
        std::optional<ProjectCreationOptions> m_projectCreationOptions;

        /// @brief 本次打开项目的来源模式。
        ProjectOpenMode m_projectOpenMode{ ProjectOpenMode::Normal };
    };

    /// @brief 获取当前项目。
    /// @return 当前项目指针；未打开项目时返回 nullptr。
    Project* currentProject();

    /// @brief 获取当前项目。
    /// @return 当前项目只读指针；未打开项目时返回 nullptr。
    const Project* currentProject() const;

    /// @brief 请求打开项目，必要时等待 UI 完成旧画布关闭。
    /// @param projectPath 要打开的项目目录或谱面文件路径。
    void requestOpenProject(const std::filesystem::path& projectPath);

    /// @brief 请求打开谱面包为临时只读项目。
    /// @param packagePath 要解压阅览的谱面包路径。
    void requestOpenTemporaryProjectPackage(
        const std::filesystem::path& packagePath);

    /// @brief 请求创建并打开项目，必要时等待 UI 完成旧画布关闭。
    /// @param projectPath 要创建的项目根目录。
    /// @param options 新项目初始设置。
    void requestCreateProject(const std::filesystem::path&  projectPath,
                              const ProjectCreationOptions& options);

    /// @brief 请求关闭当前项目，必要时等待 UI 完成旧画布关闭。
    void requestCloseProject();

    /// @brief 是否存在等待旧谱面画布关闭后的项目打开或关闭流程。
    /// @return 有挂起项目切换流程时返回 true。
    bool hasPendingProjectSwitch() const;

    /// @brief 完成 UI 侧逐个关闭旧谱面画布后的项目切换流程。
    void completePendingProjectSwitch();

    /// @brief 取消所有挂起项目切换流程。
    void cancelPendingProjectSwitch();

    /// @brief 消费逻辑线程本轮需要处理的项目切换动作。
    /// @param needsCanvasClose 当前是否需要先关闭旧谱面画布。
    /// @return 本轮需要执行的关闭或打开动作。
    PendingProjectAction consumePendingProjectAction(bool needsCanvasClose);

    /// @brief 判断是否存在待逻辑线程消费的项目切换动作。
    /// @return 有待处理动作时返回 true。
    /// @warning 逻辑热路径原子：loop 每 update 读取；只作为是否需要进入
    /// consumePendingProjectAction 和计算旧画布关闭条件的门闩。
    bool hasPendingProjectAction() const;

    /// @brief 打开项目并启动项目目录监听。
    /// @param projectPath 要打开的项目目录或谱面文件路径。
    /// @return 打开项目后的结果信息。
    OpenProjectResult openProject(
        const std::filesystem::path&                 projectPath,
        const std::optional<ProjectCreationOptions>& creationOptions =
            std::nullopt,
        const std::optional<TemporaryProjectInfo>& temporaryInfo =
            std::nullopt);

    /// @brief 解压谱面包并作为临时只读项目打开。
    /// @param packagePath 要打开的谱面包路径。
    /// @return 打开项目后的结果信息。
    OpenProjectResult openTemporaryProjectPackage(
        const std::filesystem::path& packagePath);

    /// @brief 仅解压谱面包并准备临时项目目录，不切换当前项目。
    /// @param packagePath 要准备的谱面包路径。
    /// @return 临时项目准备结果。
    PreparedTemporaryProjectResult prepareTemporaryProjectPackage(
        const std::filesystem::path& packagePath) const;

    /// @brief 当前是否打开了临时项目。
    /// @return 当前项目是临时项目时返回 true。
    bool isCurrentProjectTemporary() const;

    /// @brief 获取当前临时项目信息。
    /// @return 当前临时项目源文件与缓存路径；非临时项目时返回默认值。
    TemporaryProjectInfo currentTemporaryProjectInfo() const;

    /// @brief 将当前临时项目复制保存到正式目录。
    /// @param destinationPath 用户选择的保存目录。
    /// @return 保存结果。
    SaveTemporaryProjectResult saveTemporaryProjectTo(
        const std::filesystem::path& destinationPath) const;


    /// @brief 关闭当前项目并停止项目目录监听。
    /// @return 被关闭项目的信息。
    CloseProjectResult closeProject();

    /// @brief 停止项目目录监听。
    void stopDirectoryWatcher();

    /// @brief 消费项目目录监听器捕获到的变更标记。
    /// @return 有待处理目录变更时返回 true。
    bool consumeDirectoryChangePending();

    /// @brief 扫描当前项目目录并同步项目资源列表。
    /// @return 目录同步结果，包含是否改变项目和需要预加载的音效。
    ProjectResourceService::DirectorySyncResult scanProjectDirectory();

    /// @brief 保存当前项目配置。
    /// @return 保存成功时返回 true。
    bool saveProject();

    /// @brief 创建谱面文件并登记到当前项目。
    /// @param cmd 新建谱面命令。
    /// @return 新建谱面的处理结果。
    ProjectCommandService::CreateBeatmapResult createBeatmap(
        const CmdCreateBeatmap& cmd);

    /// @brief 导入音频文件并登记到当前项目。
    /// @param cmd 导入音频命令。
    /// @return 导入音频的处理结果。
    ProjectCommandService::ImportAudioResult importAudio(
        const CmdImportAudio& cmd);

    /// @brief 将单个谱面文件同步到当前项目谱面列表。
    /// @param mapPath 需要同步的谱面文件路径。
    /// @return 当前项目是否发生变化。
    ProjectCommandService::ProjectMutationResult syncProjectWithFile(
        const std::filesystem::path& mapPath);

    /// @brief 更新当前项目内谱面条目的文件路径关联。
    /// @param oldPath 旧谱面路径。
    /// @param newPath 新谱面路径。
    /// @return 当前项目是否发生变化。
    ProjectCommandService::ProjectMutationResult updateBeatmapFilePath(
        const std::filesystem::path& oldPath,
        const std::filesystem::path& newPath);

    /// @brief 更新当前项目的音频资源类型。
    /// @param cmd 更新音频资源命令。
    /// @return 更新音频资源的处理结果。
    ProjectCommandService::UpdateAudioResourceResult updateAudioResource(
        const CmdUpdateAudioResource& cmd);

    /// @brief 从当前项目中删除音频资源。
    /// @param cmd 删除音频资源命令。
    /// @return 删除音频资源的处理结果。
    ProjectCommandService::RemoveAudioResourceResult removeAudioResource(
        const CmdRemoveAudioResource& cmd);

    /// @brief 从当前项目谱面列表中删除谱面。
    /// @param cmd 删除谱面命令。
    /// @return 当前项目是否发生变化。
    ProjectCommandService::ProjectMutationResult removeBeatmap(
        const CmdRemoveBeatmap& cmd);

private:
    /// @brief 构造项目控制器并订阅项目请求事件。
    ProjectController();

    /// @brief 发布项目切换需要关闭旧画布的事件。
    /// @param projectPathToOpen 旧画布关闭后需要打开的项目路径。
    /// @param closeOnly 是否只关闭当前项目而不打开新项目。
    void publishProjectSwitchNeedsCanvasClose(
        const std::filesystem::path& projectPathToOpen, bool closeOnly) const;

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

    /// @brief 当前打开的项目。
    std::unique_ptr<Project> m_currentProject;

    /// @brief 扫描当前项目目录中的谱面和音频文件。
    ProjectDirectoryScanner m_projectDirectoryScanner;

    /// @brief 监听当前项目目录中的文件系统变更。
    ProjectDirectoryWatcher m_projectDirectoryWatcher;

    /// @brief 根据项目目录扫描结果构建和同步项目资源。
    ProjectResourceService m_projectResourceService;

    /// @brief 处理项目资源命令并返回调用方需要执行的副作用请求。
    ProjectCommandService m_projectCommandService;

    /// @brief 已确认可由逻辑线程直接打开的项目路径。
    std::filesystem::path m_pendingProjectPath;

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

    /// @brief 等待 UI 逐个关闭旧谱面画布后再处理的项目路径。
    std::filesystem::path m_pendingProjectSwitchPath;

    /// @brief 等待 UI 关闭旧谱面画布后的项目打开模式。
    ProjectOpenMode m_pendingProjectSwitchOpenMode{ ProjectOpenMode::Normal };

    /// @brief 等待 UI 关闭旧谱面画布后的项目创建初始设置。
    std::optional<ProjectCreationOptions> m_pendingProjectSwitchCreationOptions;

    /// @brief 是否已有项目关闭请求正在等待逻辑线程分发。
    bool m_requestedProjectClose{ false };

    /// @brief UI 是否正在项目关闭前逐个关闭旧谱面画布。
    bool m_pendingProjectClose{ false };

    /// @brief 所有脏谱面关闭提示是否已完成，项目清理是否可以执行。
    bool m_projectCloseReady{ false };

    /// @brief 保护项目打开和关闭请求状态的轻量级锁。
    mutable std::mutex m_pendingMutex;

    /// @brief 是否存在需要逻辑线程消费的项目切换动作。
    /// @warning 逻辑热路径原子：loop 每 update 读取；UI/事件线程在请求项目
    /// 打开、创建、关闭或完成旧画布关闭后写入。仅作为进入 m_pendingMutex
    /// 的脏位门闩，使用 acquire/release。
    std::atomic<bool> m_hasPendingProjectAction{ false };
};

}  // namespace MMM::Logic
