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
/// @note 编译器 ID 缺失时显示稳定占位文本，版本缺失时不追加空格。
/// @warning 只允许使用构建生成宏，不应在此执行运行时工具链探测。
std::string buildCompilerText()
{
    // 宏值先复制为字符串，便于按可用性组合最终显示文本。
    std::string       compilerText = MMM_BUILD_COMPILER_ID;
    const std::string versionText  = MMM_BUILD_COMPILER_VERSION;

    // 部分非标准工具链可能没有可识别的编译器 ID。
    if ( compilerText.empty() ) {
        compilerText = "Unknown";
    }

    // 只有版本有效时才追加分隔空格，避免产生尾随空白。
    if ( !versionText.empty() ) {
        compilerText += " ";
        compilerText += versionText;
    }

    // 返回值独立拥有宏文本，适合在本帧表格渲染期间使用。
    return compilerText;
}

/// @brief 显示关于窗口动作。
/// @details 处理器只保存一次性打开请求，窗口内容均从构建信息和翻译读取。
/// @warning 不得跨帧缓存 SkinManager 或翻译系统返回的裸指针。
class ShowAboutAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 请求显示关于窗口。
    /// @param context 单帧主菜单上下文，本动作无需读取。
    /// @param activation 激活来源，不改变弹窗打开语义。
    /// @note 实际 OpenPopup 延迟到菜单作用域结束后的渲染阶段。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        // 仅登记请求，避免在主菜单弹窗内部直接打开模态窗口。
        m_showPopup = true;
    }

    /// @brief 渲染关于弹窗。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 每帧调用路径；仅在用户点击外部链接时触发系统浏览器打开。
    /// @note 关于窗口自身读取当前 DPI，因此无需使用菜单上下文。
    void renderDeferred(MainMenuContext& context) override
    {
        (void)context;
        renderAboutPopup();
    }

private:
    /// @brief 渲染关于弹窗。
    /// @warning UI 每帧调用路径；仅在用户点击外部链接时触发系统浏览器打开。
    /// @note 布局宽度受主视口工作区限制，高度交给 ImGui 按内容计算。
    /// @note 可见标题由翻译提供，当前弹窗状态依赖同一帧稳定文本。
    void renderAboutPopup()
    {
        if ( m_showPopup ) {
            // 将一次性请求转换为 ImGui 弹窗状态后立即清除。
            ::MMM::UI::FeedbackOpenPopup(TR("ui.help.about_title").data());
            m_showPopup = false;
        }

        // 每帧获取内容缩放，使弹窗能跟随运行时 DPI 变化。
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();
        // 工作区排除系统任务栏和平台窗口保留区域，更适合作为尺寸上限。
        ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        // 四周预留边距，防止最大化弹窗紧贴视口边缘。
        const float aboutWindowMargin = 32.0f * dpiScale;
        // 小窗口下仍提供可用的最小约束，实际平台裁剪由 ImGui 处理。
        const ImVec2 availableAboutWindowSize{
            std::max(360.0f * dpiScale,
                     mainViewport->WorkSize.x - aboutWindowMargin),
            std::max(360.0f * dpiScale,
                     mainViewport->WorkSize.y - aboutWindowMargin),
        };
        // 常规宽度限制为 680 逻辑像素，高度使用自动适配。
        const ImVec2 aboutWindowSize{
            std::min(680.0f * dpiScale, availableAboutWindowSize.x),
            0.0f,
        };

        // 最小宽度不超过当前可用宽度，避免约束上下界相互冲突。
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(std::min(360.0f * dpiScale, aboutWindowSize.x), 0.0f),
            availableAboutWindowSize);

        // RAII 作用域统一设置居中位置、圆角和模态弹窗样式。
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        bool popupOpen = modalScope.begin(TR("ui.help.about_title").data(),
                                          nullptr,
                                          ImGuiWindowFlags_None,
                                          aboutWindowSize,
                                          true);
        // 弹窗未打开时跳过字体和表格等全部内容布局。
        if ( !popupOpen ) return;

        // 标题优先使用皮肤菜单字体，缺失时沿用当前字体。
        ImFont* titleFont = Config::SkinManager::instance().getFont("menu");
        if ( titleFont ) ImGui::PushFont(titleFont, titleFont->LegacySize);

        // 图标与应用名合成单个文本，便于整体水平居中。
        std::string appLabel =
            std::string(ICON_MMM_MUSIC) + "  " + TR("ui.help.app_name").data();
        // 基于当前字体测量真实标题宽度，而非使用固定偏移。
        float titleWidth = ImGui::CalcTextSize(appLabel.c_str()).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - titleWidth) * 0.5f);
        ImGui::TextColored(
            ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "%s", appLabel.c_str());

        // 仅在前面成功压入字体时恢复字体栈。
        if ( titleFont ) ImGui::PopFont();

        // 分隔标题与主体信息，维持不同 DPI 下的视觉层次。
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 主体按构建信息、鸣谢和版权的稳定顺序绘制。
        renderBuildInfoTable(dpiScale);
        renderSpecialThanks(dpiScale);
        renderCopyright(dpiScale);

        // 确认按钮使用逻辑宽度缩放并在当前窗口内居中。
        float btnWidth = 140.0f * dpiScale;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
        if ( ::MMM::UI::FeedbackButton(TR("ui.help.ok").data(),
                                       ImVec2(btnWidth, 36.0f * dpiScale)) ) {
            // 关闭只改变 ImGui 弹窗状态，无需保留额外动作标志。
            ImGui::CloseCurrentPopup();
        }

        // 与 modalScope.begin 成功路径严格配对。
        ImGui::EndPopup();
    }

    /// @brief 渲染关于窗口中的构建信息表。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 绘制路径：只绘制静态文本。
    /// @note 左列固定宽度显示标签，右列拉伸承载不同长度的构建值。
    /// @note 表格行值均在调用期间有效，不保存指向局部字符串的指针。
    void renderBuildInfoTable(float dpiScale)
    {
        // 禁用表格持久化，避免静态关于窗口污染用户布局配置。
        if ( ImGui::BeginTable("AboutTable",
                               2,
                               ImGuiTableFlags_SizingFixedFit |
                                   ImGuiTableFlags_NoSavedSettings) ) {
            // 固定标签列使各行值起点一致，并随 DPI 缩放。
            ImGui::TableSetupColumn(
                "L", ImGuiTableColumnFlags_WidthFixed, 140.0f * dpiScale);
            ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthStretch);

            // 局部 helper 统一单行对齐与次要文本颜色。
            auto addRow = [&](const char* label, const char* value) {
                ImGui::TableNextRow();
                // 第一列使用普通文本颜色突出字段含义。
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                // 第二列使用禁用色降低构建细节的视觉权重。
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::AlignTextToFramePadding();
                // Unformatted 避免构建字符串中的百分号被解释为格式符。
                ImGui::TextUnformatted(value);
                ImGui::PopStyleColor();
            };

            // 编译器字符串只构造一次，保证 c_str 在所有表格行绘制期间有效。
            const std::string compilerText = buildCompilerText();

            // 版本号来自构建生成头，直接反映当前可执行文件版本。
            addRow(TR("ui.help.current_version").data(), MMM_VERSION_STRING);

#if BUILD_TYPE_DEBUG
            // 构建类型在编译期选择，避免运行时猜测优化配置。
            addRow(TR("ui.help.build_type").data(), "Debug");
#else
            // 非 Debug 构建统一面向用户显示为 Release。
            addRow(TR("ui.help.build_type").data(), "Release");
#endif
            // 编译器和目标平台用于故障报告中的环境辨识。
            addRow(TR("ui.help.compiler").data(), compilerText.c_str());
            addRow(TR("ui.help.platform").data(), MMM_PLATFORM);

            // 仅在 BeginTable 成功时结束表格作用域。
            ImGui::EndTable();
        }

        // 表格后统一添加分隔，衔接特别鸣谢区域。
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    /// @brief 渲染关于窗口中的特别鸣谢区域。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 每帧调用路径；仅在用户点击链接时打开浏览器。
    /// @note 首行突出主要贡献者，详细分组在后续表格列出。
    /// @note 所有宽度以逻辑像素定义，并统一乘以 dpiScale。
    void renderSpecialThanks(float dpiScale)
    {
        // 翻译文本指针只在当前帧使用，不跨帧缓存。
        const char* thanksTitle = TR("ui.help.special_thanks").data();
        const char* thanksName  = TR("ui.help.special_thanks_bassor").data();
        float       thanksTitleWidth = ImGui::CalcTextSize(thanksTitle).x;
        float       nameWidth        = ImGui::CalcTextSize(thanksName).x;
        // 使用主题项目间距连接标题与姓名，避免硬编码字符空格。
        float titleGap = ImGui::GetStyle().ItemSpacing.x;
        // 组合宽度用于将整行而非单段文本居中。
        float thanksHeaderWidth = thanksTitleWidth + titleGap + nameWidth;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - thanksHeaderWidth) *
                             0.5f);
        ImGui::TextUnformatted(thanksTitle);
        // SameLine 的显式间距保持标题和链接属于同一视觉行。
        ImGui::SameLine(0.0f, titleGap);

        // 链接颜色与标题色相近，但保持足够亮度以表达可点击性。
        ImVec4 linkColor(0.35f, 0.65f, 1.0f, 1.0f);
        // URL 为固定外部资料地址，不从翻译文本中解析。
        constexpr const char* bassorProfileUrl =
            "https://malody.mugzone.net/player/1676762";
        renderLinkedText(thanksName,
                         TR("ui.help.special_thanks_bassor_contact").data(),
                         bassorProfileUrl,
                         linkColor,
                         dpiScale);

        // 描述宽度限制为 420 逻辑像素，同时适应更窄内容区域。
        float thanksTextWidth =
            std::min(420.0f * dpiScale, ImGui::GetContentRegionAvail().x);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - thanksTextWidth) *
                             0.5f);
        // 换行位置基于居中后的起点，保证描述块左右边界对称。
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + thanksTextWidth);
        // 描述使用次要文本颜色，使姓名链接仍是视觉焦点。
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s",
                           TR("ui.help.special_thanks_bassor_desc").data());
        // 恢复颜色和换行栈，防止影响后续鸣谢表格。
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();

        // 详细贡献分组与简介保留一个标准控件间距。
        ImGui::Spacing();
        renderThanksTable(linkColor, dpiScale);

        // 末尾分隔鸣谢与版权区域。
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    /// @brief 渲染鸣谢分组表格。
    /// @param linkColor 链接颜色。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 每帧调用路径；仅在用户点击链接时打开浏览器。
    /// @note 表格第一列为贡献角色，第二列按本地化分隔符排列姓名。
    /// @note 固定 URL 与翻译姓名按代码中的贡献关系成对维护。
    void renderThanksTable(const ImVec4& linkColor, float dpiScale)
    {
        // 资料地址使用编译期常量，避免每帧构造字符串。
        constexpr const char* bassorProfileUrl =
            "https://malody.mugzone.net/player/1676762";
        constexpr const char* mizarProfileUrl =
            "https://space.bilibili.com/102030000";
        constexpr const char* lingyunProfileUrl =
            "https://space.bilibili.com/311780529";
        constexpr const char* xiuluoProfileUrl =
            "https://space.bilibili.com/106515370";

        // 本地化分隔符允许不同语言决定姓名列表之间的标点与空格。
        const char* thanksSeparator =
            TR("ui.help.special_thanks_separator").data();
        const char* longTermTestingTitle =
            TR("ui.help.special_thanks_long_term_testing").data();
        const char* beatmapSupportTitle =
            TR("ui.help.special_thanks_beatmap_support").data();
        // 姓名和角色文本均在本帧表格绘制完成前有效。
        const char* mizarName   = TR("ui.help.special_thanks_mizar").data();
        const char* lingyunName = TR("ui.help.special_thanks_lingyun").data();
        const char* bassorName  = TR("ui.help.special_thanks_bassor").data();
        const char* xiuluoName  = TR("ui.help.special_thanks_xiuluo7").data();
        const char* mzYoakeName = TR("ui.help.special_thanks_mz_yoake").data();

        // 姓名分隔 helper 保持标点与相邻链接在同一行。
        auto renderSeparator = [&]() {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(thanksSeparator);
            ImGui::SameLine(0.0f, 0.0f);
        };

        // 分组标题统一使用次要文本颜色，突出右侧贡献者链接。
        auto renderGroupTitle = [](const char* groupTitle) {
            ImGui::PushStyleColor(
                ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(groupTitle);
            ImGui::PopStyleColor();
        };

        // 静态表格禁用布局持久化，列宽完全由当前 DPI 决定。
        if ( ImGui::BeginTable("SpecialThanksRows",
                               2,
                               ImGuiTableFlags_SizingFixedFit |
                                   ImGuiTableFlags_NoSavedSettings) ) {
            // 角色列固定宽度，姓名列吸收剩余空间并允许自然布局。
            ImGui::TableSetupColumn(
                "Role", ImGuiTableColumnFlags_WidthFixed, 96.0f * dpiScale);
            ImGui::TableSetupColumn("Names",
                                    ImGuiTableColumnFlags_WidthStretch);

            // 第一行列出长期测试贡献者。
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            renderGroupTitle(longTermTestingTitle);
            ImGui::TableNextColumn();
            renderLinkedText(mizarName,
                             TR("ui.help.special_thanks_mizar_contact").data(),
                             mizarProfileUrl,
                             linkColor,
                             dpiScale);
            // 分隔符 helper 负责维持同行布局状态。
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

            // 第二行列出谱面支持贡献者。
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            renderGroupTitle(beatmapSupportTitle);
            ImGui::TableNextColumn();
            renderLinkedText(
                xiuluoName, nullptr, xiuluoProfileUrl, linkColor, dpiScale);
            // 无联系方式文本的条目仍保留可点击个人主页。
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
            // 最后一个姓名没有外部 URL，仅显示悬停联系方式。
            ImGui::TextUnformatted(mzYoakeName);
            if ( ImGui::IsItemHovered() ) {
                // Tooltip 使用翻译文本，避免将联系方式硬编码进布局。
                ImGui::SetTooltip(
                    "%s", TR("ui.help.special_thanks_mz_yoake_contact").data());
            }

            // 与成功 BeginTable 配对，恢复表格栈。
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
    /// @note 点击区域沿用文本项目矩形，下划线只负责视觉提示。
    /// @pre labelName 和 profileUrl 必须指向有效的空字符结尾字符串。
    /// @note tooltipText 可为空，此时悬停只改变鼠标光标。
    /// @warning profileUrl 必须是代码维护的可信地址，不接受外部输入。
    void renderLinkedText(const char* labelName, const char* tooltipText,
                          const char* profileUrl, const ImVec4& linkColor,
                          float dpiScale)
    {
        // 先提交文本项目，后续交互查询均针对该项目矩形。
        ImGui::TextColored(linkColor, "%s", labelName);
        // 获取实际布局矩形，使下划线精确贴合当前字体宽度。
        ImVec2 linkMin = ImGui::GetItemRectMin();
        ImVec2 linkMax = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(linkMin.x, linkMax.y + 1.0f),
            ImVec2(linkMax.x, linkMax.y + 1.0f),
            ImGui::GetColorU32(linkColor),
            // 高 DPI 下增加线宽，最低仍保持一个物理像素。
            std::max(1.0f, dpiScale));
        if ( ImGui::IsItemClicked(ImGuiMouseButton_Left) ) {
            // 浏览器启动只发生在明确左键点击时，不由悬停预加载。
            MMM::Network::UpdateChecker::openUrlInBrowser(profileUrl);
        }
        if ( ImGui::IsItemHovered() ) {
            // 手型光标补充链接可交互提示。
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if ( tooltipText ) {
                // 调用方未提供联系方式时不创建空 Tooltip。
                ImGui::SetTooltip("%s", tooltipText);
            }
        }
    }

    /// @brief 渲染版权信息。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 绘制路径：只绘制静态文本。
    /// @note dpiScale 当前仅保留接口一致性，文本宽度直接由活动字体测量。
    /// @note 固定版权字符串不参与本地化，确保法律声明内容一致。
    void renderCopyright(float dpiScale)
    {
        (void)dpiScale;
        // 版权行使用次要文本色，避免与确认按钮争夺视觉焦点。
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const char* copyright =
            "Copyright (C) 2025 xiang233. All rights reserved.";
        // 依据当前字体精确测宽后居中，不依赖固定字符数量。
        float cpWidth = ImGui::CalcTextSize(copyright).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - cpWidth) * 0.5f);
        ImGui::TextUnformatted(copyright);
        // 恢复调用者文本颜色，避免确认按钮继承禁用色。
        ImGui::PopStyleColor();

        // 版权行与底部确认按钮之间保留标准间距。
        ImGui::Spacing();
    }

    /// @brief 是否在下一帧打开关于弹窗。
    /// @note execute 置位，renderAboutPopup 在转换为 ImGui 状态后清除。
    bool m_showPopup = false;
};
}  // namespace

/// @brief 创建显示关于窗口动作处理器。
/// @return 独占所有权的关于窗口处理器。
/// @note 处理器只持有弹窗打开请求，不持有翻译或皮肤资源。
/// @warning 必须在 ImGui 所属 UI 线程执行和销毁。
/// @warning 外部链接只会在用户点击关于窗口中的姓名时打开。
std::unique_ptr<IMainMenuItemActionHandler> createShowAboutAction()
{
    return std::make_unique<ShowAboutAction>();
}

}  // namespace MMM::UI
