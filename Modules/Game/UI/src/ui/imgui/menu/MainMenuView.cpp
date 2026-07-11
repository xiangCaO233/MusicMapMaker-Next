#include "ui/imgui/menu/MainMenuView.h"

#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/utils/UIWidgetUtils.h"

#include <imgui.h>

namespace MMM::UI
{

/// @brief 构造主菜单视图并创建默认菜单注册表。
MainMenuView::MainMenuView() : m_registeredMenus(createDefaultMainMenus()) {}

/// @brief 销毁主菜单视图。
MainMenuView::~MainMenuView() = default;

/// @brief 让已注册菜单项和一级菜单导航尝试消费当前快捷键。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅遍历注册菜单并读取固定数量导航键。
void MainMenuView::handleHotkeys(MainMenuContext& context)
{
    const ImGuiIO& io = ImGui::GetIO();

    // 文本输入和快捷键录制期间禁止菜单快捷键穿透。
    if ( io.WantTextInput || ShortcutUtils::isShortcutRecordingActive() ) {
        return;
    }

    const bool anyPopupOpen = ImGui::IsPopupOpen(
        nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if ( anyPopupOpen ) {
        bool mainMenuPopupOpen = false;
        for ( const auto& menu : m_registeredMenus ) {
            if ( menu && ImGui::IsPopupOpen(menu->label(context)) ) {
                mainMenuPopupOpen = true;
                break;
            }
        }
        if ( mainMenuPopupOpen ) {
            m_navigationController.handleShortcuts();
        }
        return;
    }

    for ( auto& menu : m_registeredMenus ) {
        if ( menu && menu->handleShortcut(context) ) return;
    }

    m_navigationController.handleShortcuts();
}

/// @brief 遍历更新已注册菜单持有的 action 状态。
/// @param sourceManager 当前 UI 管理器。
/// @param statusMessageSink 状态消息接收接口。
void MainMenuView::update(UIManager*          sourceManager,
                          IStatusMessageSink& statusMessageSink)
{
    MainMenuContext context{
        .statusMessageSink = statusMessageSink,
        .sourceManager     = sourceManager,
        .dpiScale = Config::AppConfig::instance().getWindowContentScale(),
    };
    for ( auto& menu : m_registeredMenus ) {
        if ( menu ) menu->update(context);
    }
}

/// @brief 遍历绘制已注册的一级菜单接口。
/// @param sourceManager 当前 UI 管理器。
/// @param statusMessageSink 状态消息接收接口。
void MainMenuView::renderMenus(UIManager*          sourceManager,
                               IStatusMessageSink& statusMessageSink)
{
    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    MainMenuContext context{
        .statusMessageSink = statusMessageSink,
        .sourceManager     = sourceManager,
        .dpiScale          = dpiScale,
    };
    handleHotkeys(context);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(8.0f * dpiScale, 8.0f * dpiScale));
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(6.0f * dpiScale, ImGui::GetStyle().FramePadding.y));

    ImFont* menuFont = Config::SkinManager::instance().getFont("menu");
    if ( menuFont ) ImGui::PushFont(menuFont, menuFont->LegacySize);

    for ( auto& menu : m_registeredMenus ) {
        if ( !menu ) continue;

        const MainMenuId menuId    = menu->id();
        const char*      menuLabel = menu->label(context);
        if ( m_navigationController.consumeOpenRequest(menuId) ) {
            ImGui::OpenPopup(menuLabel);
        }

        if ( ::MMM::UI::FeedbackBeginMenu(menuLabel) ) {
            if ( m_navigationController.consumeCloseRequest(menuId) ) {
                ImGui::CloseCurrentPopup();
            } else {
                menu->render(context);
            }
            ::MMM::UI::FeedbackEndMenu();
        }
    }

    if ( menuFont ) ImGui::PopFont();
    ImGui::PopStyleVar(2);
}

/// @brief 遍历渲染菜单 action 触发的延迟窗口和弹窗。
/// @param sourceManager 当前 UI 管理器。
/// @param dpiScale 当前窗口内容缩放。
/// @param statusMessageSink 状态消息接收接口。
/// @warning UI 热路径：每帧执行；阻塞操作只能来自用户明确点击。
void MainMenuView::renderDeferredPopups(UIManager*          sourceManager,
                                        float               dpiScale,
                                        IStatusMessageSink& statusMessageSink)
{
    MainMenuContext context{
        .statusMessageSink = statusMessageSink,
        .sourceManager     = sourceManager,
        .dpiScale          = dpiScale,
    };

    ImFont* menuFont = Config::SkinManager::instance().getFont("menu");
    if ( menuFont ) ImGui::PushFont(menuFont, menuFont->LegacySize);

    for ( auto& menu : m_registeredMenus ) {
        if ( menu ) menu->renderDeferred(context);
    }

    if ( menuFont ) ImGui::PopFont();
}

}  // namespace MMM::UI
