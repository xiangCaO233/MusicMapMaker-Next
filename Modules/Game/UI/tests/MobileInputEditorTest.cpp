/**
 * @file MobileInputEditorTest.cpp
 * @brief 用真实控件和原始触点检查移动精调事务及键盘几何契约。
 * @details CPU 测试不把字段值直接改成预期结果，也不调用按钮的业务回调。
 * 长按、键盘字符、Tab 与 Enter 都进入 ImGui 正式输入队列。
 * 源字段、Changed 次数与弹窗边界共同判断结果，截图或成功打开不算提交成功。
 */
#include "ui/workspace/MobileInputEditor.h"
#include "config/skin/translation/Translation.h"
#include "graphic/mobile/MobileKeyboard.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "log/colorful-log.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include "ui/workspace/MobileTouchScroll.h"
#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
namespace
{
// 所有场景都运行完再返回失败，前一场景不能阻止边界回归。
int failures{};  ///< 每个独立断言累计，不能由后续阳性覆盖失败。
/// @brief 回归不启动窗口、音频或 Android SDK，失败由现有日志入口报告。
void check(bool ok, const char* label)
{
    // 统一日志只输出断言标签，不读取用户配置或设备内容。
    if ( !ok ) {
        XERROR("Mobile input: {}", label);
        ++failures;
    }
}
/// @brief 使用生产精调、滚动和原始触点，业务值只存在于当前夹具。
/// @details 工作区接口只提供呈现能力，真实业务状态由单个数值模拟。
/// 原始跟踪器与滚动器仍来自生产代码，因此按下不能绕过页面仲裁。
/// @warning 夹具只在测试线程访问，不启动窗口系统或并发会话更新。
struct Fixture : MMM::UI::IWorkspacePresentation {
    // 页面仲裁器与平台原始状态都是生产实例，夹具不替换识别算法。
    MMM::UI::MobileTouchScroll m_scroll;  ///< 与移动外壳相同的按下方向仲裁。
    // 数值跨帧保持，测试取消时不从磁盘或初值重置。
    float m_value{ .25f };  ///< 取消前后必须逐值保持，不能依靠重新载入恢复。
    int   m_changes{};  ///< 一次应用仅有一个 changed，不把输入过程算作提交。
    /// @brief 只在编辑完成才提交的旧业务入口，也必须收到一次精调结束边沿。
    int m_deactivated{};
    /// @brief 模拟外壳上一帧的键盘状态，收起边沿必须结束输入请求。
    bool   m_keyboardVisible{};
    float  m_clock{};       ///< 原始事件使用可推进时钟，不等待真实长按时间。
    ImVec2 m_field{};       ///< 首次实际提交后的控件中心，不硬编码目标位置。
    bool   m_show{ true };  ///< 隐藏控件模拟目标消失，不能保留旧指针提交。
    // 本场景无独立 Android 窗口，呈现能力由接口显式发布。
    /// @brief 桌面测试主动绑定触摸外壳，不改全局平台编译选择。
    /// @return false 保证公共包装器进入触屏路径，不依赖宿主 OS 宏。
    bool allowsHoverFeedback() const override { return false; }
    /// @brief 只提供参数窗口宿主，测试不创建完整业务工作区。
    void renderWorkspace(MMM::UI::MainDockSpaceUI&,
                         MMM::UI::UIManager&) override
    {
    }
    /// @brief 复用生产滚动器，原始输入不能直接绕过延迟点击协议。
    /// @details 否则测试会绕过普通纵向页面与滑块横拖的方向仲裁。
    void registerHorizontalDragItem() override
    {
        m_scroll.registerHorizontalDragItem();
    }
    /// @brief 保留非零原点，并按平台比例扣除键盘；可检查逻辑坐标换算。
    /// @return 与主视口不同的区域，防止实现误用主视口中心仍能通过。
    /// 本夹具只模拟外壳已换算好的比例，不复制生产弹窗尺寸公式。
    MMM::UI::WorkspaceWindowArea textInputWindowArea() const override
    {
        return { { 30, 20 },
                 { 740,
                   760 * (1 - MMM::Graphic::mobileKeyboardBottomFraction()) } };
    }
    /// @brief 真实 ImGui 上下文独占于本场景，不读取个人 ini 或配置路径。
    /// @details 固定帧间隔用于输入阈值，不使用系统时间影响回归稳定性。
    /// 字体与显示大小只提供几何，测试不比较不同机器的字形像素。
    Fixture()
    {
        // 每个场景独立上下文，状态机必须自行识别上下文或帧代际变化。
        // ini 关闭，避免本机窗口布局使测试通过或改变个人工作区。
        ImGui::CreateContext();
        auto& io                        = ImGui::GetIO();
        io.IniFilename                  = nullptr;
        io.DisplaySize                  = { 800, 800 };
        io.DeltaTime                    = .02f;
        io.ConfigInputTrickleEventQueue = false;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        // 生成 CPU 字体纹理即可满足 ImGui 布局要求，不创建 Vulkan 资源。
        unsigned char* pixels{};
        int            w{}, h{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        // 清除上个场景可能留下的原始触摸和 IME 快照，不能从系统取样。
        MMM::Graphic::publishMobileKeyboardOcclusion(0, 800);
        MMM::Graphic::cancelMobileTouch();
        // 方案切换采用安全帧协议，第二次同步才交付待切换的触摸方案。
        MMM::UI::Interaction::synchronizeProfile("touch-gestures", true);
        MMM::UI::Interaction::synchronizeProfile("touch-gestures", true);
        // 两帧先完成窗口创建和布局，随后触点来自实际提交的控件矩形。
        frame();
        frame();
    }
    /// @brief 销毁前清理平台值，不能让下一用例继承虚构的键盘。
    /// @details 键盘快照为进程级独立状态，必须在上下文销毁前主动清空。
    /// 保留数值事务的正常取消路径，不能用析构证明取消按钮可用。
    ~Fixture()
    {
        MMM::Graphic::publishMobileKeyboardOcclusion(0, 0);
        ImGui::DestroyContext();
    }
    /// @brief 与生产一样先采样输入、再提交控件、最后恢复原始 IO。
    /// @details 正式 WorkspaceFrameScope 将呈现能力绑定给通用控件。
    /// 每帧完整 Render，Popup 与导航生命周期不能用只调用 NewFrame 替代。
    /// @warning 该循环不读取真实用户配置、窗口系统或音频设备。
    void frame()
    {
        // 与外壳相同的顺序：NewFrame 后仲裁原始输入，Render 前恢复 IO。
        // 若先提交控件再仲裁，长按按下就可能误改滑块值。
        ImGui::NewFrame();
        const bool keyboardVisible =
            MMM::Graphic::mobileKeyboardBottomFraction() > 0;
        MMM::UI::releaseMobileTextFocusAfterKeyboardDismiss(m_keyboardVisible,
                                                            keyboardVisible);
        m_keyboardVisible = keyboardVisible;
        m_scroll.beginFrame(1);
        MMM::UI::WorkspaceFrameScope binding(*this);
        ImGui::SetNextWindowPos({ 10, 10 });
        ImGui::SetNextWindowSize({ 780, 780 });
        ImGui::Begin("Properties");
        // 目标隐藏时故意不提交包装器，检测旧事务能否错误复活。
        if ( m_show ) {
            if ( MMM::UI::FeedbackSliderFloat("Gain", &m_value, 0, 1) )
                ++m_changes;
            // 与全局 UI 倍率等正式设置相同，只观察原控件编辑结束后的提交边沿。
            if ( ImGui::IsItemDeactivatedAfterEdit() ) ++m_deactivated;
            const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            m_field = { (a.x + b.x) * .5f, (a.y + b.y) * .5f };
        }
        // 可滚动父窗防止把无滚动的简化夹具误当成完整触屏环境。
        ImGui::Dummy({ 1, 1500 });
        ImGui::End();
        m_scroll.endFrame();
        ImGui::Render();
    }
    /// @brief 每次平台事件只改变真实跟踪器，不能直接改解释器私有状态。
    /// @param phase Down 和 Up 经过同一发布入口，取消不能伪装成抬手。
    /// 单指身份固定，坐标由最近一次控件布局计算后再归一化。
    void touch(MMM::Graphic::TouchPhase phase)
    {
        // 跟踪器使用平台秒数，分类器的长按时长由 ImGui 帧时间推进。
        // 不用真实 sleep，测试只验证输入契约，不宣称测量设备手感。
        m_clock += .02f;
        MMM::Graphic::publishMobileTouch(
            phase, 1, 1, m_field.x / 800, m_field.y / 800, m_clock);
        auto& io = ImGui::GetIO();
        // IO 同时标为触屏，保证页面滚动不会按桌面鼠标分支处理。
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(m_field.x, m_field.y);
        io.AddMouseButtonEvent(0, phase != MMM::Graphic::TouchPhase::Up);
        frame();
    }
    /// @brief 按住多帧后抬手，必须由原始长按打开一次模态。
    /// @details 断言源值不变，避免只检查弹窗存在掩盖按下误改值。
    /// 抬手后再推进一帧，允许一次性的键盘聚焦请求正常交付。
    void open()
    {
        touch(MMM::Graphic::TouchPhase::Down);
        // 真实按住超过分类器阈值，随后完整抬手才允许首次文本聚焦。
        for ( int i = 0; i < 30; ++i ) frame();
        touch(MMM::Graphic::TouchPhase::Up);
        frame();
        check(ImGui::GetTopMostPopupModal() != nullptr,
              "hold opens precision modal");
        check(m_value == .25f && !m_changes,
              "hold and release do not edit source");
    }
    /// @brief 控件仍走正式 InputText 键事件，不写入内部编辑缓冲。
    /// @param value ASCII 测试草稿，经真实字符队列交给当前活动字段。
    /// 不访问 ImGuiInputTextState 缓冲，不通过内部 ID 直接指定编辑目标。
    void text(const char* value)
    {
        auto& io = ImGui::GetIO();
        // 分帧释放组合键，避免把字符与仍按住的 Ctrl 合并成命令。
        io.AddKeyEvent(ImGuiKey_LeftCtrl, true);
        io.AddKeyEvent(ImGuiKey_A, true);
        frame();
        io.AddKeyEvent(ImGuiKey_A, false);
        io.AddKeyEvent(ImGuiKey_LeftCtrl, false);
        frame();
        io.AddInputCharactersUTF8(value);
        frame();
    }
    /// @brief 通过真实焦点导航找到按钮，矩形与 ID 均由当前模态计算。
    /// @param label 与生产翻译入口相同的操作标签，不预填哈希 ID。
    /// @details 有界导航失败会明确报错，不能以盲按 Enter 当作成功点击。
    void button(const char* label)
    {
        auto* modal = ImGui::GetTopMostPopupModal();
        if ( !modal ) {
            check(false, "modal remains open");
            return;
        }
        // 根据当前窗口与显示标签计算身份，不知道按钮的屏幕硬编码位置。
        // 正文与父窗必须共享导航范围，否则用户无法走到确认／取消。
        const auto id = modal->GetID(label);
        auto&      io = ImGui::GetIO();
        for ( int i = 0; i < 24 && ImGui::GetCurrentContext()->NavId != id;
              ++i ) {
            io.AddKeyEvent(ImGuiKey_Tab, true);
            frame();
            io.AddKeyEvent(ImGuiKey_Tab, false);
            frame();
        }
        // 不能只发送 Enter 再检查结果：先证明选中的正是所需操作按钮。
        check(ImGui::GetCurrentContext()->NavId == id,
              "button reachable by normal navigation");
        io.AddKeyEvent(ImGuiKey_Enter, true);
        frame();
        io.AddKeyEvent(ImGuiKey_Enter, false);
        frame();
    }
};
/// @brief 输入、换焦点、取消均不保存；显式应用一次才写回当前有效控件。
/// @details 第二次打开沿用已取消的上下文，确认状态确实结束而非暂时隐藏。
/// 写回次数约束在一帧，即使按钮释放后的帧继续渲染也不重复增加。
void transactions()
{
    Fixture f;
    f.open();
    f.text("0.625");
    // 输入完成尚未点击操作按钮，源值与 Changed 次数必须完全不变。
    check(f.m_value == .25f && !f.m_changes, "typing remains a draft");
    // 通过 Tab 离开输入框再点取消，焦点离开不能提前写回草稿。
    f.button(TR("ui.timeline.event_editor.cancel").data());
    check(f.m_value == .25f && !f.m_changes, "cancel discards draft");
    // 继续使用同一个上下文重新打开，不能靠重建夹具掩盖取消残留。
    f.open();
    f.text("0.625");
    // 应用仍由生产按钮处理，后续多帧必须保持单次 Changed。
    f.button(TR("ui.timeline.event_editor.apply").data());
    check(f.m_value == .625f && f.m_changes == 1, "apply writes exactly once");
    // 精调结束也应唤醒依赖释放时提交的调用者，且不能在后续空闲帧重复。
    f.frame();
    check(f.m_deactivated == 1, "apply delivers one completed edit edge");
}
/// @brief 越界与不完整输入不得应用，隐藏后也不能把旧草稿交给新字段。
/// @details 越界输入不自动夹取到一，也不通过最近合法草稿静默提交。
/// 隐藏数帧覆盖目标不再调用包装器的情况，旧业务指针不得跨帧保存。
void invalidAndHidden()
{
    Fixture f;
    f.open();
    // 滑块上界为一，合法数字二是语义越界而非解析错误。
    f.text("2");
    check(f.m_value == .25f && !f.m_changes,
          "out-of-range value does not write");
    // 保持对象仍活着但不提交控件，模拟关闭页面而非释放内存。
    f.m_show = false;
    f.frame();
    f.frame();
    // 同一字段 ID 再次可见也不能接收上次打开留下的草稿。
    f.m_show = true;
    f.frame();
    check(f.m_value == .25f && !f.m_changes,
          "hidden source rejects stale draft");
    // 数值未写回还不够，隐藏目标留下的模态也不能继续阻挡其它控件。
    // 再次显示原字段后仍应保持取消态，不复活旧的打开请求。
    check(!ImGui::GetTopMostPopupModal(), "hidden source releases modal input");
}
/// @brief 完整平台快照与居中约束共同覆盖键盘出现、收起和错误尺寸。
/// @details 检查实际 BeginPopupModal 后的最终几何，不只检查期望尺寸参数。
/// 该场景独立创建上下文，IME 边界不会影响此前的编辑事务结果。
void keyboardGeometry()
{
    Fixture f;
    // 非零原点与比例遮挡同时存在，检查物理尺寸未被直接当逻辑坐标。
    MMM::Graphic::publishMobileKeyboardOcclusion(500, 800);
    f.open();
    const auto* modal = ImGui::GetTopMostPopupModal();
    // 读取同一外壳的可用区，与窗口最终底边比较，而非比较固定中心。
    const auto area = f.textInputWindowArea();
    check(
        modal && modal->Pos.y >= area.m_origin.y &&
            modal->Pos.y + modal->Size.y <= area.m_origin.y + area.m_size.y + 1,
        "modal stays above keyboard with nonzero origin");
    // 旋转过渡可能给出过大遮挡，发布端必须夹取而非产生负可用高度。
    MMM::Graphic::publishMobileKeyboardOcclusion(900, 800);
    check(MMM::Graphic::mobileKeyboardBottomFraction() == 1,
          "occlusion clamps to content height");
    // 无参考高度不能除零，也不能沿用之前已经关闭的键盘比例。
    MMM::Graphic::publishMobileKeyboardOcclusion(1, 0);
    check(MMM::Graphic::mobileKeyboardBottomFraction() == 0,
          "invalid geometry clears occlusion");
}
/// @brief IME 出现保持输入身份；主动收起结束焦点，均不能绕过显式应用。
/// @details 先输入半成品，再模拟键盘动画到最终几何及收起，字段始终是草稿。
/// 测试平台快照而非伪造窗口位置，布局与焦点处理仍运行生产路径。
void keyboardFocusAndConflict()
{
    Fixture f;
    f.open();
    f.text("0.625");
    // 活动 ID 必须来自正式聚焦；零 ID 会使后续相等断言失去意义。
    const auto active = ImGui::GetActiveID();
    check(active != 0, "precision text receives initial focus");
    // 出现时保留同一子窗身份；收起只结束输入请求，不能应用草稿。
    MMM::Graphic::publishMobileKeyboardOcclusion(400, 800);
    // 连续两帧排除只在动画首帧偶然保留焦点的情况。
    f.frame();
    f.frame();
    check(ImGui::GetActiveID() == active,
          "IME appearance preserves active field");
    MMM::Graphic::publishMobileKeyboardOcclusion(0, 800);
    f.frame();
    check(ImGui::GetActiveID() == 0,
          "IME dismissal ends focus without applying draft");
    // 系统完成键可能映射为 Enter；结束输入仍不等于点击应用。
    auto& io = ImGui::GetIO();
    // 不将 Enter 映射到应用按钮，因此输入完成不会生成 Changed。
    io.AddKeyEvent(ImGuiKey_Enter, true);
    f.frame();
    io.AddKeyEvent(ImGuiKey_Enter, false);
    f.frame();
    check(f.m_value == .25f && !f.m_changes, "Enter does not save draft");
    // 模拟正式模型从其它操作更新源值，旧草稿不得把它覆盖回去。
    // 这是外部源更新的模拟，不是把待验证的精调结果直接赋给字段。
    f.m_value = .75f;
    f.frame();
    f.frame();
    check(f.m_value == .75f && !f.m_changes,
          "external update cancels stale draft");
    // 只验证数值不变还不够，残留模态会阻断其它属性的交互。
    check(!ImGui::GetTopMostPopupModal(), "conflicting precision modal closes");
}
/// @brief 真实长表单在键盘缩小后仍保留完整操作行，并滚动到活动末字段。
/// @details 使用自动高度父模态和生产正文容器，复现 Timing 多字段的布局约束。
/// 只比较实际提交的矩形及裁剪区域，不把设置的期望高度当作布局成功。
void longFormFooter()
{
    Fixture f;
    double  value{};
    ImVec2  fieldMin{}, fieldMax{}, footerMin{}, footerMax{}, clipMin{},
        clipMax{};
    ImGuiWindow* modalWindow{};
    // 独立表单帧没有原始触点；焦点由正式键盘请求建立，不修改内部 ActiveId。
    /// @param open 只在首帧请求打开，不能以反复 OpenPopup 掩盖焦点丢失。
    /// @param focus 只交付一次末字段聚焦请求，后续尺寸变化必须自行维护可见性。
    /// @warning 测试帧不读系统窗口几何、不注入真实输入法，也不调用业务提交。
    const auto formFrame = [&](bool open, bool focus) {
        ImGui::NewFrame();
        MMM::UI::WorkspaceFrameScope binding(f);
        ImGui::Begin("LongFormHost");
        // 弹窗只在请求边沿打开，后续帧沿用同一父级与窗口 ID。
        if ( open ) ImGui::OpenPopup("LongForm");
        {
            // 样式必须在宿主 End 前恢复，与生产弹窗调用的嵌套作用域一致。
            MMM::UI::Utils::CenteredModalPopupScope modal;
            if ( modal.begin("LongForm", nullptr, 0, { 350, 0 }) ) {
                modalWindow = ImGui::GetCurrentWindow();
                MMM::UI::MobileInputBodyScope body("LongFields");
                for ( int i = 0; i < 8; ++i ) {
                    ImGui::PushID(i);
                    ImGui::TextUnformatted("Field");
                    // 末字段在键盘出现前处于可见区，缩小时不能仅靠重新点击取得焦点。
                    if ( focus && i == 7 ) ImGui::SetKeyboardFocusHere();
                    ImGui::InputDouble("##Value", &value);
                    if ( i == 7 ) {
                        fieldMin = ImGui::GetItemRectMin();
                        fieldMax = ImGui::GetItemRectMax();
                    }
                    ImGui::PopID();
                }
                // 采集真实正文裁剪区；活动字段必须在滚动完成后完整落入其中。
                clipMin = ImGui::GetCurrentWindow()->ClipRect.Min;
                clipMax = ImGui::GetCurrentWindow()->ClipRect.Max;
                body.end();
                // 与 Timing 操作行相同，按钮在父窗且具有明确触摸高度。
                MMM::UI::FeedbackButton("Confirm", { 0, 48 });
                footerMin = ImGui::GetItemRectMin();
                footerMax = ImGui::GetItemRectMax();
                ImGui::EndPopup();
            }
        }
        ImGui::End();
        ImGui::Render();
    };
    // 创建、自动尺寸与一次性聚焦分帧交付，避免首次隐藏测量帧的偶然几何。
    formFrame(true, false);
    formFrame(false, true);
    formFrame(false, false);
    const auto active = ImGui::GetActiveID();
    check(active != 0, "long form last field receives focus");
    // 该遮挡让表单总高度超过模态的实际最大值，触发正文局部滚动。
    // 非零安全区原点由 Fixture 提供，不得把屏幕原点当作表单起点。
    // 上限还需扣除父模态留白，不能只与整块键盘安全区比较。
    MMM::Graphic::publishMobileKeyboardOcclusion(500, 800);
    // 尺寸与滚动目标都经过真实 ImGui 帧推进，没有固定时长阻塞等待。
    for ( int i = 0; i < 4; ++i ) formFrame(false, false);
    // 与父窗 ClipRect 比较可抓住正文预留过高导致的按钮半截可见问题。
    // 仅比较按钮底边是否在键盘上方会漏掉父模态内部裁剪。
    // 同时检查顶边，防止通过负坐标偏移把按钮移出窗口来满足底边断言。
    check(modalWindow && footerMin.y >= modalWindow->ClipRect.Min.y &&
              footerMax.y <= modalWindow->ClipRect.Max.y,
          "long form footer fits parent modal clip");
    // 最后一个输入框必须整行可见；光标露出一点不等于可以正常编辑。
    // 该检查依赖生产正文滚动目标，与用于确认行的父窗裁剪边界独立。
    check(fieldMin.y >= clipMin.y && fieldMax.y <= clipMax.y,
          "long form active field scrolls fully into view");
    // 几何收敛不能以结束文本焦点来隐藏活动字段问题，也不能写入业务值。
    // 数值仍为初值，表明焦点滚动不会伪造字符输入或编辑完成通知。
    check(ImGui::GetActiveID() == active && value == 0,
          "long form resize retains focus without writing value");
}
}  // namespace
/// @brief 场景各自拥有 UI 上下文，不以新建对象掩盖同一场景内的取消行为。
/// @details 每个失败累计到退出码，不能依赖日志级别或测试运行顺序。
/// 不启动 Android 系统输入法；真实 IME 分派需要另做设备验收。
int main()
{
    transactions();
    invalidAndHidden();
    keyboardGeometry();
    keyboardFocusAndConflict();
    longFormFooter();
    return failures ? 1 : 0;
}
