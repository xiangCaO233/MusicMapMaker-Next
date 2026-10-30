#include "ui/workspace/MobileTouchScroll.h"
#include "log/colorful-log.h"
#include <cmath>
#include <imgui_internal.h>

/// @file MobileTouchScrollTest.cpp
/// @brief 用真实 ImGui 输入和窗口滚动量验证移动端的空白拖动策略。
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
    /// @brief 用真实滑条确认滚动不会抢走活动控件的所有权。
    float value{ .2f };
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

    /// @brief 提交一次输入和完整 ImGui 帧，位置采用屏幕逻辑像素。
    /// @param wide 为 true 时同时提供横向超宽内容。
    /// @details 禁止读取用户 ini，所有几何固定，断言不依赖屏幕与帧率。
    /// withPopup 场景把控件放入子窗口，覆盖设置页的嵌套滚动结构。
    /// 弹层仍由 BeginCombo 创建，使阻挡背景和焦点恢复使用真实 ImGui 路径。
    /// 调用顺序与移动外壳一致：布局前采样，所有控件提交后确认归属。
    void frame(ImVec2 pointer, bool down, bool wide = false)
    {
        // 输入来源显式设置为触屏；生产桌面鼠标不应该受此控制器影响。
        // 同一帧事件进入同一个上下文，避免上一场景残留的源类型干扰结果。
        auto& io = ImGui::GetIO();
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(pointer.x, pointer.y);
        io.AddMouseButtonEvent(0, down);
        ImGui::NewFrame();
        scroll.beginFrame();
        ImGui::SetNextWindowPos({ 20, 20 });
        ImGui::SetNextWindowSize({ 360, 260 });
        const auto flags = ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoTitleBar |
                           ImGuiWindowFlags_NoSavedSettings |
                           (wide ? ImGuiWindowFlags_HorizontalScrollbar : 0);
        ImGui::Begin("TouchFixture", nullptr, flags);
        // 设置页的滚动发生在子窗口，父窗口仍是弹层的焦点恢复目标。
        if ( withPopup ) ImGui::BeginChild("SettingsBody");
        // 无标题固定位置保证测试触点不会落到窗口拖动区或系统边框。
        // 控件位于顶部，其他位置保持空白，便于区分两种触摸起点。
        ImGui::SliderFloat("value", &value, 0, 1);
        if ( withPopup ) {
            // 弹层移到背景窗口旁边，入口、背景空白和弹层内部互不重叠。
            ImGui::SetNextWindowPos({ 420, 80 });
            if ( ImGui::BeginCombo(
                     "profile", "desktop", ImGuiComboFlags_HeightSmall) ) {
                // 真正的长内容产生滚动范围，不能通过手动改写 ScrollMax 伪造。
                // 保留可选项和空白两类区域，空白滚动不能用选择动作代替。
                // 原生弹层限制高度，长内容不会因为自动扩展而失去滚动范围。
                ImGui::Selectable("desktop");
                ImGui::Selectable("touch");
                const auto position = ImGui::GetCursorScreenPos();
                // 起点落在选项下方、滚动条左侧，避免被可点击项目正常占用。
                popupBlank = { position.x + 60, position.y + 12 };
                ImGui::Dummy({ 180, 700 });
                popupOffset = ImGui::GetScrollY();
                ImGui::EndCombo();
            }
            // EndCombo 恢复入口的 LastItem，中心点对应实际可见触摸区域。
            const auto min = ImGui::GetItemRectMin();
            const auto max = ImGui::GetItemRectMax();
            comboCenter    = { (min.x + max.x) / 2, (min.y + max.y) / 2 };
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
    void reset(bool wide = false)
    {
        // SetScroll 写入下一次 Begin 的目标，先推进帧再建立新的触摸起点。
        if ( auto* window = ImGui::FindWindowByName("TouchFixture") ) {
            ImGui::SetScrollX(window, 0);
            ImGui::SetScrollY(window, 0);
        }
        frame({ -100, -100 }, false, wide);
        frame({ -100, -100 }, false, wide);
        // 第二帧让横向滚动范围从新内容宽度稳定下来，不读取旧场景的 ScrollMax。
    }
};
}  // namespace

/// @brief 验证空白纵向与双轴拖动、释放保护和活动滑条互斥。
/// @details 测试只依赖 CPU 字体图集，失败通过返回值和项目日志报告。
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
    // 相同坐标再提交一个非释放帧，保护应自然结束而不是永久拦截。
    fixture.frame({ 330, 100 }, false);
    check(!fixture.scroll.ownsPointer(), "next gesture is no longer blocked");
    fixture.reset();
    // 重置后顶部控件重新进入可见区，滑条输入不能被上一次滚动遮住。
    // 滑条和空白共用窗口，必须根据控件 ActiveID 判断，而非整窗接管。
    fixture.frame({ 120, 40 }, true);
    check(!fixture.scroll.ownsPointer(), "slider retains ownership");
    fixture.frame({ 220, 40 }, true);
    // 初始值是 0.2，跨过 0.5 可确认真实滑条处理了水平移动。
    // 同时检查数值和滚动量，避免滑条不响应却被误判为成功保留归属。
    check(fixture.value > .5f && fixture.offset.y < .01f,
          "slider changes value without scrolling");
    fixture.frame({ 220, 40 }, false);
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
    fixture.tap(fixture.comboCenter);
    check(fixture.popupOpen, "combo opens on touch");
    // 背景具有真实滚动范围，复现空白滚动控制器抢占关闭点击的条件。
    // 弹层阻挡的背景没有活动控件，不能将这种状态误认为允许空白拖动。
    fixture.frame({ 330, 210 }, true);
    check(!fixture.scroll.ownsPointer(),
          "outside popup press cannot capture scrolling");
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
    // 销毁上下文之前释放任何残留输入，测试可以连续创建新的 ImGui 上下文。
    fixture.frame({ 250, 120 }, false, true);
    ImGui::DestroyContext();
    // Fixture 只持有稳定 ID 与值状态，随后析构不会触及已销毁的窗口。
    return failures == 0 ? 0 : 1;
}
