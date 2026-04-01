#include "ui/imgui/menu/MainMenuView.h"
#include "config/skin/SkinConfig.h"
#include <imgui.h>

namespace MMM::UI
{

MainMenuView::MainMenuView() {}

MainMenuView::~MainMenuView() {}

void MainMenuView::update()
{
    if ( ImGui::BeginMenu(TR("ui.file")) ) {
        if ( ImGui::MenuItem(TR("ui.file.new_map")) ) {}
        if ( ImGui::MenuItem(TR("ui.file.new_pro")) ) {}
        ImGui::Separator();
        if ( ImGui::MenuItem(TR("ui.file.open_map")) ) {}
        if ( ImGui::MenuItem(TR("ui.file.open_pro")) ) {}
        if ( ImGui::MenuItem(TR("ui.file.open_recent")) ) {
            /// 遍历最近打开过的项目
        }
        ImGui::Separator();
        if ( ImGui::MenuItem(TR("ui.file.save")) ) {}
        if ( ImGui::MenuItem(TR("ui.file.save_as")) ) {}
        ImGui::EndMenu();
    }
    if ( ImGui::BeginMenu(TR("ui.edit")) ) {
        ImGui::EndMenu();
    }
}


}  // namespace MMM::UI
