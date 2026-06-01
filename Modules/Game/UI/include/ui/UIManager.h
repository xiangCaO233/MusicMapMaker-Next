#pragma once

#include "IUIView.h"
#include "graphic/imguivk/IGraphicUserHook.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/layout/CLayWrapperCore.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMM::Graphic
{
class NativeWindow;
}

namespace MMM
{
class Project;
struct ProjectWorkspaceState;
}  // namespace MMM

namespace MMM::Event
{
enum class SettingsTab;
}  // namespace MMM::Event

namespace MMM::UI
{
class IRenderableView;
class ITextureLoader;

class UIManager : public MMM::Graphic::IGraphicUserHook
{
public:
    UIManager()
    {
        // 初始化CLay
        CLayWrapperCore::instance().setupClayTextMeasurement();
    }
    UIManager(UIManager&&)                 = delete;
    UIManager(const UIManager&)            = delete;
    UIManager& operator=(UIManager&&)      = delete;
    UIManager& operator=(const UIManager&) = delete;
    ~UIManager()                           = default;

    /// @brief 注册视图，转交所有权
    void registerView(const std::string& name, std::unique_ptr<IUIView> view);

    /// @brief 注销并销毁视图
    void unregisterView(const std::string& name);

    /// @brief 清理所有ui
    void clearAllViews();

    /// @brief 绑定主原生窗口，用于项目工作区保存和恢复窗口位置。
    /// @param window 主原生窗口指针，生命周期由 GameLoop 持有。
    void setNativeWindow(Graphic::NativeWindow* window);

    /// @brief 捕获当前项目工作区 UI 状态到内存中的项目配置。
    void captureProjectWorkspaceState();

    /// @brief 打开独立设置窗口，切换到指定标签页并请求聚焦。
    /// @param tab 需要激活的设置标签页。
    void openSettingsWindow(MMM::Event::SettingsTab tab);

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
    /// @brief 无项目时应用一次默认侧边栏工作区。
    void applyNoProjectDefaultWorkspace();

    /// @brief 同步当前项目的 ImGui 布局保存和恢复状态。
    /// @warning UI 热路径低频分支：每帧只比较当前项目路径；实际保存 ImGui
    /// ini 和窗口状态按时间间隔触发，禁止在这里写文件。
    void syncProjectWorkspaceState();

    /// @brief 捕获当前项目打开的动态工作区视图。
    /// @param workspace 需要写入的项目工作区状态。
    void captureProjectWorkspaceViews(ProjectWorkspaceState& workspace);

    /// @brief 恢复项目工作区中保存的动态视图。
    /// @param workspace 项目工作区状态。
    void restoreProjectWorkspaceViews(const ProjectWorkspaceState& workspace);

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

    /// @brief 所有ui接口
    std::unordered_map<std::string, std::unique_ptr<IUIView>> m_uiviews;

    /// @brief ui接口注册顺序
    std::vector<std::string> m_uiSequence;

    /// @brief 可再渲染ui接口注册顺序
    std::vector<std::string> m_renderableUiSequence;

    /// @brief 纹理加载器接口注册顺序
    std::vector<std::string> m_textureLoaderSequence;

    /// @brief 主原生窗口观察指针，不持有所有权。
    Graphic::NativeWindow* m_nativeWindow{ nullptr };

    /// @brief 上一次已应用项目工作区的项目路径。
    std::string m_workspaceProjectPath;

    /// @brief 上一次已应用项目工作区的项目实例地址。
    const Project* m_workspaceProjectInstance{ nullptr };

    /// @brief 无项目默认工作区是否已经应用。
    bool m_noProjectWorkspaceDefaultApplied{ false };

    /// @brief 下一次允许捕获项目工作区的 ImGui 时间。
    double m_nextWorkspaceCaptureTime{ 0.0 };
};
}  // namespace MMM::UI
