#pragma once
#include "event/project/ProjectOpenOrigin.h"

#include "logic/ProjectCommandResults.h"
#include "logic/ProjectDirectoryWatcher.h"
#include "logic/ProjectResourceTypes.h"
#include "logic/ProjectTypes.h"
#include "logic/ProjectUiState.h"
#include "logic/PublishedSharedPtr.h"
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
struct CmdCreateBeatmap;
struct CmdImportAudio;
struct CmdRemoveAudioResource;
struct CmdRemoveBeatmap;
struct CmdUpdateAudioResource;


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

    /// @brief 保留原嵌套类型名，兼容现有项目控制器调用方。
    using OpenProjectResult = MMM::Logic::OpenProjectResult;

    /// @brief 关闭项目后的结果信息。
    struct CloseProjectResult {
        /// @brief 是否实际关闭了项目。
        bool m_closed{ false };

        /// @brief 被关闭项目的标题。
        std::string m_projectTitle;

        /// @brief 被关闭项目实例，用于调用方执行音频卸载等副作用。
        std::unique_ptr<Project> m_project;
    };

    /// @brief 保留原嵌套类型名，兼容现有项目控制器调用方。
    using ProjectCreationOptions = MMM::Logic::ProjectCreationOptions;

    /// @brief 项目打开请求的来源模式。
    enum class ProjectOpenMode {
        Normal,           ///< 直接打开普通项目目录或谱面文件。
        TemporaryPackage  ///< 解压谱面包并作为临时只读项目打开。
    };

    /// @brief 保留原嵌套类型名，兼容现有项目控制器调用方。
    using TemporaryProjectInfo = MMM::Logic::TemporaryProjectInfo;

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
        /// @brief 随项目切换传递的用户入口。
        Event::ProjectOpenOrigin m_origin{ Event::ProjectOpenOrigin::Unknown };
    };

    /// @brief 获取当前项目。
    /// @return 当前项目指针；未打开项目时返回 nullptr。
    Project* currentProject();

    /// @brief 获取当前项目。
    /// @return 当前项目只读指针；未打开项目时返回 nullptr。
    const Project* currentProject() const;

    /// @brief 请求打开项目，必要时等待 UI 完成旧画布关闭。
    /// @param projectPath 要打开的项目目录或谱面文件路径。
    void requestOpenProject(
        const std::filesystem::path& projectPath,
        Event::ProjectOpenOrigin origin = Event::ProjectOpenOrigin::Unknown);

    /// @brief 请求打开谱面包为临时只读项目。
    /// @param packagePath 要解压阅览的谱面包路径。
    void requestOpenTemporaryProjectPackage(
        const std::filesystem::path& packagePath,
        Event::ProjectOpenOrigin origin = Event::ProjectOpenOrigin::Unknown);

    /// @brief 请求创建并打开项目，必要时等待 UI 完成旧画布关闭。
    /// @param projectPath 要创建的项目根目录。
    /// @param options 新项目初始设置。
    /// @param origin 唤出新建向导的用户入口。
    void requestCreateProject(
        const std::filesystem::path&  projectPath,
        const ProjectCreationOptions& options,
        Event::ProjectOpenOrigin origin = Event::ProjectOpenOrigin::Unknown);

    /// @brief 请求关闭当前项目，必要时等待 UI 完成旧画布关闭。
    void requestCloseProject();

    /// @brief 设置协作访客在线期间的本机项目打开门闩。
    /// @param blocked 为 true
    /// 时取消挂起打开请求并拒绝新的打开、创建和谱面包请求。
    /// @warning UI/逻辑跨线程低频调用；状态变化时会取得项目请求锁并清空队列。
    void setLocalProjectOpeningBlockedByCollaboration(bool blocked);

    /// @brief 查询本机项目是否因访客协作连接而禁止打开。
    /// @return 项目打开入口被锁定时返回 true。
    [[nodiscard]] bool isLocalProjectOpeningBlockedByCollaboration() const;

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

    /// @brief 将当前临时项目复制保存到正式目录，并原地转为正式项目。
    /// @param destinationPath 用户选择的保存目录。
    /// @return 保存结果。
    SaveTemporaryProjectResult saveTemporaryProjectTo(
        const std::filesystem::path& destinationPath);


    /// @brief 关闭当前项目并停止项目目录监听。
    /// @return 被关闭项目的信息。
    CloseProjectResult closeProject();

    /// @brief 停止项目目录监听。
    void stopDirectoryWatcher();

    /// @brief 消费项目目录监听器捕获到的变更标记。
    /// @return 有待处理目录变更时返回 true。
    bool consumeDirectoryChangePending();

    /// @brief 已消费的目录通知代次，逻辑线程用它使路径缓存失效。
    [[nodiscard]] std::uint64_t directoryRevision() const;

    /// @brief 文件线程扫描完成后要在逻辑线程提交的结果。
    struct DirectoryScanUpdate {
        /// @brief 已应用的资源增删和运行时音频更新。
        DirectorySyncResult m_sync;
        /// @brief 同一工程在扫描期间已编辑，重新取值再扫。
        bool m_retry{};
    };

    /// @brief 将独立工程资源快照交给文件池。
    /// @return 已提交或无需扫描时为真，已有任务时保留最新待处理通知。
    /// @pre 调用方串行调用目录刷新入口。
    bool startDirectoryScan();

    /// @brief 仅查询任务是否完成，不等待磁盘或进入会话锁。
    [[nodiscard]] bool isDirectoryScanReady() const;

    /// @brief 提交就绪结果，仅替换资源表，丢弃已切换或已编辑工程上的旧结果。
    /// @pre 调用方持有会话锁，成功时可在释放该锁前同步运行时状态。
    DirectoryScanUpdate applyDirectoryScan();

    /// @brief 冻结目录刷新产生的项目配置保存，文件线程执行分片写入。
    /// @pre 调用方持有会话锁，并已捕获本次修改对应的工作区。
    void queueDirectorySave();

    /// @brief 仅消费已就绪的写入结果并提交最新待写请求。
    /// @warning 逻辑循环调用；不等待文件线程和会话锁，普通帧仅检查任务槽。
    void pollDirectorySave();

    /// @brief 保存当前项目配置。
    /// @return 保存成功时返回 true。
    bool saveProject();

    /// @brief 创建谱面文件并登记到当前项目。
    /// @param cmd 新建谱面命令。
    /// @return 新建谱面的处理结果。
    CreateBeatmapResult createBeatmap(const CmdCreateBeatmap& cmd);

    /// @brief 导入音频文件并登记到当前项目。
    /// @param cmd 导入音频命令。
    /// @return 导入音频的处理结果。
    ImportAudioResult importAudio(const CmdImportAudio& cmd);

    /// @brief 将单个谱面文件同步到当前项目谱面列表。
    /// @param mapPath 需要同步的谱面文件路径。
    /// @return 当前项目是否发生变化。
    ProjectMutationResult syncProjectWithFile(
        const std::filesystem::path& mapPath);

    /// @brief 更新当前项目内谱面条目的文件路径关联。
    /// @param oldPath 旧谱面路径。
    /// @param newPath 新谱面路径。
    /// @return 当前项目是否发生变化。
    ProjectMutationResult updateBeatmapFilePath(
        const std::filesystem::path& oldPath,
        const std::filesystem::path& newPath);

    /// @brief 更新当前项目的音频资源类型。
    /// @param cmd 更新音频资源命令。
    /// @param openBeatmapReferences 已同步的打开会话内存谱面引用。
    /// @return 更新音频资源的处理结果。
    UpdateAudioResourceResult updateAudioResource(
        const CmdUpdateAudioResource&             cmd,
        const std::vector<BeatmapAudioReference>& openBeatmapReferences = {});

    /// @brief 从当前项目中删除音频资源。
    /// @param cmd 删除音频资源命令。
    /// @param openBeatmapReferences 已同步的打开会话内存谱面引用。
    /// @return 删除音频资源的处理结果。
    RemoveAudioResourceResult removeAudioResource(
        const CmdRemoveAudioResource&             cmd,
        const std::vector<BeatmapAudioReference>& openBeatmapReferences = {});

    /// @brief 读取用于 UI 的不可变活动项目视图。
    /// @warning 界面可见时每帧读取；共享句柄保证并发关闭后仍能绘制旧页。
    /// 兼容路径只锁住句柄复制，不等待会话锁或访问可变 Project。
    [[nodiscard]] std::shared_ptr<const ProjectUiState> uiSnapshot() const;

    /// @brief 合并同一工程的设置改动，供逻辑线程处理。
    /// @note 合并以字段为单位；新工程的改动会代替尚未处理的旧工程请求。
    void queueUiSettingsEdit(ProjectUiSettingsEdit edit);

    /// @brief 将已排队的设置变更应用于仍然有效的工程。
    /// @return 已修改配置的工程。调用方必须在会话锁内同步运行策略和保存。
    [[nodiscard]] const Project* applyPendingUiSettingsEdit();

    /// @brief 逻辑循环检查是否有设置写入待处理。
    /// @warning 每轮 relaxed 读取脏位；UI 写入，真正数据由设置请求锁同步。
    [[nodiscard]] bool hasPendingUiSettingsEdit() const;

    /// @brief 从当前项目谱面列表中删除谱面。
    /// @param cmd 删除谱面命令。
    /// @return 当前项目是否发生变化。
    ProjectMutationResult removeBeatmap(const CmdRemoveBeatmap& cmd);

private:
    /// @brief 构造项目控制器并订阅项目请求事件。
    ProjectController();

    /// @brief 发布项目切换需要关闭旧画布的事件。
    /// @param projectPathToOpen 旧画布关闭后需要打开的项目路径。
    /// @param closeOnly 是否只关闭当前项目而不打开新项目。
    void publishProjectSwitchNeedsCanvasClose(
        const std::filesystem::path& projectPathToOpen, bool closeOnly) const;

    /// @brief 在工程接管、保存或撤回后发布设置和向导读取模型。
    /// @pre 调用方与项目字段修改及所有权切换串行访问。
    void publishUiSnapshot();

    /// @brief 当前工程身份，由会话锁保护；切换和正式目录接管时递增。
    std::uint64_t m_projectGeneration{};
    /// @brief 仅逻辑线程访问；同一批目录通知可共享一个代次。
    std::uint64_t m_directoryRevision{};
    /// @brief 工程所有者发布不可变快照，供 UI 和其他读者获取共享句柄。
    /// @warning 原子实现采用 release/acquire；兼容实现只同步句柄交换与复制。
    /// 读者持有旧值可跨越工程关闭，快照构造与回收均不占用发布槽的锁。
    PublishedSharedPtr<const ProjectUiState> m_uiSnapshot;
    /// @brief 短锁，仅保护待处理设置，不覆盖业务逻辑与磁盘保存。
    std::mutex m_uiSettingsMutex;
    /// @brief 同一代次未提交的配置字段。
    std::optional<ProjectUiSettingsEdit> m_pendingUiSettingsEdit;
    /// @brief UI 发布请求、逻辑线程每轮查看；槽本体由短锁保护。
    /// @warning relaxed 只作为空闲提示。获取互斥锁之后才访问待处理配置。
    std::atomic<bool> m_hasPendingUiSettingsEdit{ false };

    /// @brief 当前项目直接持有，热路径查询不经过私有状态间接访问。
    std::unique_ptr<Project> m_currentProject;
    /// @brief 监听器直接持有，逐次消费变更标记保持原访问路径。
    ProjectDirectoryWatcher m_projectDirectoryWatcher;
    /// @brief 隔离低频服务和受锁保护的请求字段。
    struct PrivateState;
    /// @brief 构造时分配一次；退出时在停止监听后释放，不按请求重建。
    std::unique_ptr<PrivateState> m_state;

    /// @brief 等待 UI 逐个关闭旧谱面画布后再处理的项目路径。
    std::filesystem::path m_pendingProjectSwitchPath;

    /// @brief UI 是否正在项目关闭前逐个关闭旧谱面画布。
    bool m_pendingProjectClose{ false };

    /// @brief 保护项目打开和关闭请求状态的轻量级锁。
    mutable std::mutex m_pendingMutex;
    /// @brief 是否存在需要逻辑线程消费的项目切换动作。
    /// @warning 逻辑热路径原子：loop 每 update 读取；UI/事件线程在请求项目
    /// 打开、创建、关闭或完成旧画布关闭后写入。仅作为进入 m_pendingMutex
    /// 的脏位门闩，使用 acquire/release。
    std::atomic<bool> m_hasPendingProjectAction{ false };

    /// @brief 访客加入协作房间后统一阻止本机项目打开的门闩。
    /// @warning 项目请求入口跨线程读取；仅协作生命周期变化时写入，使用
    /// acquire/release 保证请求队列清理对后续请求可见。
    std::atomic_bool m_localProjectOpeningBlockedByCollaboration{ false };
};

}  // namespace MMM::Logic
