#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "log/colorful-log.h"
#include "ui/imgui/manager/ToolbarView.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include "ui/workspace/MobileTouchScroll.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <imgui_internal.h>

namespace
{
int failures{};  ///< 场景累计失败，最后一次成功不能覆盖此前误触。
/// @brief 分别报告滚动、音量及静音状态，不以窗口存在代替交互成功。
void check(bool passed, const char* message)
{
    if ( !passed ) {
        ++failures;
        XERROR("Sound card scrolling: {}", message);
    }
}
/// @brief 使用正式工作区方向仲裁，控件登记不得在测试中被省略。
/// 页面拖动与数值拖动采用同一个仲裁器，不能在测试里直接操作 ScrollTarget。
/// 只转发横向登记，纵向音量手势应始终由外层列表接管。
struct Workspace : MMM::UI::IWorkspacePresentation {
    MMM::UI::MobileTouchScroll m_scroll;  ///< 与原移动外壳同顺序驱动。
    /// @brief 采用触控布局，防止测试只走桌面数值控件路径。
    bool allowsHoverFeedback() const override { return false; }
    /// @brief 不绘制完整编辑器，避免给待测窗口额外增加一个滚动祖先。
    void renderWorkspace(MMM::UI::MainDockSpaceUI&,
                         MMM::UI::UIManager&) override
    {
    }
    /// @brief 正式滑块通过公共控件协议登记方向，而非在夹具里猜测 ActiveID。
    void registerHorizontalDragItem() override
    {
        m_scroll.registerHorizontalDragItem();
    }
};
/// @brief 直接绘制正式音效工具，不重建一份相似卡片或访问工具栏私有状态。
/// @details CPU ImGui 场景不启动图形／音频设备，配置由 CTest 独立根接收。
/// 真正的窗口树决定命中与滚动，测试只观测生产窗口及音频总线状态。
struct Fixture {
    /// @brief 不拥有实际手机或窗口后端，策略实例只服务本次 CPU 测试。
    Workspace            m_workspace;  ///< 外壳策略生命周期覆盖每次 UI 帧。
    MMM::UI::ToolbarView m_toolbar{ "SoundScrollFixture" };  ///< 正式业务视图。
    /// @brief 每帧重新查询外层列表；窗口标识不来自测试创建的替身。
    ImGuiWindow* m_list{};  ///< 仅在同一 ImGui 上下文中借用，不跨帧销毁窗口。
    /// @brief 起手点必须来自子卡片，不能用相邻标题空白代替。
    ImGuiWindow* m_card{};  ///< 第一张完整可见的实际设置卡片。
    ImVec2       m_label{}, m_blank{}, m_mute{},
        m_slider{};  ///< 从真实卡片布局取点。

    /// @brief 先建立字体与窗口树，避免首次按下承担初始化及悬浮两种职责。
    /// @details 不创建谱面会话，普通混音控制行仍必须能完成触控滚动。
    /// 工具只通过公开的移动入口打开，不能把私有打开标志直接改成预期值。
    /// 总线初值显式指定，禁止借用前一测试遗留的增益和静音状态。
    Fixture()
    {
        // 不载入个人停靠与主题文件，几何只能由本次实际提交的窗口产生。
        ImGui::CreateContext();
        auto& io       = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = { 1000, 600 };
        // 虚拟帧间隔只推进控件状态，避免真实睡眠影响稳定性或测试时长。
        io.DeltaTime = .02f;
        // 同一输入批次当帧消费，不能靠队列延迟把错发的按下拖到下一场景。
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels{};
        int            width{}, height{};
        // 字体仅在 CPU 构建；图标缺字不影响命中几何，不上传 GPU 纹理。
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        // 关闭交互音效，不加载设备或用户皮肤；总线值仍由正式管理器读写。
        MMM::Config::AppConfig::instance()
            .getEditorSettings()
            .interactionSfxGain = 0;
        auto& audio             = MMM::Audio::AudioManager::instance();
        // 运行时初值不持久化；真实控件确认后的持久行为仍由正式管理器执行。
        audio.setSFXGain(1, false);
        audio.setSFXGainMute(false);
        m_toolbar.toggleMobileTool(
            MMM::UI::ToolbarView::MobileTool::SoundEffect, { 920, 50 });
        // 三轮建立内容高度及悬浮关系，初始 ScrollMax 不能由测试自行伪造。
        frame({}, false);
        frame({}, false);
        frame({}, false);
    }
    /// @brief 先结束捕获再销毁上下文，不能让析构掩盖未完成输入。
    ~Fixture()
    {
        // 仅通过正常释放帧结束手势，不访问解释器私有复位状态。
        frame({}, false);
        ImGui::DestroyContext();
    }
    /// @brief 驱动原控件提交、触控过滤及帧末恢复，输入完全走 ImGui IO。
    /// @param point 当前物理坐标，允许从卡片拖到另一张卡片上。
    /// @param down 当前物理按住状态，不使用过滤后的上一帧 IO 反推。
    /// @details 正式 ToolbarView 负责卡片虚拟化、静音按钮和统一百分比滑块。
    /// 夹具只提供工作台锚点与外壳协议，不复制窗口标志或音量更新算法。
    /// 当前窗口指针只在 ImGui 上下文活着时使用，不成为产品接口。
    /// @warning 测试只推进虚拟帧，不 sleep 或替换 ActiveID。
    /// 按下、位移和释放均通过正式 IO，不能用 ActivateItem 模拟滚动成功。
    /// 工作区作用域覆盖全部业务控件，保持生产过滤、登记、恢复的执行顺序。
    /// 读回的是窗口已经应用的 Scroll，而不是等待下帧处理的目标值。
    void frame(ImVec2 point, bool down)
    {
        auto& io = ImGui::GetIO();
        // 指定真实触屏源类型；鼠标路径不受移动滚动器过滤。
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(point.x, point.y);
        io.AddMouseButtonEvent(0, down);
        ImGui::NewFrame();
        MMM::UI::WorkspaceFrameScope scope(m_workspace);
        // 过滤先于正式控件，纵滑不能先改一次音量再被解释为列表滚动。
        m_workspace.m_scroll.beginFrame();
        // 工作台只是正式浮层的定位锚点，不能提供替代的音量控制或滚动区。
        ImGui::SetNextWindowPos({ 900, 20 });
        ImGui::SetNextWindowSize({ 90, 560 });
        ImGui::Begin(
            "###MobileWorkbench",
            nullptr,
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        ImGui::End();
        m_toolbar.renderMobileTools(nullptr, 1, true);
        // 输入只临时过滤，帧末必须恢复物理按住状态，避免第二帧变成新按下。
        m_workspace.m_scroll.endFrame();
        // 虚拟列表会裁剪不可见卡片，每帧丢弃上一轮观察指针并重新选取。
        m_list = m_card = nullptr;
        // 观察正式提交的父子关系；遍历仅存在于测试，不进入生产热路径。
        // 首先定位唯一外层列表，后续卡片必须确属它的直接子窗口。
        for ( auto* window : GImGui->Windows ) {
            if ( window->Active &&
                 std::strstr(window->Name, "##SoundEffectControlScroller") &&
                 !std::strstr(window->Name, "##SettingCard") )
                m_list = window;
        }
        for ( auto* window : GImGui->Windows ) {
            if ( window->Active && window->ParentWindow == m_list &&
                 std::strstr(window->Name, "##SettingCard") &&
                 (!m_card || window->Pos.y < m_card->Pos.y) )
                m_card = window;
        }
        // 初始列表顶端的第一张卡对应总线混音，能直接读回正式音量值。
        // 不读取被裁剪卡片的坐标，滚动期间悬浮经过其他行也不能重定向手势。
        if ( m_card ) {
            // 与正式两行布局相同的主题尺寸；不用固定屏幕坐标蒙中某个空白间隙。
            const auto& style = ImGui::GetStyle();
            const float row   = ImGui::GetFrameHeight();
            // 第二行基线取主题高度和实际 padding，大字体不能误落到首行文字。
            const float secondY =
                m_card->Pos.y + m_card->WindowPadding.y + row +
                std::max(1.f, std::floor(style.ItemSpacing.y * .25f)) +
                row * .5f;
            // 固定逻辑宽度来自生产控件布局；右端取实际子窗，不取屏幕绝对值。
            const float sliderStart =
                m_card->Pos.x + m_card->Size.x - m_card->WindowPadding.x - 170;
            m_label  = { m_card->Pos.x + 30, m_card->Pos.y + 12 };
            m_blank  = { m_label.x, secondY };
            m_slider = { sliderStart + 150, secondY };
            m_mute = { sliderStart - style.ItemSpacing.x - row * .5f, secondY };
        }
        ImGui::Render();
    }
    /// @brief 归零后预热，确保每个场景都从同一张卡片起手。
    /// @details 重置不会给滑块写默认值，必须暴露上一场景误改音量的结果。
    /// 所有音量断言采用交互前快照，不能靠重新初始化总线掩盖误触。
    /// 前一轮释放已经完整消费；这里只重置滚动位置，不清空业务结果。
    /// 预热使 ImGui 应用目标值后再采集控件矩形，避免用陈旧坐标触摸。
    void reset()
    {
        // 仅通过正常释放帧结束手势，不访问解释器私有复位状态。
        frame({}, false);
        if ( m_list ) ImGui::SetScrollY(m_list, 0);
        frame({}, false);
        frame({}, false);
    }
    /// @brief 一个完整拖动保留稳定初始点，滚动后经过别的卡片不能换归属。
    /// @param origin 从生产卡片布局计算的实际起点，不能落在分组空白。
    /// @param delta 相对起点的完整屏幕位移，不逐帧累加滚动结果。
    /// 按下前提供一帧悬浮，保证 HoveredWindow 指向本次起点对应的卡片。
    /// 经过方向阈值后再提供稳定帧，检查列表实际应用而非只生成目标。
    /// 释放帧和下一空帧分别执行，覆盖延迟按钮释放的误触风险。
    void drag(ImVec2 origin, ImVec2 delta)
    {
        frame(origin, false);
        frame(origin, true);
        // 确定方向前不应改值；后续移动直接更新父列表或原滑块。
        frame({ origin.x + delta.x, origin.y + delta.y }, true);
        frame({ origin.x + delta.x, origin.y + delta.y }, true);
        frame({ origin.x + delta.x, origin.y + delta.y }, false);
        frame({}, false);
    }
};
}  // namespace
/// @brief 卡片主体、按钮、滑块均可起手纵滑，横向与轻点仍交给原控件。
/// @details 每个场景读取音频总线，不通过可见数值字符串推断业务状态。
/// 列表末端采用真实范围夹取，输入超过剩余高度时应准确停在末端。
/// 纵滑不能改变音量、切换静音，也不能依赖从分组标题起手。
int main()
{
    Fixture fixture;
    check(fixture.m_list && fixture.m_card, "production windows created");
    if ( !fixture.m_list || !fixture.m_card ) return 1;
    auto& audio = MMM::Audio::AudioManager::instance();
    // 四种起点分别复位，避免上一轮滚动把后续起点移动到分组间隙。
    // 标签、卡片留白、静音和滑块都必须把纵向意图交给相同的外层列表。
    // 每次重新取点，不能使用滚动之前已经移到离屏区域的控件矩形。
    for ( int origin = 0; origin < 4; ++origin ) {
        fixture.reset();
        const ImVec2 points[]{
            fixture.m_label, fixture.m_blank, fixture.m_mute, fixture.m_slider
        };
        // 子卡不能成为第二个隐藏滚动容器，否则列表仍只会在间隙移动。
        check(fixture.m_card->ScrollMax.y == 0,
              "card has no private scrolling range");
        const float gain  = audio.getSFXGain();
        const bool  muted = audio.isSFXGainMuted();
        fixture.drag(points[origin], { 0, -65 });
        // 固定 65px 输入可能超过真实范围，必须比较夹取后的位移。
        check(fixture.m_list->Scroll.y > 0 &&
                  std::abs(fixture.m_list->Scroll.y -
                           std::min(65.f, fixture.m_list->ScrollMax.y)) < 1,
              "vertical swipe scrolls outer list");
        check(std::abs(audio.getSFXGain() - gain) < 1e-6,
              "vertical swipe preserves volume");
        check(audio.isSFXGainMuted() == muted,
              "vertical swipe does not click mute");
    }
    // 单独验证横向编辑，防止修复把卡片内的全部手势都交给父列表。
    fixture.reset();
    const auto slider = fixture.m_slider;
    fixture.drag(slider, { -90, 0 });
    // 横向调节必须产生真实总线增益，而不是只显示一个拖动动画。
    check(audio.getSFXGain() < .8f,
          "horizontal swipe adjusts original volume slider");
    check(fixture.m_list->Scroll.y == 0,
          "horizontal adjustment does not scroll list");
    // 新的完整轻点必须仍有业务意义，静音状态不继承纵滑误触。
    fixture.reset();
    const auto mute = fixture.m_mute;
    fixture.frame(mute, false);
    fixture.frame(mute, true);
    fixture.frame(mute, false);
    fixture.frame(mute, false);
    // 轻点仍只翻转一次静音，不能把所有控件输入都吞成滚动。
    // 前面的全部纵滑都要求保持未静音，因此这里同时能发现重复的延迟激活。
    check(audio.isSFXGainMuted(), "tap still toggles original mute button");
    return failures ? 1 : 0;
}
