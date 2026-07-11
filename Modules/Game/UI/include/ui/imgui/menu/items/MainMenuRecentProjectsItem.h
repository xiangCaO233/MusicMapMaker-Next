#pragma once

#include "ui/imgui/menu/interfaces/IMainMenuItem.h"
#include "ui/imgui/menu/interfaces/IMainMenuItemActionHandler.h"

#include <memory>

namespace MMM::UI
{

/// @brief 最近项目子菜单项。
class MainMenuRecentProjectsItem final : public IMainMenuItem
{
public:
    /// @brief 构造最近项目子菜单项。
    /// @param actionHandler 最近项目点击业务处理器。
    explicit MainMenuRecentProjectsItem(
        std::unique_ptr<IMainMenuItemActionHandler> actionHandler);

    /// @brief 更新最近项目动作处理器跨帧状态。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；仅转发给 action handler。
    void update(MainMenuContext& context) override;

    /// @brief 绘制最近项目子菜单并在点击时执行自身 action handler。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径低频分支：仅在最近项目子菜单展开时格式化显示文本。
    void render(MainMenuContext& context) override;

    /// @brief 渲染最近项目动作处理器延迟窗口。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；仅转发给 action handler。
    void renderDeferred(MainMenuContext& context) override;

private:
    /// @brief 最近项目点击业务处理器。
    std::unique_ptr<IMainMenuItemActionHandler> m_actionHandler;
};

}  // namespace MMM::UI
