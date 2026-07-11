#pragma once

#include "ui/imgui/menu/MainMenuInterfaces.h"
#include "ui/imgui/menu/MainMenuNavigationController.h"

#include <memory>
#include <vector>

namespace MMM::UI
{
class UIManager;

class IStatusMessageSink;

/// @brief ImGui 顶部主菜单视图，负责遍历注册菜单并转发 UI 生命周期。
class MainMenuView
{
public:
    /// @brief 构造主菜单视图。
    MainMenuView();

    /// @brief 默认移动构造主菜单视图。
    MainMenuView(MainMenuView&&) = default;

    /// @brief 禁止拷贝构造，避免复制菜单注册所有权。
    MainMenuView(const MainMenuView&) = delete;

    /// @brief 默认移动赋值主菜单视图。
    MainMenuView& operator=(MainMenuView&&) = default;

    /// @brief 禁止拷贝赋值，避免复制菜单注册所有权。
    MainMenuView& operator=(const MainMenuView&) = delete;

    /// @brief 销毁主菜单视图。
    ~MainMenuView();

    /// @brief 遍历更新已注册菜单持有的 action 状态。
    /// @param sourceManager 当前 UI 管理器。
    /// @param statusMessageSink 状态消息接收接口。
    void update(UIManager*          sourceManager,
                IStatusMessageSink& statusMessageSink);

    /// @brief 渲染顶部主菜单。
    /// @param sourceManager 当前 UI 管理器。
    /// @param statusMessageSink 状态消息接收接口。
    void renderMenus(UIManager*          sourceManager,
                     IStatusMessageSink& statusMessageSink);

    /// @brief 渲染由主菜单触发但必须位于菜单栏窗口外的弹窗和辅助窗口。
    /// @param sourceManager 当前 UI 管理器，用于消费菜单延迟动作。
    /// @param dpiScale 当前窗口内容缩放。
    /// @param statusMessageSink 状态消息接收接口。
    /// @warning UI 热路径：每帧执行；只允许消费已置位菜单动作并渲染可见弹窗，
    /// 文件选择器等阻塞操作只能来自用户明确点击。
    void renderDeferredPopups(UIManager* sourceManager, float dpiScale,
                              IStatusMessageSink& statusMessageSink);

private:
    /// @brief 让已注册菜单项和一级菜单导航尝试消费当前快捷键。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；仅遍历注册菜单并读取固定数量导航键。
    void handleHotkeys(MainMenuContext& context);

    /// @brief 已注册的一级主菜单绘制接口。
    std::vector<std::unique_ptr<IMainMenu>> m_registeredMenus;

    /// @brief 一级菜单 Alt 导航和跨帧开关请求控制器。
    MainMenuNavigationController m_navigationController;
};

}  // namespace MMM::UI
