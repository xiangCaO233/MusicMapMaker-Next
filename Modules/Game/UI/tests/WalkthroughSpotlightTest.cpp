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
/// - 同一语义目标由多个并列控件上报时合并为共同可选范围；
/// - 视口右侧的纵向工具栏目标把提示放到左侧而不覆盖按钮；
/// - 目标几何扩张、提示边距与视口裁剪共同保持稳定的安全间隔；
/// - 提示窗口的位置在 ImGui 完成布局后按真实几何验证；
/// - 几何回归不依赖 GPU 后端，保证常规 CTest 环境可重复执行；
/// - 新的并列候选用例与已有单调状态机用例相互隔离；
/// - 后续目标一旦出现即推进状态机，消失后不再回退到前序目标；
/// - 业务成功通知与“知道了”共用同一阶段完成入口；
/// - 业务层可在控件出现前查询当前水位，以准备固定的临时教学目标；
/// - 显式矩形与普通 ImGui Item 使用同一解析路径；
/// - 遮罩向 ForegroundDrawList 增加顶点，提示创建固定 ID 的 Tooltip 层窗口；
/// - 确认当前阶段后旧目标不再产生遮罩，后续目标仍可被解析；
/// - 确认最后一个阶段会进入 Completed，供路线会话衔接下一步；
/// - 模态向导存在时提示按钮仍能悬浮、按下并释放激活；
/// - 提示按钮通过自身矩形补充模态输入判定，不解锁其他窗口区域；
/// - 鼠标点击确认只影响 Spotlight，不关闭底层业务模态框；
/// - 确认只推进内部目标水位，不模拟被突出控件的业务点击；
/// - 当前界面保持不变时，已确认目标不能在下一帧重新出现；
/// - 晚于确认水位的目标出现后，应立即成为新的解析结果；
/// - 无目标的快捷键或外部应用步骤绘制可确认的提示气泡；
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
/// - 模态环境完成一次真实按钮激活后，单目标引导必须进入完成态；
/// - 鼠标释放帧之后模态弹窗仍保持打开，证明没有代替业务关闭；
/// - 提示窗口创建后继续复用，不会随目标切换持续增加窗口数量；
/// - 下一帧未上报第二项时必须解析为第一项；
/// - 阶段水位只允许向后推进，不会重新选中已确认目标；
/// - 确认中间目标后 Spotlight 必须继续保持 active；
/// - 确认最终目标后 Spotlight 必须保持 active 并进入 Completed；
/// - Completed 与 Inactive 必须可区分，让路线接续和主动退出拥有不同语义；
/// - 新的 start 必须覆盖前一步 Completed，并建立独立目标水位；
/// - 纯文字“知道了”与目标按钮确认最终都落到相同 Completed 终态；
/// - 阶段确认后本帧 Anchor 会清空，避免继续暴露过期解析结果；
/// - 第三阶段测试与真实三页项目向导采用相同的有序候选结构；
/// - Completed 状态下 render 必须安全成为无操作，不再次创建提示内容；
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
/// @brief 验证绘制回退只消费当前和即将重练的产物，正常结束不删除成果。
/// @details 独立计数模拟三类绘制的副作用，不依赖逻辑线程或真实物件。
/// 按照 Note、Flick、Hold 的正式路线顺序验证，避免用单个回调掩盖跨步问题。
/// 回调不负责推进教程，导航状态仍由 requestPrevious/start 驱动。
/// 最后检查退出后的身份隔离，防止重练意外删除之前保留的成果。
bool testDrawingRollback()
{
    MMM::UI::Walkthrough::Spotlight spotlight;
    int                             notes = 0, flicks = 0, holds = 0;
    spotlight.start({ "note" }, "Note", true);
    const auto noteToken = spotlight.stepToken();
    spotlight.registerRollback("note", [&] { ++notes; });
    spotlight.completeTarget("note", true);
    spotlight.start({ "flick" }, "Flick", true);
    // 正常衔接保留单键，不把完成步骤当成需要撤销的退出。
    if ( notes != 0 || spotlight.stepToken() == noteToken ) return false;
    spotlight.registerRollback("flick", [&] { ++flicks; });
    spotlight.completeTarget("flick", true);
    spotlight.start({ "hold" }, "Hold", true);
    // 模拟创建已成功而页面尚未衔接的窄窗口，返回仍需处理当前成果。
    // 不依赖完成态来判断是否有产物，唯一依据是已登记的业务补偿。
    spotlight.registerRollback("hold", [&] { ++holds; });
    spotlight.requestPrevious();
    // 请求立即消费当前步骤，页面确认前重复点击也不会执行第二次。
    spotlight.requestPrevious();
    if ( holds != 1 || flicks != 0 || notes != 0 ||
         !spotlight.consumePreviousStepRequest() )
        return false;
    spotlight.start({ "flick" }, "Flick", true, true);
    // 重练滑键只清除滑键，必须保留作为邻近定位基准的单键。
    if ( flicks != 1 || notes != 0 ) return false;
    spotlight.completeTarget("flick");
    if ( spotlight.completed() ) return false;
    // 回看防自动跳步不应阻止用户重新画出的正确结果。
    spotlight.registerRollback("flick", [&] { ++flicks; });
    spotlight.completeTarget("flick", true);
    if ( !spotlight.completed() ) return false;
    spotlight.requestPrevious();
    spotlight.consumePreviousStepRequest();
    spotlight.start({ "note" }, "Note", true, true);
    // 连续返回必须同时移除重新绘制的滑键和首次单键，但不能重复撤长条。
    // 这也验证同一个语义目标在重练后可以登记新的独立补偿。
    if ( flicks != 2 || notes != 1 || holds != 1 ) return false;
    // 正常退出保留成果，并且下轮返回不能消费上一轮的闭包。
    spotlight.registerRollback("note", [&] { ++notes; });
    spotlight.stop();
    spotlight.start({ "note" }, "Note", true, true);
    // 退出不重置单调步骤身份，旧补偿则必须已经释放。
    spotlight.requestPrevious();
    return notes == 1;
}

/// @brief 删除练习不能通过确认跳过，必须等待业务身份核对后显式完成。
/// @details 此处模拟已经有可见高亮框的状态，而非缺失目标等待态。
/// 如确认按钮在普通步骤可用、强制步骤仍可跳过，测试必须能区分。
/// 完成后的业务回调沿用原有 completeTarget 状态转换，不引入第二终态。
/// 此测试只验证高亮层权限，具体物件存活由逻辑快照负责判断。
/// 返回后必须显式确认，不能根据历史步骤完成度自动前进。
/// 高亮框传入主视口，测试在非窗口提交阶段也不会借用当前窗口。
bool testRequiresAction()
{
    MMM::UI::Walkthrough::Spotlight spotlight;
    spotlight.start(
        { "practice.delete-hold" }, "Right-click Hold", true, false, true);
    spotlight.reportTarget("practice.delete-hold",
                           { 10.0F, 10.0F },
                           { 30.0F, 30.0F },
                           ImGui::GetMainViewport());
    spotlight.acknowledgeCurrentStage();
    // 突出区域存在也不代表业务成功，用户确认不能清掉实际物件。
    if ( spotlight.completed() ) return false;
    spotlight.completeTarget("practice.delete-hold", true);
    if ( !spotlight.completed() ) return false;
    spotlight.start({ "practice.delete-hold" }, "Review", true, true, true);
    // 返回已删过的步骤不能自动跳走，但重新核实后可由用户明确继续。
    spotlight.reportReviewedActionSatisfied();
    if ( spotlight.completed() ) return false;
    spotlight.acknowledgeCurrentStage();
    return spotlight.completed();
}

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

/// @brief 覆盖显式回退、目标锁定、缺席导航和模态返回按钮的完整手势。
/// @details 状态部分不依赖控件存在，输入部分使用真实 ImGui 模态窗口。
/// 两部分分别定位导航协议错误与输入层级错误，不能互相替代。
/// 缺席目标只检查导航气泡，不能把没有高亮矩形误当作返回失败。
/// 所有坐标来自当帧按钮实际中心，不依赖文本宽度或硬编码屏幕位置。
/// 返回不负责修改业务窗口，这里只断言模态仍打开以及教程水位变化。
/// @return 返回不会被已有状态推走，且模态按钮仅在合法释放时激活。
bool testPreviousNavigation()
{
    MMM::UI::Walkthrough::Spotlight spotlight;
    spotlight.start({ "before", "after" }, "Review");
    // 首目标且无前一步时禁止返回，不将索引减成无符号上溢值。
    // 默认调用没有路线前驱，保持独立 Spotlight 实例的首步边界。
    if ( spotlight.canGoBack() ) return false;
    spotlight.requestPrevious();
    if ( !spotlight.awaitingTarget("before") ) return false;
    spotlight.reportTarget(
        "after", { 20, 20 }, { 80, 80 }, ImGui::GetMainViewport());
    spotlight.requestPrevious();
    if ( !spotlight.reviewing() || !spotlight.awaitingTarget("before") ||
         spotlight.resolvedTargetBounds() )
        return false;
    // 后续窗口仍存在、当前业务持续上报完成，都不能抢走主动回看的目标。
    // 这模拟已经打开的标签页持续报告完成，而不是新的用户确认。
    // 返回当帧还要立即清除原锚点，不能等到下一次清帧才撤掉。
    spotlight.reportTarget(
        "after", { 20, 20 }, { 80, 80 }, ImGui::GetMainViewport());
    spotlight.completeTarget("before");
    if ( !spotlight.awaitingTarget("before") ) return false;
    // 目标已关闭时仍允许“知道了”显式向前，不伪造重新打开窗口的动作。
    // 显式确认只解开当前目标，仍留在同一配置步骤的回看模式。
    spotlight.acknowledgeCurrentStage();
    if ( !spotlight.awaitingTarget("after") ) return false;
    spotlight.start({}, "Text only", true);
    spotlight.requestPrevious();
    // 跨步骤请求只能消费一次，不能在下一帧再次回退一整步。
    // 纯文字步骤没有目标索引，也必须能请求回到路线前一步。
    // 后续 stop 必须清空回看标记和返回能力，不污染另一条路线。
    if ( !spotlight.consumePreviousStepRequest() ||
         spotlight.consumePreviousStepRequest() )
        return false;
    spotlight.stop();
    if ( spotlight.canGoBack() || spotlight.reviewing() ) return false;

    /// @brief 使用真实模态窗口驱动返回按钮，不绕开 ImGui 输入分发。
    const auto frame = [&](bool open) {
        // 顺序与生产一致：清帧、业务目标上报、续租、最终导航气泡。
        ImGui::NewFrame();
        spotlight.beginFrame();
        const bool modalOpen = submitModalTarget(spotlight, open);
        spotlight.keepAlive();
        spotlight.render(1.0f, "Got it", "Previous");
        ImGui::Render();
        return modalOpen;
    };
    spotlight.start(
        { "modal.before", "modal.target" }, "Back inside a modal", true);
    frame(true);
    frame(false);
    const auto center = spotlight.previousButtonCenter();
    // 前两帧已使即时模式窗口落到最终布局，此时才取得输入坐标。
    if ( !center ) return false;
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(center->x, center->y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame(false);
    // 按下只建立手势，不提前改变步骤或借机关闭模态业务窗口。
    // 释放必须走与普通按钮一致的激活边界，不能仅凭鼠标命中就导航。
    if ( !spotlight.awaitingTarget("modal.target") ) return false;
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    const bool modalSurvived = frame(false);
    if ( !modalSurvived || !spotlight.awaitingTarget("modal.before") )
        return false;
    // 前序目标在当前模态窗口中缺席，导航气泡仍可返回或继续。
    // 既要没有虚假目标，也要两个导航入口仍存在。
    // 模态仍在遮挡普通窗口，按钮不能靠关闭弹窗来绕过输入限制。
    frame(false);
    if ( spotlight.resolvedTargetBounds() ||
         !spotlight.previousButtonCenter() ||
         !spotlight.acknowledgeButtonCenter() )
        return false;

    // 从按钮按下再拖出取消，不允许仅靠释放时的旧坐标触发返回。
    // 切换目标时原按键锁存应已清空，不得继承上一轮合法点击。
    spotlight.start({ "modal.target" }, "Cancel a dragged button", true);
    frame(false);
    frame(false);
    const auto cancelCenter = spotlight.previousButtonCenter();
    if ( !cancelCenter ) return false;
    io.AddMousePosEvent(cancelCenter->x, cancelCenter->y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame(false);
    io.AddMousePosEvent(-100, -100);
    // 拖出单独经历一帧，以验证手势失效状态被锁存。
    frame(false);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame(false);
    if ( spotlight.consumePreviousStepRequest() || spotlight.reviewing() )
        return false;
    // 最后一次合法点击走跨步骤请求，确认按钮不得同时完成本步。
    // 不重新 start，证明取消状态能供下一次合法点击继续使用。
    // 此时在首目标请求返回，页面层接手而不是再次修改局部索引。
    // 请求与完成终态必须互斥，否则页面下一帧可能前进而不是后退。
    // 这也检查两个按钮的手势缓存没有被前一次拖出操作串在一起。
    // 一次合法请求的消费不会触发其它窗口动作或更改项目状态。
    const auto finalCenter = spotlight.previousButtonCenter();
    if ( !finalCenter ) return false;
    io.AddMousePosEvent(finalCenter->x, finalCenter->y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame(false);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame(false);
    return spotlight.consumePreviousStepRequest() && !spotlight.completed();
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

    // 启动后业务层应能在目标矩形首次上报前识别当前水位，供画布只在
    // 对应步骤生成随机但整段手势内固定的教学几何。
    spotlight.start(targets, "Follow the highlighted control");
    if ( !spotlight.awaitingTarget("test.first") ||
         spotlight.awaitingTarget("test.second") ) {
        ImGui::DestroyContext();
        return 62;
    }

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
        spotlight.awaitingTarget("test.second") &&
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
    // 最后一项目标成功后进入 Completed，路线页面下一帧可衔接后续步骤。
    spotlight.completeTarget("test.second");
    if ( !spotlight.active() || !spotlight.completed() ) {
        ImGui::DestroyContext();
        return 10;
    }

    // 多个谱面标签共享同一语义目标；高亮范围应覆盖全部并列候选，
    // 而不是被最后一个上报标签覆盖成固定选择。
    // 两个矩形故意留出间隔，证明结果取外接范围而非第二项本身。
    // 它们共用主视口，符合实际同一 Dock 标签栏内多个谱面的条件。
    // 本用例只检查目标解析；具体标签点击由画布自身命中区负责，
    // 避免把 ImGui 内部 Dock 构造细节复制进 Spotlight 单元测试。
    ImGui::NewFrame();
    spotlight.beginFrame();
    spotlight.start({ "choice.tab" }, "Choose any tab");
    spotlight.reportTarget("choice.tab",
                           { 80.0F, 20.0F },
                           { 180.0F, 52.0F },
                           ImGui::GetMainViewport());
    spotlight.reportTarget("choice.tab",
                           { 260.0F, 20.0F },
                           { 380.0F, 52.0F },
                           ImGui::GetMainViewport());
    const auto choiceBounds = spotlight.resolvedTargetBounds();
    const bool choiceRangeValid =
        choiceBounds && choiceBounds->minimum.x == 80.0F &&
        choiceBounds->minimum.y == 20.0F && choiceBounds->maximum.x == 380.0F &&
        choiceBounds->maximum.y == 52.0F;
    spotlight.stop();
    ImGui::Render();
    if ( !choiceRangeValid ) {
        ImGui::DestroyContext();
        return 60;
    }

    // 右侧纵向工具栏没有上下空间，但左侧足够容纳收窄后的提示气泡。
    // 提示窗口必须完整位于目标左侧，避免遮住上方工具按钮。
    // 目标高度覆盖几乎整个视口，确保上下两个候选方向都会失败。
    // 长提示触发宽度约束，覆盖实际中文说明可能出现的换行路径。
    // 断言保留十二像素间距，同时验证方向选择与目标边距。
    // 窗口位置必须在 render 后读取，因为 Begin 才会应用请求尺寸。
    // 全程只依赖 ImGui 内存路径，无需建立图形后端或上传字体纹理。
    ImGui::NewFrame();
    spotlight.beginFrame();
    spotlight.start({ "toolbar.vertical" },
                    "This toolbar contains the visible editing tools and "
                    "common chart controls.");
    spotlight.reportTarget("toolbar.vertical",
                           { 720.0F, 20.0F },
                           { 792.0F, 580.0F },
                           ImGui::GetMainViewport());
    spotlight.keepAlive();
    spotlight.render(1.0F, "Got it");
    const auto* toolbarHint =
        ImGui::FindWindowByName("###WalkthroughSpotlightHint");
    const bool toolbarHintClear =
        toolbarHint && toolbarHint->Pos.x + toolbarHint->Size.x <= 708.0F;
    spotlight.stop();
    ImGui::Render();
    if ( !toolbarHintClear ) {
        ImGui::DestroyContext();
        return 61;
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
    if ( !spotlight.active() || !spotlight.completed() ) {
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
    // 第三项是最终目标，确认后保留 Completed，供页面衔接下一路线步骤。
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
    const bool finalStageValid =
        nextStageValid && spotlight.active() && spotlight.completed();
    spotlight.render(1.0f, "Got it");
    ImGui::Render();
    if ( !finalStageValid ) {
        ImGui::DestroyContext();
        return 7;
    }

    // 无候选目标的快捷键步骤仍提交提示，但不能解析出虚假的控件身份。
    // 纯文字提示不绘制前景遮罩，但提供“知道了”作为明确的路线推进入口。
    ImGui::NewFrame();
    spotlight.beginFrame();
    spotlight.start({}, "Press Ctrl+Shift+N");
    spotlight.keepAlive();
    foreground = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    const int promptVerticesBefore = foreground->VtxBuffer.Size;
    spotlight.render(1.0f, "Got it");
    const bool promptOnlyValid =
        spotlight.resolvedTargetId().empty() &&
        foreground->VtxBuffer.Size == promptVerticesBefore &&
        spotlight.acknowledgeButtonCenter().has_value();
    spotlight.acknowledgeCurrentStage();
    const bool promptCompleted = spotlight.completed();
    ImGui::Render();
    if ( !promptOnlyValid || !promptCompleted ) {
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
    const bool previousValid = testPreviousNavigation() &&
                               testDrawingRollback() && testRequiresAction();
    ImGui::DestroyContext();
    return !hiddenValid ? 9 : previousValid ? 0 : 63;
}
