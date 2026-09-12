#pragma once

#include "ui/imgui/menu/interfaces/IMainMenuItem.h"

namespace MMM::UI
{

/// @brief 不持有状态、只表达相邻菜单命令视觉分组的分隔线项。
/// @note 继承统一菜单项接口以便直接插入任意菜单的项目序列。
class MainMenuSeparatorItem final : public IMainMenuItem
{
public:
    /// @brief 绘制菜单分隔线。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：仅在所属菜单展开时执行。
    void render(MainMenuContext& context) override;
};

}  // namespace MMM::UI
