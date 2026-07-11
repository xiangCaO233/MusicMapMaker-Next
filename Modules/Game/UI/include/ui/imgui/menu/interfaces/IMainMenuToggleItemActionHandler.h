#pragma once

#include "ui/imgui/menu/MainMenuTypes.h"

namespace MMM::UI
{

/// @brief 主菜单勾选菜单项的业务处理接口。
class IMainMenuToggleItemActionHandler
{
public:
    /// @brief 默认析构勾选菜单项业务处理接口。
    virtual ~IMainMenuToggleItemActionHandler() = default;

    /// @brief 更新 action 持有的跨帧状态。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；默认实现为空。
    virtual void update(MainMenuContext& context);

    /// @brief 尝试消费当前帧快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 当前 action 消费快捷键时返回 true。
    /// @warning UI 热路径：每帧执行；默认实现不消费。
    virtual bool handleShortcut(MainMenuContext& context);

    /// @brief 获取当前勾选项是否可用。
    /// @param context 单帧主菜单上下文。
    /// @return 可交互时返回 true。
    virtual bool isEnabled(const MainMenuContext& context) const;

    /// @brief 获取当前勾选项绑定的布尔状态。
    /// @param context 单帧主菜单上下文。
    /// @return 可修改的布尔状态指针；为空时菜单项会禁用。
    virtual bool* value(MainMenuContext& context) = 0;

    /// @brief 执行勾选项状态变化后的业务动作。
    /// @param context 单帧主菜单上下文。
    /// @param activation 菜单项激活载荷。
    virtual void execute(MainMenuContext&              context,
                         const MainMenuItemActivation& activation) = 0;

    /// @brief 渲染 action 触发的延迟窗口或弹窗。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；默认实现为空。
    virtual void renderDeferred(MainMenuContext& context);
};

}  // namespace MMM::UI
