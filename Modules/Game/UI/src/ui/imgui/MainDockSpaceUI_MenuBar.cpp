#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "event/core/EventBus.h"
#include "event/ui/GLFWNativeEvent.h"
#include "graphic/imguivk/VKTexture.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <fmt/format.h>
#include <memory>
#include <string>

namespace MMM::UI
{

/// @brief 渲染无系统标题栏窗口的顶部菜单、性能信息和窗口控制区。
/// @param sourceManager UI 管理器，用于菜单动作和原生窗口拖拽区域同步。
/// @param menuBarHeight 菜单栏的目标高度。
/// @param sidebarWidth 当前侧栏宽度；保留参数用于统一布局接口。
/// @param toolbarWidth 当前工具栏宽度；保留参数用于统一布局接口。
/// @param dpiScale 调用方提供的缩放值；函数会以当前配置值校正。
///
/// 顶栏从左到右由 Logo、主菜单、居中标题、性能文本和三个窗口按钮组成。
/// 标题使用整个视口的中心点，不在左右内容之间做剩余空间居中；因此窄窗口中
/// 各区域可能重叠。重叠时只跳过宽度无效的拖拽命中区，不改变菜单和原生窗口
/// 按钮的优先级。拖拽矩形每帧同步给平台层，用于无边框窗口的命中测试。
///
/// 交互边界：
/// - Logo 当前只承担品牌展示和统一悬停反馈，不发布业务动作；
/// - 菜单动作由 MainMenuView 管理，本函数不解释或缓存动作状态；
/// - 性能文本只读取 EditorEngine 已汇总的 UPS，不参与采样；
/// - 双击最大化和三个窗口按钮都通过 GLFWNativeEvent 请求平台操作；
/// - 原生回调是最大化状态的权威来源，本函数不乐观修改 m_isMaximized；
/// - Tooltip 贴靠按钮左侧，避免超出主视口右边缘。
/// - 所有可见窗口按钮均经 FeedbackButton 绘制；
/// - 透明拖拽命中区是没有按钮外观的例外，不触发按钮反馈。
/// @warning UI 热路径：每帧调用；不得在按钮绘制、文本排版或拖拽区域生成中
/// 引入文件系统访问、阻塞等待或跨线程资源创建。
void MainDockSpaceUI::renderMenuBar(UIManager* sourceManager,
                                    float menuBarHeight, float sidebarWidth,
                                    float toolbarWidth, float dpiScale)
{
    // 窗口可能在运行中切换显示器，使用当前配置缩放覆盖调用方的旧帧值。
    dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // 顶栏覆盖主视口完整宽度，其他固定区域从其下边缘开始布局。
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, menuBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    // 该窗口替代原生标题栏，只承载菜单和自绘控制，不参与 Docking。
    ImGuiWindowFlags menu_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoDocking;

    ImGuiStyle& style             = ImGui::GetStyle();
    float       extraPaddingBaseY = 4.0f;
    // 垂直内边距按整数像素取整，减少字体基线在分数 DPI 下抖动。
    float extraPaddingY = std::floor(extraPaddingBaseY * dpiScale);

    // 外层窗口无边距；菜单项自身通过 FramePadding 获得可点击高度。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(style.FramePadding.x, style.FramePadding.y + extraPaddingY));

    // MenuBarBg 与 WindowBg 同步，填满菜单窗口中菜单栏控件未覆盖的像素。
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg,
                          ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg]);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg]);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyle().Colors[ImGuiCol_TextLink]);

    ImGui::Begin(
        "TopMenuBarHost", nullptr, menu_flags & ~ImGuiWindowFlags_NoBackground);

    // BeginMenuBar 失败时不提交内部控件，但函数末尾仍会平衡外层样式栈。
    if ( ImGui::BeginMenuBar() ) {
        // 内层样式栈约定：
        // - FrameRounding 和 FrameBorderSize 覆盖整个菜单栏内容；
        // - 菜单区临时覆盖一次 FramePadding，渲染完菜单立即恢复；
        // - 窗口按钮区临时覆盖一次 ItemSpacing；
        // - 两个按钮辅助函数各自负责恢复自己压入的颜色和固定样式；
        // - menuFont 只覆盖标题和性能文本，不改变主菜单自身字体管理。
        // 所有提前返回都必须位于压栈之前；当前实现没有内部提前返回路径。
        float  buttonSize          = menuBarHeight;
        ImVec2 defaultFramePadding = ImGui::GetStyle().FramePadding;

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

        /// @brief 绘制带纹理覆盖层的透明标题栏按钮。
        /// @param textureScale 皮肤纹理倍率，仅作用于图像，不影响按钮布局。
        /// 按钮本体统一经过 FeedbackButton，以保留悬停和点击反馈。
        /// 纹理只作为视觉层，不单独创建第二个交互区域。
        /// 返回值仅表示本帧点击，不保存按下状态或纹理所有权。
        auto DrawIconButton = [&](const char*                          str_id,
                                  std::unique_ptr<Graphic::VKTexture>& tex,
                                  float                                btnSize,
                                  ImVec4 hoverColor,
                                  float  textureScale) -> bool {
            // 固定按钮样式消除皮肤内边距差异，保证窗口按钮等宽。
            Utils::pushFixedButtonStyleVars();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);

            bool clicked =
                ::MMM::UI::FeedbackButton(str_id, ImVec2(btnSize, btnSize));

            if ( tex ) {
                // VKTexture 描述符由资源管理层持有，本函数不接管其生命周期。
                ImTextureID imTexId = (ImTextureID)tex->getImTextureID();
                // 以按钮中心缩放纹理，按钮本体与后续菜单的占位保持不变。
                // 倍率大于一时允许图像延伸到按钮外，仍遵循窗口现有裁剪。
                float  iconSize = btnSize * 0.65f * textureScale;
                ImVec2 p_min    = ImGui::GetItemRectMin();
                float  offsetX  = std::floor((btnSize - iconSize) * 0.5f);
                float  offsetY  = std::floor((btnSize - iconSize) * 0.5f);
                ImVec2 img_p1   = { p_min.x + offsetX, p_min.y + offsetY };
                ImVec2 img_p2   = { img_p1.x + iconSize, img_p1.y + iconSize };
                ImU32  tint     = ImGui::IsItemActive()
                                      ? IM_COL32(180, 180, 180, 255)
                                      : IM_COL32_WHITE;
                // 按下态只改变颜色，不替换纹理，避免触发资源切换。
                ImGui::GetWindowDrawList()->AddImage(
                    imTexId, img_p1, img_p2, { 0, 0 }, { 1, 1 }, tint);
            }
            ImGui::PopStyleColor(2);
            Utils::popFixedButtonStyleVars();
            return clicked;
        };

        /// 绘制字体图标形式的标题栏按钮。
        /// 字体图标沿用普通文本色，悬停色由具体操作语义决定。
        auto DrawFontIconButton =
            [&](const char* icon, float btnSize, ImVec4 hoverColor) -> bool {
            // 与纹理按钮使用相同反馈入口，保证交互声效和渐变一致。
            Utils::pushFixedButtonStyleVars();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);

            // 临时恢复普通文本颜色，避免外层 TextLink 色改变图标语义。
            ImVec4 iconVec4 = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::PushStyleColor(ImGuiCol_Text, iconVec4);

            bool clicked =
                ::MMM::UI::FeedbackButton(icon, ImVec2(btnSize, btnSize));

            ImGui::PopStyleColor(3);
            Utils::popFixedButtonStyleVars();
            return clicked;
        };

        // 普通窗口按钮只使用低透明度悬停底色，不抢占菜单视觉层级。
        ImVec4 textCol   = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImVec4 hoverVec4 = ImVec4(textCol.x, textCol.y, textCol.z, 0.1f);

        // Logo 固定在最左侧，并占据一个与菜单栏等高的方形区域。
        ImGui::SetCursorPosX(0.0f);
        // 静态键避免热路径重复构造字符串；切换皮肤后立即读取新倍率。
        static const std::string LOGO_KEY = "logo";
        DrawIconButton("##logo",
                       m_logo_texture,
                       buttonSize,
                       hoverVec4,
                       skinCfg.getTextureScale(LOGO_KEY));

        // 主菜单紧跟 Logo；横向留白按 DPI 缩放，纵向沿用默认基线。
        ImGui::PushStyleVar(
            ImGuiStyleVar_FramePadding,
            ImVec2(std::floor(10.0f * dpiScale), defaultFramePadding.y));
        ImGui::SetCursorPosX(buttonSize + 4.0f * dpiScale);

        ImFont* menuFont = skinCfg.getFont("menu");
        // 菜单视图负责动作注册和状态消息写入，此处只提供布局宿主。
        m_mainMenuview.renderMenus(sourceManager, m_statusMessageService);
        ImGui::PopStyleVar(1);

        float barWidth = ImGui::GetWindowWidth();
        // menusEndX 标记左侧不可拖拽区域终点，也是首段拖拽区起点。
        float menusEndX = ImGui::GetCursorPosX();

        // 应用标题以整个窗口为基准严格居中，不受左右控件宽度影响。
        const char* titleText = "MusicMapMaker-Next";
        if ( menuFont ) ImGui::PushFont(menuFont, menuFont->LegacySize);
        float titleWidth = ImGui::CalcTextSize(titleText).x;
        float titleX     = (barWidth - titleWidth) * 0.5f;
        ImGui::SetCursorPosX(titleX);
        ImGui::TextUnformatted(titleText);
        float titleEndX = titleX + titleWidth;

        // 性能文本放在窗口按钮左侧，显示帧耗时、FPS 与逻辑 UPS。
        ImGuiIO&    io       = ImGui::GetIO();
        float       logicUps = Logic::EditorEngine::instance().getLogicUps();
        std::string fpsStr   = TR_FMT("ui.menu.frame_stats_fmt",
                                    1000.0f / io.Framerate,
                                    io.Framerate,
                                    logicUps);
        float       fpsWidth = ImGui::CalcTextSize(fpsStr.c_str()).x;

        // 右侧区域固定容纳最小化、最大化和关闭三个等宽按钮。
        float numberOfButtons  = 3;
        float buttonsAreaWidth = buttonSize * numberOfButtons;
        float buttonsStartX    = barWidth - buttonsAreaWidth;

        float fpsGap = std::floor(12.0f * dpiScale);
        // 预留间距避免帧信息与窗口控制分隔线发生视觉粘连。
        float fpsX = buttonsStartX - fpsGap - fpsWidth;
        ImGui::SetCursorPosX(fpsX);
        ImGui::TextUnformatted(fpsStr.c_str());
        float fpsEndX = fpsX + fpsWidth;
        if ( menuFont ) ImGui::PopFont();

        // 自绘标题栏把没有菜单或窗口按钮的部分声明为原生拖拽区域：
        // 区域一覆盖菜单尾部到标题起点；
        // 区域二覆盖标题文字本身，使标题也可拖动窗口；
        // 区域三覆盖标题尾部到按钮起点，并包含性能文本。
        // 区域坐标使用窗口局部坐标，由 UIManager 转换给平台窗口层。
        // 三段拆开注册可排除左侧菜单以及右侧控制按钮的交互范围。

        std::vector<Event::DragArea> currentAreas;
        // 宽度在这里保留原始差值，平台层和 ImGui 命中区共用同一组数据。
        // UIManager 可据此替换上一帧区域，避免窗口缩放后仍使用陈旧坐标。
        currentAreas.push_back(
            { menusEndX, 0.0f, titleX - menusEndX, menuBarHeight });
        currentAreas.push_back(
            { titleX, 0.0f, titleEndX - titleX, menuBarHeight });
        currentAreas.push_back(
            { titleEndX, 0.0f, buttonsStartX - titleEndX, menuBarHeight });

        // 管理器为空时仍可绘制顶栏，但无法同步平台命中测试区域。
        if ( sourceManager ) {
            sourceManager->setNativeWindowDragAreas(currentAreas);
        }

        // 透明命中区仅负责标题栏双击，不呈现可见按钮外观。
        for ( const auto& area : currentAreas ) {
            // 负宽度意味着菜单或标题与右侧控件重叠，不创建无效控件。
            if ( area.w > 0 ) {
                // ID 拼入横坐标用于区分三个同帧命中区；坐标只参与本帧控件标识。
                // 这些控件不持久化状态，因此窗口移动后 ID
                // 改变不会丢失业务数据。
                ImGui::SetCursorPosX(area.x);
                ImGui::InvisibleButton(
                    fmt::format("DragArea##{}", area.x).c_str(),
                    ImVec2(area.w, area.h));
                if ( ImGui::IsItemHovered() &&
                     ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ) {
                    // 最大化由原生事件统一处理，保持各平台窗口行为一致。
                    Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                        .type = Event::NativeEventType::
                            GLFW_TOGGLE_WINDOW_MAXIMIZE });
                }
            }
        }

        // 分隔线将性能文本与高风险的原生窗口控制区明确分组。
        float  lineX     = buttonsStartX - fpsGap * 0.5f;
        ImVec2 windowPos = ImGui::GetWindowPos();
        ImVec4 sepCol    = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        sepCol.w *= 0.5f;  // 降低分隔线透明度，避免比文字更醒目。
        ImGui::GetWindowDrawList()->AddLine(
            { windowPos.x + lineX, windowPos.y + menuBarHeight * 0.25f },
            { windowPos.x + lineX, windowPos.y + menuBarHeight * 0.75f },
            ImGui::GetColorU32(sepCol),
            1.5f);  // 高 DPI 下仍保持可辨识的线宽。

        ImGui::SetCursorPosX(buttonsStartX);

        // 按钮之间不留空隙，使三个区域连续覆盖标题栏最右侧。
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

        // 三个按钮只发布操作意图，实际窗口调用由原生事件订阅方执行。
        if ( DrawFontIconButton(ICON_MMM_MINIMIZE, buttonSize, hoverVec4) ) {
            // 最小化不改变本地最大化状态，该状态由后续原生回调刷新。
            Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                .type = Event::NativeEventType::GLFW_ICONFY_WINDOW });
        }
        Utils::renderTooltip(TR("ui.window.minimize").data(),
                             Utils::TooltipDir::Left);

        ImGui::SameLine();

        const char* maxIcon =
            m_isMaximized ? ICON_MMM_RESTORE : ICON_MMM_MAXIMIZE;
        // 图标和提示依赖同一状态，避免视觉含义与操作结果相反。
        const char* maxTip = m_isMaximized ? TR("ui.window.restore").data()
                                           : TR("ui.window.maximize").data();

        if ( DrawFontIconButton(maxIcon, buttonSize, hoverVec4) ) {
            // 不在点击帧预测 m_isMaximized，避免平台拒绝请求时显示错误图标。
            Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                .type = Event::NativeEventType::GLFW_TOGGLE_WINDOW_MAXIMIZE });
        }
        Utils::renderTooltip(maxTip, Utils::TooltipDir::Left);

        ImGui::SameLine();

        ImVec4 dangerCol = Utils::UIThemeUtils::getDangerColor();
        // 关闭按钮仅在悬停态使用危险色，默认态保持标题栏文字颜色。
        if ( DrawFontIconButton(ICON_MMM_CLOSE, buttonSize, dangerCol) ) {
            // 关闭请求仍需经过应用退出流程，以便上层处理未保存项目。
            Event::EventBus::instance().publish(Event::GLFWNativeEvent{
                .type = Event::NativeEventType::GLFW_CLOSE_WINDOW });
        }
        Utils::renderTooltip(TR("ui.window.close").data(),
                             Utils::TooltipDir::Left);

        // 依次恢复按钮间距以及菜单栏内部的圆角、边框设置。
        ImGui::PopStyleVar(1);
        ImGui::PopStyleVar(2);
        ImGui::EndMenuBar();
    }
    // 外层窗口颜色与尺寸样式必须在 End 后成组恢复。
    ImGui::End();
    // 三项颜色依次对应菜单背景、窗口背景和顶栏文本色。
    ImGui::PopStyleColor(3);  // MenuBarBg, WindowBg, Text
    // 三项尺寸依次对应窗口内边距、窗口圆角和菜单项帧内边距。
    ImGui::PopStyleVar(3);
}

}  // namespace MMM::UI
