#include "ui/imgui/menu/MainMenuNavigationController.h"

#include "config/skin/SkinConfig.h"

#include <imgui.h>

namespace MMM::UI
{

/// @brief 处理一级菜单的 Alt 导航快捷键。
/// @warning UI 热路径：每帧执行；只读取固定数量按键和弹窗状态。
void MainMenuNavigationController::handleShortcuts()
{
    const ImGuiIO& io = ImGui::GetIO();
    if ( !io.KeyAlt ) return;

    if ( ImGui::IsKeyPressed(ImGuiKey_F, false) ) {
        toggleMenu(MainMenuId::File, TR("ui.file").data());
    }
    if ( ImGui::IsKeyPressed(ImGuiKey_E, false) ) {
        toggleMenu(MainMenuId::Edit, TR("ui.edit").data());
    }
    if ( ImGui::IsKeyPressed(ImGuiKey_T, false) ) {
        toggleMenu(MainMenuId::Tools, TR("ui.tools").data());
    }
    if ( ImGui::IsKeyPressed(ImGuiKey_V, false) ) {
        toggleMenu(MainMenuId::View, TR("ui.view").data());
    }
    if ( ImGui::IsKeyPressed(ImGuiKey_H, false) ) {
        toggleMenu(MainMenuId::Help, TR("ui.help").data());
    }
}

/// @brief 消费指定一级菜单的打开请求。
/// @param id 一级菜单标识。
/// @return 本帧存在打开请求时返回 true。
bool MainMenuNavigationController::consumeOpenRequest(MainMenuId id)
{
    const std::size_t index = mainMenuIdIndex(id);
    if ( index >= m_openNextFrame.size() ) return false;

    const bool requested   = m_openNextFrame[index];
    m_openNextFrame[index] = false;
    return requested;
}

/// @brief 消费指定一级菜单的关闭请求。
/// @param id 一级菜单标识。
/// @return 本帧存在关闭请求时返回 true。
bool MainMenuNavigationController::consumeCloseRequest(MainMenuId id)
{
    const std::size_t index = mainMenuIdIndex(id);
    if ( index >= m_closeNextFrame.size() ) return false;

    const bool requested    = m_closeNextFrame[index];
    m_closeNextFrame[index] = false;
    return requested;
}

/// @brief 根据当前弹窗状态切换指定一级菜单的请求。
/// @param id 一级菜单标识。
/// @param popupLabel 当前语言下的一级菜单弹窗标识。
void MainMenuNavigationController::toggleMenu(MainMenuId  id,
                                              const char* popupLabel)
{
    const std::size_t index = mainMenuIdIndex(id);
    if ( index >= m_openNextFrame.size() ) return;

    if ( ImGui::IsPopupOpen(popupLabel) ) {
        m_closeNextFrame[index] = true;
    } else {
        m_openNextFrame[index] = true;
    }
}

}  // namespace MMM::UI
