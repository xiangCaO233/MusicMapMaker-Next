#pragma once

#include "ui/imgui/menu/MainMenuTypes.h"

#include <array>

namespace MMM::UI
{

/// @brief 管理一级菜单的 Alt 快捷键和跨帧打开、关闭请求。
class MainMenuNavigationController final
{
public:
    /// @brief 处理一级菜单的 Alt 导航快捷键。
    /// @warning UI 热路径：每帧执行；只读取固定数量按键和弹窗状态。
    void handleShortcuts();

    /// @brief 消费指定一级菜单的打开请求。
    /// @param id 一级菜单标识。
    /// @return 本帧存在打开请求时返回 true。
    bool consumeOpenRequest(MainMenuId id);

    /// @brief 消费指定一级菜单的关闭请求。
    /// @param id 一级菜单标识。
    /// @return 本帧存在关闭请求时返回 true。
    bool consumeCloseRequest(MainMenuId id);

private:
    /// @brief 根据当前弹窗状态切换指定一级菜单的请求。
    /// @param id 一级菜单标识。
    /// @param popupLabel 当前语言下的一级菜单弹窗标识。
    void toggleMenu(MainMenuId id, const char* popupLabel);

    /// @brief 下一帧需要打开的一级菜单标志。
    std::array<bool, MAIN_MENU_ID_COUNT> m_openNextFrame{};

    /// @brief 下一帧需要关闭的一级菜单标志。
    std::array<bool, MAIN_MENU_ID_COUNT> m_closeNextFrame{};
};

}  // namespace MMM::UI
