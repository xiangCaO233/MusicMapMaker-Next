#pragma once

#include "IUIView.h"
#include "event/core/EventBus.h"
#include "event/ui/UpdateDragAreaEvent.h"
#include "graphic/imguivk/IGraphicUserHook.h"
#include "mmm/project/Project.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/layout/CLayWrapperCore.h"
#include "ui/project/ProjectUiLifecycleState.h"
#include <atomic>
#include <concurrentqueue.h>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMM::Graphic
{
class IWindowFrameAdapter;
class NativeWindow;
}  // namespace MMM::Graphic

namespace MMM::Event
{
enum class SettingsTab;
}  // namespace MMM::Event

namespace MMM::UI
{
class IRenderableView;
class ITextureLoader;
class IParallelUiPreparable;

class UIManager : public MMM::Graphic::IGraphicUserHook
{
public:
    /// @brief 初始化 UI 管理器并订阅项目生命周期事件。
    UIManager();
    UIManager(UIManager&&)                 = delete;
    UIManager(const UIManager&)            = delete;
    UIManager& operator=(UIManager&&)      = delete;
    UIManager& operator=(const UIManager&) = delete;

    /// @brief 取消项目生命周期订阅并销毁 UI 管理器。
    ~UIManager();

    /// @brief 注册视图，转交所有权
    void registerView(const std::string& name, std::unique_ptr<IUIView> view);

    /// @brief 注销并销毁视图
    void unregisterView(const std::string& name);

    /// @brief 清理所有ui
    void clearAllViews();

    /// @brief 绑定主原生窗口，用于项目工作区保存和恢复窗口位置。
    /// @param window 主原生窗口指针，生命周期由 GameLoop 持有。
    void setNativeWindow(Graphic::NativeWindow* window);

    /// @brief 获取主原生窗口观察指针。
    /// @return 主原生窗口指针；未绑定时返回 nullptr。
    /// @warning UI 热路径：每帧可能读取；只返回观察指针，不复制所有权。
    [[nodiscard]] Graphic::NativeWindow* getNativeWindow() const;

    /// @brief 获取无原生装饰窗口的平台行为适配器。
    /// @return 平台适配器观察指针；未绑定或当前平台无适配器时返回 nullptr。
    /// @warning UI 热路径：每帧可能读取；只返回观察指针，不复制所有权。
    [[nodiscard]] Graphic::IWindowFrameAdapter* getWindowFrameAdapter() const;

    /// @brief 判断项目是否正在切换，新项目加载完成前保持为 true。
    /// @return 项目切换流程正在执行时返回 true。
    /// @warning UI 热路径：逻辑线程生命周期回调负责写入，UI 和渲染线程
    /// 负责读取；为阻止同帧访问已关闭项目，必须执行一次 acquire 原子读取。
    [[nodiscard]] bool isProjectTransitionInProgress() const;

    /// @brief 判断 UI 是否持有已加载项目的生命周期快照。
    /// @return 已加载项目仍有效时返回 true。
    /// @warning UI 热路径：只读取 UI 线程维护的本地状态。
    [[nodiscard]] bool hasActiveProjectUiState() const;

    /// @brief 判断时间线窗口是否正在拖动 Timing 框选区域。
    /// @return 时间线正在框选时返回 true。
    /// @warning UI 热路径：空格快捷键按下时调用；只读取已注册视图的本地状态。
    [[nodiscard]] bool isTimelineTimingMarqueeSelecting();

    /// @brief 判断时间线窗口是否正在通过抓取工具拖动 Timing。
    /// @return 时间线正在拖动 Timing 时返回 true。
    /// @warning UI 热路径：空格快捷键按下时调用；只读取已注册视图的本地状态。
    [[nodiscard]] bool isTimelineTimingDragging();

    /// @brief 获取 UI 生命周期快照中的当前项目根目录。
    /// @return 当前项目根目录；无项目时为空路径。
    /// @warning UI 热路径：返回 UI 线程本地路径引用，不访问逻辑线程项目。
    [[nodiscard]] const std::filesystem::path& getActiveProjectRoot() const;

    /// @brief 更新主窗口标题栏基础原生拖拽区域。
    /// @param areas 当前标题栏允许原生窗口拖拽的矩形区域。
    /// @warning UI 热路径：每帧由主菜单栏更新，只复制少量矩形。
    void setNativeWindowDragAreas(std::vector<Event::DragArea> areas);

    /// @brief 捕获当前项目工作区 UI 状态到内存中的项目配置。
    void captureProjectWorkspaceState();

    /// @brief 打开独立设置窗口，切换到指定标签页并请求聚焦。
    /// @param tab 需要激活的设置标签页。
    void openSettingsWindow(MMM::Event::SettingsTab tab);

    /// @brief 请求下一次资源准备阶段重载皮肤相关图形资源。
    /// @warning 低频资源重载路径：皮肤热切换后调用，只置脏位；实际 Vulkan
    /// 资源释放和重建在 onPrepareResources 中执行。
    void requestSkinResourceReload();

    /// @brief 打开音轨控制器并默认停靠到谱面画布标签组。
    /// @param trackId 音轨标识符。
    /// @param trackName 音轨显示名称。
    /// @param type 音轨类型。
    void openAudioTrackController(const std::string&                trackId,
                                  const std::string&                trackName,
                                  AudioTrackControllerUI::TrackType type);

    /// @brief 泛型获取裸指针 外部不负责销毁
    template<typename T> T* getView(const std::string& name)
    {
        auto it = m_uiviews.find(name);
        if ( it != m_uiviews.end() ) {
            IUIView* raw = it->second.get();
            if constexpr ( std::is_same_v<T, IUIView> ) {
                return raw;
            } else {
                // 核心修复：由于项目禁用了 RTTI 且使用了虚继承 (virtual public
                // IUIView)， 编译器禁止从 IUIView* 直接 static_cast 到派生类。
                // 我们通过 getActualInstance() 虚函数获取校正后的指针，
                // 然后通过 void* 桥接进行 static_cast。
                return static_cast<T*>(raw->getActualInstance());
            }
        }
        return nullptr;
    }


    /// @brief 分派所有imgui事件
    void DispatchGlobalUIEvents();

    /// @brief 准备资源
    /// @warning 热路径：渲染循环每帧在 ImGui NewFrame
    /// 前执行；只允许检查脏位，重建/重载必须由低频标志触发。
    void onPrepareResources(vk::PhysicalDevice&   physicalDevice,
                            vk::Device&           logicalDevice,
                            Graphic::VKSwapchain& swapchain,
                            vk::CommandPool&      cmdPool,
                            vk::Queue&            queue) override;

    /// @brief 更新ui
    /// @warning 热路径：渲染循环每帧执行；禁止文件系统访问、完整 ECS
    /// 遍历、完整排序和共享指针所有权复制。
    void onUpdateUI() override;

    /// @brief 录制所有离屏渲染指令
    /// @warning 热路径：每帧命令录制阶段执行；只遍历已注册的可渲染视图序列。
    void onRecordOffscreen(vk::CommandBuffer& cmd,
                           uint32_t           frameIndex) override;

    /// @brief 获取当前帧可并行录制的离屏视图数量。
    /// @return 当前可渲染视图序列的数量。
    /// @warning 渲染热路径：每帧命令录制前调用，只读取稳定序列长度。
    uint32_t getOffscreenRecordTaskCount() const override;

    /// @brief 录制指定可渲染视图的离屏命令。
    /// @param cmd 当前任务独占的命令缓冲。
    /// @param frameIndex 当前并发帧索引。
    /// @param taskIndex 可渲染视图序列索引。
    /// @warning 渲染热路径：可能在渲染线程池中执行，只能读取 UIManager
    /// 的稳定视图表并录制对应视图。
    void onRecordOffscreenTask(vk::CommandBuffer& cmd, uint32_t frameIndex,
                               uint32_t taskIndex) override;

private:
    /// @brief 跨线程投递到 UI 的项目生命周期快照。
    struct ProjectUiLifecycleUpdate {
        /// @brief 本次更新类型。
        ProjectUiLifecycleKind kind{ ProjectUiLifecycleKind::Closed };

        /// @brief 事件对应的项目路径。
        std::filesystem::path projectRoot;

        /// @brief 加载完成时复制的项目工作区。
        ProjectWorkspaceState workspace;

        /// @brief 加载完成时复制的项目音频资源，用于恢复音轨控制器。
        std::vector<AudioResource> audioResources;

        /// @brief 是否包含可用于恢复的项目快照。
        bool hasProjectSnapshot{ false };
    };

    /// @brief 无项目时应用一次默认侧边栏工作区。
    void applyNoProjectDefaultWorkspace();

    /// @brief 消费逻辑线程投递的项目生命周期更新。
    /// @warning UI 热路径：每帧只清空低频生命周期队列并更新本地状态。
    void consumePendingProjectLifecycleUpdates();

    /// @brief 同步当前项目的 ImGui 布局保存和恢复状态。
    /// @warning UI 热路径低频分支：每帧只比较当前项目路径；实际保存 ImGui
    /// ini 和窗口状态按时间间隔触发，禁止在这里写文件。
    void syncProjectWorkspaceState();

    /// @brief 捕获当前项目打开的动态工作区视图。
    /// @param workspace 需要写入的项目工作区状态。
    void captureProjectWorkspaceViews(ProjectWorkspaceState& workspace);

    /// @brief 恢复项目工作区中保存的动态视图。
    /// @param workspace 项目工作区状态。
    void restoreProjectWorkspaceViews(
        const ProjectWorkspaceState&      workspace,
        const std::vector<AudioResource>& audioResources);

    /// @brief 关闭上一个项目遗留的动态工作区视图。
    void clearProjectWorkspaceViews();

    /// @brief 为新打开的音轨控制器选择默认 Dock 节点。
    /// @return 目标 Dock 节点 ID；无法解析时返回 0。
    ImGuiID resolveAudioControllerDockId();

    /// @brief 在销毁可能持有 Vulkan 资源的视图前等待 GPU 完成在途命令。
    /// @param view 即将被销毁的 UI 视图。
    /// @warning 不可中断操作：可能调用
    /// vkDeviceWaitIdle；只能在视图销毁等低频路径执行，严禁放入每帧路径。
    void waitForGpuBeforeDestroyView(IUIView& view);

    /// @brief 将当前帧标题栏原生拖拽区域和 ImGui 遮挡区域同步到底层窗口系统。
    /// @warning UI 热路径：每帧在所有 UI 绘制后执行；只遍历当前 ImGui
    /// 窗口列表并投递少量矩形。
    void syncNativeWindowDragAreas();

    /// @brief 所有ui接口
    std::unordered_map<std::string, std::unique_ptr<IUIView>> m_uiviews;

    /// @brief ui接口注册顺序
    std::vector<std::string> m_uiSequence;

    /// @brief 可再渲染ui接口注册顺序
    std::vector<std::string> m_renderableUiSequence;

    /// @brief 纹理加载器接口注册顺序
    std::vector<std::string> m_textureLoaderSequence;

    /// @brief 每帧可预先准备 UI 数据的候选视图缓存。
    std::vector<IParallelUiPreparable*> m_uiPrepareCandidates;

    /// @brief 当前帧实际需要执行准备任务的视图缓存。
    std::vector<IParallelUiPreparable*> m_uiPrepareViews;

    /// @brief 当前帧必须在 UI 主线程执行准备的视图缓存。
    std::vector<IParallelUiPreparable*> m_mainThreadUiPrepareViews;

    /// @brief 当前帧允许在线程池执行准备的纯数据视图缓存。
    std::vector<IParallelUiPreparable*> m_parallelUiPrepareViews;

    /// @brief 主窗口标题栏允许原生拖拽的基础区域。
    std::vector<Event::DragArea> m_nativeWindowDragAreas;

    /// @brief 主原生窗口观察指针，不持有所有权。
    Graphic::NativeWindow* m_nativeWindow{ nullptr };

    /// @brief 上一次已应用项目工作区的项目路径。
    std::string m_workspaceProjectPath;

    /// @brief 当前 UI 已确认加载完成的项目根目录。
    std::filesystem::path m_activeProjectRoot;

    /// @brief 等待 UI 线程应用的新项目工作区快照。
    ProjectWorkspaceState m_pendingProjectWorkspace;

    /// @brief 与待应用工作区一起捕获的项目音频资源快照。
    std::vector<AudioResource> m_pendingProjectAudioResources;

    /// @brief UI 线程归约后的项目生命周期状态。
    ProjectUiLifecycleState m_projectLifecycleState;

    /// @brief 是否有加载完成后的项目工作区等待应用。
    bool m_projectWorkspaceRestorePending{ false };

    /// @brief 项目生命周期跨线程更新队列。
    moodycamel::ConcurrentQueue<ProjectUiLifecycleUpdate>
        m_pendingProjectLifecycleUpdates;

    /// @brief 逻辑线程已经发起、但 UI 线程可能尚未消费的项目切换标志。
    /// @warning 写入者为逻辑线程生命周期回调，读取者为 UI 线程；用于阻止
    /// 同一 UI 帧后半段继续读取已关闭项目，必须使用跨线程原子同步。
    std::atomic_bool m_projectTransitionSignal{ false };

    /// @brief 项目开始打开事件订阅 ID。
    Event::SubscriptionID m_projectOpenStartedSubId{ 0 };

    /// @brief 项目加载完成事件订阅 ID。
    Event::SubscriptionID m_projectLoadedSubId{ 0 };

    /// @brief 项目关闭完成事件订阅 ID。
    Event::SubscriptionID m_projectClosedSubId{ 0 };

    /// @brief 项目打开失败事件订阅 ID。
    Event::SubscriptionID m_projectOpenFailedSubId{ 0 };

    /// @brief 临时项目保存到正式目录事件订阅 ID。
    Event::SubscriptionID m_temporaryProjectSaveResultSubId{ 0 };

    /// @brief 无项目默认工作区是否已经应用。
    bool m_noProjectWorkspaceDefaultApplied{ false };

    /// @brief 是否已请求重载皮肤相关图形资源。
    bool m_skinResourceReloadRequested{ false };

    /// @brief 下一次允许捕获项目工作区的 ImGui 时间。
    double m_nextWorkspaceCaptureTime{ 0.0 };
};
}  // namespace MMM::UI
