#include "../src/ui/utils/MobileFileDialogImpl.h"
#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "log/colorful-log.h"
#include "ui/imgui/menu/package/BmsExportTouch.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include <cmath>
#include <imgui_internal.h>
#include <string>

namespace
{
int failures{};  ///< 几何、输入与结果分别累计失败，不由后续成功覆盖。
/// @brief 失败标签明确区分裁切、身份变化及确认协议。
void check(bool value, const char* label)
{
    if ( !value ) {
        ++failures;
        XERROR("File dialog: {}", label);
    }
}
/// @brief 只提供外壳安全区，键盘动画通过改变区域高度模拟。
/// @details 真正的输入、按钮和结果由生产选择器及 ImGui 执行。
struct Workspace : MMM::UI::IWorkspacePresentation {
    /// @brief 可用高度独立于物理屏幕尺寸，键盘只改变安全区域。
    float m_height{ 1216 };  ///< 完整高度和 424px 之间切换，不能重开选择器。
    /// @brief 平台能力决定密度，不能只按测试场景名称切换生产分支。
    bool m_mobile{ true };  ///< 桌面反例保持原生控件及浏览区行为。
    /// @brief 触控能力来自工作区协议，反例允许传统悬浮反馈。
    bool allowsHoverFeedback() const override { return !m_mobile; }
    /// @brief 不绘制完整编辑器，隔离与本回归无关的 Vulkan 和谱面。
    void renderWorkspace(MMM::UI::MainDockSpaceUI&,
                         MMM::UI::UIManager&) override
    {
    }
    /// @brief 键盘区域与父视口坐标一致，不能通过扩大屏幕掩盖裁切。
    MMM::UI::WorkspaceWindowArea textInputWindowArea() const override
    {
        return { { 0, 0 }, { 2688, m_height } };
    }
};
/// @brief 在公开扩展点读取实际控件矩形，不改生产缓冲或确认状态。
struct Probe : MMM::UI::MobileFileDialog {
    ImRect m_input{}, m_cancel{};  ///< 过滤器行和取消按钮的真实矩形。
    ImVec2 m_inputPoint{};         ///< 同行过滤器左侧是正式文件名输入区域。
    bool   m_nameActive{};         ///< 正式输入控件报告的真实焦点。
    /// @brief 观察原按钮提交前后的布局，所有成功状态仍由真实输入驱动。
    bool m_DrawValidationButtons() override
    {
        // 前一个 Item 是同一行的过滤器，不由测试重新绘制一份输入框。
        m_input      = { ImGui::GetItemRectMin(), ImGui::GetItemRectMax() };
        m_inputPoint = {
            m_input.Min.x -
                100 *
                    MMM::Config::AppConfig::instance().getWindowContentScale(),
            m_input.GetCenter().y
        };
        const bool result = MobileFileDialog::m_DrawValidationButtons();
        // 取消在最后提交；确认必须通过真实点击完成，不直接写 isOk。
        m_cancel     = { ImGui::GetItemRectMin(), ImGui::GetItemRectMax() };
        m_nameActive = m_FileDialogInternal.fileInputIsActive;
        return result;
    }
};
/// @brief 跑完真实选择、键盘开合与确认／取消，测试不写输出文件。
/// @details 所有输入通过 IO 队列，保留同一文件名控件和缓冲直到完成。
/// 覆盖的是原 Display，而不是重新绘制的相似测试窗口：
/// - 浏览模式先从原目录产生真实输入和过滤器行；
/// - 文件名聚焦后才改变可用高度，不能跳过原始身份建立；
/// - 高 DPI 字体和 48dp 行高必须同时存在，否则容易得到假阳性；
/// - 完成态必须来自真实按钮释放，不能给 isOk 写入预期值；
/// - 取消分支也编辑过名称，证明退出不会偷偷保留确认结果；
/// - 桌面只核对原密度，不把移动按钮排列强加到桌面上。
/// @param confirm 真时验证路径结果，假时验证取消协议。
/// @param mobile 桌面分支检查公共能力回退，不模拟 Android SDK。
void scenario(bool confirm, bool mobile = true)
{
    // 每个分支拥有独立上下文；关闭 INI，避免读取或污染个人布局。
    // 最终检查输入与确认结果，因此控件仅存在或截图完整都不算通过。
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    // 每个场景都禁止加载个人停靠树，结果只能取决于本次输入和安全区。
    io.IniFilename = nullptr;
    // 高 DPI 同时扩大真实字体和主题，不能只增加画布像素。
    io.DisplaySize = { 2688, 1216 };
    // 固定帧步长仅推进 UI 分类，不 sleep，也不等待真实系统动画。
    io.DeltaTime      = .016f;
    const float scale = mobile ? 2.7625f : 1.f;
    // 主题缩放与字体倍率分别设置，避免只扩大按钮而遗漏文本高度。
    // AppConfig 的应用倍率也需同步，因为生产控件读的是它而非测试参数。
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleMain = scale;
    // CPU 图集只用于测量，回归不加载 Vulkan、SDL 窗口或音频设备。
    // 字形内容不参与截图比对，平台字体栅格化差异不能影响结果。
    unsigned char* pixels{};
    int            w{}, h{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    MMM::Config::AppConfig::instance().setNativeContentScale(scale);
    MMM::Config::AppConfig::instance().getEditorSettings().uiScaleMultiplier =
        scale;
    // 工作区对象活到最后一帧之后；帧绑定不持有它的共享所有权。
    Workspace workspace;
    workspace.m_mobile = mobile;
    Probe dialog;
    // 路径只供上游初始化目录；本测试不创建或写出任何谱面文件。
    // 过滤器为真实单文件 BMS，最终结果必须保留相同的扩展名。
    IGFD::FileDialogConfig config;
    config.path     = ".";
    config.fileName = "initial.bms";
    config.flags    = ImGuiFileDialogFlags_Modal;
    dialog.OpenDialog("Picker", "Picker", ".bms", config);
    // 完成标志每帧接收真实 Display 返回值，不能沿用先前一次成功。
    bool       done  = false;
    const auto frame = [&] {
        // 与实际调用方相同：安全区先绑定，再居中并驱动原 Display。
        // 不手写子窗口尺寸或把按钮放到另一个测试模态中。
        // 先推进 IO 队列再绘制，鼠标按下和释放不能被合并为手动激活。
        ImGui::NewFrame();
        ImGui::Begin("Host");
        MMM::UI::WorkspaceFrameScope scope(workspace);
        const auto size = MMM::UI::exportFileDialogSize(scale);
        {
            MMM::UI::Utils::CenteredModalPopupScope style(scale);
            MMM::UI::Utils::prepareCenteredModalWindow(size);
            done = dialog.Display(
                "Picker",
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings,
                size);
        }
        ImGui::End();
        ImGui::Render();
        // 仅看输出文件名不能发现样式栈泄漏；每帧恢复计数也必须为零。
        check(GImGui->ErrorCountCurrentFrame == 0, "no ImGui recovery");
        check(GImGui->StyleVarStack.Size == 0, "styles restored");
    };
    // 自动尺寸先稳定；后续键盘变更不能通过重开窗口隐藏焦点问题。
    for ( int i = 0; i < 4; ++i ) frame();
    // 单击产生正式 ActiveId；下面只读它，不直接调用 SetActiveID。
    io.AddMousePosEvent(dialog.m_inputPoint.x, dialog.m_inputPoint.y);
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
    // 起始点来自真实控件同行矩形；没有真实聚焦就不能进入后续断言。
    check(dialog.m_nameActive, "real filename focused");
    // 保存实际 ActiveId，只用来比较键盘前后身份，不反向写入 ImGui。
    const auto active = GImGui->ActiveId;
    // 424px 来自真实高键盘场景，两个触摸行不能按比例缩小来适应。
    // 桌面保持完整高度，只验证未受移动扩展影响的原有行为。
    workspace.m_height = mobile ? 424.f : 1216.f;
    // 连续稳定帧必须保持同一身份，防止自动布局反复重置输入状态。
    for ( int i = 0; i < 4; ++i ) frame();
    check(active != 0 && GImGui->ActiveId == active, "focus survives keyboard");
    // 过滤器与输入同一行且同高；另行验证后面的真实编辑结果。
    // 与屏幕安全区比较，而非只与可能已经越界的父窗口比较。
    check(
        dialog.m_input.Min.y >= 0 && dialog.m_input.Max.y <= workspace.m_height,
        "filename row visible");
    check(dialog.m_cancel.Min.y >= 0 &&
              dialog.m_cancel.Max.y <= workspace.m_height,
          "buttons visible");
    // 允许不足一个物理像素的顶点舍入；容差不随 DPI 放大。
    // 这个下限独立于窗口存在和文字内容，能发现裁掉半行的回归。
    check(!mobile || dialog.m_cancel.GetHeight() + 1 >= 48 * scale,
          "touch height retained");
    if ( !mobile ) {
        // 桌面反例只核对原始密度，不套用移动按钮排列来推算其点击坐标。
        check(dialog.m_cancel.GetHeight() < 48, "desktop density unchanged");
        dialog.Close();
        MMM::Config::AppConfig::instance().setNativeContentScale(1);
        MMM::Config::AppConfig::instance()
            .getEditorSettings()
            .uiScaleMultiplier = 1;
        ImGui::DestroyContext();
        return;
    }
    // 键盘收起必须重新显示浏览区，仍使用原对话框而非创建第二个实例。
    // 这里模拟窗口安全区变化，不主动清空 ActiveID 或替换输入缓冲。
    workspace.m_height = 1216;
    frame();
    frame();
    check(dialog.m_cancel.Max.y <= 1216, "browser restored after keyboard");
    check(dialog.GetCurrentFileName() == "initial.bms", "browser retains name");
    // 同一输入控件再次展开高键盘，不能继承上次紧凑布局的陈旧高度。
    // 收起与展开各经过稳定帧，确认几何更新不依赖重新 OpenDialog。
    workspace.m_height = 424;
    frame();
    frame();
    check(dialog.m_cancel.Max.y <= 424, "keyboard reopened safely");
    check(GImGui->ActiveId == active, "focus survives reopen");
    // 全选和输入走正式 ImGui 编辑，不调用 SetDefaultFileName 偷换结果。
    // Ctrl+A 的按下、释放分别经过完整帧；不修改选区索引来伪装全选。
    // CPU 夹具沿用宿主的文本编辑约定，macOS 的全选来自 Command+A。
    // 不修改控件缓冲或关闭平台行为；选区必须由正式输入路径产生。
    const ImGuiKey inputModifier =
        io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
    io.AddKeyEvent(inputModifier, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    frame();
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(inputModifier, false);
    frame();
    // 输入从 IO 队列进入上游正式控件，不能直接覆盖文件名缓存。
    io.AddInputCharactersUTF8("HighKeyboard_谱面.bms");
    frame();
    // UTF-8 名称经真实控件编辑后再读取路径接口，不能只比较绘制文字。
    // 过滤器的扩展名规则仍由上游实现，测试不手工拼接结果。
    check(dialog.GetCurrentFileName() == "HighKeyboard_谱面.bms",
          "typed name retained");
    // 键盘展开时直接按确认；按下失焦不能在释放前把按钮移走。
    // 点击矩形取自刚绘制的按钮，禁止使用离屏旧帧或自己创建的隐藏按钮。
    // 确认紧邻取消左侧；最终返回值证明点击命中了正式生产控件。
    const auto   cancelRect = dialog.m_cancel;
    const auto   gap        = ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 target =
        confirm ? ImVec2(cancelRect.Min.x - gap - cancelRect.GetWidth() * .5f,
                         cancelRect.GetCenter().y)
                : cancelRect.GetCenter();
    io.AddMousePosEvent(target.x, target.y);
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
    // 必须完整经过松手；只发按下事件或直接调用动作会漏掉几何跳变。
    check(done, "click completes while keyboard visible");
    // 完成态与成功态是两件事，取消完成不能被误算为确认成功。
    check(dialog.IsOk() == confirm, "confirmation result unchanged");
    // 显式关闭上游事务，取消分支也不能遗留下一次可消费的确认结果。
    // 倍率恢复发生在本场景结束，不能让桌面反例继承手机身份。
    dialog.Close();
    MMM::Config::AppConfig::instance().setNativeContentScale(1);
    MMM::Config::AppConfig::instance().getEditorSettings().uiScaleMultiplier =
        1;
    ImGui::DestroyContext();
}
}  // namespace
/// @brief 分别验证确认与取消，不用取消成功代替输出目标正确。
int main()
{
    // 确认路径包含 UTF-8 编辑和键盘反复开合；不能用预填文本代替输入。
    scenario(true);
    // 取消也经过相同输入，验证它只结束选择而不会返回一个成功目标。
    scenario(false);
    // 最后运行桌面反例，确认前面手机场景没有泄漏应用倍率或活动控件。
    scenario(true, false);
    // 场景累计的失败数决定退出码，不因最后一个桌面分支成功而清零。
    return failures ? 1 : 0;
}
