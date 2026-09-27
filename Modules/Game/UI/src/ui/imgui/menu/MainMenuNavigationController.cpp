#include "ui/imgui/menu/MainMenuNavigationController.h"

#include "config/skin/SkinConfig.h"

#include <imgui.h>

namespace MMM::UI
{

/// @brief 处理一级菜单的 Alt 导航快捷键。
/// @warning UI 热路径：每帧执行；只读取固定数量按键和弹窗状态。
/// @note 每个按键使用非重复触发，长按 Alt 不会连续切换菜单。
void MainMenuNavigationController::handleShortcuts()
{
    const ImGuiIO& io = ImGui::GetIO();
    // 未按住 Alt 时立即退出，避免查询其余固定快捷键。
    if ( !io.KeyAlt ) return;

    // 各入口只登记下一帧请求，由菜单绘制阶段实际操作弹窗。
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
/// @warning UI 热路径：按固定索引读写一个布尔值，不得引入动态查找。
/// @note 请求采用一次性消费语义，读取后立即清除。
bool MainMenuNavigationController::consumeOpenRequest(MainMenuId id)
{
    const std::size_t index = mainMenuIdIndex(id);
    // 防御未知枚举值，避免越界访问请求数组。
    if ( index >= m_openNextFrame.size() ) return false;

    const bool requested = m_openNextFrame[index];
    // 无论当前值为何都清零，使请求最多影响一次菜单绘制。
    m_openNextFrame[index] = false;
    return requested;
}

/// @brief 消费指定一级菜单的关闭请求。
/// @param id 一级菜单标识。
/// @return 本帧存在关闭请求时返回 true。
/// @warning UI 热路径：只访问固定大小请求数组。
/// @note 请求读取后清除，防止关闭状态延续到后续帧。
bool MainMenuNavigationController::consumeCloseRequest(MainMenuId id)
{
    const std::size_t index = mainMenuIdIndex(id);
    // 与打开请求保持相同的无效标识防御策略。
    if ( index >= m_closeNextFrame.size() ) return false;

    const bool requested    = m_closeNextFrame[index];
    m_closeNextFrame[index] = false;
    return requested;
}

/// @brief 根据当前弹窗状态切换指定一级菜单的请求。
/// @param id 一级菜单标识。
/// @param popupLabel 当前语言下的一级菜单弹窗标识。
/// @note 只登记请求，不在快捷键处理阶段直接改变 ImGui 弹窗栈。
/// @warning UI 热路径：popupLabel 必须与当前帧 BeginMenu 使用的文本一致。
void MainMenuNavigationController::toggleMenu(MainMenuId  id,
                                              const char* popupLabel)
{
    const std::size_t index = mainMenuIdIndex(id);
    // 枚举映射异常时忽略输入，保持两个请求数组完整。
    if ( index >= m_openNextFrame.size() ) return;

    // 已打开菜单登记关闭请求，否则登记下一帧打开请求。
    if ( ImGui::IsPopupOpen(popupLabel) ) {
        m_closeNextFrame[index] = true;
    } else {
        m_openNextFrame[index] = true;
    }
}

}  // namespace MMM::UI
