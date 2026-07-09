#include "ui/imgui/menu/MainMenuView.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/BeatmapSaveResultEvent.h"
#include "mmm/project/PackageFileTypes.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <concurrentqueue.h>
#include <filesystem>
#include <imgui.h>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 跨线程传递给 UI 帧内消费的保存提示载荷。
struct SaveTooltipPayload {
    /// @brief 目标文件路径，使用 UTF-8 字符串。
    std::string path;
    /// @brief 是否为成功状态。
    bool success{ true };
    /// @brief 是否来自另存为/导出流程。
    bool isExport{ false };
};

/// @brief 获取保存结果提示队列。
moodycamel::ConcurrentQueue<SaveTooltipPayload>& getSaveTooltipQueue()
{
    static moodycamel::ConcurrentQueue<SaveTooltipPayload> queue;
    return queue;
}

/// @brief 根据保存结果事件构建用户可见的提示文本。
std::string buildSaveTooltipMessage(const SaveTooltipPayload& payload)
{
    auto       path      = Config::utf8ToPath(payload.path);
    const bool isPackage = findPackageSupportedFileTypes(
                               Config::pathToUtf8(path.extension())) != nullptr;
    if ( !payload.success ) {
        if ( isPackage ) return "打包失败";
        return payload.isExport ? "导出失败" : "保存失败";
    }
    if ( !payload.isExport ) {
        return TR("ui.status.beatmap.saved").data();
    }

    std::string fileName = Config::pathToUtf8(path.filename());
    if ( fileName.empty() ) {
        if ( isPackage ) return "打包成功";
        return "导出成功";
    }
    if ( isPackage ) {
        return "打包 " + fileName + " 成功";
    }
    return "导出 " + fileName + " 成功";
}

/// @brief 订阅保存结果事件，并将 Tooltip 负载排队到 UI 线程。
void ensureSaveResultSubscription()
{
    static bool subscribed = false;
    if ( subscribed ) return;

    Event::EventBus::instance().subscribe<Event::BeatmapSaveResultEvent>(
        [](const Event::BeatmapSaveResultEvent& event) {
            getSaveTooltipQueue().enqueue(SaveTooltipPayload{
                .path     = event.path,
                .success  = event.success,
                .isExport = event.isExport,
            });
        });
    subscribed = true;
}

}  // namespace

/// @brief 构造主菜单视图并初始化菜单注册表和通用反馈订阅。
MainMenuView::MainMenuView() : m_registeredMenus(createDefaultMainMenus())
{
    ensureSaveResultSubscription();
}

/// @brief 销毁主菜单视图。
MainMenuView::~MainMenuView() {}

/// @brief 显示状态栏临时消息。
/// @param message 状态消息文本。
/// @param durationSeconds 显示时长，单位秒。
void MainMenuView::showStatusMessage(std::string message, float durationSeconds)
{
    m_statusMessage      = std::move(message);
    m_statusMessageTimer = durationSeconds;
}

/// @brief 请求下一帧打开指定一级菜单。
/// @param id 一级菜单标识。
void MainMenuView::requestMenuOpen(MainMenuId id)
{
    const std::size_t index = mainMenuIdIndex(id);
    if ( index >= m_openMenuNextFrame.size() ) return;
    m_openMenuNextFrame[index] = true;
}

/// @brief 请求下一帧关闭指定一级菜单。
/// @param id 一级菜单标识。
void MainMenuView::requestMenuClose(MainMenuId id)
{
    const std::size_t index = mainMenuIdIndex(id);
    if ( index >= m_closeMenuNextFrame.size() ) return;
    m_closeMenuNextFrame[index] = true;
}

/// @brief 消费指定一级菜单的打开请求。
/// @param id 一级菜单标识。
/// @return 本帧存在打开请求时返回 true。
bool MainMenuView::consumeMenuOpenRequest(MainMenuId id)
{
    const std::size_t index = mainMenuIdIndex(id);
    if ( index >= m_openMenuNextFrame.size() ) return false;

    const bool requested       = m_openMenuNextFrame[index];
    m_openMenuNextFrame[index] = false;
    return requested;
}

/// @brief 消费指定一级菜单的关闭请求。
/// @param id 一级菜单标识。
/// @return 本帧存在关闭请求时返回 true。
bool MainMenuView::consumeMenuCloseRequest(MainMenuId id)
{
    const std::size_t index = mainMenuIdIndex(id);
    if ( index >= m_closeMenuNextFrame.size() ) return false;

    const bool requested        = m_closeMenuNextFrame[index];
    m_closeMenuNextFrame[index] = false;
    return requested;
}

/// @brief 处理主菜单相关的全局快捷键。
/// @param sourceManager 当前 UI 管理器，用于打开向导或访问视图。
void MainMenuView::handleHotkeys(UIManager* sourceManager)
{
    ImGuiIO& io = ImGui::GetIO();

    // 如果 ImGui 当前处于文本输入状态，跳过全局快捷键以避免穿透输入框。
    if ( io.WantTextInput ) return;
    if ( ShortcutUtils::isShortcutRecordingActive() ) return;

    MainMenuContext context{
        .view          = *this,
        .sourceManager = sourceManager,
        .dpiScale      = Config::AppConfig::instance().getWindowContentScale(),
    };
    for ( auto& menu : m_registeredMenus ) {
        if ( menu && menu->handleShortcut(context) ) return;
    }

    if ( io.KeyAlt ) {
        if ( ImGui::IsKeyPressed(ImGuiKey_F, false) ) {
            if ( ImGui::IsPopupOpen(TR("ui.file")) ) {
                requestMenuClose(MainMenuId::File);
            } else {
                requestMenuOpen(MainMenuId::File);
            }
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_E, false) ) {
            if ( ImGui::IsPopupOpen(TR("ui.edit")) ) {
                requestMenuClose(MainMenuId::Edit);
            } else {
                requestMenuOpen(MainMenuId::Edit);
            }
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_T, false) ) {
            if ( ImGui::IsPopupOpen(TR("ui.tools")) ) {
                requestMenuClose(MainMenuId::Tools);
            } else {
                requestMenuOpen(MainMenuId::Tools);
            }
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_V, false) ) {
            if ( ImGui::IsPopupOpen(TR("ui.view")) ) {
                requestMenuClose(MainMenuId::View);
            } else {
                requestMenuOpen(MainMenuId::View);
            }
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_H, false) ) {
            if ( ImGui::IsPopupOpen(TR("ui.help")) ) {
                requestMenuClose(MainMenuId::Help);
            } else {
                requestMenuOpen(MainMenuId::Help);
            }
        }
    }
}

/// @brief 更新主菜单计时器、弹窗和启动检查状态。
/// @param sourceManager 当前 UI 管理器。
void MainMenuView::update(UIManager* sourceManager)
{
    MainMenuContext context{
        .view          = *this,
        .sourceManager = sourceManager,
        .dpiScale      = Config::AppConfig::instance().getWindowContentScale(),
    };
    for ( auto& menu : m_registeredMenus ) {
        if ( menu ) {
            menu->update(context);
        }
    }

    SaveTooltipPayload payload;
    while ( getSaveTooltipQueue().try_dequeue(payload) ) {
        m_saveTooltipMessage = buildSaveTooltipMessage(payload);
        m_saveTooltipSuccess = payload.success;
        m_saveTooltipTimer   = payload.success ? 2.0f : 3.0f;
    }

    if ( m_statusMessageTimer > 0.0f )
        m_statusMessageTimer -= ImGui::GetIO().DeltaTime;

    renderSaveTooltip();
}

/// @brief 渲染首次启动 PGO 性能数据上传授权弹窗。
/// @param dpiScale 当前窗口内容缩放。
void MainMenuView::renderPgoUploadConsentWindow(float dpiScale)
{
#ifndef MMM_PGO_INSTRUMENT
    (void)dpiScale;
    return;
#else
    auto& appConfig = Config::AppConfig::instance();
    auto& settings  = appConfig.getEditorSettings();
    if ( settings.pgoProfileUploadConsentAsked ) return;

    const std::string popupId = std::string(TR("ui.pgo.consent.title").data()) +
                                "###PgoUploadConsentModal";
    ImGui::OpenPopup(popupId.c_str());

    {
        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId.c_str(),
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(620.0f * dpiScale, 0.0f)) ) {
            ImGui::TextWrapped("%s", TR("ui.pgo.consent.message").data());
            ImGui::Spacing();
            ImGui::TextWrapped("%s", TR("ui.pgo.consent.detail").data());

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            auto applyConsent = [&](bool allowUpload) {
                settings.autoUploadPgoProfiles        = allowUpload;
                settings.pgoProfileUploadConsentAsked = true;
                appConfig.save();
                ImGui::CloseCurrentPopup();
            };

            const ImGuiStyle& style = ImGui::GetStyle();
            const ImVec2      buttonSize(128.0f * dpiScale, 0.0f);
            const float       buttonRowWidth =
                buttonSize.x * 2.0f + style.ItemSpacing.x;
            const float availableWidth = ImGui::GetContentRegionAvail().x;
            if ( availableWidth > buttonRowWidth ) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                     (availableWidth - buttonRowWidth) * 0.5f);
            }

            if ( ::MMM::UI::FeedbackButton(TR("ui.pgo.consent.accept").data(),
                                           buttonSize) ) {
                applyConsent(true);
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.pgo.consent.decline").data(),
                                           buttonSize) ) {
                applyConsent(false);
            }

            ImGui::EndPopup();
        }
    }
#endif
}

/// @brief 遍历已注册的一级菜单接口。
/// @param sourceManager 当前 UI 管理器，用于菜单项 action handler
/// 打开对应视图。
void MainMenuView::renderMenus(UIManager* sourceManager)
{
    handleHotkeys(sourceManager);

    Config::SkinManager& skinCfg = Config::SkinManager::instance();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(8.0f * dpiScale, 8.0f * dpiScale));
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(6.0f * dpiScale, ImGui::GetStyle().FramePadding.y));

    MainMenuContext context{
        .view          = *this,
        .sourceManager = sourceManager,
        .dpiScale      = dpiScale,
    };

    ImFont* menuFont = skinCfg.getFont("menu");
    if ( menuFont ) ImGui::PushFont(menuFont, menuFont->LegacySize);

    for ( auto& menu : m_registeredMenus ) {
        if ( !menu ) continue;

        const MainMenuId menuId    = menu->id();
        const char*      menuLabel = menu->label(context);
        if ( consumeMenuOpenRequest(menuId) ) {
            ImGui::OpenPopup(menuLabel);
        }

        if ( ::MMM::UI::FeedbackBeginMenu(menuLabel) ) {
            const bool shouldClose = consumeMenuCloseRequest(menuId);
            if ( shouldClose ) {
                ImGui::CloseCurrentPopup();
            } else {
                menu->render(context);
            }
            ::MMM::UI::FeedbackEndMenu();
        }
    }

    if ( menuFont ) ImGui::PopFont();
    ImGui::PopStyleVar(2);  // Pop WindowPadding and FramePadding
}

/// @brief 渲染由主菜单触发但必须位于菜单栏窗口外的弹窗和辅助窗口。
/// @param sourceManager 当前 UI 管理器，用于消费菜单延迟动作。
/// @param dpiScale 当前窗口内容缩放。
/// @warning UI 热路径：每帧执行；只允许消费已置位菜单动作并渲染可见弹窗，
/// 文件选择器等阻塞操作只能来自用户明确点击。
void MainMenuView::renderDeferredPopups(UIManager* sourceManager,
                                        float      dpiScale)
{
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    ImFont*              menuFont = skinCfg.getFont("menu");
    if ( menuFont ) ImGui::PushFont(menuFont, menuFont->LegacySize);

    renderPgoUploadConsentWindow(dpiScale);

    MainMenuContext context{
        .view          = *this,
        .sourceManager = sourceManager,
        .dpiScale      = dpiScale,
    };
    for ( auto& menu : m_registeredMenus ) {
        if ( menu ) {
            menu->renderDeferred(context);
        }
    }

    if ( menuFont ) ImGui::PopFont();
}

/// @brief 渲染底部提示文本占位区域。
void MainMenuView::renderInfoText()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("MusicMapMaker(Gamma)");
    ImGui::SameLine();
    ImGui::Text(
        "%.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
}

}  // namespace MMM::UI
