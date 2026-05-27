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

private:
    static inline ImGuiID s_centerDockId{ 0 };

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
