#include "ui/workspace/MobileTouchScroll.h"
#include "log/colorful-log.h"
#include <cmath>
#include <imgui_internal.h>

/// @file MobileTouchScrollTest.cpp
/// @brief 用真实 ImGui 输入验证组件起手滚动、延后点击和数值方向仲裁。
/// @details 测试不创建 SDL 窗口、不打开音频设备，也不读取用户配置。
/// 字体图集在 CPU 构建，滚动位置从窗口 Begin 后的公开 API 读取。
/// 手势按下、移动、释放分别驱动，不能只调用内部状态赋值伪造结果。
/// 同一个窗口同时包含滑条与空白，因此可以检查二者的输入互斥。

namespace
{
/// @brief 在没有平台和 GPU 后端的上下文中驱动真实窗口与触摸事件。
struct Fixture {
    /// @brief 与生产移动外壳使用同一滚动控制器，不模拟其内部判断。
    MMM::UI::MobileTouchScroll scroll;
    /// @brief 用真实滑条确认横向调值与纵向滚动在确定方向前都不会误改值。
    float value{ .2f };
    /// @brief 竖滑条单独启用，原有按钮与弹层场景的位置保持不变。
    bool m_withVertical{};
    /// @brief 直接观察真实竖滑条数值，不通过改写内部 ActiveID 模拟调节。
    float m_verticalValue{ .5f };
    /// @brief 每帧记录真实命中中心，测试无需假设滑条的边框和留白。
    ImVec2 m_verticalCenter{};
    /// @brief 原生按钮的真实激活次数，用于检查滑动和轻点是否被正确区分。
    int buttonClicks{};
    /// @brief 由实际控件布局记录起点，避免字号变化使测试落在空白处。
    ImVec2 buttonCenter{};
    /// @brief 原生折叠标题默认在按下沿响应，必须验证拖动不会误折叠。
    bool headerOpen{};
    /// @brief 折叠标题的真实中心用于复现按下即响应组件。
    ImVec2 headerCenter{};
    /// @brief 下拉选项不能在滑动抬手时被误选中并收起弹层。
    int selectionClicks{};
    /// @brief 首个真实选项的中心位置，覆盖弹层组件起手而非仅空白滚动。
    ImVec2 optionCenter{};
    /// @brief 当前帧已应用的滚动位置，取自 Begin 后的窗口内容。
    ImVec2 offset{};
    /// @brief 按需在同一个可滚动背景中加入真实下拉框与背景控件。
    bool withPopup{ false };
    /// @brief 背景控件不得被用于关闭弹层的同一次触摸激活。
    bool checked{ false };
    /// @brief 从真实布局记录入口和背景控件的位置，不依赖默认字体尺寸。
    ImVec2 comboCenter{}, checkboxCenter{}, popupBlank{};
    /// @brief 弹层滚动量与最终开合状态，分别验证内容拖动和原生收起。
    float popupOffset{};
    /// @brief 在 Render 之后采样，包含 ImGui 帧末执行的关闭行为。
    bool popupOpen{ false };
    /// @brief 子窗口真实身份用于重置场景，避免父窗口复位掩盖子窗口旧滚动量。
    ImGuiID bodyWindow{};

    /// @brief 提交一次输入和完整 ImGui 帧，位置采用屏幕逻辑像素。
    /// @param wide 为 true 时同时提供横向超宽内容。
    /// @details 禁止读取用户 ini，所有几何固定，断言不依赖屏幕与帧率。
    /// withPopup 场景把控件放入子窗口，覆盖设置页的嵌套滚动结构。
    /// 弹层仍由 BeginCombo 创建，使阻挡背景和焦点恢复使用真实 ImGui 路径。
    /// 调用顺序与移动外壳一致：布局前采样，所有控件提交后确认归属。
    /// @param pointer 当前物理坐标，可拖出窗口；捕获不能依赖后续
    /// HoveredWindow。
    /// @param down 物理按住状态，不把控制器补交的点击当作下一次设备事件。
    /// 每次调用都经过 NewFrame 的真实输入更新，避免直接写 MouseClicked
    /// 绕过后端状态。
    /// 按下、移动与释放分别提交，因此能检测拖动途中短暂误开菜单或误改数值。
    /// 宽内容与嵌套内容由真实 Dummy 产生滚动范围，不伪造窗口的内部最大值。
    /// 弹层另设位置，确保外部点击检查不会退化成点击同一个选项。
    void frame(ImVec2 pointer, bool down, bool wide = false)
    {
        // 输入来源显式设置为触屏；生产桌面鼠标不应该受此控制器影响。
        // 同一帧事件进入同一个上下文，避免上一场景残留的源类型干扰结果。
        auto& io = ImGui::GetIO();
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(pointer.x, pointer.y);
        io.AddMouseButtonEvent(0, down);
        ImGui::NewFrame();
        // 过滤先于任何组件，但不妨碍组件记录真实几何和鼠标命中身份。
        // 帧末恢复输入后继续 Render，覆盖原生帧末弹层收起和窗口移动判断。
        scroll.beginFrame();
        ImGui::SetNextWindowPos({ 20, 20 });
        ImGui::SetNextWindowSize({ 360, 260 });
        const auto flags = ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoTitleBar |
                           ImGuiWindowFlags_NoSavedSettings |
                           (wide ? ImGuiWindowFlags_HorizontalScrollbar : 0);
        ImGui::Begin("TouchFixture", nullptr, flags);
        // 设置页的滚动发生在子窗口，父窗口仍是弹层的焦点恢复目标。
        if ( withPopup ) {
            ImGui::BeginChild("SettingsBody");
            bodyWindow = ImGui::GetCurrentWindow()->ID;
        }
        // 无标题固定位置保证测试触点不会落到窗口拖动区或系统边框。
        // 控件位于顶部，其他位置保持空白，便于区分两种触摸起点。
        if ( m_withVertical ) {
            // 纵向主方向应编辑参数，横向则留给超宽频段列表滚动。
            // 起手先登记几何，不让物理按下提前改变数值。
            ImGui::VSliderFloat(
                "##vertical", { 48, 160 }, &m_verticalValue, 0, 1, "");
            scroll.registerVerticalDragItem();
            const auto min   = ImGui::GetItemRectMin();
            const auto max   = ImGui::GetItemRectMax();
            m_verticalCenter = { (min.x + max.x) * .5f, (min.y + max.y) * .5f };
        } else {
            ImGui::SliderFloat("value", &value, 0, 1);
            // 与生产 FeedbackSlider
            // 使用同一语义登记；测试无需创建音频反馈设备。
            // 不登记其他组件，以证明普通按钮和下拉框不需要各自添加滚动补丁。
            scroll.registerHorizontalDragItem();
        }
        // 原生按钮没有统一反馈辅助也须受全局滚动仲裁保护。
        // 该控件只属于无音频设备的测试夹具，不属于业务可见按钮。
        if ( ImGui::Button("Action", { 200, 30 }) ) ++buttonClicks;
        const auto buttonMin = ImGui::GetItemRectMin();
        const auto buttonMax = ImGui::GetItemRectMax();
        buttonCenter         = { (buttonMin.x + buttonMax.x) / 2,
                                 (buttonMin.y + buttonMax.y) / 2 };
        headerOpen           = ImGui::CollapsingHeader("Section");
        const auto headerMin = ImGui::GetItemRectMin();
        const auto headerMax = ImGui::GetItemRectMax();
        headerCenter         = { (headerMin.x + headerMax.x) / 2,
                                 (headerMin.y + headerMax.y) / 2 };
        if ( withPopup ) {
            // 弹层移到背景窗口旁边，入口、背景空白和弹层内部互不重叠。
            ImGui::SetNextWindowPos({ 420, 80 });
            const bool comboVisible = ImGui::BeginCombo(
                "profile", "desktop", ImGuiComboFlags_HeightSmall);
            // 弹层 Begin 本身也可能改写 LastItem，只在关闭状态读取真实入口。
            // 打开期间父布局不变，继续使用此前冻结的入口，不能把弹层当作入口。
            if ( !comboVisible ) {
                // 不读回弹层坐标，确保“再点入口”的断言确实落在父窗口入口上。
                const auto comboMin = ImGui::GetItemRectMin();
                const auto comboMax = ImGui::GetItemRectMax();
                comboCenter         = { (comboMin.x + comboMax.x) / 2,
                                        (comboMin.y + comboMax.y) / 2 };
            }
            if ( comboVisible ) {
                if ( ImGui::IsWindowAppearing() ) ImGui::SetScrollY(0);
                // 真正的长内容产生滚动范围，不能通过手动改写 ScrollMax 伪造。
                // 保留可选项和空白两类区域，空白滚动不能用选择动作代替。
                // 原生弹层限制高度，长内容不会因为自动扩展而失去滚动范围。
                if ( ImGui::Selectable("desktop") ) ++selectionClicks;
                const auto optionMin = ImGui::GetItemRectMin();
                const auto optionMax = ImGui::GetItemRectMax();
                optionCenter         = { (optionMin.x + optionMax.x) / 2,
                                         (optionMin.y + optionMax.y) / 2 };
                ImGui::Selectable("touch");
                const auto position = ImGui::GetCursorScreenPos();
                // 起点落在选项下方、滚动条左侧，避免被可点击项目正常占用。
                popupBlank = { position.x + 60, position.y + 12 };
                ImGui::Dummy({ 180, 700 });
                popupOffset = ImGui::GetScrollY();
                ImGui::EndCombo();
            }
            ImGui::Checkbox("background", &checked);
            // 背景控件独立记录命中区，不假设下拉入口和行高保持固定偏移。
            const auto checkMin = ImGui::GetItemRectMin();
            const auto checkMax = ImGui::GetItemRectMax();
            checkboxCenter      = { (checkMin.x + checkMax.x) / 2,
                                    (checkMin.y + checkMax.y) / 2 };
        }
        ImGui::Dummy({ wide ? 800.0f : 1.0f, 700 });
        offset = { ImGui::GetScrollX(), ImGui::GetScrollY() };
        if ( withPopup ) ImGui::EndChild();
        // 读取实际应用值，不能以控制器计划提交的目标代替视觉已更新的证据。
        ImGui::End();
        scroll.endFrame();
        ImGui::Render();
        // 原生关闭发生在帧末，BeginCombo 的返回值不足以证明已经收起。
        popupOpen = !ImGui::GetCurrentContext()->OpenPopupStack.empty();
    }

    /// @brief 轻点包含完整按下与释放，检查入口不会在释放时重新打开。
    /// @details 不合并成一个帧，以保留原生按钮和弹层的按下、释放语义。
    /// 抬手后补一轮普通帧，让原生按钮处理已确认点击的配对释放。
    /// 再推进一轮检查稳定态，不能把仍在释放保护中的中间结果当作成功。
    void tap(ImVec2 pointer)
    {
        frame(pointer, true);
        frame(pointer, false);
        // 经过出现帧保护后再执行下一手势，同时验证释放没有再次打开入口。
        frame(pointer, false);
        frame(pointer, false);
    }

    /// @brief 清除滚动目标并推进布局，避免上一个场景的位置掩盖断言。
    /// @details 使用原生滚动 API，不修改被测控制器私有成员。
    /// 父窗口和实际滚动子窗口分别归零，避免旧偏移让预定组件落到裁剪区外。
    /// 重置只属于测试准备，不用于修补被测控制器的状态机或未配对触点。
    void reset(bool wide = false)
    {
        // SetScroll 写入下一次 Begin 的目标，先推进帧再建立新的触摸起点。
        if ( auto* window = ImGui::FindWindowByName("TouchFixture") ) {
            ImGui::SetScrollX(window, 0);
            ImGui::SetScrollY(window, 0);
        }
        if ( auto* body = ImGui::FindWindowByID(bodyWindow) ) {
            ImGui::SetScrollX(body, 0);
            ImGui::SetScrollY(body, 0);
        }
        frame({ -100, -100 }, false, wide);
        frame({ -100, -100 }, false, wide);
        // 第二帧让横向滚动范围从新内容宽度稳定下来，不读取旧场景的 ScrollMax。
    }
};
}  // namespace

/// @brief 验证组件起手纵向与双轴拖动、轻点补交、弹层及数值方向互斥。
/// @details 测试只依赖 CPU 字体图集，失败通过返回值和项目日志报告。
/// 所有业务值在真实控件调用后读取，不以模拟的触摸状态替代行为断言。
/// 按方向分组验证数值编辑，按控件激活沿分组验证防误触，二者不能互相抵扣。
/// 弹层内外分别测试，保证优先滚动不会破坏点击外部收起的既有契约。
/// @warning 独占测试上下文，不运行平台事件泵，不连接设备或修改用户设置。
int main()
{
    ImGui::CreateContext();
    // 每次运行独占上下文，ActiveID 和捕获状态不会来自真实软件窗口。
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = { 900, 700 };
    io.DeltaTime   = 1.0f / 60;
    // 每帧完整输入立即消费，测试不依赖输入队列分帧细节。
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels           = nullptr;
    int            width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    // 动态字体相关状态由同一 UI 线程建立，后续帧只提交固定测试内容。
    Fixture fixture;
    int     failures = 0;
    auto    check    = [&](bool result, const char* message) {
        if ( !result ) {
            ++failures;
            XERROR("Touch scroll: {}", message);
        }
    };
    fixture.reset();
    // 先保留既有空白拖动能力作为基线，新组件路径不能靠禁用整窗输入换取成功。
    // 滚动在移动帧读出视觉已应用值，避免只登记目标、释放后才刷新界面的实现。
    // 起点在空白处，下一移动帧就应出现滚动，而不是等到手指释放。
    fixture.frame({ 330, 200 }, true);
    // 330,200 远离顶部滑条和右侧滚动条，确实属于窗口空白内容。
    check(fixture.scroll.ownsPointer(), "blank press captures pointer");
    fixture.frame({ 330, 100 }, true);
    // 移动量超过抖动阈值且小于滚动上限，可识别未及时应用目标的问题。
    check(fixture.offset.y > 90, "vertical drag updates in the same frame");
    fixture.frame({ 330, 100 }, false);
    check(fixture.scroll.ownsPointer(),
          "release cannot reach background canvas");
    // 保护只覆盖当前触点的最终释放，不以固定毫秒数延迟下一次操作。
    // 之后的独立轻点必须可用，否则快速连续编辑会产生新的粘手问题。
    // 相同坐标再提交一个非释放帧，保护应自然结束而不是永久拦截。
    fixture.frame({ 330, 100 }, false);
    check(!fixture.scroll.ownsPointer(), "next gesture is no longer blocked");
    fixture.reset();
    // 重置后顶部控件重新进入可见区，滑条输入不能被上一次滚动遮住。
    // 滑条和空白共用窗口，必须根据控件 ActiveID 判断，而非整窗接管。
    // 相同滑条首先等待意图，不应仅因物理按下就跳到触点对应的数值。
    // 水平移动确认后必须正常改值，证明仲裁没有把滑条永久变成只可滚动区域。
    fixture.frame({ 120, 40 }, true);
    check(fixture.scroll.ownsPointer() && std::abs(fixture.value - .2f) < .001f,
          "slider press is deferred until intent is known");
    fixture.frame({ 220, 40 }, true);
    // 初始值是 0.2，跨过 0.5 可确认真实滑条处理了水平移动。
    // 同时检查数值和滚动量，避免滑条不响应却被误判为成功保留归属。
    check(fixture.value > .5f && fixture.offset.y < .01f,
          "slider changes value without scrolling");
    fixture.frame({ 220, 40 }, false);
    fixture.reset();
    // 滑条起点使用同一物理坐标，唯一区别是后续位移方向。
    // 数值保持不变可排除先改值再回滚的实现，滚动量证明手势确实被消费。
    const float beforeVertical = fixture.value;
    fixture.frame({ 120, 40 }, true);
    fixture.frame({ 120, -60 }, true);
    check(fixture.offset.y > 90 && fixture.value == beforeVertical,
          "vertical gesture on slider scrolls without changing value");
    fixture.frame({ 120, -60 }, false);
    fixture.reset();
    // 按钮区域不是空白起点，回归旧实现“组件占用便放弃滚动”的条件。
    // 手指移出窗口后继续消费同一手势，不把边界外释放交给背景画布。
    const auto button = fixture.buttonCenter;
    fixture.frame(button, true);
    check(fixture.buttonClicks == 0,
          "button does not activate on physical down");
    fixture.frame({ button.x, button.y - 110 }, true);
    fixture.frame({ button.x, button.y - 110 }, false);
    fixture.frame({ button.x, button.y - 110 }, false);
    check(fixture.offset.y > 100 && fixture.buttonClicks == 0,
          "gesture beginning on button scrolls without activation");
    fixture.reset();
    fixture.tap(fixture.buttonCenter);
    check(fixture.buttonClicks == 1, "short button tap activates exactly once");
    fixture.reset();
    // 原生折叠标题在按下时就可能执行动作，比释放触发的按钮更早。
    // 按下帧先检查展开状态，确保不是拖动后再补救性恢复。
    const auto header = fixture.headerCenter;
    fixture.frame(header, true);
    check(!fixture.headerOpen,
          "folding header does not react to physical down");
    fixture.frame({ header.x, header.y - 100 }, true);
    fixture.frame({ header.x, header.y - 100 }, false);
    check(!fixture.headerOpen && fixture.offset.y > 90,
          "gesture beginning on folding header scrolls without folding");
    fixture.reset();
    fixture.tap(fixture.headerCenter);
    check(fixture.headerOpen, "short header tap changes folding state");
    fixture.tap(fixture.headerCenter);
    fixture.reset(true);
    // 超宽 Dummy 真正产生横向范围，没有直接改写窗口内部 ScrollMax。
    // 表格可能同时具有两条滚动轴，同一空白手势应更新两个滚动目标。
    fixture.frame({ 330, 200 }, true, true);
    fixture.frame({ 250, 120 }, true, true);
    check(fixture.offset.x > 70 && fixture.offset.y > 70,
          "wide content supports both axes");
    fixture.frame({ 250, 120 }, false, true);
    fixture.withPopup = true;
    fixture.reset();
    // 先证明子窗口确实可以滚动，否则关闭检查可能绕过问题路径。
    fixture.frame({ 330, 210 }, true);
    check(fixture.scroll.ownsPointer(),
          "scrollable settings child captures blank");
    fixture.frame({ 330, 210 }, false);
    // 下拉框与折叠标题共享“按下即响应”的风险，但还会改变模态层级。
    // 滑动后无弹层，可证明没有先打开弹层再因为手指移出而关闭的闪烁。
    const auto combo = fixture.comboCenter;
    fixture.frame(combo, true);
    check(!fixture.popupOpen, "combo does not open on physical down");
    fixture.frame({ combo.x, combo.y - 100 }, true);
    fixture.frame({ combo.x, combo.y - 100 }, false);
    check(!fixture.popupOpen && fixture.offset.y > 90,
          "gesture beginning on combo scrolls without opening");
    fixture.reset();
    fixture.tap(fixture.comboCenter);
    check(fixture.popupOpen, "combo opens after short touch release");
    // 外部关闭手势没有新业务目标，背景复选框即使恰在触点下也不能切换。
    // 每轮先重新打开弹层，避免后续断言只是重复检查已经关闭的状态。
    // 背景具有真实滚动范围，复现空白滚动控制器抢占关闭点击的条件。
    // 弹层阻挡的背景没有活动控件，不能将这种状态误认为允许空白拖动。
    fixture.frame({ 330, 210 }, true);
    check(fixture.scroll.ownsPointer(),
          "outside popup press is consumed without background interaction");
    fixture.frame({ 330, 210 }, false);
    check(!fixture.popupOpen, "outside blank touch dismisses combo");
    // 各场景重新打开真实弹层，保证关闭状态不会让后续检查退化成空操作。
    fixture.tap(fixture.comboCenter);
    check(fixture.popupOpen, "combo reopens after outside dismissal");
    // 再点入口属于弹层外点击，释放以后也必须保持关闭。
    fixture.tap(fixture.comboCenter);
    check(!fixture.popupOpen, "second touch on opener dismisses combo");
    fixture.tap(fixture.comboCenter);
    check(fixture.popupOpen, "combo opens before background control test");
    fixture.tap(fixture.checkboxCenter);
    // 与空白位置分别验证：用户经常直接点击另一个设置项来收起列表。
    // 关闭手势不能穿透，避免用户收起菜单时意外修改设置。
    check(!fixture.popupOpen && !fixture.checked,
          "outside control dismisses without activating background");
    fixture.tap(fixture.comboCenter);
    check(fixture.popupOpen, "combo opens before content scrolling test");
    fixture.frame(fixture.popupBlank, false);
    const auto blank = fixture.popupBlank;
    // 稳定布局后固定手势原点，拖动过程中不追随内容移动。
    fixture.frame(blank, true);
    check(fixture.scroll.ownsPointer(), "popup blank still captures scrolling");
    fixture.frame({ blank.x, blank.y - 30 }, true);
    // 弹层内部的空白拖动仍应立即滚动，不能为了关闭行为禁用所有弹层滚动。
    check(fixture.popupOpen && fixture.popupOffset > 20,
          "popup content scrolls without closing");
    fixture.frame({ blank.x, blank.y - 30 }, false);
    // 空白和组件拖动共享同一捕获机制，两者均须在释放帧阻止编辑画布收到触点。
    // 完成后再执行轻点与连点，检查输入恢复不会让后续手势沿用旧活动身份。
    // 销毁上下文之前释放任何残留输入，测试可以连续创建新的 ImGui 上下文。
    fixture.frame({ 250, 120 }, false, true);
    fixture.tap({ 330, 210 });
    check(!fixture.popupOpen, "popup is dismissed before checkbox scenarios");
    // 所有动作均走真实控件，滑动不能通过先切换再回滚状态蒙混通过。
    fixture.reset();
    // 复选框的值可能直接写入配置，所以滚动过程中不能发生任何切换。
    // 随后的短按必须仍能切换一次，避免用禁用控件的方式通过防误触检查。
    const auto checkbox = fixture.checkboxCenter;
    fixture.frame(checkbox, true);
    fixture.frame({ checkbox.x, checkbox.y - 100 }, true);
    fixture.frame({ checkbox.x, checkbox.y - 100 }, false);
    fixture.frame({ checkbox.x, checkbox.y - 100 }, false);
    check(!fixture.checked && fixture.offset.y > 90,
          "gesture beginning on checkbox scrolls without toggling");
    fixture.reset();
    fixture.tap(fixture.checkboxCenter);
    check(fixture.checked, "checkbox short tap toggles once");
    fixture.tap(fixture.comboCenter);
    check(fixture.popupOpen, "combo opens before option gesture");
    // 同一弹层前面已经有合法选择历史，以本手势之前的次数为基线。
    // 只判断增量，不把此前真实点击误当作这次拖动的副作用。
    const int  beforeSelection = fixture.selectionClicks;
    const auto option          = fixture.optionCenter;
    fixture.frame(option, true);
    fixture.frame({ option.x, option.y - 45 }, true);
    fixture.frame({ option.x, option.y - 45 }, false);
    fixture.frame({ option.x, option.y - 45 }, false);
    if ( !fixture.popupOpen || fixture.selectionClicks != beforeSelection ||
         fixture.popupOffset <= 35 )
        XERROR(
            "Option drag state: open={}, selections={}, offset={}, origin=({}, "
            "{})",
            fixture.popupOpen,
            fixture.selectionClicks,
            fixture.popupOffset,
            option.x,
            option.y);
    check(fixture.popupOpen && fixture.selectionClicks == beforeSelection &&
              fixture.popupOffset > 35,
          "dragging a popup option scrolls without selection or dismissal");
    // 前一次点击的配对释放与后一次物理按下处于同帧时，两个动作都不能丢失。
    fixture.tap({ 330, 210 });
    fixture.reset();
    // 两个触点选择不同组件，后一个触点的位置不能覆盖前一次点击的释放位置。
    // 不插入空闲帧，专门覆盖真实低帧率下输入边沿相邻的情况。
    const int  beforeRapid = fixture.buttonClicks;
    const bool beforeCheck = fixture.checked;
    const auto rapidButton = fixture.buttonCenter;
    const auto rapidCheck  = fixture.checkboxCenter;
    fixture.frame(rapidButton, true);
    fixture.frame(rapidButton, false);
    fixture.frame(rapidCheck, true);
    fixture.frame(rapidCheck, false);
    fixture.frame(rapidCheck, false);
    fixture.frame(rapidCheck, false);
    if ( fixture.buttonClicks != beforeRapid + 1 ||
         fixture.checked == beforeCheck )
        XERROR("Rapid tap state: button={}/{}, checkbox={}/{}, combo={}",
               fixture.buttonClicks,
               beforeRapid,
               fixture.checked,
               beforeCheck,
               fixture.popupOpen);
    check(fixture.buttonClicks == beforeRapid + 1 &&
              fixture.checked != beforeCheck,
          "back-to-back taps on different controls preserve both actions");
    // 与横向数值控件对称验证竖滑条；窗口和控制器仍使用前面同一个实例。
    // 关闭弹层并复位，避免历史滚动量把滑条移到裁剪区之外。
    fixture.withPopup      = false;
    fixture.m_withVertical = true;
    fixture.reset(true);
    const auto vertical   = fixture.m_verticalCenter;
    const auto beforeGain = fixture.m_verticalValue;
    // 不只检查最终值：按下帧必须仍保持旧值，方向确定后才交给原生滑条。
    // 55 像素行程位于滑条内部，排除窗口外捕获或上下边界钳制带来的巧合。
    // 同时检查父页偏移，避免错误地让调值与纵向滚动一起发生。
    fixture.frame(vertical, true, true);
    check(fixture.m_verticalValue == beforeGain,
          "vertical slider defers physical press");
    fixture.frame({ vertical.x, vertical.y - 55 }, true, true);
    check(fixture.m_verticalValue > beforeGain + .2f && fixture.offset.y < .01f,
          "vertical slider adjusts on its primary axis without scrolling page");
    fixture.frame({ vertical.x, vertical.y - 55 }, false, true);
    fixture.reset(true);
    const auto beforePan = fixture.m_verticalValue;
    const auto panOrigin = fixture.m_verticalCenter;
    // 使用相同组件验证正交方向，不能通过只登记空白区域绕过组件起手问题。
    // 宽窗口的真实内容提供横向范围；断言的是本帧已应用位置而非待提交目标。
    fixture.frame(panOrigin, true, true);
    fixture.frame({ panOrigin.x - 100, panOrigin.y }, true, true);
    // 横向浏览必须当帧可见，同时不能先误改增益再恢复。
    check(fixture.offset.x > 90 && fixture.m_verticalValue == beforePan,
          "horizontal gesture on vertical slider pans bands without changing "
          "value");
    fixture.frame({ panOrigin.x - 100, panOrigin.y }, false, true);
    ImGui::DestroyContext();
    // Fixture 只持有稳定 ID 与值状态，随后析构不会触及已销毁的窗口。
    return failures == 0 ? 0 : 1;
}
