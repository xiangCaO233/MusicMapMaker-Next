#pragma once

#include "ui/imgui/menu/interfaces/IMainMenuItem.h"

namespace MMM::UI
{

/// @brief 主菜单分隔线项。
class MainMenuSeparatorItem final : public IMainMenuItem
{
public:
    /// @brief 绘制菜单分隔线。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：仅在所属菜单展开时执行。
    void render(MainMenuContext& context) override;
};

}  // namespace MMM::UI
