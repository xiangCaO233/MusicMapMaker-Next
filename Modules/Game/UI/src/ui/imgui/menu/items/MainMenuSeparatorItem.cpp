#include "ui/imgui/menu/items/MainMenuSeparatorItem.h"
#include <imgui.h>

namespace MMM::UI
{

/// @brief 绘制菜单分隔线。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：仅在所属菜单展开时执行。
/// @note 分隔线不读取业务上下文，也不产生可点击命令。
void MainMenuSeparatorItem::render(MainMenuContext& context)
{
    // 显式忽略统一接口上下文，保持该叶节点无状态。
    (void)context;
    ImGui::Separator();
}

}  // namespace MMM::UI
