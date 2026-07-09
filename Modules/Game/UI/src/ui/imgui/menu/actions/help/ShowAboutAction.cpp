#define IMGUI_DEFINE_MATH_OPERATORS
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "mmmversion.h"
#include "network/UpdateChecker.h"
#include "ui/Icons.h"
#include "ui/imgui/menu/actions/MainMenuHelpActions.h"
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

/// @brief 显示关于窗口动作。
class ShowAboutAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 请求显示关于窗口。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        m_showPopup = true;
    }

    /// @brief 渲染关于弹窗。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 每帧调用路径；仅在用户点击外部链接时触发系统浏览器打开。
    void renderDeferred(MainMenuContext& context) override
    {
        (void)context;
        renderAboutPopup();
    }

private:
    /// @brief 渲染关于弹窗。
    /// @warning UI 每帧调用路径；仅在用户点击外部链接时触发系统浏览器打开。
    void renderAboutPopup()
    {
        if ( m_showPopup ) {
            ImGui::OpenPopup(TR("ui.help.about_title"));
            m_showPopup = false;
        }

        float dpiScale = Config::AppConfig::instance().getWindowContentScale();
        ImGuiViewport* mainViewport      = ImGui::GetMainViewport();
        const float    aboutWindowMargin = 32.0f * dpiScale;
        const ImVec2   availableAboutWindowSize{
            std::max(360.0f * dpiScale,
                     mainViewport->WorkSize.x - aboutWindowMargin),
            std::max(360.0f * dpiScale,
                     mainViewport->WorkSize.y - aboutWindowMargin),
        };
        const ImVec2 aboutWindowSize{
            std::min(680.0f * dpiScale, availableAboutWindowSize.x),
            0.0f,
        };

        ImGui::SetNextWindowSizeConstraints(
            ImVec2(std::min(360.0f * dpiScale, aboutWindowSize.x), 0.0f),
            availableAboutWindowSize);

        Utils::CenteredModalPopupScope modalScope(dpiScale);
        bool popupOpen = modalScope.begin(TR("ui.help.about_title"),
                                          nullptr,
                                          ImGuiWindowFlags_None,
                                          aboutWindowSize,
                                          true);
        if ( !popupOpen ) return;

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

        renderBuildInfoTable(dpiScale);
        renderSpecialThanks(dpiScale);
        renderCopyright(dpiScale);

        float btnWidth = 140.0f * dpiScale;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
        if ( ::MMM::UI::FeedbackButton(TR("ui.help.ok").data(),
                                       ImVec2(btnWidth, 36.0f * dpiScale)) ) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    /// @brief 渲染关于窗口中的构建信息表。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 绘制路径：只绘制静态文本。
    void renderBuildInfoTable(float dpiScale)
    {
        if ( ImGui::BeginTable("AboutTable",
                               2,
                               ImGuiTableFlags_SizingFixedFit |
                                   ImGuiTableFlags_NoSavedSettings) ) {
            ImGui::TableSetupColumn(
                "L", ImGuiTableColumnFlags_WidthFixed, 140.0f * dpiScale);
            ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthStretch);

            auto addRow = [&](const char* label, const char* value) {
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

            addRow(TR("ui.help.current_version").data(), MMM_VERSION_STRING);

#if BUILD_TYPE_DEBUG
            addRow(TR("ui.help.build_type").data(), "Debug");
#else
            addRow(TR("ui.help.build_type").data(), "Release");
#endif
            addRow(TR("ui.help.compiler").data(), compilerText.c_str());
            addRow(TR("ui.help.platform").data(), MMM_PLATFORM);

            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    /// @brief 渲染关于窗口中的特别鸣谢区域。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 每帧调用路径；仅在用户点击链接时打开浏览器。
    void renderSpecialThanks(float dpiScale)
    {
        const char* thanksTitle = TR("ui.help.special_thanks").data();
        const char* thanksName  = TR("ui.help.special_thanks_bassor").data();
        float       thanksTitleWidth  = ImGui::CalcTextSize(thanksTitle).x;
        float       nameWidth         = ImGui::CalcTextSize(thanksName).x;
        float       titleGap          = ImGui::GetStyle().ItemSpacing.x;
        float       thanksHeaderWidth = thanksTitleWidth + titleGap + nameWidth;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - thanksHeaderWidth) *
                             0.5f);
        ImGui::TextUnformatted(thanksTitle);
        ImGui::SameLine(0.0f, titleGap);

        ImVec4                linkColor(0.35f, 0.65f, 1.0f, 1.0f);
        constexpr const char* bassorProfileUrl =
            "https://malody.mugzone.net/player/1676762";
        renderLinkedText(thanksName,
                         TR("ui.help.special_thanks_bassor_contact").data(),
                         bassorProfileUrl,
                         linkColor,
                         dpiScale);

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
        renderThanksTable(linkColor, dpiScale);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    /// @brief 渲染鸣谢分组表格。
    /// @param linkColor 链接颜色。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 每帧调用路径；仅在用户点击链接时打开浏览器。
    void renderThanksTable(const ImVec4& linkColor, float dpiScale)
    {
        constexpr const char* bassorProfileUrl =
            "https://malody.mugzone.net/player/1676762";
        constexpr const char* mizarProfileUrl =
            "https://space.bilibili.com/102030000";
        constexpr const char* lingyunProfileUrl =
            "https://space.bilibili.com/311780529";
        constexpr const char* xiuluoProfileUrl =
            "https://space.bilibili.com/106515370";

        const char* thanksSeparator =
            TR("ui.help.special_thanks_separator").data();
        const char* longTermTestingTitle =
            TR("ui.help.special_thanks_long_term_testing").data();
        const char* beatmapSupportTitle =
            TR("ui.help.special_thanks_beatmap_support").data();
        const char* mizarName   = TR("ui.help.special_thanks_mizar").data();
        const char* lingyunName = TR("ui.help.special_thanks_lingyun").data();
        const char* bassorName  = TR("ui.help.special_thanks_bassor").data();
        const char* xiuluoName  = TR("ui.help.special_thanks_xiuluo7").data();
        const char* mzYoakeName = TR("ui.help.special_thanks_mz_yoake").data();

        auto renderSeparator = [&]() {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(thanksSeparator);
            ImGui::SameLine(0.0f, 0.0f);
        };

        auto renderGroupTitle = [](const char* groupTitle) {
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
            renderGroupTitle(longTermTestingTitle);
            ImGui::TableNextColumn();
            renderLinkedText(mizarName,
                             TR("ui.help.special_thanks_mizar_contact").data(),
                             mizarProfileUrl,
                             linkColor,
                             dpiScale);
            renderSeparator();
            renderLinkedText(
                lingyunName,
                TR("ui.help.special_thanks_lingyun_contact").data(),
                lingyunProfileUrl,
                linkColor,
                dpiScale);
            renderSeparator();
            renderLinkedText(bassorName,
                             TR("ui.help.special_thanks_bassor_contact").data(),
                             bassorProfileUrl,
                             linkColor,
                             dpiScale);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            renderGroupTitle(beatmapSupportTitle);
            ImGui::TableNextColumn();
            renderLinkedText(
                xiuluoName, nullptr, xiuluoProfileUrl, linkColor, dpiScale);
            renderSeparator();
            renderLinkedText(mizarName,
                             TR("ui.help.special_thanks_mizar_contact").data(),
                             mizarProfileUrl,
                             linkColor,
                             dpiScale);
            renderSeparator();
            renderLinkedText(bassorName,
                             TR("ui.help.special_thanks_bassor_contact").data(),
                             bassorProfileUrl,
                             linkColor,
                             dpiScale);
            renderSeparator();
            ImGui::TextUnformatted(mzYoakeName);
            if ( ImGui::IsItemHovered() ) {
                ImGui::SetTooltip(
                    "%s", TR("ui.help.special_thanks_mz_yoake_contact").data());
            }

            ImGui::EndTable();
        }
    }

    /// @brief 渲染可点击文本链接。
    /// @param labelName 显示文本。
    /// @param tooltipText 鼠标悬停提示，可为空。
    /// @param profileUrl 点击后打开的 URL。
    /// @param linkColor 链接颜色。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 每帧调用路径；仅在用户点击链接时打开浏览器。
    void renderLinkedText(const char* labelName, const char* tooltipText,
                          const char* profileUrl, const ImVec4& linkColor,
                          float dpiScale)
    {
        ImGui::TextColored(linkColor, "%s", labelName);
        ImVec2 linkMin = ImGui::GetItemRectMin();
        ImVec2 linkMax = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(linkMin.x, linkMax.y + 1.0f),
                                            ImVec2(linkMax.x, linkMax.y + 1.0f),
                                            ImGui::GetColorU32(linkColor),
                                            std::max(1.0f, dpiScale));
        if ( ImGui::IsItemClicked(ImGuiMouseButton_Left) ) {
            MMM::Network::UpdateChecker::openUrlInBrowser(profileUrl);
        }
        if ( ImGui::IsItemHovered() ) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if ( tooltipText ) {
                ImGui::SetTooltip("%s", tooltipText);
            }
        }
    }

    /// @brief 渲染版权信息。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 绘制路径：只绘制静态文本。
    void renderCopyright(float dpiScale)
    {
        (void)dpiScale;
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const char* copyright =
            "Copyright (C) 2025 xiang233. All rights reserved.";
        float cpWidth = ImGui::CalcTextSize(copyright).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - cpWidth) * 0.5f);
        ImGui::TextUnformatted(copyright);
        ImGui::PopStyleColor();

        ImGui::Spacing();
    }

    /// @brief 是否在下一帧打开关于弹窗。
    bool m_showPopup = false;
};
}  // namespace

/// @brief 创建显示关于窗口动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createShowAboutAction()
{
    return std::make_unique<ShowAboutAction>();
}

}  // namespace MMM::UI
