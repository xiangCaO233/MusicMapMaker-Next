#include "ui/imgui/SideBarUI.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/ui/UISettingsTabEvent.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "imgui.h"
#include "logic/ProjectController.h"
#include "mmm/SafeParse.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/FloatingManagerUI.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/walkthrough/WalkthroughSpotlight.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

/// @file SideBarUI.cpp
/// @brief 主视口侧栏的动态宽度、页签状态同步和 Clay 按钮布局实现。
/// @details 侧栏一端监听 FloatingManagerUI 的子视图事件，另一端把用户选择
/// 发布回管理器并写入项目工作区；设置按钮不占用侧栏内容区。

namespace MMM::UI
{
namespace
{
/// @brief 无异常解析侧边栏布局浮点配置。
/// @param value 配置字符串。
/// @param fallback 解析失败时的默认值。
/// @return 解析成功的有限浮点数或默认值。
/// @note 允许合法数值后存在未解析后缀，以兼容皮肤布局值的既有表示。
float parseSidebarLayoutFloat(std::string_view value, float fallback)
{
    // 空配置直接使用调用点给出的皮肤默认值。
    if ( value.empty() ) return fallback;

    // SafeParse 只解析数值前缀，不进入异常路径。
    const auto  result = Internal::parseFloatingPrefix(value);
    const float parsed = static_cast<float>(result.value);
    if ( result.error == std::errc{} && result.parsedLength != 0 &&
         std::isfinite(parsed) && parsed > 0.0f ) {
        // 侧栏尺寸只接受有限正数，避免窗口获得无效几何。
        return parsed;
    }
    // 解析失败、无字符消费、NaN 和非正值统一回退。
    return fallback;
}
}  // namespace

/// @brief 计算侧栏当前语言和标签显示模式需要的内容宽度。
/// @param dpiScale 当前窗口内容缩放。
/// @return 足以容纳图标或最宽短标签的整数像素宽度。
/// @details 纯图标模式使用皮肤基准；标签模式测量六个短标签，字体未加载时
/// 使用稳定保底宽度。
/// @warning UI 热路径：每帧布局调用，只测量固定六个短标签。
float SideBarUI::GetSidebarWidth(float dpiScale)
{
    // 皮肤 width 表示纯图标模式的逻辑像素基准。
    Config::SkinManager& skinCfg = Config::SkinManager::instance();
    std::string sidebarBaseWStr  = skinCfg.getLayoutConfig("side_bar.width");
    float       sidebarBaseW = parseSidebarLayoutFloat(sidebarBaseWStr, 32.0f);

    // 图标区域按 DPI 缩放并对齐物理像素。
    float iconAreaW = std::floor(sidebarBaseW * dpiScale);
    if ( !Config::AppConfig::instance()
              .getEditorSettings()
              .showManagerLabels ) {
        // 隐藏文字时无需访问字体或测量翻译标签。
        return iconAreaW;
    }

    // 文本模式需要找到固定页签集合中的最大短标签宽度。
    float maxLabelWidth = 0.0f;
    // 标签与菜单使用相同字体，保持主菜单视觉密度。
    ImFont* menuFont = skinCfg.getFont("menu");
    if ( menuFont && ImGui::GetCurrentContext() ) {
        // CalcTextSize 使用当前字体栈，因此在循环外只压栈一次。
        ImGui::PushFont(menuFont, menuFont->LegacySize);
        // 数组顺序与实际侧栏按钮顺序保持一致。
        const std::array tabs = {
            SideBarTab::Search,        SideBarTab::FileExplorer,
            SideBarTab::AudioExplorer, SideBarTab::BeatMapExplorer,
            SideBarTab::Collaboration, SideBarTab::Settings
        };
        for ( auto tab : tabs ) {
            // 短标签已包含缺失翻译时的稳定回退文本。
            std::string label = TabToShortLabel(tab);
            if ( !label.empty() ) {
                // 只保留最大值，侧栏所有按钮共享统一宽度。
                maxLabelWidth = std::max(maxLabelWidth,
                                         ImGui::CalcTextSize(label.c_str()).x);
            }
        }
        // 恢复调用方字体栈。
        ImGui::PopFont();
    }

    // 启动阶段字体或 ImGui 上下文尚未就绪时使用稳定保底宽度。
    if ( maxLabelWidth < 1.0f ) {
        maxLabelWidth = std::floor(40.0f * dpiScale);
    }

    // 左右总计十二逻辑像素使标签不会贴近按钮边缘。
    float labelPadding = std::floor(12.0f * dpiScale);
    // 标签模式仍不能比皮肤图标基准更窄。
    return std::floor(std::max(iconAreaW, maxLabelWidth + labelPadding));
}

/// @brief 创建侧栏并订阅外部子视图显示状态变化。
/// @param name UIManager 注册和事件来源识别使用的稳定名称。
/// @warning 订阅回调捕获 this，实例地址在析构前必须保持稳定。
SideBarUI::SideBarUI(const std::string& name) : IUIView(name)
{
    // 保存订阅 ID 以便析构时精确取消当前回调。
    m_subId =
        Event::EventBus::instance().subscribe<Event::UISubViewToggleEvent>(
            [this](const Event::UISubViewToggleEvent& e) {
                // 只消费目标为侧栏内容管理器的事件。
                if ( e.targetFloatManagerName == "SideBarManager" ) {
                    // 外部事件使用子视图 ID，先反向映射为页签枚举。
                    auto tab = SubViewIdToTab(e.subViewId);
                    // 未知子视图不改变按钮，避免无关事件误收起侧栏。
                    if ( tab != SideBarTab::None ) {
                        if ( e.showSubView ) {
                            // 显示事件使对应按钮进入激活状态。
                            m_activeTab = tab;
                            if ( e.sourceUiName != m_name ) {
                                // 其他 UI 发起的变化需要同步项目工作区。
                                persistWorkspaceActiveTab(m_activeTab);
                            }
                        } else if ( m_activeTab == tab ) {
                            // 仅当关闭的是当前页签时才收起侧栏状态。
                            m_activeTab = SideBarTab::None;
                            if ( e.sourceUiName != m_name ) {
                                // 自身点击已提前持久化，来源过滤避免重复保存。
                                persistWorkspaceActiveTab(m_activeTab);
                            }
                        }
                    }
                }
            });
}

/// @brief 取消侧栏子视图事件订阅。
SideBarUI::~SideBarUI()
{
    if ( m_subId != 0 ) {
        // 仅对有效订阅 ID 调用 unsubscribe，避免无效总线操作。
        Event::EventBus::instance().unsubscribe<Event::UISubViewToggleEvent>(
            m_subId);
    }
}

/// @brief 将页签枚举映射为项目工作区使用的稳定英文名称。
/// @param tab 待保存页签。
/// @return 不受界面语言变化影响的持久化名称。
/// @note Settings 不表示侧栏内容，因此与 None 一同序列化为 None。
std::string SideBarUI::workspaceNameFromTab(SideBarTab tab)
{
    // 持久化值不使用本地化标题，保证跨语言重启仍能恢复。
    switch ( tab ) {
    case SideBarTab::Search: return "Search";
    case SideBarTab::FileExplorer: return "FileExplorer";
    case SideBarTab::AudioExplorer: return "AudioExplorer";
    case SideBarTab::BeatMapExplorer: return "BeatMapExplorer";
    case SideBarTab::Collaboration: return "Collaboration";
    case SideBarTab::Settings:
    case SideBarTab::None:
        // Settings 是独立窗口入口，不作为侧栏内容页签保存。
    default: return "None";
    }
}

/// @brief 从项目工作区稳定名称恢复页签枚举。
/// @param name 已保存的英文页签名称。
/// @return 对应内容页签，未知值和 None 返回 None。
/// @note 解析区分大小写，以保持项目文件中的规范形式。
SideBarTab SideBarUI::workspaceNameToTab(const std::string& name)
{
    // 显式白名单允许未来新增值时安全回退收起状态。
    if ( name == "Search" ) return SideBarTab::Search;
    if ( name == "FileExplorer" ) return SideBarTab::FileExplorer;
    if ( name == "AudioExplorer" ) return SideBarTab::AudioExplorer;
    if ( name == "BeatMapExplorer" ) return SideBarTab::BeatMapExplorer;
    if ( name == "Collaboration" ) return SideBarTab::Collaboration;
    return SideBarTab::None;
}

/// @brief 获取当前激活页签。
/// @return 本地 UI 线程维护的页签枚举。
SideBarTab SideBarUI::getActiveTab() const
{
    return m_activeTab;
}

/// @brief 设置本地激活页签但不发布显示事件。
/// @param tab 项目工作区恢复出的目标页签。
/// @note 调用方负责在需要时同步 FloatingManagerUI 可见状态。
void SideBarUI::setActiveTab(SideBarTab tab)
{
    m_activeTab = tab;
}

/// @brief 将活动页签写入当前项目工作区并保存项目。
/// @param tab 待持久化页签。
/// @warning 用户交互低频路径：存在项目时执行项目保存。
void SideBarUI::persistWorkspaceActiveTab(SideBarTab tab) const
{
    // 无活动项目时侧栏状态只保留在本次 UI 会话。
    auto* project = Logic::ProjectController::instance().currentProject();
    if ( !project ) {
        return;
    }

    // 稳定英文名称写入项目级工作区，不依赖当前翻译文本。
    project->m_settings.m_workspace.m_sidebarActiveTab =
        SideBarUI::workspaceNameFromTab(tab);
    // 页签点击后立即保存，保证异常退出前仍可恢复工作区。
    Logic::ProjectController::instance().saveProject();
}

/// @brief 同步侧栏管理器状态并绘制固定页签条。
/// @param sourceManager 当前 UI 管理器。
/// @details 先以 FloatingManagerUI 为权威同步状态，再计算主视口固定窗口几何，
/// 最后使用 Clay 排列按钮并通过 ImGui 完成交互绘制。
/// @warning UI 热路径：每帧查询管理器、计算窗口几何并绘制六个按钮。
void SideBarUI::update(UIManager* sourceManager)
{
    // 先读取 SideBarManager 权威可见状态，覆盖外部入口造成的变化。
    if ( auto* sideBarManager =
             sourceManager->getView<FloatingManagerUI>("SideBarManager") ) {
        SideBarTab managerTab = SideBarTab::None;
        if ( sideBarManager->isVisible() ) {
            // 可见时根据当前子视图恢复对应激活按钮。
            managerTab = SubViewIdToTab(sideBarManager->getCurrentSubViewId());
        }
        if ( managerTab != m_activeTab ) {
            // 帧同步只更新本地显示，不反向保存，避免事件循环。
            m_activeTab = managerTab;
        }
    }

    // 皮肤、主视口和 DPI 共同决定侧栏本帧几何。
    Config::SkinManager& skinCfg = Config::SkinManager::instance();
    // 主视口 WorkPos 和 WorkSize 已排除平台保留区域。
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // 内容缩放来自窗口配置，侧栏不使用平台 DPI 的隐式推断。
    float dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();

    // 皮肤基准宽度用于图标按钮尺寸，完整宽度还会考虑短标签。
    float sidebarBaseW = parseSidebarLayoutFloat(
        skinCfg.getLayoutConfig("side_bar.width"), 32.0f);
    float sidebarWidth = GetSidebarWidth(dpiScale);
    // 纯图标按钮以皮肤基准宽度构造正方形高度。
    float btnSize = std::floor(sidebarBaseW * dpiScale);
    // 标签显示模式把按钮高度扩展到可容纳上下两行内容。
    bool showManagerLabels =
        Config::AppConfig::instance().getEditorSettings().showManagerLabels;
    float btnHeight =
        showManagerLabels ? std::floor(48.0f * dpiScale) : btnSize;
    // 所有按钮共享侧栏内容宽度。
    // 该值保留展开按钮宽度语义，便于与纯图标基准区分。
    float expandedBtnW = sidebarWidth;

    // 菜单栏和状态栏使用相同字体高度与垂直 padding。
    float       extraPaddingY = std::floor(4.0f * dpiScale);
    ImGuiStyle& style         = ImGui::GetStyle();
    float       menuBarHeight =
        ImGui::GetFontSize() + (style.FramePadding.y + extraPaddingY) * 2.0f;
    float statusBarHeight = menuBarHeight;
    // 相同高度保证侧栏上下边界相对主视口保持对称。

    // 外观配置控制侧栏与主视口边缘的浮动间距和圆角。
    auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    float floatGap = std::floor(aesthetics.windowGap * dpiScale);
    // 外观配置的逻辑像素统一在本帧转换为物理像素。
    float windowRound = std::floor(aesthetics.windowRounding * dpiScale);

    // ImGui 窗口总宽度还需包含左右 WindowPadding。
    float windowPaddingVal  = std::floor(aesthetics.windowPadding * dpiScale);
    float totalSidebarWidth = sidebarWidth + 2.0f * windowPaddingVal;

    // 侧栏位于菜单栏下方、状态栏上方，并保留统一浮动间距。
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + floatGap,
               viewport->WorkPos.y + menuBarHeight + floatGap),
        ImGuiCond_Always);
    // 高度覆盖工作区扣除上下工具栏与两个间隙后的区域。
    ImGui::SetNextWindowSize(ImVec2(totalSidebarWidth,
                                    viewport->WorkSize.y - menuBarHeight -
                                        statusBarHeight - 2.0f * floatGap));
    // 固定归属主视口，避免多视口模式把侧栏拆成平台窗口。
    ImGui::SetNextWindowViewport(viewport->ID);

    // 侧栏位置尺寸完全由代码管理，不参与 Docking 或 ini 持久化。
    ImGuiWindowFlags sidebar_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking;

    // 窗口级样式在 Begin 前压栈，并在 End 后统一恢复。
    float windowPadding = std::floor(aesthetics.windowPadding * dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(windowPadding, windowPadding));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    // 外层窗口圆角与其他浮动管理器保持统一。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    if ( ImGui::Begin("SideBarUI", nullptr, sidebar_flags) ) {
        // 每个 IUIView 拥有独立 Clay 上下文，生成布局前必须切换。
        CLayWrapperCore::instance().makeCurrent(m_layoutCtx.context);
        // 按皮肤固定按钮圆角、边框与项间距，避免继承外部窗口样式。
        float rounding = std::floor(aesthetics.frameRounding * dpiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        // FeedbackButton 的固定样式辅助函数还会压入按钮相关 StyleVar。
        Utils::pushFixedButtonStyleVars();

        // 单个侧栏按钮回调统一处理选择状态、事件、图标、标签和提示。
        auto DrawSidebarButton = [&](const char*      iconStr,
                                     SideBarTab       tab,
                                     Clay_BoundingBox rect) {
            // rect 只在当前 Clay 执行阶段有效，回调不会保存其地址。
            // 只有当前内容页签使用实色激活背景。
            bool isActive = (m_activeTab == tab);

            // 激活按钮的普通、悬浮和按下状态保持同一强调色。
            if ( isActive ) {
                ImVec4 activeCol =
                    ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
                ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeCol);
            } else {
                // 非激活按钮使用主题文本色派生的透明交互背景。
                Utils::UIThemeUtils::pushTransparentButtonStyles();
            }

            // 图标和短标签共享文本色，未激活时降低透明度。
            ImVec4 iconVec4 = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            if ( !isActive ) {
                // 仍保留足够对比度，让未选页签清晰可辨。
                iconVec4.w *= 0.7f;
            }
            // 文本颜色压栈覆盖后续 DrawList 取色。
            ImGui::PushStyleColor(ImGuiCol_Text, iconVec4);

            // 透明标签隐藏可见文本，实际内容稍后由 DrawList 精确定位。
            ImGui::SetCursorScreenPos({ rect.x, rect.y });
            // 枚举数值为每个按钮提供稳定且互异的 ImGui ID。
            std::string btnId   = "##tab_btn_" + std::to_string((int)tab);
            const bool  clicked = ::MMM::UI::FeedbackButton(
                btnId.c_str(), { rect.width, rect.height });
            if ( tab == SideBarTab::AudioExplorer )
                // 上报按钮实际 Item 矩形，标签模式和纯图标模式无需分别推导。
                sourceManager->walkthroughSpotlight().reportLastItem(
                    "editor.sidebar.audio");
            if ( clicked ) {
                // 所有可见按钮统一经反馈入口处理悬浮渐变和音效。
                if ( tab == SideBarTab::Settings ) {
                    // 设置入口打开独立窗口，并默认定位软件设置页。
                    sourceManager->openSettingsWindow(
                        Event::SettingsTab::Software);
                } else {
                    // 点击活动页签收起内容，点击其他页签切换并展开。
                    m_activeTab = (m_activeTab == tab) ? SideBarTab::None : tab;
                    // 用户选择立即写入项目工作区。
                    persistWorkspaceActiveTab(m_activeTab);
                    // 通过事件通知 FloatingManagerUI 切换对应子视图。
                    using namespace MMM::Event;

                    // sourceUiName 防止订阅回调再次持久化同一次点击。
                    UISubViewToggleEvent evt;
                    evt.sourceUiName           = m_name;
                    evt.uiManager              = sourceManager;
                    evt.targetFloatManagerName = "SideBarManager";
                    // 子视图 ID 与管理器注册时的本地化名称一致。
                    evt.subViewId = TabToSubViewId(tab);

                    if ( m_activeTab != SideBarTab::None ) {
                        // 默认 false 表示关闭；只有激活页签显式请求显示。
                        evt.showSubView = true;
                    }

                    // 同步事件在本帧交给管理器处理，不直接持有其子视图。
                    EventBus::instance().publish(evt);
                    if ( tab == SideBarTab::AudioExplorer &&
                         m_activeTab == SideBarTab::AudioExplorer ) {
                        // 只有实际点击并展开音频页签才完成此操作目标。
                        // 点击已激活标签会收起管理器，因此不能把该分支算作完成。
                        // 发布事件后读取本地状态，保持与用户看到的按钮状态一致。
                        sourceManager->walkthroughSpotlight().completeTarget(
                            "editor.sidebar.audio");
                    }
                }
            }

            // 图标始终在完整按钮宽度内水平居中。
            float iconAreaW = rect.width;

            // 图标优先使用皮肤 pure_icons 字体。
            ImFont* sideBarFont = skinCfg.getFont("pure_icons");
            if ( sideBarFont ) {
                // 推入字体后 ImGui::GetFontSize 与该字体基准尺寸一致。
                ImGui::PushFont(sideBarFont, sideBarFont->LegacySize);
            }
            // 先测量图标字形，再计算按钮内居中位置。
            ImVec2 iconSize = ImGui::CalcTextSize(iconStr);
            float  iconY    = rect.y + (rect.height - iconSize.y) * 0.5f;
            if ( showManagerLabels ) {
                // 标签模式把图标固定在按钮顶部，给底部文字留出空间。
                iconY = rect.y + std::floor(5.0f * dpiScale);
            }
            // 水平居中使用图标实际字形宽度。
            ImVec2 iconPos = { rect.x + (iconAreaW - iconSize.x) * 0.5f,
                               iconY };
            // 直接向窗口 DrawList 写入，避免额外 ImGui 布局项改变游标。
            ImGui::GetWindowDrawList()->AddText(
                sideBarFont,
                ImGui::GetFontSize(),
                iconPos,
                ImGui::GetColorU32(ImGuiCol_Text),
                iconStr);
            // 仅在实际压入专用字体时恢复字体栈。
            if ( sideBarFont ) ImGui::PopFont();

            if ( showManagerLabels ) {
                // 短标签在图标下方单独使用菜单字体。
                std::string label    = TabToShortLabel(tab);
                ImFont*     menuFont = skinCfg.getFont("menu");
                if ( menuFont ) {
                    // 字号不超过当前字号，也受按钮宽度比例限制。
                    float labelFontSize = std::floor(
                        std::min(ImGui::GetFontSize(), rect.width * 0.42f));
                    // 使用实际字体计算 UTF-8 标签边界，允许多语言宽度变化。
                    ImVec2 labelSize = menuFont->CalcTextSizeA(
                        labelFontSize,
                        std::numeric_limits<float>::max(),
                        0.0f,
                        label.c_str());
                    // 标签水平居中并贴近按钮底部保留四像素边距。
                    ImVec2 labelPos = {
                        rect.x + (rect.width - labelSize.x) * 0.5f,
                        rect.y + rect.height - labelSize.y -
                            std::floor(4.0f * dpiScale),
                    };
                    // DrawList 绘制不改变 ImGui 光标或按钮命中区域。
                    ImGui::GetWindowDrawList()->AddText(
                        menuFont,
                        labelFontSize,
                        labelPos,
                        ImGui::GetColorU32(ImGuiCol_Text),
                        label.c_str());
                }
                // 菜单字体缺失时保留图标，不以错误字体挤压标签区。
            }

            // 完整名称提示固定从按钮右侧展开，补足短标签信息。
            Utils::renderTooltip(TabToTooltip(tab).c_str(),
                                 Utils::TooltipDir::Right);

            // 首先恢复文本颜色，再按激活分支恢复三项按钮颜色。
            ImGui::PopStyleColor(1);
            if ( isActive ) {
                // 激活分支手动压入了 Button 三态颜色。
                ImGui::PopStyleColor(3);
            } else {
                // 非激活分支由语义辅助函数成对恢复颜色。
                Utils::UIThemeUtils::popTransparentButtonStyles();
            }
        };

        // 纵向 Clay 容器按固定顺序排列五个管理器入口和底部设置入口。
        CLayVBox vbox;
        // 页面间距来自外观配置，容器自身不再增加内边距。
        vbox.setPadding(0, 0, 0, 0)
            .setSpacing(std::floor(aesthetics.itemSpacing * dpiScale))
            .addElement("SearchButton",
                        Sizing::Grow(),
                        Sizing::Fixed(btnHeight),
                        [&](Clay_BoundingBox rect, bool isHovered) {
                            // Clay 悬停值由 FeedbackButton
                            // 自行查询，不在此使用。
                            // 搜索入口固定为纵向按钮列表首项。
                            DrawSidebarButton(
                                ICON_MMM_SEARCH, SideBarTab::Search, rect);
                        })
            .addElement("FileExplorerButton",
                        Sizing::Grow(),
                        Sizing::Fixed(btnHeight),
                        [&](Clay_BoundingBox rect, bool isHovered) {
                            // 文件浏览器使用打开文件夹图标。
                            DrawSidebarButton(ICON_MMM_FOLDER_OPEN,
                                              SideBarTab::FileExplorer,
                                              rect);
                        })
            .addElement("AudioExplorerButton",
                        Sizing::Grow(),
                        Sizing::Fixed(btnHeight),
                        [&](Clay_BoundingBox rect, bool isHovered) {
                            // 音频浏览器使用音乐符号图标。
                            DrawSidebarButton(ICON_MMM_MUSIC,
                                              SideBarTab::AudioExplorer,
                                              rect);
                        })
            .addElement("BeatMapExplorerButton",
                        Sizing::Grow(),
                        Sizing::Fixed(btnHeight),
                        [&](Clay_BoundingBox rect, bool isHovered) {
                            // 谱面浏览器使用文件图标以区别项目目录。
                            DrawSidebarButton(ICON_MMM_FILE,
                                              SideBarTab::BeatMapExplorer,
                                              rect);
                        })
            .addElement("CollaborationButton",
                        Sizing::Grow(),
                        Sizing::Fixed(btnHeight),
                        [&](Clay_BoundingBox rect, bool isHovered) {
                            // 协作管理器使用多人图标。
                            DrawSidebarButton(ICON_MMM_USERS,
                                              SideBarTab::Collaboration,
                                              rect);
                        })
            // 弹簧吸收剩余高度，使设置入口固定在侧栏底部。
            .addSpring()
            .addElement("SettingsButton",
                        Sizing::Grow(),
                        Sizing::Fixed(btnHeight),
                        [&](Clay_BoundingBox rect, bool isHovered) {
                            // 设置按钮打开独立窗口，不改变 m_activeTab。
                            DrawSidebarButton(
                                ICON_MMM_COG, SideBarTab::Settings, rect);
                        });

        // Clay 布局覆盖当前 ImGui 窗口完整内容区。
        ImVec2 startPos = ImGui::GetCursorScreenPos();
        float  availH   = ImGui::GetContentRegionAvail().y;
        // 宽度单独读取，避免前一次查询结果在未来布局调用后失效。
        float availW = ImGui::GetContentRegionAvail().x;
        // Begin 后的内容区尺寸已扣除本窗口 WindowPadding。
        // 固定可用高度使中间弹簧得到明确剩余空间。
        ImVec2 sz = vbox.renderInCurrent(startPos, { availW, availH });
        // 将游标推进到布局末尾，保持 ImGui 内容边界正确。
        ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

        // 固定按钮 StyleVar 与本窗口四项 StyleVar 按逆序恢复。
        Utils::popFixedButtonStyleVars();
        ImGui::PopStyleVar(4);
    }
    // Begin 即使返回 false 也必须调用 End。
    ImGui::End();
    // 恢复 Begin 前压入的 WindowPadding、BorderSize 与 Rounding。
    ImGui::PopStyleVar(3);
}

}  // namespace MMM::UI
