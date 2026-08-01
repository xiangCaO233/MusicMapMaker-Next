#pragma once

#include <cstdint>

namespace MMM::UI
{
enum class MainMenuId : std::uint8_t;
struct MainMenuContext;

/// @brief 一级主菜单绘制接口。
class IMainMenu
{
public:
    /// @brief 默认析构主菜单接口。
    virtual ~IMainMenu() = default;

    /// @brief 获取一级菜单标识。
    /// @return 一级菜单标识。
    virtual MainMenuId id() const = 0;

    /// @brief 获取一级菜单显示文本。
    /// @param context 单帧主菜单上下文。
    /// @return 当前语言下的菜单显示文本。
    virtual const char* label(const MainMenuContext& context) const = 0;

    /// @brief 更新菜单持有的跨帧 action 状态。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；具体实现只能做轻量状态消费。
    virtual void update(MainMenuContext& context) = 0;

    /// @brief 让菜单项尝试消费当前帧快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 有菜单项消费快捷键时返回 true。
    /// @warning UI 热路径：每帧执行；只允许读取输入状态并触发轻量 action。
    virtual bool handleShortcut(MainMenuContext& context) = 0;

    /// @brief 绘制当前一级菜单内部条目。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：仅在对应菜单展开时执行；业务动作由具体菜单项持有的
    /// action handler 处理。
    virtual void render(MainMenuContext& context) = 0;

    /// @brief 渲染菜单 action 触发的延迟窗口或弹窗。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；阻塞操作只能来自用户明确点击。
    virtual void renderDeferred(MainMenuContext& context) = 0;
};

}  // namespace MMM::UI
