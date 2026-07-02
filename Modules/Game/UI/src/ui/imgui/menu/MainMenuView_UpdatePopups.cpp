#define IMGUI_DEFINE_MATH_OPERATORS
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "mmmversion.h"
#include "network/UpdateChecker.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/menu/MainMenuView.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>

namespace MMM::UI
{

namespace
{

/// @brief 生成关于窗口显示的构建编译器信息。
/// @return CMake 配置阶段识别到的 C++ 编译器 ID 与版本号。
std::string buildCompilerText()
{
    std::string       compilerText = MMM_BUILD_COMPILER_ID;
    const std::string versionText  = MMM_BUILD_COMPILER_VERSION;

    if ( compilerText.empty() ) {
        compilerText = "Unknown";
    }

    if ( !versionText.empty() ) {
        compilerText += " ";
        compilerText += versionText;
    }

    return compilerText;
}

}  // namespace

/// @brief 启动一次非静默更新检查并打开检查中弹窗。
void MainMenuView::startUpdateCheck()
{
    m_isSilentCheck       = false;
    m_showCheckingPopup   = true;
    m_updatePopupCanceled = false;
    m_updateRestartError.clear();
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

    /// @brief 当前窗口内容缩放倍率，用于将全局审美配置换算为实际像素。
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    /// @brief 当前 ImGui 主视口，用于按可见工作区限制关于弹窗尺寸。
    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    /// @brief 关于弹窗与主视口边缘保留的最小空隙。
    const float aboutWindowMargin = 32.0f * dpiScale;
    /// @brief 关于弹窗允许占用的最大尺寸，防止小窗口或高 DPI 下完全越界。
    const ImVec2 availableAboutWindowSize{
        std::max(360.0f * dpiScale,
                 mainViewport->WorkSize.x - aboutWindowMargin),
        std::max(360.0f * dpiScale,
                 mainViewport->WorkSize.y - aboutWindowMargin),
    };
    /// @brief 关于弹窗目标宽度；高度交给内容自适应，避免底部按钮被裁切。
    const ImVec2 aboutWindowSize{
        std::min(680.0f * dpiScale, availableAboutWindowSize.x),
        0.0f,
    };

    ImGui::SetNextWindowSizeConstraints(
        ImVec2(std::min(360.0f * dpiScale, aboutWindowSize.x), 0.0f),
        availableAboutWindowSize);

    Utils::CenteredModalPopupScope modalScope(dpiScale);
    /// @brief 关于弹窗是否已成功开始渲染。
    bool popupOpen = modalScope.begin(TR("ui.help.about_title"),
                                      nullptr,
                                      ImGuiWindowFlags_None,
                                      aboutWindowSize,
                                      true);

    if ( popupOpen ) {

        // --- Logo & Title ---
        ImFont* titleFont = Config::SkinManager::instance().getFont("menu");
        if ( titleFont ) ImGui::PushFont(titleFont, titleFont->LegacySize);

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

            const std::string compilerText = buildCompilerText();

            AddRow(TR("ui.help.current_version").data(), MMM_VERSION_STRING);

#if BUILD_TYPE_DEBUG
            AddRow(TR("ui.help.build_type").data(), "Debug");
#else
            AddRow(TR("ui.help.build_type").data(), "Release");
#endif
            AddRow(TR("ui.help.compiler").data(), compilerText.c_str());
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
        /// @brief
        /// 凌云归故里用户主页地址，点击姓名链接时交给系统默认浏览器打开。
        constexpr const char* lingyunProfileUrl =
            "https://space.bilibili.com/311780529";
        /// @brief x1u1u0233
        /// 用户主页地址，点击姓名链接时交给系统默认浏览器打开。
        constexpr const char* xiuluoProfileUrl =
            "https://space.bilibili.com/106515370";
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
        /// @brief yoAke 鸣谢用户名称文本。
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

        /// @brief 渲染可点击但没有联系方式提示的鸣谢姓名链接。
        /// @param labelName 要显示的鸣谢姓名。
        /// @param profileUrl 点击姓名后打开的用户主页。
        auto renderThanksProfileLink = [&](const char* labelName,
                                           const char* profileUrl) {
            ImGui::TextColored(linkColor, "%s", labelName);
            /// @brief 当前无联系方式姓名链接文本的左上角坐标，用于绘制下划线。
            ImVec2 currentProfileLinkMin = ImGui::GetItemRectMin();
            /// @brief 当前无联系方式姓名链接文本的右下角坐标，用于绘制下划线。
            ImVec2 currentProfileLinkMax = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(currentProfileLinkMin.x, currentProfileLinkMax.y + 1.0f),
                ImVec2(currentProfileLinkMax.x, currentProfileLinkMax.y + 1.0f),
                ImGui::GetColorU32(linkColor),
                std::max(1.0f, dpiScale));
            if ( ImGui::IsItemClicked(ImGuiMouseButton_Left) ) {
                MMM::Network::UpdateChecker::openUrlInBrowser(profileUrl);
            }
            if ( ImGui::IsItemHovered() ) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
        };

        /// @brief 渲染带联系方式悬浮提示但无跳转主页的鸣谢姓名。
        /// @param labelName 要显示的鸣谢姓名。
        /// @param tooltipText 鼠标悬停时显示的联系方式。
        auto renderThanksNameWithTooltip = [](const char* labelName,
                                              const char* tooltipText) {
            ImGui::TextUnformatted(labelName);
            if ( ImGui::IsItemHovered() ) {
                ImGui::SetTooltip("%s", tooltipText);
            }
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
            renderThanksLink(
                lingyunName,
                TR("ui.help.special_thanks_lingyun_contact").data(),
                lingyunProfileUrl);
            renderThanksSeparator();
            renderThanksLink(thanksName,
                             TR("ui.help.special_thanks_bassor_contact").data(),
                             bassorProfileUrl);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            renderThanksGroupTitle(beatmapSupportTitle);
            ImGui::TableNextColumn();
            renderThanksProfileLink(xiuluoName, xiuluoProfileUrl);
            renderThanksSeparator();
            renderThanksLink(mizarName,
                             TR("ui.help.special_thanks_mizar_contact").data(),
                             mizarProfileUrl);
            renderThanksSeparator();
            renderThanksLink(thanksName,
                             TR("ui.help.special_thanks_bassor_contact").data(),
                             bassorProfileUrl);
            renderThanksSeparator();
            renderThanksNameWithTooltip(
                mzYoakeName,
                TR("ui.help.special_thanks_mz_yoake_contact").data());

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
        if ( ::MMM::UI::FeedbackButton(TR("ui.help.ok").data(),
                                       ImVec2(btnWidth, 36.0f * dpiScale)) ) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

/// @brief 渲染更新检查中的状态弹窗。
void MainMenuView::renderUpdateCheckingPopup()
{
    if ( m_showCheckingPopup ) {
        ImGui::OpenPopup(TR("ui.help.check_update"));
        m_showCheckingPopup = false;
    }

    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    Utils::CenteredModalPopupScope modalScope(dpiScale);
    if ( modalScope.begin(TR("ui.help.check_update")) ) {
        auto info = m_updateChecker->getInfo();

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
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.help.ok").data(),
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
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.help.ok").data(),
                     ImVec2(btnWidth, 32.0f * dpiScale)) ) {
                ImGui::CloseCurrentPopup();
            }
        }

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

    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    Utils::CenteredModalPopupScope modalScope(dpiScale);
    if ( modalScope.begin(TR("ui.help.update_found")) ) {
        info = m_updateChecker->getInfo();

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
                if ( ::MMM::UI::FeedbackButton(
                         TR("ui.help.download_and_install").data(),
                         ImVec2(buttonWidth, 36.0f * dpiScale)) ) {
                    m_updateRestartError.clear();
                    m_updateChecker->downloadAsync();
                }
                ImGui::SameLine();
            }

            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.help.cancel").data(),
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

            float btnWidth = std::max(
                200.0f * dpiScale,
                ImGui::CalcTextSize(TR("ui.help.restart_to_update").data()).x +
                    48.0f * dpiScale);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.help.restart_to_update").data(),
                     ImVec2(btnWidth, 40.0f * dpiScale)) ) {
                std::string restartError;
                m_updateRestartError.clear();
                if ( !MMM::Network::UpdateChecker::applyUpdateAndRestart(
                         info.downloadedFilePath,
                         info.updaterFilePath,
                         &restartError) ) {
                    m_updateRestartError = restartError.empty()
                                               ? "Failed to launch updater"
                                               : restartError;
                }
            }

            if ( !m_updateRestartError.empty() ) {
                ImGui::Spacing();
                ImVec4 errColor(1.0f, 0.4f, 0.4f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, errColor);
                ImGui::TextWrapped("%s", m_updateRestartError.c_str());
                ImGui::PopStyleColor();
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
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.help.ok").data(),
                     ImVec2(btnWidth, 32.0f * dpiScale)) ) {
                ImGui::CloseCurrentPopup();
            }
        }

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

    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    Utils::CenteredModalPopupScope modalScope(dpiScale);
    if ( modalScope.begin(TR("ui.help.update_success")) ) {

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
        if ( ::MMM::UI::FeedbackButton(TR("ui.help.ok").data(),
                                       ImVec2(btnWidth, 32.0f * dpiScale)) ) {
            ImGui::CloseCurrentPopup();
        }

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
    const float    dpiScale =
        Config::AppConfig::instance().getWindowContentScale();

    // 始终跟随鼠标，并根据屏幕位置自动调整对齐方式（边缘翻转）
    ImVec2 pivot = ImVec2(0.0f, 0.0f);
    if ( mousePos.x > viewport->WorkPos.x + viewport->WorkSize.x * 0.7f )
        pivot.x = 1.0f;
    if ( mousePos.y > viewport->WorkPos.y + viewport->WorkSize.y * 0.7f )
        pivot.y = 1.0f;

    float offsetX = (pivot.x == 0.0f) ? 20.0f * dpiScale : -20.0f * dpiScale;
    float offsetY = (pivot.y == 0.0f) ? 20.0f * dpiScale : -20.0f * dpiScale;

    std::string message = m_saveTooltipMessage.empty()
                              ? TR("ui.status.beatmap.saved").data()
                              : m_saveTooltipMessage;
    std::string text    = std::string(ICON_MMM_SAVE) + "  " + message;

    ImVec2 padding{ 16.0f * dpiScale, 10.0f * dpiScale };
    ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    ImVec2 size{ textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f };
    ImVec2 pos{ mousePos.x + offsetX, mousePos.y + offsetY };
    ImVec2 rectMin{ pos.x - size.x * pivot.x, pos.y - size.y * pivot.y };
    ImVec2 rectMax{ rectMin.x + size.x, rectMin.y + size.y };

    ImDrawList* drawList = ImGui::GetForegroundDrawList(viewport);
    ImU32 bgColor   = ImGui::GetColorU32(ImVec4(0.04f, 0.05f, 0.07f, 0.88f));
    ImU32 textColor = ImGui::GetColorU32(
        m_saveTooltipSuccess ? ImVec4(0.45f, 1.0f, 0.48f, 1.0f)
                             : ImVec4(1.0f, 0.42f, 0.42f, 1.0f));
    drawList->AddRectFilled(rectMin, rectMax, bgColor, 8.0f * dpiScale);
    drawList->AddText(ImVec2(rectMin.x + padding.x, rectMin.y + padding.y),
                      textColor,
                      text.c_str());
}

}  // namespace MMM::UI
