#pragma once

#include "ui/imgui/menu/interfaces/IMainMenu.h"
#include "ui/imgui/menu/interfaces/IMainMenuItem.h"

#include <memory>
#include <vector>

namespace MMM::UI
{

/// @brief 编辑一级菜单绘制接口实现。
class MainMenuEdit final : public IMainMenu
{
public:
    /// @brief 构造编辑菜单并注册默认菜单项。
    MainMenuEdit();

    /// @brief 获取编辑菜单标识。
    /// @return 编辑菜单标识。
    MainMenuId id() const override;

    /// @brief 获取编辑菜单显示文本。
    /// @param context 单帧主菜单上下文。
    /// @return 当前语言下的编辑菜单文本。
    const char* label(const MainMenuContext& context) const override;

    /// @brief 遍历更新编辑菜单项。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；仅转发给菜单项。
    void update(MainMenuContext& context) override;

    /// @brief 遍历编辑菜单项。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：仅在编辑菜单展开时执行。
    void render(MainMenuContext& context) override;

    /// @brief 遍历编辑菜单项消费快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 有菜单项消费快捷键时返回 true。
    /// @warning UI 热路径：每帧执行；仅转发给菜单项。
    bool handleShortcut(MainMenuContext& context) override;

    /// @brief 遍历渲染编辑菜单项的延迟窗口。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；仅转发给菜单项。
    void renderDeferred(MainMenuContext& context) override;

private:
    /// @brief 注册编辑菜单项。
    /// @param item 待注册菜单项。
    void registerItem(std::unique_ptr<IMainMenuItem> item);

    /// @brief 编辑菜单项列表。
    std::vector<std::unique_ptr<IMainMenuItem>> m_items;
};

}  // namespace MMM::UI
