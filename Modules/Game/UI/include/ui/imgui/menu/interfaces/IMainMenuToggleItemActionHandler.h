#pragma once

namespace MMM::UI
{
struct MainMenuContext;
struct MainMenuItemActivation;

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

    /// @brief 返回禁用原因的翻译键，默认不为普通不可用状态添加解释。
    /// @param context 单帧主菜单上下文。
    /// @return 稳定翻译键；为空时不绘制原因提示。
    /// @warning 菜单热路径只读现有状态，禁止阻塞查询或创建字符串。
    /// @note 由动作区分专业能力与其他禁用条件，渲染器不猜测原因。
    virtual const char* disabledTooltipKey(const MainMenuContext& context) const
    {
        (void)context;
        return nullptr;
    }

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
