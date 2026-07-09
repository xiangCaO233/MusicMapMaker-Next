#include "ui/imgui/menu/items/MainMenuSeparatorItem.h"
#include <imgui.h>

namespace MMM::UI
{

/// @brief 绘制菜单分隔线。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：仅在所属菜单展开时执行。
void MainMenuSeparatorItem::render(MainMenuContext& context)
{
    (void)context;
    ImGui::Separator();
}

}  // namespace MMM::UI
