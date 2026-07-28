#pragma once

#include "event/core/EventBus.h"
#include "event/ui/GLFWNativeEvent.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "ui/ITextureLoader.h"
#include "ui/imgui/feedback/BeatmapLoadDiagnosticFeedback.h"
#include "ui/imgui/feedback/SaveResultFeedback.h"
#include "ui/imgui/manager/ToolbarView.h"
#include "ui/imgui/menu/MainMenuView.h"
#include "ui/imgui/status/StatusMessageService.h"
#include "ui/imgui/windows/PgoUploadConsentWindow.h"
#include <functional>
#include <memory>


namespace MMM::UI
{
class MainDockSpaceUI : public ITextureLoader, virtual public IUIView
{
public:
    static ImGuiID getCenterDockId() { return s_centerDockId; }
    static void    setCenterDockId(ImGuiID id) { s_centerDockId = id; }
    /// @brief 获取解除固定时工具窗口使用的右侧停靠节点。
    static ImGuiID getToolDockId() { return s_toolDockId; }
    /// @brief 设置解除固定时工具窗口使用的右侧停靠节点。
    /// @param id 右侧工具停靠节点 ID。
    static void setToolDockId(ImGuiID id) { s_toolDockId = id; }
    /// @brief 标记本帧已加载项目专属 ImGui 布局，跳过默认 DockBuilder 重置。
    static void markProjectWorkspaceLayoutLoaded()
    {
        s_projectWorkspaceLayoutLoaded = true;
    }
    /// @brief 消费项目专属布局加载标记。
    static bool consumeProjectWorkspaceLayoutLoaded()
    {
        bool loaded                    = s_projectWorkspaceLayoutLoaded;
        s_projectWorkspaceLayoutLoaded = false;
        return loaded;
    }

private:
    static inline ImGuiID s_centerDockId{ 0 };
    static inline ImGuiID s_toolDockId{ 0 };
    static inline bool    s_projectWorkspaceLayoutLoaded{ false };

    /// @brief 临时项目保存后需要继续执行的 UI 动作。
    enum class TemporaryProjectAfterSaveAction {
        None,          ///< 保存后不继续执行额外动作。
        CloseProject,  ///< 保存后关闭当前项目。
        ExitApp        ///< 保存后退出应用。
    };

public:
    MainDockSpaceUI(const std::string& name)
        : IUIView(name), ITextureLoader(name)
    {
        // 订阅原生事件以同步窗口最大化状态
        Event::EventBus::instance().subscribe<Event::GLFWNativeEvent>(
            [&](Event::GLFWNativeEvent e) {
                if ( e.hasStateChange &&
                     e.type ==
                         Event::NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE ) {
                    m_isMaximized = e.isMaximized;
                }
            });

        // 订阅音频导入触发事件
        Event::EventBus::instance().subscribe<Event::AudioImportTriggerEvent>(
            [&](Event::AudioImportTriggerEvent e) {
                m_pendingImportPath   = e.path;
                m_showImportTypeModal = true;
            });
    }
    MainDockSpaceUI(MainDockSpaceUI&&)                 = delete;
    MainDockSpaceUI(const MainDockSpaceUI&)            = delete;
    MainDockSpaceUI& operator=(MainDockSpaceUI&&)      = delete;
    MainDockSpaceUI& operator=(const MainDockSpaceUI&) = delete;

    ~MainDockSpaceUI() override = default;

    void update(UIManager* sourceManager) override;

    void* getActualInstance() override { return this; }

    /// @brief 是否需要重载
    bool needReload() override;

    /// @brief 重载纹理
    void reloadTextures(vk::PhysicalDevice& physicalDevice,
                        vk::Device& logicalDevice, vk::CommandPool& cmdPool,
                        vk::Queue& queue) override;

    /// @brief 渲染顶部菜单栏和无边框窗口控制区域。
    /// @param sourceManager 当前 UI 管理器。
    /// @param menuBarHeight 顶部菜单栏高度。
    /// @param sidebarWidth 左侧边栏宽度。
    /// @param toolbarWidth 右侧工具栏宽度。
    /// @param dpiScale 当前 DPI 缩放。
    /// @warning UI 热路径：每帧执行；只允许常量规模布局、绘制和事件投递。
    void renderMenuBar(UIManager* sourceManager, float menuBarHeight,
                       float sidebarWidth, float toolbarWidth, float dpiScale);
    void renderDockingSpace(UIManager* sourceManager, float menuBarHeight,
                            float statusBarHeight, float sidebarWidth,
                            float toolbarWidth);

    /// @brief 处理无边框主窗口边缘缩放命中。
    /// @param sourceManager 当前 UI 管理器。
    /// @param dpiScale 当前 DPI 缩放。
    /// @warning UI 热路径：主窗口每帧执行；只做常量规模鼠标命中检测。
    void handleNativeWindowFrameInteraction(UIManager* sourceManager,
                                            float      dpiScale);

    /// @brief 绘制无边框主窗口的自绘外框、内阴影和圆角轮廓。
    /// @param sourceManager 当前 UI 管理器。
    /// @param dpiScale 当前 DPI 缩放。
    /// @warning UI 热路径：主窗口每帧执行；只提交固定数量 ImGui 绘制命令。
    void renderNativeWindowFrameOverlay(UIManager* sourceManager,
                                        float      dpiScale) const;

    /// @brief 渲染底部状态栏。
    /// @param sourceManager 当前 UI 管理器。
    /// @param statusBarHeight 状态栏高度。
    /// @param dpiScale 当前 DPI 缩放。
    /// @warning UI 热路径：每帧执行；只允许常量规模布局和状态快照读取。
    void renderStatusBar(UIManager* sourceManager, float statusBarHeight,
                         float dpiScale);

    /// @brief 请求选择临时项目正式保存位置。
    /// @warning UI 低频路径：用户点击保存临时项目时调用系统/统一文件选择器。
    void requestTemporaryProjectSaveFolder();

    /// @brief 渲染临时项目只读和关闭确认弹窗。
    /// @param dpiScale 当前 DPI 缩放。
    /// @param viewport 主 ImGui 视口。
    /// @warning UI 热路径低频分支：只在弹窗打开时绘制少量文本和按钮。
    void renderTemporaryProjectPopups(float dpiScale, ImGuiViewport* viewport);

    /// @brief 处理临时项目提示和保存结果队列。
    /// @warning UI 热路径：每帧只消费少量跨线程 UI 事件载荷。
    void consumeTemporaryProjectQueues();

    ///@brief 是否需要重载
    bool m_needReload{ true };

    /// @brief 跨菜单 action 和状态栏共享的临时状态消息服务。
    StatusMessageService m_statusMessageService;

    /// @brief 保存结果事件反馈气泡组件。
    SaveResultFeedback m_saveResultFeedback;

    /// @brief 谱面加载兼容诊断的中央通知组件。
    BeatmapLoadDiagnosticFeedback m_beatmapLoadDiagnosticFeedback;

    /// @brief PGO 性能数据上传授权窗口组件。
    PgoUploadConsentWindow m_pgoUploadConsentWindow;

    /// @brief 主菜单注册与绘制视图。
    MainMenuView m_mainMenuview;

    ///@brief 工具栏
    ToolbarView m_toolbarView{ "Toolbar" };

    ///@brief 图标纹理
    std::unique_ptr<Graphic::VKTexture> m_logo_texture;

    /// @brief 窗口是否最大化 (通过事件同步)
    bool m_isMaximized{ false };

    /// @brief 是否显示退出确认弹窗
    bool m_showExitConfirmation{ false };

    /// @brief 待导入的音频路径 (用于模态弹窗)
    std::string m_pendingImportPath;
    /// @brief 是否显示音频导入类型选择弹窗
    bool m_showImportTypeModal{ false };

    // 文件覆盖确认
    bool                  m_showOverwriteModal = false;
    std::string           m_pendingOverwritePath;
    std::function<void()> m_onOverwriteConfirm;

    /// @brief 是否显示临时项目只读提示弹窗。
    bool m_showTemporaryProjectReadOnlyModal{ false };
    /// @brief 是否显示临时项目关闭确认弹窗。
    bool m_showTemporaryProjectCloseModal{ false };
    /// @brief 是否正在等待临时项目保存结果。
    bool m_temporaryProjectSaveInProgress{ false };
    /// @brief 临时项目保存成功后需要继续执行的动作。
    TemporaryProjectAfterSaveAction m_temporaryProjectAfterSaveAction{
        TemporaryProjectAfterSaveAction::None
    };
    /// @brief 当前临时项目的原始包文件路径。
    std::string m_temporaryProjectSourcePath;
    /// @brief 当前临时项目的缓存目录路径。
    std::string m_temporaryProjectCachePath;
    /// @brief 临时项目保存失败时显示的错误信息。
    std::string m_temporaryProjectSaveError;
    /// @brief 临时项目保存成功后是否需要设置主窗口退出标记。
    bool m_exitAfterTemporaryProjectSave{ false };
    /// @brief 临时项目退出弹窗是否已经确认允许本次窗口关闭。
    bool m_temporaryProjectExitConfirmed{ false };
};

}  // namespace MMM::UI
