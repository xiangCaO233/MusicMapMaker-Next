#pragma once

namespace MMM::UI
{
struct MainMenuContext;
struct MainMenuItemActivation;

/// @brief 主菜单普通菜单项的业务处理接口。
class IMainMenuItemActionHandler
{
public:
    /// @brief 默认析构菜单项业务处理接口。
    virtual ~IMainMenuItemActionHandler() = default;

    /// @brief 更新 action 持有的跨帧状态。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；默认实现为空。
    virtual void update(MainMenuContext& context);

    /// @brief 尝试消费当前帧快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 当前 action 消费快捷键时返回 true。
    /// @warning UI 热路径：每帧执行；默认实现不消费。
    virtual bool handleShortcut(MainMenuContext& context);

    /// @brief 获取当前菜单项是否可用。
    /// @param context 单帧主菜单上下文。
    /// @return 可点击时返回 true。
    virtual bool isEnabled(const MainMenuContext& context) const;

    /// @brief 获取当前菜单项图标。
    /// @param context 单帧主菜单上下文。
    /// @param fallbackIcon 菜单项配置的默认图标。
    /// @return 当前帧应显示的图标文本。
    virtual const char* icon(const MainMenuContext& context,
                             const char*            fallbackIcon) const;

    /// @brief 获取当前菜单项快捷键提示。
    /// @param context 单帧主菜单上下文。
    /// @param fallbackShortcut 菜单项配置的默认快捷键提示。
    /// @return 当前帧应显示的快捷键提示，可为空。
    virtual const char* shortcut(const MainMenuContext& context,
                                 const char*            fallbackShortcut) const;

    /// @brief 执行菜单项业务动作。
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
