#include "ui/walkthrough/WalkthroughSpotlight.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <string>
#include <vector>

/// @file WalkthroughSpotlightTest.cpp
/// @brief 验证任意控件目标解析、前景遮罩和页面可见性续租约束。
/// @details 测试只创建无后端 ImGui 上下文，不创建窗口系统或 GPU 资源。
///
/// 覆盖场景：
/// - 同帧多个候选目标按配置顺序选择最后一个可见目标；
/// - 后续目标一旦出现即推进状态机，消失后不再回退到前序目标；
/// - 业务成功通知与“知道了”共用同一阶段完成入口；
/// - 显式矩形与普通 ImGui Item 使用同一解析路径；
/// - 遮罩向 ForegroundDrawList 增加顶点，提示创建固定 ID 的 Tooltip 层窗口；
/// - 确认当前阶段后旧目标不再产生遮罩，后续目标仍可被解析；
/// - 确认最后一个阶段会停止引导；
/// - 模态向导存在时提示按钮仍能悬浮、按下并释放激活；
/// - 提示按钮通过自身矩形补充模态输入判定，不解锁其他窗口区域；
/// - 鼠标点击确认只影响 Spotlight，不关闭底层业务模态框；
/// - 确认只推进内部目标水位，不模拟被突出控件的业务点击；
/// - 当前界面保持不变时，已确认目标不能在下一帧重新出现；
/// - 晚于确认水位的目标出现后，应立即成为新的解析结果；
/// - 无目标的快捷键或外部应用步骤只绘制提示气泡；
/// - 演练页面未续租时不绘制上一帧残留引导。
///
/// 测试环境约束：
/// - DisplaySize 固定为 800x600，避免依赖桌面分辨率；
/// - DeltaTime 固定为 60 FPS，只供呼吸描边读取时间；
/// - 鼠标输入使用 ImGui 事件队列按下和释放两帧提交；
/// - IniFilename 为空，禁止生成或读取用户窗口布局；
/// - 字体图集只在内存构建，不创建纹理或调用渲染后端；
/// - 所有目标使用测试专用语义 ID，不依赖翻译资源；
/// - 确认按钮直接传入测试英文文本，不初始化应用翻译单例；
/// - 窗口位置由 SetNextWindowPos 指定，布局变化可重复验证；
/// - 每帧严格按 beginFrame、控件上报、keepAlive、render 的顺序驱动；
/// - 每次 NewFrame 都与 Render 配对，失败分支不遗留活动帧；
/// - 模态测试每帧从同一宿主窗口调用 BeginPopupModal；
/// - 按钮中心来自 Spotlight 暴露的实际 Item 矩形，不硬编码气泡坐标；
/// - 提示窗口使用 NoSavedSettings，因此不会写入测试 ini；
/// - 阶段水位单独验证接口，模态场景则提交真实鼠标按下与释放事件；
/// - ImGui 上下文在所有返回路径前销毁。
///
/// 可观察结果：
/// - resolvedTargetId 暴露配置优先级的最终解析结果；
/// - ForegroundDrawList 顶点增长证明目标遮罩实际提交；
/// - 固定提示窗口存在、位于 Tooltip 层且只比原窗口表增加一个窗口；
/// - NavWindow 指针不变证明绘制没有抢占键盘导航焦点；
/// - 模态环境完成一次真实按钮激活后，单目标引导必须停止；
/// - 鼠标释放帧之后模态弹窗仍保持打开，证明没有代替业务关闭；
/// - 提示窗口创建后继续复用，不会随目标切换持续增加窗口数量；
/// - 下一帧未上报第二项时必须解析为第一项；
/// - 阶段水位只允许向后推进，不会重新选中已确认目标；
/// - 确认中间目标后 Spotlight 必须继续保持 active；
/// - 确认最终目标后 Spotlight 必须立即转为 inactive；
/// - 阶段确认后本帧 Anchor 会清空，避免继续暴露过期解析结果；
/// - 第三阶段测试与真实三页项目向导采用相同的有序候选结构；
/// - 最终停止后 render 必须安全成为无操作，不再次创建提示内容；
/// - 自定义矩形不需要先提交 ImGui Button；
/// - 空目标列表用于只能通过快捷键或外部窗口完成的步骤；
/// - 纯提示帧仍需续租，避免页面离开后残留气泡；
/// - 纯提示不伪造目标矩形，也不改变后续帧的目标优先级；
/// - 纯提示不提交前景遮罩顶点，只保留可阅读的气泡窗口；
/// - 缺少 keepAlive 时前景顶点数量必须保持不变；
/// - 测试不判断具体颜色，允许皮肤主题自由调整；
/// - 仅确认按钮模拟鼠标点击，被突出控件仍由用户负责实际操作；
/// - 测试不检查音效资源，统一反馈实现只验证可安全链接与绘制；
/// - 测试不持久化引导状态，Spotlight 只拥有易失 UI 状态；
/// - 返回码区分字体、单调阶段、自定义区域、阶段和续租失败；
/// - 测试完成后不保留全局 ImGui 上下文或活动引导对象。

namespace
{
/// @brief 在测试宿主内提交保持打开的模态窗口及其引导目标。
/// @param spotlight 接收目标矩形的突出引导实例。
/// @param requestOpen 本帧是否请求首次打开弹窗。
/// @return 模态弹窗在当前帧是否仍保持打开。
/// @details 即时模式弹窗必须由同一宿主窗口逐帧提交；统一此顺序可确保鼠标
/// 事件测试只改变输入，不会因测试样板差异改变 Popup 栈或目标矩形。
bool submitModalTarget(MMM::UI::Walkthrough::Spotlight& spotlight,
                       bool                             requestOpen)
{
    ImGui::Begin("ModalHost");
    if ( requestOpen ) ImGui::OpenPopup("ModalWizard");
    const bool modalOpen = ImGui::BeginPopupModal("ModalWizard");
    if ( modalOpen ) {
        // 目标位于模态窗口内部，复现新建项目向导的输入阻挡层级。
        ImGui::Button("ModalTarget");
        spotlight.reportTarget("modal.target",
                               ImGui::GetItemRectMin(),
                               ImGui::GetItemRectMax(),
                               ImGui::GetWindowViewport());
        ImGui::EndPopup();
    }
    ImGui::End();
    return modalOpen;
}

}  // namespace

/// @brief 在最小 ImGui 帧中验证 Spotlight 的配置驱动行为。
/// @return 0 表示全部约束成立，非零值定位失败场景。
int main()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io           = ImGui::GetIO();
    io.IniFilename        = nullptr;
    io.DisplaySize        = { 800.0f, 600.0f };
    io.DeltaTime          = 1.0f / 60.0f;
    unsigned char* pixels = nullptr;
    int            width = 0, height = 0;
    // 文本气泡需要已构建字体图集，但测试不向任何图形后端上传纹理。
    // 这里取得像素只触发 ImGui 的内存构建路径，失败表示无后端测试环境
    // 尚未具备文字排版前置条件，后续几何断言也就没有意义。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    if ( !pixels || width <= 0 || height <= 0 ) {
        ImGui::DestroyContext();
        return 1;
    }

    MMM::UI::Walkthrough::Spotlight spotlight;
    const std::vector<std::string>  targets{ "test.first", "test.second" };

    // 第一帧同时提交两个控件，配置中靠后的第二项应成为当前亮区。
    // 窗口数用于约束 Spotlight 只建立一个提示窗口，前景顶点用于证明
    // 遮罩确实被提交；两者结合可以区分“解析成功但没有绘制”的退化。
    // NavWindow 则保存绘制前状态，确保提示出现不会夺走键盘导航焦点。
    ImGui::NewFrame();
    spotlight.beginFrame();
    spotlight.start(targets, "Follow the highlighted control");
    ImGui::SetNextWindowPos({ 40.0f, 40.0f });
    ImGui::SetNextWindowSize({ 320.0f, 180.0f });
    ImGui::Begin("SpotlightTargets");
    ImGui::Button("First");
    spotlight.reportLastItem("test.first");
    ImGui::Button("Second");
    spotlight.reportLastItem("test.second");
    ImGui::End();
    spotlight.keepAlive();
    ImGuiContext* context       = ImGui::GetCurrentContext();
    ImGuiWindow*  navBefore     = context->NavWindow;
    const int     windowsBefore = context->Windows.Size;
    ImDrawList*   foreground =
        ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    const int verticesBefore = foreground->VtxBuffer.Size;
    spotlight.render(1.0f, "Got it");
    const ImGuiWindow* firstHint =
        ImGui::FindWindowByName("###WalkthroughSpotlightHint");
    const bool firstFrameValid =
        spotlight.resolvedTargetId() == "test.second" &&
        foreground->VtxBuffer.Size > verticesBefore &&
        context->Windows.Size == windowsBefore + 1 && firstHint &&
        (firstHint->Flags & ImGuiWindowFlags_Tooltip) != 0 &&
        context->NavWindow == navBefore;
    ImGui::Render();
    if ( !firstFrameValid ) {
        ImGui::DestroyContext();
        return 2;
    }

    // 第二帧只提交第一项，状态机仍不得退回已经越过的前序目标。
    // 这覆盖点击菜单项后弹窗尚未出现的过渡帧：旧按钮即使仍可见，
    // 也不能立刻再次成为高亮位置；等待态只接受当前或后续阶段。
    ImGui::NewFrame();
    spotlight.beginFrame();
    ImGui::SetNextWindowPos({ 260.0f, 220.0f });
    ImGui::Begin("SpotlightTargets");
    ImGui::Button("First");
    spotlight.reportLastItem("test.first");
    ImGui::End();
    spotlight.keepAlive();
    foreground = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    const int waitingVerticesBefore = foreground->VtxBuffer.Size;
    spotlight.render(1.0f, "Got it");
    const bool noReboundValid =
        spotlight.resolvedTargetId().empty() && spotlight.active() &&
        foreground->VtxBuffer.Size == waitingVerticesBefore;
    ImGui::Render();
    if ( !noReboundValid ) {
        ImGui::DestroyContext();
        return 3;
    }

    // 业务控件确认第二项目标实际成功后直接完成状态机，不依赖鼠标位置猜测。
    // 重复或晚到通知由目标索引约束保持幂等。
    // 最后一项目标成功后 active 立即清除，页面下一帧即可同步结束按钮状态。
    spotlight.completeTarget("test.second");
    if ( spotlight.active() ) {
        ImGui::DestroyContext();
        return 10;
    }

    // 自定义区域无需对应标准控件，证明画布或复合控件同样可以注册目标。
    // 显式屏幕矩形与 Item 路径共享同一候选解析，避免框架被限制为按钮。
    ImGui::NewFrame();
    spotlight.beginFrame();
    spotlight.start({ "canvas.custom" }, "Custom region");
    spotlight.reportTarget("canvas.custom",
                           { 100.0f, 120.0f },
                           { 280.0f, 260.0f },
                           ImGui::GetMainViewport());
    spotlight.keepAlive();
    spotlight.render(1.0f, "Got it");
    const bool customTargetValid =
        spotlight.resolvedTargetId() == "canvas.custom";
    ImGui::Render();
    if ( !customTargetValid ) {
        ImGui::DestroyContext();
        return 4;
    }

    // 首帧打开模态向导并记录气泡中“知道了”按钮的真实屏幕中心。
    // 模态 Popup 会阻挡独立窗口的常规 HoveredWindow 判定，这是用户报告
    // “向导已经弹出但仍无法结束遮罩”时最关键的窗口层级组合。
    // 坐标由成品按钮提供，测试不会复制生产代码中的气泡尺寸算法。
    ImGui::NewFrame();
    spotlight.beginFrame();
    spotlight.start({ "modal.target" }, "Acknowledge this stage");
    submitModalTarget(spotlight, true);
    spotlight.keepAlive();
    spotlight.render(1.0f, "Got it");
    ImGuiWindow* modalHint =
        ImGui::FindWindowByName("###WalkthroughSpotlightHint");
    const bool modalHintSubmitted =
        modalHint && modalHint->Active && !modalHint->SkipItems;
    auto acknowledgeCenter = spotlight.acknowledgeButtonCenter();
    ImGui::Render();
    if ( !modalHintSubmitted || !acknowledgeCenter ) {
        ImGui::DestroyContext();
        return 56;
    }

    // 模态首帧会完成居中布局，再绘制一帧取得稳定后的提示按钮位置。
    // 第二帧排除了弹窗首次自动定位对鼠标命中坐标造成的偶发偏差。
    ImGui::NewFrame();
    spotlight.beginFrame();
    submitModalTarget(spotlight, false);
    spotlight.keepAlive();
    spotlight.render(1.0f, "Got it");
    acknowledgeCenter = spotlight.acknowledgeButtonCenter();
    ImGui::Render();
    if ( !acknowledgeCenter ) {
        ImGui::DestroyContext();
        return 56;
    }
    ImGuiWindow* modalWindow = ImGui::FindWindowByName("ModalWizard");
    modalHint = ImGui::FindWindowByName("###WalkthroughSpotlightHint");
    const int modalDisplayIndex =
        modalWindow ? ImGui::FindWindowDisplayIndex(modalWindow) : -1;
    const int hintDisplayIndex =
        modalHint ? ImGui::FindWindowDisplayIndex(modalHint) : -1;
    // Tooltip 标志与根窗口显示顺序同时受约束：提示框排在模态窗口之后，
    // 才能同样压过文件浏览、停靠页签等普通窗口。
    // 这里直接检查 ImGui 的窗口序列，而非仅比较 Begin 调用顺序，因为
    // 已存在窗口会保留旧层级，正是实际截图中提示被后来窗口覆盖的原因。
    // 断言失败表示视觉层级尚未真正修复，即使按钮命中测试仍可能通过。
    if ( modalDisplayIndex < 0 || hintDisplayIndex <= modalDisplayIndex ) {
        ImGui::DestroyContext();
        return 59;
    }

    // 按下帧只记录起点，尚不应提前结束引导。
    // 使用输入事件队列而非直接改 MouseDown，行为与平台后端提交鼠标事件
    // 的路径一致；确认必须遵循完整的按下再释放手势。
    io.AddMousePosEvent(acknowledgeCenter->x, acknowledgeCenter->y);
    io.AddMouseButtonEvent(0, true);
    ImGui::NewFrame();
    spotlight.beginFrame();
    submitModalTarget(spotlight, false);
    spotlight.keepAlive();
    spotlight.render(1.0f, "Got it");
    const ImGuiWindow* pressedHint =
        ImGui::FindWindowByName("###WalkthroughSpotlightHint");
    const bool pressedHintSubmitted =
        pressedHint && pressedHint->Active && !pressedHint->SkipItems;
    const bool activeWhilePressed = spotlight.active();
    const bool modalTargetResolved =
        spotlight.resolvedTargetId() == "modal.target";
    const bool pressObserved     = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool pressDownObserved = io.MouseDown[ImGuiMouseButton_Left];
    ImGui::Render();

    // 同一坐标释放才构成点击，确认按钮必须在模态层之上收到该事件。
    // 释放发生时仍逐帧提交模态目标，证明确认结果不是由于目标消失或
    // Popup 被测试代码提前关闭而产生的假阳性。
    io.AddMousePosEvent(acknowledgeCenter->x, acknowledgeCenter->y);
    io.AddMouseButtonEvent(0, false);
    ImGui::NewFrame();
    spotlight.beginFrame();
    const bool modalStillOpen = submitModalTarget(spotlight, false);
    spotlight.keepAlive();
    spotlight.render(1.0f, "Got it");
    const ImGuiWindow* releasedHint =
        ImGui::FindWindowByName("###WalkthroughSpotlightHint");
    const bool releasedHintSubmitted =
        releasedHint && releasedHint->Active && !releasedHint->SkipItems;
    const bool releaseObserved = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    const bool releaseDownCleared = !io.MouseDown[ImGuiMouseButton_Left];
    ImGui::Render();
    // 分开检查输入、窗口和业务状态，失败码可直接指出是事件队列、提示
    // 提交、模态生命周期还是 Spotlight 阶段推进出现了回归。
    // 其中模态必须保持打开，因为“知道了”只确认说明，不能代替用户取消
    // 或完成新建项目向导。
    if ( !activeWhilePressed ) {
        ImGui::DestroyContext();
        return 51;
    }
    if ( !modalTargetResolved ) {
        ImGui::DestroyContext();
        return 57;
    }
    if ( !pressedHintSubmitted || !releasedHintSubmitted ) {
        ImGui::DestroyContext();
        return 58;
    }
    if ( !modalStillOpen ) {
        ImGui::DestroyContext();
        return 52;
    }
    if ( !pressObserved ) {
        ImGui::DestroyContext();
        return 54;
    }
    if ( !pressDownObserved || !releaseDownCleared ) {
        ImGui::DestroyContext();
        return 56;
    }
    if ( !releaseObserved ) {
        ImGui::DestroyContext();
        return 55;
    }
    if ( spotlight.active() ) {
        ImGui::DestroyContext();
        return 53;
    }

    // “知道了”记录当前阶段水位；旧目标仍可绘制，但不再成为亮区。
    // 同帧先后上报两项目标模拟“文件”和“新建项目”同时可解析的情况，
    // 确认时应一次跳过当前优先级及所有更早阶段，流程保持单调前进。
    ImGui::NewFrame();
    spotlight.beginFrame();
    spotlight.start({ "stage.first", "stage.second", "stage.third" },
                    "Advance stages");
    spotlight.reportTarget("stage.first",
                           { 40.0f, 80.0f },
                           { 100.0f, 120.0f },
                           ImGui::GetMainViewport());
    spotlight.reportTarget("stage.second",
                           { 120.0f, 80.0f },
                           { 200.0f, 120.0f },
                           ImGui::GetMainViewport());
    spotlight.keepAlive();
    spotlight.acknowledgeCurrentStage();
    const bool acknowledgedValid =
        spotlight.active() && spotlight.resolvedTargetId().empty();
    spotlight.render(1.0f, "Got it");
    ImGui::Render();
    if ( !acknowledgedValid ) {
        ImGui::DestroyContext();
        return 6;
    }

    // 下一帧重复上报旧阶段无效，第三阶段出现后才恢复遮罩。
    // 这保证用户关闭当前遮罩后可以继续填写现有窗口，不会被相同目标
    // 立即重新暗化；业务布局进入新阶段时框架才重新显示引导。
    // 第三项是最终目标，确认后 active 必须同时结束，供页面清理按钮状态。
    ImGui::NewFrame();
    spotlight.beginFrame();
    spotlight.reportTarget("stage.first",
                           { 40.0f, 80.0f },
                           { 100.0f, 120.0f },
                           ImGui::GetMainViewport());
    spotlight.reportTarget("stage.second",
                           { 120.0f, 80.0f },
                           { 200.0f, 120.0f },
                           ImGui::GetMainViewport());
    spotlight.reportTarget("stage.third",
                           { 220.0f, 80.0f },
                           { 300.0f, 120.0f },
                           ImGui::GetMainViewport());
    spotlight.keepAlive();
    const bool nextStageValid = spotlight.resolvedTargetId() == "stage.third";
    spotlight.acknowledgeCurrentStage();
    const bool finalStageValid = nextStageValid && !spotlight.active();
    spotlight.render(1.0f, "Got it");
    ImGui::Render();
    if ( !finalStageValid ) {
        ImGui::DestroyContext();
        return 7;
    }

    // 无候选目标的快捷键步骤仍提交提示，但不能解析出虚假的控件身份。
    // 纯文字提示没有“大遮罩阶段”，因此既不绘制前景遮罩，也不生成
    // 会错误推进目标水位的确认按钮。
    // 该分支仍保持 active，等待用户以配置描述的外部动作完成演练。
    ImGui::NewFrame();
    spotlight.beginFrame();
    spotlight.start({}, "Press Ctrl+Shift+N");
    spotlight.keepAlive();
    foreground = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    const int promptVerticesBefore = foreground->VtxBuffer.Size;
    spotlight.render(1.0f, "Got it");
    const bool promptOnlyValid =
        spotlight.resolvedTargetId().empty() &&
        foreground->VtxBuffer.Size == promptVerticesBefore;
    ImGui::Render();
    if ( !promptOnlyValid ) {
        ImGui::DestroyContext();
        return 8;
    }

    // 页面不再可见时 beginFrame 清除续租，render 不得追加旧提示顶点。
    // 此约束防止用户切换欢迎页或关闭演练后，上一页的引导覆盖其他界面。
    ImGui::NewFrame();
    spotlight.beginFrame();
    foreground = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    const int hiddenVerticesBefore = foreground->VtxBuffer.Size;
    spotlight.render(1.0f, "Got it");
    const bool hiddenValid = foreground->VtxBuffer.Size == hiddenVerticesBefore;
    ImGui::Render();
    ImGui::DestroyContext();
    return hiddenValid ? 0 : 9;
}
