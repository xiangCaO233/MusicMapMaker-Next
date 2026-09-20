#include "ui/imgui/menu/MainMenuView.h"

#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "ui/UIManager.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/walkthrough/WalkthroughSpotlight.h"

#include <imgui.h>
#include <string_view>

namespace MMM::UI
{
namespace
{
/// @brief 返回一级菜单供演练配置引用的稳定语义目标。
/// @param id 菜单注册标识。
/// @return 与显示语言无关的常量 ID。
/// @warning UI 热路径：只执行固定大小 switch，不读取翻译或分配字符串。
/// @details 一级菜单只在此处集中绑定语义目标，演练配置不依赖本地化 label。
/// 新增菜单枚举时必须同步补充映射，Count 哨兵始终返回空目标。
/// 目标矩形仍由 BeginMenu 的实际 Item 提供，因此横向排版变化无需同步配置。
/// 这里只声明位置身份，不决定当前是否有演练需要消费该目标。
constexpr std::string_view walkthroughTarget(MainMenuId id)
{
    switch ( id ) {
    case MainMenuId::File: return "main-menu.file";
    case MainMenuId::Edit: return "main-menu.edit";
    case MainMenuId::Tools: return "main-menu.tools";
    case MainMenuId::View: return "main-menu.view";
    case MainMenuId::Help: return "main-menu.help";
    case MainMenuId::Count: break;
    }
    return {};
}
}  // namespace

/// @brief 构造主菜单视图并创建默认菜单注册表。
/// @note 注册顺序同时决定一级菜单的显示与快捷键遍历顺序。
MainMenuView::MainMenuView() : m_registeredMenus(createDefaultMainMenus()) {}

/// @brief 销毁主菜单视图。
/// @note 菜单项由注册表中的 unique_ptr 按逆序自动释放。
MainMenuView::~MainMenuView() = default;

/// @brief 让已注册菜单项和一级菜单导航尝试消费当前快捷键。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅遍历注册菜单并读取固定数量导航键。
/// @note 普通动作快捷键与 Alt 一级菜单导航在互斥的弹窗状态下处理。
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
        // 弹窗存在时仅允许已打开的一级菜单响应 Alt 切换，保护模态交互。
        bool mainMenuPopupOpen = false;
        // 注册菜单数量固定且很小，此处只检查对应 ImGui 弹窗标识。
        for ( const auto& menu : m_registeredMenus ) {
            if ( menu && ImGui::IsPopupOpen(menu->label(context)) ) {
                mainMenuPopupOpen = true;
                break;
            }
        }
        if ( mainMenuPopupOpen ) {
            // 一级菜单之间仍可通过 Alt 组合切换或关闭。
            m_navigationController.handleShortcuts();
        }
        // 其他弹窗打开时禁止编辑快捷键穿透到菜单动作。
        return;
    }

    // 按注册顺序消费快捷键，首个命中项阻止后续重复执行。
    for ( auto& menu : m_registeredMenus ) {
        if ( menu && menu->handleShortcut(context) ) return;
    }

    // 没有业务动作消费按键后，再处理一级菜单导航。
    m_navigationController.handleShortcuts();
}

/// @brief 遍历更新已注册菜单持有的 action 状态。
/// @param sourceManager 当前 UI 管理器。
/// @param statusMessageSink 状态消息接收接口。
/// @warning UI 热路径：每帧执行；不得在菜单更新中引入阻塞或文件系统操作。
/// @note 上下文只在本次调用有效，菜单项不得跨帧保存其引用。
void MainMenuView::update(UIManager*          sourceManager,
                          IStatusMessageSink& statusMessageSink)
{
    // 每帧从配置读取内容缩放，使运行时 DPI 变化立即传递给菜单动作。
    MainMenuContext context{
        .statusMessageSink = statusMessageSink,
        .sourceManager     = sourceManager,
        .dpiScale = Config::AppConfig::instance().getWindowContentScale(),
    };
    // 空槽位被容忍，便于注册表构建失败时保持其余菜单可用。
    for ( auto& menu : m_registeredMenus ) {
        if ( menu ) menu->update(context);
    }
}

/// @brief 遍历绘制已注册的一级菜单接口。
/// @param sourceManager 当前 UI 管理器。
/// @param statusMessageSink 状态消息接收接口。
/// @warning UI 热路径：每帧执行；样式栈与字体栈必须严格配对。
/// @note 一级菜单打开和关闭请求在对应菜单绘制点消费。
void MainMenuView::renderMenus(UIManager*          sourceManager,
                               IStatusMessageSink& statusMessageSink)
{
    // 同一帧复用缩放快照，保证菜单内边距和上下文数值一致。
    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    MainMenuContext context{
        .statusMessageSink = statusMessageSink,
        .sourceManager     = sourceManager,
        .dpiScale          = dpiScale,
    };
    // 在构建菜单弹窗前收集快捷键请求，供本帧逐项消费。
    handleHotkeys(context);

    // 菜单样式只覆盖主菜单绘制作用域，不泄漏到后续 UI。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(8.0f * dpiScale, 8.0f * dpiScale));
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(6.0f * dpiScale, ImGui::GetStyle().FramePadding.y));

    ImFont* menuFont = Config::SkinManager::instance().getFont("menu");
    // 皮肤未配置专用字体时沿用当前 ImGui 字体。
    if ( menuFont ) ImGui::PushFont(menuFont, menuFont->LegacySize);

    for ( auto& menu : m_registeredMenus ) {
        if ( !menu ) continue;

        const MainMenuId menuId    = menu->id();
        const char*      menuLabel = menu->label(context);
        // Alt 导航登记的请求必须在 BeginMenu 前转换为 ImGui 打开操作。
        if ( m_navigationController.consumeOpenRequest(menuId) ) {
            ImGui::OpenPopup(menuLabel);
        }

        const bool menuOpen = ::MMM::UI::FeedbackBeginMenu(menuLabel);
        // BeginMenu 刚提交的 LastItem 就是一级菜单按钮，须在弹窗内容前捕获。
        if ( sourceManager ) {
            const auto target    = walkthroughTarget(menuId);
            auto&      spotlight = sourceManager->walkthroughSpotlight();
            spotlight.reportLastItem(target);
            if ( menuOpen )
                // 菜单实际展开是一级入口成功，不用鼠标位置推测点击结果。
                spotlight.completeTarget(target);
        }
        if ( menuOpen ) {
            // 关闭请求优先于内容渲染，避免关闭帧仍触发菜单项。
            if ( m_navigationController.consumeCloseRequest(menuId) ) {
                ImGui::CloseCurrentPopup();
            } else {
                menu->render(context);
            }
            ::MMM::UI::FeedbackEndMenu();
        }
    }

    // 只在此前成功压入专用字体时恢复字体栈。
    if ( menuFont ) ImGui::PopFont();
    // 一次性弹出本函数压入的两个样式变量。
    ImGui::PopStyleVar(2);
}

/// @brief 遍历渲染菜单 action 触发的延迟窗口和弹窗。
/// @param sourceManager 当前 UI 管理器。
/// @param dpiScale 当前窗口内容缩放。
/// @param statusMessageSink 状态消息接收接口。
/// @warning UI 热路径：每帧执行；阻塞操作只能来自用户明确点击。
/// @note 延迟窗口在主菜单栏作用域外绘制，避免 ImGui 窗口嵌套错误。
void MainMenuView::renderDeferredPopups(UIManager*          sourceManager,
                                        float               dpiScale,
                                        IStatusMessageSink& statusMessageSink)
{
    // 延迟阶段使用调用方已确定的缩放值，保持与外层 UI 帧一致。
    MainMenuContext context{
        .statusMessageSink = statusMessageSink,
        .sourceManager     = sourceManager,
        .dpiScale          = dpiScale,
    };

    ImFont* menuFont = Config::SkinManager::instance().getFont("menu");
    // 弹窗沿用菜单字体；缺失时安全回退到当前字体。
    if ( menuFont ) ImGui::PushFont(menuFont, menuFont->LegacySize);

    // 每个一级菜单负责转发到其动作处理器持有的延迟视图。
    for ( auto& menu : m_registeredMenus ) {
        if ( menu ) menu->renderDeferred(context);
    }

    // 与条件 PushFont 严格配对，保持调用者字体栈不变。
    if ( menuFont ) ImGui::PopFont();
}

}  // namespace MMM::UI
