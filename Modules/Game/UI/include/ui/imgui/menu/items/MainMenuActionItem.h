#pragma once

#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/interfaces/IMainMenuItem.h"
#include "ui/imgui/menu/interfaces/IMainMenuItemActionHandler.h"

#include <memory>
#include <string>

namespace MMM::UI
{

/// @brief 带图标的普通主菜单项。
class MainMenuActionItem final : public IMainMenuItem
{
public:
    /// @brief 构造普通主菜单项。
    /// @param icon 菜单项默认图标文本，可为空。
    /// @param label 菜单项文本或翻译键。
    /// @param textKind 菜单项文本来源。
    /// @param shortcut 菜单项默认快捷键提示，可为空。
    /// @param actionHandler 菜单项业务处理器。
    MainMenuActionItem(
        const char* icon, std::string label, MainMenuItemTextKind textKind,
        const char*                                 shortcut,
        std::unique_ptr<IMainMenuItemActionHandler> actionHandler);

    /// @brief 更新菜单项业务处理器跨帧状态。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；仅转发给 action handler。
    void update(MainMenuContext& context) override;

    /// @brief 绘制菜单项并在点击时执行自身 action handler。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：仅在所属菜单展开时执行。
    void render(MainMenuContext& context) override;

    /// @brief 转发快捷键消费给业务处理器。
    /// @param context 单帧主菜单上下文。
    /// @return 业务处理器消费快捷键时返回 true。
    /// @warning UI 热路径：每帧执行；仅转发给 action handler。
    bool handleShortcut(MainMenuContext& context) override;

    /// @brief 渲染菜单项业务处理器延迟窗口。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；仅转发给 action handler。
    void renderDeferred(MainMenuContext& context) override;

private:
    /// @brief 获取当前帧的显示文本。
    /// @return 当前帧应显示的菜单项文本。
    const char* resolveLabel() const;

    /// @brief 默认图标文本。
    const char* m_icon = nullptr;

    /// @brief 菜单项文本或翻译键。
    std::string m_label;

    /// @brief 菜单项文本来源。
    MainMenuItemTextKind m_textKind = MainMenuItemTextKind::TranslationKey;

    /// @brief 默认快捷键提示。
    const char* m_shortcut = nullptr;

    /// @brief 菜单项业务处理器。
    std::unique_ptr<IMainMenuItemActionHandler> m_actionHandler;
};

}  // namespace MMM::UI
