#pragma once

#include "event/core/EventBus.h"
#include "event/ui/GLFWNativeEvent.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "ui/ITextureLoader.h"
#include "ui/imgui/manager/ToolbarView.h"
#include "ui/imgui/menu/MainMenuView.h"
#include <functional>
#include <memory>


namespace MMM::UI
{
class MainDockSpaceUI : public ITextureLoader, virtual public IUIView
{
public:
    static ImGuiID getCenterDockId() { return s_centerDockId; }
    static void    setCenterDockId(ImGuiID id) { s_centerDockId = id; }
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
    static inline bool    s_projectWorkspaceLayoutLoaded{ false };

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

    ///@brief 是否需要重载
    bool m_needReload{ true };

    ///@brief 主菜单
    MainMenuView m_mainMenuview;

    ///@brief 工具栏
    ToolbarView m_toolbarView{ "Toolbar" };

    ///@brief 图标纹理
    std::unique_ptr<Graphic::VKTexture> m_logo_texture;

    /// @brief 窗口是否最大化 (通过事件同步)
    bool m_isMaximized{ false };

    /// @brief 是否已初始化窗口状态
    bool m_initializedWindow{ false };

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
};

}  // namespace MMM::UI
