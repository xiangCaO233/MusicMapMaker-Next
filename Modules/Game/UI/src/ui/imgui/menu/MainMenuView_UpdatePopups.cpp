#define IMGUI_DEFINE_MATH_OPERATORS
#include "canvas/TimeFormatUtils.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/project/ProjectEvents.h"
#include "event/ui/UISettingsTabEvent.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Hold.h"
#include "mmm/note/Polyline.h"
#include "mmmversion.h"
#include "network/UpdateChecker.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/imgui/menu/MainMenuView.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <imgui_internal.h>
#include <nfd.h>

namespace MMM::UI
{

/// @brief 启动一次非静默更新检查并打开检查中弹窗。
void MainMenuView::startUpdateCheck()
{
    m_isSilentCheck       = false;
    m_showCheckingPopup   = true;
    m_updatePopupCanceled = false;
    m_updateChecker->checkAsync();
}

/// @brief 渲染帮助菜单及其菜单项。
/// @param sourceManager 当前 UI 管理器；保留用于后续帮助菜单扩展。
void MainMenuView::renderHelpMenu(UIManager* sourceManager)
{
    auto MenuItemWithFontIcon = [](const char* icon,
                                   const char* label,
                                   const char* shortcut = nullptr,
                                   bool        enabled  = true) -> bool {
        ImVec4 iconVec4 = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text, iconVec4);

        float gap = ImGui::CalcTextSize(" ").x * 0.5f;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(gap, 0));

        const char* iconPtr = icon ? icon : "  ";

        bool clicked =
            ImGui::MenuItemEx(label, iconPtr, shortcut, false, enabled);

        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return clicked;
    };

    if ( m_openHelpMenuNextFrame ) {
        ImGui::OpenPopup(TR("ui.help"));
        m_openHelpMenuNextFrame = false;
    }
    if ( ImGui::BeginMenu(TR("ui.help")) ) {
        if ( m_closeHelpMenuNextFrame ) {
            ImGui::CloseCurrentPopup();
            m_closeHelpMenuNextFrame = false;
        }

        if ( MenuItemWithFontIcon(ICON_MMM_DOWNLOAD,
                                  TR("ui.help.check_update")) ) {
            startUpdateCheck();
        }
        if ( MenuItemWithFontIcon(ICON_MMM_INFO_CIRCLE, TR("ui.help.about")) ) {
            m_showAboutPopup = true;
        }
        ImGui::EndMenu();
    }
}

/// @brief 渲染关于弹窗。
/// @warning UI 每帧调用路径；仅在用户点击外部链接时触发系统浏览器打开。
void MainMenuView::renderAboutPopup()
{
    if ( m_showAboutPopup ) {
        ImGui::OpenPopup(TR("ui.help.about_title"));
        m_showAboutPopup = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    {
        static bool wasOpen = false;
        bool        isOpen  = ImGui::IsPopupOpen(TR("ui.help.about_title"));
        if ( isOpen && !wasOpen ) {
            ImGui::SetNextWindowPos(
                center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        }
        wasOpen = isOpen;
    }

    /// @brief 当前窗口内容缩放倍率，用于将全局审美配置换算为实际像素。
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    /// @brief 当前 ImGui 主视口，用于按可见工作区限制关于弹窗尺寸。
    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    /// @brief 关于弹窗与主视口边缘保留的最小空隙。
    const float aboutWindowMargin = 32.0f * dpiScale;
    /// @brief 关于弹窗的目标尺寸，按当前内容预留足够高度避免鸣谢区被裁切。
    const ImVec2 desiredAboutWindowSize{ 680.0f * dpiScale, 520.0f * dpiScale };
    /// @brief 关于弹窗允许占用的最大尺寸，防止小窗口或高 DPI 下完全越界。
    const ImVec2 availableAboutWindowSize{
        std::max(360.0f * dpiScale,
                 mainViewport->WorkSize.x - aboutWindowMargin),
        std::max(360.0f * dpiScale,
                 mainViewport->WorkSize.y - aboutWindowMargin),
    };
    /// @brief 本次弹出实际应用的尺寸，优先满足内容完整展示。
    const ImVec2 aboutWindowSize{
        std::min(desiredAboutWindowSize.x, availableAboutWindowSize.x),
        std::min(desiredAboutWindowSize.y, availableAboutWindowSize.y),
    };
    /// @brief 编辑器全局审美配置，关于弹窗也必须遵循同一套窗口样式。
    const auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    /// @brief 全局窗口圆角的 DPI 后像素值。
    float windowRound = std::floor(aesthetics.windowRounding * dpiScale);
    /// @brief 全局控件圆角的 DPI 后像素值。
    float frameRound = std::floor(aesthetics.frameRounding * dpiScale);
    /// @brief 全局窗口内边距的 DPI 后像素值。
    ImVec2 windowPadding{ std::floor(aesthetics.windowPadding * dpiScale),
                          std::floor(aesthetics.windowPadding * dpiScale) };
    /// @brief 全局控件间距的 DPI 后像素值。
    ImVec2 itemSpacing{ std::floor(aesthetics.itemSpacing * dpiScale),
                        std::floor(aesthetics.itemSpacing * dpiScale) };

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, windowPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, itemSpacing);

    /// @brief 标题字体仅用于 ImGui 窗口标题栏，Begin 后立即恢复内容字体。
    ImFont* windowTitleFont = Config::SkinManager::instance().getFont("title");
    /// @brief 关于弹窗窗口标志，禁用历史尺寸缓存并根据内容自动调整窗口大小。
    const ImGuiWindowFlags aboutPopupFlags =
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    ImGui::SetNextWindowSize(aboutWindowSize, ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(aboutWindowSize,
                                        availableAboutWindowSize);
    if ( windowTitleFont ) ImGui::PushFont(windowTitleFont);
    /// @brief 关于弹窗是否已成功开始渲染。
    bool popupOpen = ImGui::BeginPopupModal(
        TR("ui.help.about_title"), nullptr, aboutPopupFlags);
    if ( windowTitleFont ) ImGui::PopFont();

    if ( popupOpen ) {

        // --- Logo & Title ---
        ImFont* titleFont = Config::SkinManager::instance().getFont("menu");
        if ( titleFont ) ImGui::PushFont(titleFont, 0.0f);

        std::string appLabel =
            std::string(ICON_MMM_MUSIC) + "  " + TR("ui.help.app_name").data();
        float titleWidth = ImGui::CalcTextSize(appLabel.c_str()).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - titleWidth) * 0.5f);
        ImGui::TextColored(
            ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "%s", appLabel.c_str());

        if ( titleFont ) ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Info Table ---
        if ( ImGui::BeginTable("AboutTable",
                               2,
                               ImGuiTableFlags_SizingFixedFit |
                                   ImGuiTableFlags_NoSavedSettings) ) {
            ImGui::TableSetupColumn(
                "L", ImGuiTableColumnFlags_WidthFixed, 140.0f * dpiScale);
            ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthStretch);

            auto AddRow = [&](const char* label, const char* value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(value);
                ImGui::PopStyleColor();
            };

            AddRow(TR("ui.help.current_version").data(), MMM_VERSION_STRING);

#if BUILD_TYPE_DEBUG
            AddRow(TR("ui.help.build_type").data(), "Debug");
#else
            AddRow(TR("ui.help.build_type").data(), "Release");
#endif
            AddRow(TR("ui.help.platform").data(), MMM_PLATFORM);

            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Special Thanks ---
        /// @brief 特别鸣谢标题文本。
        const char* thanksTitle = TR("ui.help.special_thanks").data();
        /// @brief 特别鸣谢用户名称文本。
        const char* thanksName = TR("ui.help.special_thanks_bassor").data();
        /// @brief 特别鸣谢标题文本宽度。
        float thanksTitleWidth = ImGui::CalcTextSize(thanksTitle).x;
        /// @brief 特别鸣谢用户名称文本宽度。
        float nameWidth = ImGui::CalcTextSize(thanksName).x;
        /// @brief 特别鸣谢标题和用户名称之间的间距。
        float titleGap = ImGui::GetStyle().ItemSpacing.x;
        /// @brief 特别鸣谢标题行的总宽度，用于居中排版。
        float thanksHeaderWidth = thanksTitleWidth + titleGap + nameWidth;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - thanksHeaderWidth) *
                             0.5f);
        ImGui::TextUnformatted(thanksTitle);
        ImGui::SameLine(0.0f, titleGap);

        /// @brief 特别鸣谢姓名链接色，点击打开用户主页，悬停提示联系方式。
        ImVec4 linkColor(0.35f, 0.65f, 1.0f, 1.0f);
        /// @brief Bassor 用户主页地址，点击姓名链接时交给系统默认浏览器打开。
        constexpr const char* bassorProfileUrl =
            "https://malody.mugzone.net/player/1676762";
        ImGui::TextColored(linkColor, "%s", thanksName);
        /// @brief 特别鸣谢链接文本的左上角坐标，用于绘制下划线。
        ImVec2 linkMin = ImGui::GetItemRectMin();
        /// @brief 特别鸣谢链接文本的右下角坐标，用于绘制下划线。
        ImVec2 linkMax = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(linkMin.x, linkMax.y + 1.0f),
                                            ImVec2(linkMax.x, linkMax.y + 1.0f),
                                            ImGui::GetColorU32(linkColor),
                                            std::max(1.0f, dpiScale));
        if ( ImGui::IsItemClicked(ImGuiMouseButton_Left) ) {
            MMM::Network::UpdateChecker::openUrlInBrowser(bassorProfileUrl);
        }
        if ( ImGui::IsItemHovered() ) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip(
                "%s", TR("ui.help.special_thanks_bassor_contact").data());
        }

        /// @brief 特别鸣谢说明文本的最大换行宽度。
        float thanksTextWidth =
            std::min(420.0f * dpiScale, ImGui::GetContentRegionAvail().x);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - thanksTextWidth) *
                             0.5f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + thanksTextWidth);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s",
                           TR("ui.help.special_thanks_bassor_desc").data());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();

        ImGui::Spacing();

        /// @brief Mizar 用户主页地址，点击姓名链接时交给系统默认浏览器打开。
        constexpr const char* mizarProfileUrl =
            "https://space.bilibili.com/102030000";
        /// @brief 鸣谢名单中姓名之间使用的本地化分隔符。
        const char* thanksSeparator =
            TR("ui.help.special_thanks_separator").data();
        /// @brief 长期测试鸣谢分组标题。
        const char* longTermTestingTitle =
            TR("ui.help.special_thanks_long_term_testing").data();
        /// @brief 谱面支持鸣谢分组标题。
        const char* beatmapSupportTitle =
            TR("ui.help.special_thanks_beatmap_support").data();
        /// @brief Mizar 鸣谢用户名称文本。
        const char* mizarName = TR("ui.help.special_thanks_mizar").data();
        /// @brief 凌云归故里鸣谢用户名称文本。
        const char* lingyunName = TR("ui.help.special_thanks_lingyun").data();
        /// @brief 修罗7 鸣谢用户名称文本。
        const char* xiuluoName = TR("ui.help.special_thanks_xiuluo7").data();
        /// @brief Mzメ 夜明けの未来ˇ 鸣谢用户名称文本。
        const char* mzYoakeName = TR("ui.help.special_thanks_mz_yoake").data();

        /// @brief 渲染可点击的鸣谢姓名链接。
        /// @param labelName 要显示的鸣谢姓名。
        /// @param tooltipText 鼠标悬停时显示的联系方式。
        /// @param profileUrl 点击姓名后打开的用户主页。
        auto renderThanksLink = [&](const char* labelName,
                                    const char* tooltipText,
                                    const char* profileUrl) {
            ImGui::TextColored(linkColor, "%s", labelName);
            /// @brief 当前姓名链接文本的左上角坐标，用于绘制下划线。
            ImVec2 currentLinkMin = ImGui::GetItemRectMin();
            /// @brief 当前姓名链接文本的右下角坐标，用于绘制下划线。
            ImVec2 currentLinkMax = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(currentLinkMin.x, currentLinkMax.y + 1.0f),
                ImVec2(currentLinkMax.x, currentLinkMax.y + 1.0f),
                ImGui::GetColorU32(linkColor),
                std::max(1.0f, dpiScale));
            if ( ImGui::IsItemClicked(ImGuiMouseButton_Left) ) {
                MMM::Network::UpdateChecker::openUrlInBrowser(profileUrl);
            }
            if ( ImGui::IsItemHovered() ) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s", tooltipText);
            }
        };

        /// @brief 渲染没有额外联系方式或主页的鸣谢姓名。
        /// @param labelName 要显示的鸣谢姓名。
        auto renderPlainThanksName = [](const char* labelName) {
            ImGui::TextUnformatted(labelName);
        };

        /// @brief 在同一行中追加姓名分隔符。
        auto renderThanksSeparator = [&]() {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(thanksSeparator);
            ImGui::SameLine(0.0f, 0.0f);
        };

        /// @brief 渲染鸣谢分组表格中的左侧分组标题。
        /// @param groupTitle 要显示的分组标题。
        auto renderThanksGroupTitle = [](const char* groupTitle) {
            ImGui::PushStyleColor(
                ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(groupTitle);
            ImGui::PopStyleColor();
        };

        if ( ImGui::BeginTable("SpecialThanksRows",
                               2,
                               ImGuiTableFlags_SizingFixedFit |
                                   ImGuiTableFlags_NoSavedSettings) ) {
            ImGui::TableSetupColumn(
                "Role", ImGuiTableColumnFlags_WidthFixed, 96.0f * dpiScale);
            ImGui::TableSetupColumn("Names",
                                    ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            renderThanksGroupTitle(longTermTestingTitle);
            ImGui::TableNextColumn();
            renderThanksLink(mizarName,
                             TR("ui.help.special_thanks_mizar_contact").data(),
                             mizarProfileUrl);
            renderThanksSeparator();
            renderPlainThanksName(lingyunName);
            renderThanksSeparator();
            renderThanksLink(thanksName,
                             TR("ui.help.special_thanks_bassor_contact").data(),
                             bassorProfileUrl);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            renderThanksGroupTitle(beatmapSupportTitle);
            ImGui::TableNextColumn();
            renderPlainThanksName(xiuluoName);
            renderThanksSeparator();
            renderThanksLink(mizarName,
                             TR("ui.help.special_thanks_mizar_contact").data(),
                             mizarProfileUrl);
            renderThanksSeparator();
            renderThanksLink(thanksName,
                             TR("ui.help.special_thanks_bassor_contact").data(),
                             bassorProfileUrl);
            renderThanksSeparator();
            renderPlainThanksName(mzYoakeName);

            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Copyright ---
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const char* copyright =
            "Copyright (C) 2025 xiang233. All rights reserved.";
        float cpWidth = ImGui::CalcTextSize(copyright).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - cpWidth) * 0.5f);
        ImGui::TextUnformatted(copyright);
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // --- Button ---
        float btnWidth = 140.0f * dpiScale;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
        if ( ImGui::Button(TR("ui.help.ok").data(),
                           ImVec2(btnWidth, 36.0f * dpiScale)) ) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(7);
}

/// @brief 渲染更新检查中的状态弹窗。
void MainMenuView::renderUpdateCheckingPopup()
{
    if ( m_showCheckingPopup ) {
        ImGui::OpenPopup(TR("ui.help.check_update"));
        m_showCheckingPopup = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    {
        static bool wasOpen = false;
        bool        isOpen  = ImGui::IsPopupOpen(TR("ui.help.check_update"));
        if ( isOpen && !wasOpen ) {
            ImGui::SetNextWindowPos(
                center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        }
        wasOpen = isOpen;
    }

    bool open = true;
    if ( ImGui::BeginPopupModal(
             TR("ui.help.check_update"), &open, ImGuiWindowFlags_None) ) {
        if ( !open ) ImGui::CloseCurrentPopup();
        auto  info     = m_updateChecker->getInfo();
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(32.0f * dpiScale, 24.0f * dpiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(8.0f * dpiScale, 16.0f * dpiScale));

        if ( info.status == MMM::Network::UpdateStatus::kChecking ) {
            float textWidth =
                ImGui::CalcTextSize(TR("ui.help.checking").data()).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
            ImGui::TextUnformatted(TR("ui.help.checking").data());

            float barWidth = 240.0f * dpiScale;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - barWidth) * 0.5f);
            float fraction = -1.0f * (float)ImGui::GetTime();
            ImGui::ProgressBar(
                fraction, ImVec2(barWidth, 12.0f * dpiScale), "");
        } else if ( info.status == MMM::Network::UpdateStatus::kUpToDate ) {
            float textWidth =
                ImGui::CalcTextSize(TR("ui.help.up_to_date").data()).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
            ImGui::TextUnformatted(TR("ui.help.up_to_date").data());

            float btnWidth = 120.0f * dpiScale;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
            if ( ImGui::Button(TR("ui.help.ok").data(),
                               ImVec2(btnWidth, 32.0f * dpiScale)) ) {
                ImGui::CloseCurrentPopup();
            }
        } else if ( info.status == MMM::Network::UpdateStatus::kUpdateFound ) {
            ImGui::CloseCurrentPopup();
            m_showUpdatePopup = true;
        } else if ( info.status == MMM::Network::UpdateStatus::kError ) {
            ImVec4 errColor(1.0f, 0.4f, 0.4f, 1.0f);
            float  textWidth =
                ImGui::CalcTextSize(TR("ui.help.update_error").data()).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
            ImGui::TextColored(
                errColor, "%s", TR("ui.help.update_error").data());

            ImGui::PushStyleColor(
                ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", info.errorMessage.c_str());
            ImGui::PopStyleColor();

            float btnWidth = 120.0f * dpiScale;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
            if ( ImGui::Button(TR("ui.help.ok").data(),
                               ImVec2(btnWidth, 32.0f * dpiScale)) ) {
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::PopStyleVar(2);
        ImGui::EndPopup();
    }

    if ( m_showUpdatePopup ) {
        m_showCheckingPopup = false;
    }
}

/// @brief 渲染发现更新后的下载确认与下载进度弹窗。
void MainMenuView::renderUpdatePopup()
{
    auto info = m_updateChecker->getInfo();

    if ( m_showUpdatePopup ) {
        ImGui::OpenPopup(TR("ui.help.update_found"));
        m_showUpdatePopup = false;
    } else if ( info.status == MMM::Network::UpdateStatus::kUpdateFound &&
                !m_updatePopupCanceled ) {
        if ( !ImGui::IsPopupOpen(TR("ui.help.update_found")) )
            ImGui::OpenPopup(TR("ui.help.update_found"));
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    {
        static bool wasOpen = false;
        bool        isOpen  = ImGui::IsPopupOpen(TR("ui.help.update_found"));
        if ( isOpen && !wasOpen ) {
            ImGui::SetNextWindowPos(
                center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        }
        wasOpen = isOpen;
    }

    bool isWorking = (info.status == MMM::Network::UpdateStatus::kDownloading ||
                      info.status == MMM::Network::UpdateStatus::kDownloaded);
    bool open      = true;
    if ( ImGui::BeginPopupModal(TR("ui.help.update_found"),
                                isWorking ? nullptr : &open,
                                ImGuiWindowFlags_None) ) {
        if ( !open ) {
            ImGui::CloseCurrentPopup();
            m_updatePopupCanceled = true;
        }
        info           = m_updateChecker->getInfo();
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(32.0f * dpiScale, 24.0f * dpiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(8.0f * dpiScale, 16.0f * dpiScale));

        if ( info.status == MMM::Network::UpdateStatus::kUpdateFound ) {
            // --- Info Table ---
            if ( ImGui::BeginTable("UpdateInfoTable",
                                   2,
                                   ImGuiTableFlags_SizingFixedFit |
                                       ImGuiTableFlags_NoSavedSettings) ) {
                ImGui::TableSetupColumn(
                    "L", ImGuiTableColumnFlags_WidthFixed, 140.0f * dpiScale);
                ImGui::TableSetupColumn("R",
                                        ImGuiTableColumnFlags_WidthStretch);

                auto AddRow = [&](const char*   label,
                                  const char*   value,
                                  const ImVec4* color = nullptr) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(label);
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    if ( color )
                        ImGui::TextColored(*color, "%s", value);
                    else
                        ImGui::TextDisabled("%s", value);
                };

                AddRow(TR("ui.help.current_version").data(),
                       info.currentVersion.c_str());
                ImVec4 green(0.3f, 1.0f, 0.3f, 1.0f);
                AddRow(TR("ui.help.latest_version").data(),
                       info.latestVersion.c_str(),
                       &green);

                if ( !info.releaseDate.empty() )
                    AddRow(TR("ui.help.release_date").data(),
                           info.releaseDate.c_str());

                if ( info.downloadSize > 0 ) {
                    char sizeBuf[64];
                    if ( info.downloadSize >= 1024 * 1024 )
                        snprintf(sizeBuf,
                                 sizeof(sizeBuf),
                                 "%.1f MB",
                                 info.downloadSize / (1024.0 * 1024.0));
                    else if ( info.downloadSize >= 1024 )
                        snprintf(sizeBuf,
                                 sizeof(sizeBuf),
                                 "%.0f KB",
                                 info.downloadSize / 1024.0);
                    else
                        snprintf(sizeBuf,
                                 sizeof(sizeBuf),
                                 "%lld B",
                                 (long long)info.downloadSize);
                    AddRow(TR("ui.help.file_size").data(), sizeBuf);
                }
                ImGui::EndTable();
            }

            // --- Changelog ---
            if ( !info.changelog.empty() ) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextUnformatted(TR("ui.help.changelog").data());

                ImGui::BeginChild("ChangelogScroll",
                                  ImVec2(400.0f * dpiScale, 150.0f * dpiScale),
                                  ImGuiChildFlags_Borders);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    ImVec2(8.0f * dpiScale, 8.0f * dpiScale));
                ImGui::TextWrapped("%s", info.changelog.c_str());
                ImGui::PopStyleVar();
                ImGui::EndChild();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // --- Buttons ---
            float buttonWidth  = 140.0f * dpiScale;
            float totalButtons = info.downloadUrl.empty() ? 1.0f : 2.0f;
            float spacing      = ImGui::GetStyle().ItemSpacing.x;
            float totalWidth =
                totalButtons * buttonWidth + (totalButtons - 1) * spacing;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalWidth) * 0.5f);

            if ( !info.downloadUrl.empty() ) {
                if ( ImGui::Button(TR("ui.help.download_and_install").data(),
                                   ImVec2(buttonWidth, 36.0f * dpiScale)) ) {
                    m_updateChecker->downloadAsync();
                }
                ImGui::SameLine();
            }

            if ( ImGui::Button(TR("ui.help.cancel").data(),
                               ImVec2(buttonWidth, 36.0f * dpiScale)) ) {
                ImGui::CloseCurrentPopup();
                m_updatePopupCanceled = true;
            }
        } else if ( info.status == MMM::Network::UpdateStatus::kDownloading ) {
            float textWidth =
                ImGui::CalcTextSize(TR("ui.help.downloading").data()).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
            ImGui::TextUnformatted(TR("ui.help.downloading").data());

            float barWidth = 360.0f * dpiScale;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - barWidth) * 0.5f);
            ImGui::ProgressBar(static_cast<float>(info.downloadProgress),
                               ImVec2(barWidth, 20.0f * dpiScale));

            char progressText[128];
            if ( info.downloadSize > 0 ) {
                snprintf(progressText,
                         sizeof(progressText),
                         "%lld / %lld MB",
                         (long long)(info.downloadedBytes / (1024 * 1024)),
                         (long long)(info.downloadSize / (1024 * 1024)));
            } else {
                snprintf(progressText,
                         sizeof(progressText),
                         "%.0f%%",
                         info.downloadProgress * 100.0);
            }
            float pWidth = ImGui::CalcTextSize(progressText).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - pWidth) * 0.5f);
            ImGui::TextDisabled("%s", progressText);
        } else if ( info.status == MMM::Network::UpdateStatus::kDownloaded ) {
            float textWidth =
                ImGui::CalcTextSize(TR("ui.help.download_complete").data()).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
            ImGui::TextUnformatted(TR("ui.help.download_complete").data());

            float btnWidth = 200.0f * dpiScale;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
            if ( ImGui::Button(TR("ui.help.restart_to_update").data(),
                               ImVec2(btnWidth, 40.0f * dpiScale)) ) {
                MMM::Network::UpdateChecker::applyUpdateAndRestart(
                    info.downloadedFilePath, info.updaterFilePath);
            }
        } else if ( info.status == MMM::Network::UpdateStatus::kError ) {
            ImVec4 errColor(1.0f, 0.4f, 0.4f, 1.0f);
            float  textWidth =
                ImGui::CalcTextSize(TR("ui.help.update_error").data()).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
            ImGui::TextColored(
                errColor, "%s", TR("ui.help.update_error").data());

            ImGui::PushStyleColor(
                ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", info.errorMessage.c_str());
            ImGui::PopStyleColor();

            float btnWidth = 120.0f * dpiScale;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
            if ( ImGui::Button(TR("ui.help.ok").data(),
                               ImVec2(btnWidth, 32.0f * dpiScale)) ) {
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::PopStyleVar(2);
        ImGui::EndPopup();
    }
}

/// @brief 渲染更新下载成功后的提示弹窗。
void MainMenuView::renderUpdateSuccessPopup()
{
    if ( m_showUpdateSuccessPopup ) {
        ImGui::OpenPopup(TR("ui.help.update_success"));
        m_showUpdateSuccessPopup = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    {
        static bool wasOpen = false;
        bool        isOpen  = ImGui::IsPopupOpen(TR("ui.help.update_success"));
        if ( isOpen && !wasOpen ) {
            ImGui::SetNextWindowPos(
                center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        }
        wasOpen = isOpen;
    }

    if ( ImGui::BeginPopupModal(
             TR("ui.help.update_success"), nullptr, ImGuiWindowFlags_None) ) {
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(32.0f * dpiScale, 24.0f * dpiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(8.0f * dpiScale, 16.0f * dpiScale));

        ImVec4 greenColor(0.3f, 1.0f, 0.3f, 1.0f);
        float  textWidth =
            ImGui::CalcTextSize(TR("ui.help.update_success_msg").data()).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
        ImGui::TextColored(
            greenColor, "%s", TR("ui.help.update_success_msg").data());

        // --- Info Table ---
        if ( ImGui::BeginTable("SuccessInfoTable",
                               2,
                               ImGuiTableFlags_SizingFixedFit |
                                   ImGuiTableFlags_NoSavedSettings) ) {
            ImGui::TableSetupColumn(
                "L", ImGuiTableColumnFlags_WidthFixed, 140.0f * dpiScale);
            ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(TR("ui.help.current_version").data());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(greenColor, MMM_VERSION_STRING);

            ImGui::EndTable();
        }

        ImGui::Spacing();

        float btnWidth = 120.0f * dpiScale;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
        if ( ImGui::Button(TR("ui.help.ok").data(),
                           ImVec2(btnWidth, 32.0f * dpiScale)) ) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar(2);
        ImGui::EndPopup();
    }
}

/// @brief 渲染保存快捷提示气泡。
void MainMenuView::renderSaveTooltip()
{
    if ( m_saveTooltipTimer <= 0.0f ) return;

    m_saveTooltipTimer -= ImGui::GetIO().DeltaTime;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2         mousePos = ImGui::GetMousePos();

    // 始终跟随鼠标，并根据屏幕位置自动调整对齐方式（边缘翻转）
    ImVec2 pivot = ImVec2(0.0f, 0.0f);
    if ( mousePos.x > viewport->WorkPos.x + viewport->WorkSize.x * 0.7f )
        pivot.x = 1.0f;
    if ( mousePos.y > viewport->WorkPos.y + viewport->WorkSize.y * 0.7f )
        pivot.y = 1.0f;

    float offsetX = (pivot.x == 0.0f) ? 20.0f : -20.0f;
    float offsetY = (pivot.y == 0.0f) ? 20.0f : -20.0f;

    ImGui::SetNextWindowPos(ImVec2(mousePos.x + offsetX, mousePos.y + offsetY),
                            ImGuiCond_Always,
                            pivot);

    ImGui::SetNextWindowBgAlpha(0.8f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 10));

    if ( ImGui::Begin("##SaveTooltip", nullptr, flags) ) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                           "%s  %s",
                           ICON_MMM_SAVE,
                           TR("ui.status.beatmap.saved").data());
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

}  // namespace MMM::UI
