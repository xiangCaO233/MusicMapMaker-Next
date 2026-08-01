#pragma once

#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/interfaces/IMainMenuItem.h"
#include "ui/imgui/menu/interfaces/IMainMenuToggleItemActionHandler.h"

#include <memory>
#include <string>

namespace MMM::UI
{

/// @brief 带勾选状态的主菜单项。
class MainMenuToggleItem final : public IMainMenuItem
{
public:
    /// @brief 构造勾选主菜单项。
    /// @param label 菜单项文本或翻译键。
    /// @param textKind 菜单项文本来源。
    /// @param actionHandler 勾选菜单项业务处理器。
    MainMenuToggleItem(
        std::string label, MainMenuItemTextKind textKind,
        std::unique_ptr<IMainMenuToggleItemActionHandler> actionHandler);

    /// @brief 更新勾选菜单项业务处理器跨帧状态。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；仅转发给 action handler。
    void update(MainMenuContext& context) override;

    /// @brief 绘制勾选菜单项并在状态变化时执行自身 action handler。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：仅在所属菜单展开时执行。
    void render(MainMenuContext& context) override;

    /// @brief 转发快捷键消费给勾选业务处理器。
    /// @param context 单帧主菜单上下文。
    /// @return 业务处理器消费快捷键时返回 true。
    /// @warning UI 热路径：每帧执行；仅转发给 action handler。
    bool handleShortcut(MainMenuContext& context) override;

    /// @brief 渲染勾选菜单项业务处理器延迟窗口。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；仅转发给 action handler。
    void renderDeferred(MainMenuContext& context) override;

private:
    /// @brief 获取当前帧的显示文本。
    /// @return 当前帧应显示的菜单项文本。
    const char* resolveLabel() const;

    /// @brief 菜单项文本或翻译键。
    std::string m_label;

    /// @brief 菜单项文本来源。
    MainMenuItemTextKind m_textKind = MainMenuItemTextKind::TranslationKey;

    /// @brief 勾选菜单项业务处理器。
    std::unique_ptr<IMainMenuToggleItemActionHandler> m_actionHandler;
};

}  // namespace MMM::UI
