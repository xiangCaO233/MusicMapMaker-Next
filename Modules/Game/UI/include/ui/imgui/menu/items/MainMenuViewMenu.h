#pragma once

#include "ui/imgui/menu/interfaces/IMainMenu.h"
#include "ui/imgui/menu/interfaces/IMainMenuItem.h"

#include <memory>
#include <vector>

namespace MMM::UI
{

/// @brief 视图一级菜单绘制接口实现。
class MainMenuViewMenu final : public IMainMenu
{
public:
    /// @brief 构造视图菜单并注册默认菜单项。
    MainMenuViewMenu();

    /// @brief 获取视图菜单标识。
    /// @return 视图菜单标识。
    MainMenuId id() const override;

    /// @brief 获取视图菜单显示文本。
    /// @param context 单帧主菜单上下文。
    /// @return 当前语言下的视图菜单文本。
    const char* label(const MainMenuContext& context) const override;

    /// @brief 遍历更新视图菜单项。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；仅转发给菜单项。
    void update(MainMenuContext& context) override;

    /// @brief 遍历视图菜单项。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：仅在视图菜单展开时执行。
    void render(MainMenuContext& context) override;

    /// @brief 遍历视图菜单项消费快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 有菜单项消费快捷键时返回 true。
    /// @warning UI 热路径：每帧执行；仅转发给菜单项。
    bool handleShortcut(MainMenuContext& context) override;

    /// @brief 遍历渲染视图菜单项的延迟窗口。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；仅转发给菜单项。
    void renderDeferred(MainMenuContext& context) override;

private:
    /// @brief 注册视图菜单项。
    /// @param item 待注册菜单项。
    void registerItem(std::unique_ptr<IMainMenuItem> item);

    /// @brief 视图菜单项列表。
    std::vector<std::unique_ptr<IMainMenuItem>> m_items;
};

}  // namespace MMM::UI
