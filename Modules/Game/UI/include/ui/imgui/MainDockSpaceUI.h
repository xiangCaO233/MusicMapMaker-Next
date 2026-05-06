#pragma once

#include "ui/ITextureLoader.h"
#include "ui/imgui/manager/ToolbarView.h"
#include "ui/imgui/menu/MainMenuView.h"
#include "event/core/EventBus.h"
#include "event/ui/GLFWNativeEvent.h"
#include <memory>


namespace MMM::UI
{
class MainDockSpaceUI : public ITextureLoader, virtual public IUIView
{
public:
    MainDockSpaceUI(const std::string& name)
        : IUIView(name), ITextureLoader(name)
    {
        // 订阅原生事件以同步窗口最大化状态
        Event::EventBus::instance().subscribe<Event::GLFWNativeEvent>(
            [&](Event::GLFWNativeEvent e) {
                if ( e.hasStateChange &&
                     e.type == Event::NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE ) {
                    m_isMaximized = e.isMaximized;
                }
            });
    }
    MainDockSpaceUI(MainDockSpaceUI&&)                 = delete;
    MainDockSpaceUI(const MainDockSpaceUI&)            = delete;
    MainDockSpaceUI& operator=(MainDockSpaceUI&&)      = delete;
    MainDockSpaceUI& operator=(const MainDockSpaceUI&) = delete;

    ~MainDockSpaceUI() override = default;

    void update(UIManager* sourceManager) override;

    /// @brief 是否需要重载
    bool needReload() override;

    /// @brief 重载纹理
    void reloadTextures(vk::PhysicalDevice& physicalDevice,
                        vk::Device& logicalDevice, vk::CommandPool& cmdPool,
                        vk::Queue& queue) override;

    void renderMenuBar(UIManager* sourceManager, float menuBarHeight,
                       float sidebarWidth, float toolbarWidth, float dpiScale);
    void renderDockingSpace(UIManager* sourceManager, float menuBarHeight,
                            float statusBarHeight, float sidebarWidth,
                            float toolbarWidth);
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
};

}  // namespace MMM::UI
