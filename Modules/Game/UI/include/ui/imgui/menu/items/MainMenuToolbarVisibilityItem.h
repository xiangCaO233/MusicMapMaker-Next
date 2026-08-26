#pragma once

#include "ui/imgui/menu/interfaces/IMainMenuItem.h"

namespace MMM::UI
{

/// @brief 在查看菜单中分组控制工具栏按钮可见性的子菜单项。
class MainMenuToolbarVisibilityItem final : public IMainMenuItem
{
public:
    /// @brief 绘制工具栏按钮可见性分组子菜单。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：仅在查看菜单展开时执行，只读写 ImGui 与配置状态。
    void render(MainMenuContext& context) override;
};

}  // namespace MMM::UI
