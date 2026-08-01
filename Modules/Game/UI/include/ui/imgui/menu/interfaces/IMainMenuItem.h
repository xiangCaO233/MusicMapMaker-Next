#pragma once

namespace MMM::UI
{
struct MainMenuContext;

/// @brief 主菜单项绘制接口。
class IMainMenuItem
{
public:
    /// @brief 默认析构主菜单项接口。
    virtual ~IMainMenuItem() = default;

    /// @brief 更新菜单项持有的跨帧 action 状态。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；默认实现为空。
    virtual void update(MainMenuContext& context);

    /// @brief 尝试消费当前帧快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 当前菜单项消费快捷键时返回 true。
    /// @warning UI 热路径：每帧执行；默认实现不消费。
    virtual bool handleShortcut(MainMenuContext& context);

    /// @brief 绘制菜单项并在触发时执行自身 action handler。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：仅在所属菜单展开时执行；不得在绘制阶段做阻塞操作。
    virtual void render(MainMenuContext& context) = 0;

    /// @brief 渲染菜单项 action 的延迟窗口或弹窗。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；默认实现为空。
    virtual void renderDeferred(MainMenuContext& context);
};

}  // namespace MMM::UI
