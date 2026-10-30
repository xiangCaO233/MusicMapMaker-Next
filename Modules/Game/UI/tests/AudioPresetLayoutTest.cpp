#include "config/AppConfig.h"
#include "config/AudioPresets.h"
#include "config/skin/translation/Translation.h"
#include "log/colorful-log.h"
#include "ui/imgui/audio/AudioPresetControls.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include <array>
#include <cmath>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <string_view>

namespace
{
/// @brief 测试只替换平台按钮外观，实际预设布局与点击仍走正式实现。
/// @details 不创建 SDL/Vulkan，记录真实控件几何而非复制布局公式。
/// 测试替身只控制触摸能力，分类与正文始终由正式绘制函数生成。
/// 不提供预设字段或布局尺寸，防止被测几何在替身中被重新实现。
struct Presentation final : MMM::UI::IWorkspacePresentation {
    /// @brief 记录分类入口与固定关闭入口，索引三为关闭。
    std::array<ImRect, 4> m_buttons{};
    /// @brief 布局测试不需要外层工作区或任何画布。
    void renderWorkspace(MMM::UI::MainDockSpaceUI&,
                         MMM::UI::UIManager&) override
    {
    }
    /// @brief 激活正式布局中的触摸尺寸与自适应规则。
    bool allowsHoverFeedback() const override { return false; }
    /// @brief 绘制真实统一按钮，并借用 LastItem 获取其屏幕命中区域。
    /// @param label 正式界面提供的当前语言标签，用于识别分类和关闭入口。
    /// @param size 正式布局要求的命中区域，测试不得自行放大或修正。
    /// @return 原生统一按钮的激活结果，保留按下与释放语义。
    /// @details 仅在控件提交后读取 LastItem，不以预计坐标代替真实命中框。
    bool actionButton(const char* label, const char*, ImVec2 size) override
    {
        const bool       clicked = MMM::UI::FeedbackButton(label, size);
        const std::array keys{ "ui.audio_presets.tab_mixer",
                               "ui.audio_presets.tab_effects",
                               "ui.audio_presets.tab_manage",
                               "ui.window.close" };
        for ( size_t i = 0; i < keys.size(); ++i )
            if ( std::string_view(label) == TR(keys[i]).view() )
                m_buttons[i] = { ImGui::GetItemRectMin(),
                                 ImGui::GetItemRectMax() };
        return clicked;
    }
};
/// @brief 同一上下文跨屏幕尺寸与语言，检查真实弹层和正文子窗口。
/// @details 窗口指针在每帧重新采样，切换分类后不能继续断言旧正文窗口。
/// 夹具不修改库项，也不通过复制布局公式模拟最终窗口尺寸。
struct Fixture {
    /// @brief 平台替身只保留几何，不持有业务配置或音频对象。
    Presentation m_presentation;
    /// @brief 窗口指针只在下一次帧推进前观察，由 ImGui 管理寿命。
    ImGuiWindow* m_popup{};
    /// @brief 当前分类的正文窗口，帧末重新解析，不能跨分类沿用旧指针。
    ImGuiWindow* m_body{};
    /// @brief 完整推进输入、正式绘制和帧末关闭，不直接操作预设私有状态。
    /// @param pointer 屏幕逻辑坐标，负坐标表示没有触点。
    /// @param down 当前触点是否按下，不改写 ActiveID 强行激活控件。
    /// @param open 仅请求打开，窗口尺寸与内容由正式绘制入口提交。
    /// @details NewFrame 与 Render 成对执行，让原生帧末关闭路径参与测试。
    /// 平台策略仅绑定在当前帧，不影响之后运行的其他测试入口。
    void frame(ImVec2 pointer = { -100, -100 }, bool down = false,
               bool open = false)
    {
        // 显式标记触屏来源，防止默认鼠标行为掩盖触摸尺寸分支。
        // 事件在 NewFrame 之前进入队列，与平台后端的调用顺序一致。
        auto& io = ImGui::GetIO();
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(pointer.x, pointer.y);
        io.AddMouseButtonEvent(0, down);
        ImGui::NewFrame();
        // 生产代码通过正式作用域查询能力，不直接调用私有排版辅助函数。
        // 帧结束自动恢复之前的观察指针，跨场景不能残留平台策略。
        MMM::UI::WorkspaceFrameScope scope(m_presentation);
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("PresetHost", nullptr, ImGuiWindowFlags_NoSavedSettings);
        // 仅通过原生 OpenPopup
        // 请求打开，面板的实际位置、尺寸与分组由生产代码提交。
        if ( open ) ImGui::OpenPopup("AudioPresetSettings");
        MMM::UI::drawAudioPresetControls();
        ImGui::End();
        ImGui::Render();
        auto& context = *ImGui::GetCurrentContext();
        // 从真实打开栈取得窗口，不能把 OpenPopup 请求成功当作内容已经显示。
        // Render 以后仍开放的窗口才提供本次断言需要的最终几何。
        m_popup = context.OpenPopupStack.empty()
                      ? nullptr
                      : context.OpenPopupStack[0].Window;
        m_body  = nullptr;
        // 测试允许遍历 UI 窗口，生产每帧布局不得借用这种全局扫描。
        for ( auto* window : context.Windows )
            if ( window->Active &&
                 std::string_view(window->Name).find("/AudioPresetBody_") !=
                     std::string_view::npos )
                m_body = window;
    }
    /// @brief 分类切换和关闭都注入真实按下、释放，防止只验证按钮返回前的状态。
    /// @param button 真实按钮记录中的分类编号，三表示固定关闭入口。
    /// @details 释放后额外推进空闲帧，使新正文高度与滚动范围稳定。
    /// 不使用 sleep，状态变化全部通过 ImGui 自身的连续帧推进。
    void tap(int button)
    {
        const auto point = m_presentation.m_buttons[button].GetCenter();
        frame(point, true);
        frame(point, false);
        frame();
    }
};
}  // namespace

/// @brief 验证横屏、竖屏与矮窗口的固定框架、可滚动正文和分类切换。
/// @details 同时验证浏览界面不会更改方案参数，不需要播放设备或用户文件。
/// @param argv CTest 注入的正式中英文语言资源，避免测试从用户目录取旧文案。
/// @param argc 必须包含程序名及两份生产语言文件路径。
/// @return 所有尺寸、分类和只读浏览断言通过时返回零。
int main(int argc, char** argv)
{
    if ( argc != 3 ) return 1;
    // 独立上下文与空 ini 避免读取或写回真实用户的停靠布局。
    // 测试不启动播放设备，也不要求 Vulkan 或系统窗口存在。
    ImGui::CreateContext();
    auto& io                        = ImGui::GetIO();
    io.IniFilename                  = nullptr;
    io.DeltaTime                    = 1.0f / 60;
    io.ConfigInputTrickleEventQueue = false;
    // 放大默认字体模拟移动阅读字号，不能只用桌面小字验证长英文。
    // CPU 字体图集足以测量控件，不依赖 GPU 纹理上传。
    ImGui::GetStyle().FontScaleMain = 2;
    unsigned char* pixels{};
    int            width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto& translator = MMM::Translation::getActiveTranslator();
    bool  ok         = translator.loadLanguage("zh_cn", argv[1]) &&
                       translator.loadLanguage("en_us", argv[2]);
    // 比较完整持久格式，涵盖总线、静音、EQ、速度和质量等全部字段。
    // 只比较默认音量会漏掉浏览时意外改写效果器或当前预设的问题。
    const nlohmann::json original =
        MMM::Config::AppConfig::instance().getAudioPresets();
    Fixture fixture;
    /// @brief 累积所有失败，便于一次运行报告不同屏幕上的独立布局问题。
    const auto check = [&](bool result, const char* stage) {
        if ( !result ) XERROR("AudioPresetLayoutTest: {}", stage);
        ok &= result;
    };
    for ( const char* language : { "en_us", "zh_cn" } ) {
        check(translator.switchLang(language), "switch production language");
        // 覆盖宽屏、竖屏和矮窗口，保证排版断点前后都存在可关闭的入口。
        // 显示区域改变后先推进视口，再打开面板，不手工设置被测窗口尺寸。
        for ( const auto size : { ImVec2{ 1280, 720 },
                                  ImVec2{ 640, 960 },
                                  ImVec2{ 640, 480 } } ) {
            io.DisplaySize = size;
            fixture.frame();
            // 首帧已采纳新显示尺寸，此时请求打开同一正式预设面板。
            fixture.frame({}, false, true);
            fixture.frame();
            // 逐页提交正式控件，不能只验证空弹层或默认混音页的边界。
            for ( int page = 0; page < 3; ++page ) {
                fixture.tap(page);
                auto* popup = fixture.m_popup;
                auto* body  = fixture.m_body;
                check(popup && body, "popup and real body are active");
                if ( !popup || !body ) continue;
                // 页面切换会隐藏旧正文，空指针门禁防止误读旧窗口。
                // 关闭区域来自真实控件，正文区域则来自实际 ChildWindow。
                const auto close = fixture.m_presentation.m_buttons[3];
                // 比较实际窗口几何，不在测试中重复生产尺寸计算公式。
                check(popup->Size.x > size.x * .8f && popup->Pos.x >= 0 &&
                          popup->Pos.y >= 0 &&
                          popup->Pos.x + popup->Size.x <= size.x + 1 &&
                          popup->Pos.y + popup->Size.y <= size.y + 1,
                      "wide panel remains inside viewport");
                check(popup->ScrollMax.y < .1f && body->Size.y > 48,
                      "only body may scroll");
                check(close.Min.y >= body->Pos.y + body->Size.y - 1 &&
                          close.Max.y <= popup->Pos.y + popup->Size.y,
                      "close stays below body and inside panel");
                const float closeY = close.Min.y;
                // 短页覆盖零滚动范围；长页则真正滚到尾部，而非只检查初始状态。
                // 固定页脚的位置不能依赖当前正文滚动目标。
                ImGui::SetScrollY(body, body->ScrollMax.y);
                fixture.frame();
                // 滚动到尾部也不能把关闭入口卷走，或让正文压到固定操作行。
                check(std::abs(fixture.m_presentation.m_buttons[3].Min.y -
                               closeY) < 1,
                      "scroll does not move close button");
            }
            // 真正点击关闭入口，避免下一屏继续复用未关闭的弹层状态。
            fixture.tap(3);
            check(!fixture.m_popup, "fixed close button dismisses popup");
        }
    }
    // 分类与尺寸切换只改变显示，不得把未编辑字段重新量化或写回方案。
    check(nlohmann::json(
              MMM::Config::AppConfig::instance().getAudioPresets()) == original,
          "browsing preserves all preset values");
    ImGui::DestroyContext();
    return ok ? 0 : 1;
}
