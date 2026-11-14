/// @file MobileAnnotationTableTest.cpp
/// @brief 使用真实会话缓存、ImGui 控件和原始触点验证触屏批注列表。
#include "canvas/AnnotationTableData.h"
#include "common/LogicCommands.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/annotation/BeatmapAnnotation.h"
#include "mmm/beatmap/BeatMap.h"
#include "presentation/AnnotationTablePresentation.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include "ui/workspace/MobileTouchScroll.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace
{
using namespace MMM;
/// @brief 所有阶段继续运行并分别记录失败，避免早退掩盖收尾问题。
bool passed = true;
/// @brief 失败由项目日志报告，不依赖断言在 Release 构建中是否启用。
// 每条检查都同时依赖实际结果与具体目标，不用窗口存在代替业务成功。
// 保留后续检查可暴露一次输入错误导致的迟到提交或历史污染。
void check(bool value, const char* message)
{
    if ( !value ) {
        XERROR("Mobile annotation table: {}", message);
        passed = false;
    }
}
/// @brief 只从本次 ImGui 上下文查找实际列表子窗，不使用预设设备像素坐标。
// 这里只查本测试创建的窗口，不访问正在运行的桌面或手机应用。
// 窗口指针用于本帧几何定位，不能跨 ImGui 上下文销毁继续保存。
ImGuiWindow* listWindow()
{
    for ( auto* window : ImGui::GetCurrentContext()->Windows )
        if ( std::strstr(window->Name, "TouchAnnotationRows") ) return window;
    return nullptr;
}
/// @brief 测试外壳保留右侧区域，验证辅助窗口不会使用整个屏幕覆盖工具栏。
struct TestWorkspace final : UI::IWorkspacePresentation {
    /// @brief 不绘制其它业务内容，正式批注列表仍在夹具窗口内提交。
    void renderWorkspace(UI::MainDockSpaceUI&, UI::UIManager&) override {}
    /// @brief 安全矩形由外壳发布，测试不修改生产窗口的约束结果。
    UI::WorkspaceWindowArea floatingWindowArea() const override
    {
        return { { 30, 20 },
                 { ImGui::GetIO().DisplaySize.x - 190,
                   ImGui::GetIO().DisplaySize.y - 40 } };
    }
};
/// @brief 内存会话不启动引擎线程；UI 与动作执行分阶段推进，能检查确认前无写入。
/// @details 命令由正式 EventBus 捕获，再送回同一正式会话队列。
/// CPU 图集不上传 GPU，不写个人配置、工程文件或 imgui.ini。
struct Fixture {
    /// @brief 独立内存谱面，不读取正在编辑的用户项目。
    std::shared_ptr<BeatMap> map = std::make_shared<BeatMap>();
    /// @brief 生命周期覆盖缓存和 UI 呈现，不启动后台引擎循环。
    std::shared_ptr<Logic::BeatmapSession> session;
    /// @brief 正式移动实现，测试不替换其按钮回调或手势状态机。
    std::unique_ptr<Canvas::AnnotationTablePresentation> view =
        Canvas::createMobileAnnotationTablePresentation();
    /// @brief 与生产窗口相同的版本化数据层。
    Canvas::AnnotationTableData data;
    /// @brief 与正式外壳一样通过帧作用域发布保留区域。
    TestWorkspace workspace;
    /// @brief 实际全局仲裁，不跳过页面滑动路径。
    UI::MobileTouchScroll scroll;
    /// @brief 列表阅读选择，独立于谱面物件选区。
    std::optional<std::size_t> selected;
    /// @brief 捕获真实按钮发布，确认前不送入正式动作栈。
    std::vector<Logic::LogicCommand> commands;
    /// @brief 局部对象销毁前解除订阅。
    Event::SubscriptionID subscription{};
    /// @brief 固定时钟推进，不调用阻塞 sleep 等待长按。
    double tick{};
    /// @brief 首指和次指分别发布原始阶段。
    bool down{}, second{};
    /// @brief 收尾只关闭本夹具会话。
    int sessionIndex{};
    /// @brief 多行、同时间戳及空作者同时覆盖排序、身份和编辑保留语义。
    // 先建立独立 ImGui 上下文，再创建内存谱面会话和版本缓存。
    // 字段默认值保留业务正常初始化路径，不把 UI 行容器人工填成预期结果。
    // 四十条记录超过可见行数，才能实际验证 clipper 与页面滚动仲裁。
    // 相同时间戳的不同批注仍必须是独立行，不能被分组标记误合并。
    // 编辑空作者旧批注时不要求填入新作者，更新只能改变正文。
    Fixture()
    {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        // 禁止个人工作区影响窗口位置，固定可见范围仅属于 CPU 夹具。
        // 关闭事件分帧后，每次 frame 精确对应一次平台输入样本。
        // 默认 CPU 字体图集只提供文本尺寸，不初始化 Vulkan 或上传纹理。
        io.IniFilename                  = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        io.DisplaySize                  = { 1100, 900 };
        io.DeltaTime                    = .02f;
        unsigned char* pixels{};
        int            w{}, h{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        UI::Interaction::synchronizeProfile("touch-gestures", true);
        UI::Interaction::synchronizeProfile("touch-gestures", true);
        // 不在资源目录生成测试谱面；默认作者为空的旧批注仍允许修改正文。
        for ( int i = 0; i < 40; ++i )
            map->m_annotations.push_back(BeatmapAnnotation{
                .m_id        = "annotation-" + std::to_string(i),
                .m_timestamp = (i / 2) * 1000.0,
                .m_author    = i ? "Author" : "",
                .m_content   = "Original " + std::to_string(i) });
        auto& engine = Logic::EditorEngine::instance();
        // createSession 先排队载入，正式 update 才产生 Registry 和批注缓存。
        // 连续两次更新使载入与派生缓存都走正式流程，不直接清除脏标记。
        // 会话没有后台循环，测试可以在确认前精确观察谱面尚未改变。
        sessionIndex = engine.createSession(
            map, "Touch table", false, "TouchTableTest", true);
        session = engine.getActiveSession();
        session->update(0, engine.getEditorConfig(), true);
        session->update(0, engine.getEditorConfig(), true);
        // 捕获的是正式发布载荷，而不是编辑器私有 request 或按钮返回值。
        // 不启动引擎转发循环，后续 apply 才将本批命令交给会话执行。
        // 这样既能验证 UI 路由，又能区分准备草稿和真正的历史修改。
        subscription =
            Event::EventBus::instance().subscribe<Event::LogicCommandEvent>(
                [&](const auto& e) { commands.push_back(e.command); });
        frame({ -100, -100 });
        frame({ -100, -100 });
    }
    /// @brief 不保存工作区；订阅先解除，避免析构期间回调引用已释放容器。
    // 只关闭本测试创建的会话，并明确禁止保存工作区或项目文件。
    // 平台对象先销毁，不能让它在会话消失后继续持有缓存引用。
    // 上下文最后销毁，所有正文窗口、输入状态和 CPU 图集一起回收。
    ~Fixture()
    {
        Event::EventBus::instance().unsubscribe<Event::LogicCommandEvent>(
            subscription);
        view.reset();
        Logic::EditorEngine::instance().closeSession(sessionIndex, false);
        ImGui::DestroyContext();
    }
    /// @brief 事件顺序与 SDL 后端一致：发布原始触点，再 NewFrame，再滚动过滤。
    /// @param cancel 独立系统取消，不拿普通 Up 替代。
    /// @warning CPU 回归每帧固定推进；没有文件写入、音频输出或后台线程等待。
    // point 始终是 ImGui 屏幕坐标，发布平台事件时才除以各轴显示尺寸。
    // pressed 表示首指仍活跃，another 使用独立 finger ID 表示第二指。
    // Cancel 与普通抬手使用不同枚举，禁止用 Up 假装系统抢占成功。
    // 连续静止 Move 仍推进 UI 时钟，长按不依赖真实机器运行速度。
    // 页面过滤和正式列表绘制必须处于同一帧，以发现合成点击重复执行。
    void frame(ImVec2 point, bool pressed = false, bool another = false,
               bool cancel = false)
    {
        auto& io = ImGui::GetIO();
        using Graphic::TouchPhase;
        // 只有真实状态变化或仍按住时发布事件，空闲帧不伪造新的触点代际。
        // 首指和次指抬起分别发布，残留单指仍属于原来的双指输入。
        // 原始峰值位移由生产跟踪器维护，测试不直接设置 blocked 等结果字段。
        if ( pressed || down || cancel )
            Graphic::publishMobileTouch(
                cancel    ? TouchPhase::Cancel
                : pressed ? (down ? TouchPhase::Move : TouchPhase::Down)
                          : TouchPhase::Up,
                99,
                1,
                point.x / io.DisplaySize.x,
                point.y / io.DisplaySize.y,
                tick);
        if ( another || second )
            Graphic::publishMobileTouch(
                another ? (second ? TouchPhase::Move : TouchPhase::Down)
                        : TouchPhase::Up,
                99,
                2,
                (point.x + 50) / io.DisplaySize.x,
                point.y / io.DisplaySize.y,
                tick);
        down   = pressed && !cancel;
        second = another;
        tick += .02;
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(point.x, point.y);
        io.AddMouseButtonEvent(0, down);
        // NewFrame 之前送入完整输入，之后先让全局页面仲裁器分类。
        // 父窗口、列表和正文由生产布局创建，不在测试里补额外触摸区域。
        // 结束时恢复过滤前输入，让下一次 NewFrame 正常计算原生边沿。
        ImGui::NewFrame();
        scroll.beginFrame(1);
        UI::WorkspaceFrameScope scope(workspace);
        ImGui::SetNextWindowPos({ 20, 20 }, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({ 900, 700 }, ImGuiCond_FirstUseEver);
        // 正式平台约束必须参与布局，不能只在足够大的固定测试窗中通过。
        view->prepareWindow();
        ImGui::Begin(
            "TouchTable",
            nullptr,
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        // 数据来自当前活动谱面的真实版本化缓存。
        // 即使没有主画布和时间线窗口，批注列表也必须获得完整行集合。
        // Ready 是数据前置条件，不能把空列表或旧数据当作滑动测试通过。
        check(data.refresh().status == Canvas::AnnotationTableDataStatus::Ready,
              "real session cache ready");
        view->render(data, selected);
        ImGui::End();
        scroll.endFrame();
        ImGui::Render();
    }
    /// @brief 正式 Down／Up 再补空闲帧，等待的是 UI 点击配对而非逻辑延时。
    // 轻点包含完整按下与释放，另一个空闲帧交付页面仲裁器的鼠标配对。
    // 测试不把绘制中的触点直接转换成调用生产按钮的业务回调。
    // 同一目标连续轻点仍是两代真实输入，可检查一次性门闩不会永久吞点。
    void tap(ImVec2 p)
    {
        frame(p);
        frame(p, true);
        frame(p);
        frame(p);
    }
    /// @brief 行坐标来自实际子窗起点和已提交的行高，不把触摸热区写死。
    // 这里读取列表实际 CursorStartPos，其中已经包含当前滚动位置。
    // 行高包含两行正文、主题内边距与行间距，不能沿用默认字体常数。
    // 坐标位于摘要区域，避开滚动条、边框和独立操作按钮。
    ImVec2 row(int index)
    {
        const auto* w      = listWindow();
        const float height = ImGui::GetTextLineHeight() * 2 +
                             ImGui::GetStyle().FramePadding.y * 2 +
                             ImGui::GetStyle().ItemSpacing.y;
        return { w->Pos.x + 80,
                 w->DC.CursorStartPos.y + height * (index + .5f) };
    }
    /// @brief 非滚动区的动作行位于列表之后，文字宽度使用当前翻译与主题。
    // 操作行在列表子窗之外，纵向位置取已经提交的子窗底边。
    // 横向累计当前翻译文字宽度，英文和中文长度变化不会点击相邻动作。
    // 只以这些坐标注入正常输入，不修改窗口 ActiveID 或按钮内部状态。
    // 动作序号对应可见入口，正式发布的命令类型另有独立断言。
    ImVec2 action(int index)
    {
        const auto* w = listWindow();
        const char* keys[]{ "ui.annotation.table.jump",
                            "ui.annotation.edit",
                            "ui.annotation.delete",
                            "ui.edit.undo",
                            "ui.edit.redo" };
        float       x = w->Pos.x;
        for ( int i = 0; i < index; ++i )
            x += ImGui::CalcTextSize(TR(keys[i]).data()).x +
                 ImGui::GetStyle().FramePadding.x * 2 +
                 ImGui::GetStyle().ItemSpacing.x;
        // 按钮横向位置按当前文字测量，不假定译文恰好有固定字数。
        // 与纵向实际列表边界组合，保证点击的是可见控件而非隐藏命中区。
        // 没有使用 ImGui 内部 ButtonBehavior 或直接设置 ActiveID。
        // 因此禁用按钮和滚动中的释放保护都会真实参与用例。
        return { x + (ImGui::CalcTextSize(TR(keys[index]).data()).x +
                      ImGui::GetStyle().FramePadding.x * 2) *
                         .5f,
                 w->Pos.y + w->Size.y + ImGui::GetStyle().ItemSpacing.y +
                     ImGui::GetFrameHeight() * .5f };
    }
    /// @brief 只按正式队列应用已捕获命令，确认前的 frames 不会改变 map。
    // UI 发布后先检查命令数量和载荷，再显式推进会话更新。
    // 命令按原顺序进入正式动作栈，删除、Undo、Redo 都不能用手工改 map 替代。
    // 清空捕获容器只消除下一场景的计数基线，不移除真实历史动作。
    // 第二次 update 刷新派生批注缓存，下一帧读到的是逻辑结果。
    void apply()
    {
        for ( auto& command : commands )
            session->pushCommand(std::move(command));
        commands.clear();
        session->update(
            0, Logic::EditorEngine::instance().getEditorConfig(), true);
        session->update(
            0, Logic::EditorEngine::instance().getEditorConfig(), true);
    }
};
/// @brief 阅读与导航不产生修改，滑动、第二指和 Cancel 都不能误开编辑。
// 先检查单击阳性结果，再检查滑动与中断不会制造同样的结果。
// 不同取消来源分别开始新代际，不沿用前一次已被封锁的输入。
// 最后显式跳转是可发现性对照，双击从来不是触屏必需入口。
void navigation()
{
    Fixture f;
    check(f.data.rows().size() == 40, "complete independent annotation cache");
    // 约束结果来自正式呈现，不能由夹具在绘制后再调整来通过检查。
    // 右侧保留区模拟常驻工具栏，标题栏关闭入口必须留在它左边。
    const auto* root = ImGui::FindWindowByName("TouchTable");
    check(root->Pos.x >= 30 &&
              root->Pos.x + root->Size.x <= ImGui::GetIO().DisplaySize.x - 160,
          "floating window remains inside published safe area");
    const auto p = f.row(1);
    // 普通轻点只改变阅读选择，不移动播放头，也不生成谱面历史。
    // 因此命令计数必须为零，不能只凭正文显示了另一条来推断正确。
    // 选中索引与实际缓存排序对应，避免命中了相同时间的错误邻居。
    f.tap(p);
    check(f.selected == 1 && f.commands.empty(),
          "tap selects without editing or seeking");
    f.frame(p, true);
    // 位移超过容差后页面应立即滚动，不等手指抬起。
    // 松手与随后空闲帧继续检查，防止延后鼠标点击在滚动结束时选中别行。
    // Scroll.y 的实际变化是阳性证据，只有无命令不足以证明滚动可用。
    f.frame({ p.x, p.y - 80 }, true);
    f.frame({ p.x, p.y - 80 });
    f.frame(p);
    check(f.selected == 1 && f.commands.empty() && listWindow()->Scroll.y > 0,
          "swipe scrolls without row selection or command");
    // 归零只作为下一场景前置条件，滚动阳性对照已经由实际手势证明。
    ImGui::SetScrollY(listWindow(), 0);
    f.frame(p);
    f.frame(p);
    // 两种情形分别是第二指接管和系统 Cancel。
    // 每种情形都持续越过长按阈值，旧候选不能在等待后突然打开编辑。
    // 最终抬手再推进空闲帧，确认没有迟到的菜单请求或保存载荷。
    for ( int kind = 0; kind < 2; ++kind ) {
        const auto q = f.row(0);
        f.frame(q, true);
        f.frame(q, kind == 0, kind == 0, kind == 1);
        for ( int i = 0; i < 30; ++i ) f.frame(q, kind == 0, kind == 0);
        f.frame(q);
        f.frame(q);
        check(ImGui::GetCurrentContext()->OpenPopupStack.empty() &&
                  f.commands.empty(),
              "multi-touch or system cancel cannot open editor");
    }
    f.tap(f.action(0));
    check(f.commands.size() == 1 &&
              std::holds_alternative<Logic::CmdSeek>(f.commands.front()),
          "explicit jump is discoverable without double tap");
}
/// @brief 编辑先产生本地副本；取消不写谱面，删除与历史走真实命令载荷。
// 先验证取消，然后输入正文并显式保存，最后验证独立删除及历史。
// 各阶段都使用真实控件，不直接设置草稿缓冲区来制造预期载荷。
// 作者为空的旧条目是边界样本，保存不能把它变成新批注。
// 时间锚点为零仍是合法位置，不能因假值判断误走新增路径。
void editing()
{
    Fixture f;
    f.tap(f.row(0));
    // 正文变化之外的字段必须保留，目标 ID、作者及时间都是独立契约。
    // 本样本故意使用空作者，防止编辑被误识别成新建并套用默认 Creator。
    // 保存与删除各自形成历史，前一次文本撤销后再执行删除。
    // 测试不清空动作栈，后续 Undo 必须恢复最近的真实操作。
    // 记录原文用于取消与历史断言，不能从已变更 UI 缓冲反推预期值。
    // 真实谱面仍来自 Fixture 创建的内存模型，不保存到资源目录。
    const auto original = f.map->m_annotations.front().m_content;
    f.tap(f.action(1));
    f.frame({ -100, -100 });
    auto* popup = ImGui::FindWindowByName("###TouchAnnotationEditor");
    check(popup && !ImGui::GetCurrentContext()->OpenPopupStack.empty(),
          "explicit edit opens original-ID draft");
    if ( !popup ) return;
    // 取消位置取正式表单最后一行，不使用主窗口的动作行坐标。
    const float saveWidth =
        ImGui::CalcTextSize(TR("ui.annotation.save").data()).x +
        ImGui::GetStyle().FramePadding.x * 2;
    const float cancelWidth =
        ImGui::CalcTextSize(TR("ui.annotation.cancel").data()).x +
        ImGui::GetStyle().FramePadding.x * 2;
    const ImVec2 cancel{
        popup->DC.CursorStartPos.x + saveWidth +
            ImGui::GetStyle().ItemSpacing.x + cancelWidth * .5f,
        popup->DC.CursorPosPrevLine.y + ImGui::GetFrameHeight() * .5f
    };
    // 取消按钮的位置来自实际模态最后一行，不重用列表操作栏的坐标。
    // 命令为空与正文原值同时成立，才能排除 UI 已关但后台已保存的情况。
    // 取消也不能借用 Undo，否则会破坏用户此前真实的修改历史。
    f.tap(cancel);
    check(f.commands.empty() &&
              f.map->m_annotations.front().m_content == original,
          "cancel does not write or undo earlier history");
    f.tap(f.action(1));
    f.frame({ -100, -100 });
    popup = ImGui::FindWindowByName("###TouchAnnotationEditor");
    const ImVec2 input{ popup->DC.CursorStartPos.x + 30,
                        popup->DC.CursorStartPos.y +
                            ImGui::GetTextLineHeightWithSpacing() + 20 };
    // 输入区先通过正常轻点获得焦点，避免键盘事件落在背景或正文阅读区。
    // 字符事件进入正式 InputText 状态，控件负责字符串长度和终结符。
    // 检查编辑载荷时核对完整字符串，不只检查长度或首字符。
    // 换行是否提交由生产控件配置决定，本测试通过显式保存结束。
    // 确认前 map 必须保持原文，按钮声效或光标变化都不能替代这个检查。
    f.tap(input);
    auto& io = ImGui::GetIO();
    // CPU 环境没有 Android 输入法，使用正常键盘事件替换已聚焦文本。
    // Ctrl+A 是测试输入手段，不是为触屏操作增加修饰键依赖。
    // 文本输入后先检查草稿未发布，再通过可见保存按钮完成确认。
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    f.frame(input);
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    f.frame(input);
    io.AddInputCharactersUTF8("Edited content");
    f.frame(input);
    check(f.commands.empty() &&
              f.map->m_annotations.front().m_content == original,
          "text draft does not publish before confirmation");
    const ImVec2 save{ popup->DC.CursorStartPos.x + saveWidth * .5f,
                       popup->DC.CursorPosPrevLine.y +
                           ImGui::GetFrameHeight() * .5f };
    // 点击坐标每次来自当前模态最后一行，避免警告文本改变布局后误点。
    // 正常按下与释放都经过全局页面仲裁，不直接调用保存 lambda。
    // 收到命令之后仍推进正式会话，检查作者和位置保留的业务语义。
    // 随后一次 Undo 只能回退该次正文修改，不允许把更早历史一起清空。
    // 保存结果和撤销结果分别断言，不能只证明最终回到了初始内容。
    f.tap(save);
    // 确认必须恰好对应一条 Upsert，稳定 ID 和完整正文都逐项核对。
    // 动作栈执行后的持久字段另行检查，不能以正确载荷冒充模型已改变。
    // 原作者与时间锚点未出现在编辑表单中，仍必须保持原值。
    const auto* update = f.commands.size() == 1
                             ? std::get_if<Logic::CmdUpsertBeatmapAnnotation>(
                                   &f.commands.front())
                             : nullptr;
    check(update && update->annotationId == "annotation-0" &&
              update->content == "Edited content",
          "save emits one original-ID content update");
    f.apply();
    check(f.map->m_annotations.front().m_content == "Edited content" &&
              f.map->m_annotations.front().m_author.empty() &&
              f.map->m_annotations.front().m_timestamp == 0,
          "edit preserves original author and anchor");
    f.frame({ -100, -100 });
    f.tap(f.action(3));
    f.apply();
    check(f.map->m_annotations.front().m_content == original,
          "one undo restores text edit");
    f.frame({ -100, -100 });
    // 明确删除只操作当前阅读条目的稳定 ID，不能扩大到同时间戳分组。
    // 该数据集中相邻两条时间相同，因此错误的按时间删除也会被数量检查发现。
    // 删除事件由动作层执行，列表只是等下次缓存刷新后反映结果。
    // 先验证载荷，再验证条目数量，最后验证 Undo 与 Redo。
    // 这条链路能够区分按钮未触发、目标错误和历史边界错误。
    f.tap(f.action(2));
    check(f.commands.size() == 1 &&
              std::get_if<Logic::CmdRemoveBeatmapAnnotation>(
                  &f.commands.front()) &&
              std::get<Logic::CmdRemoveBeatmapAnnotation>(f.commands.front())
                      .annotationId == "annotation-0",
          "delete targets stable ID");
    f.apply();
    check(f.map->m_annotations.size() == 39, "delete applied by real session");
    f.frame({ -100, -100 });
    f.tap(f.action(3));
    f.apply();
    // 批注动作的 Undo 可将旧项放回不同的容器位置，身份不等于数组下标。
    // 恢复断言按稳定 ID 查找原条目，内容和作者都必须一致。
    // 缓存负责按真实时间展示，测试不额外要求底层向量物理排序不变。
    const auto restored =
        std::find_if(f.map->m_annotations.begin(),
                     f.map->m_annotations.end(),
                     [](const auto& a) { return a.m_id == "annotation-0"; });
    check(f.map->m_annotations.size() == 40 &&
              restored != f.map->m_annotations.end() &&
              restored->m_content == original && restored->m_author.empty(),
          "one undo restores original author and content");
    // 删除撤销后再重做，必须恢复同一次删除的结果。
    // 不在两次历史操作之间重新创建条目，也不使用直接修改向量作为补偿。
    // 列表选择可能按新缓存恢复，但重做对应的稳定 ID 由历史栈持有。
    // 因此后续阅读选中了别条也不能使 Redo 删除另一条批注。
    f.frame({ -100, -100 });
    f.tap(f.action(4));
    f.apply();
    check(f.map->m_annotations.size() == 39, "one redo repeats the deletion");
}
/// @brief 外部状态变化必须使旧草稿不可提交，阅读仍可持续。
// 本组主动改变外部状态，验证已经打开的副本也会失效。
// 播放门禁、协作正文改写与方案切换是三种独立边界。
// 所有变化都不得产生 UI 保存命令，更不能覆盖新发布的正文。
// 这里不通过直接清理 ImGui 栈来掩盖生产模态生命周期缺陷。
void invalidation()
{
    Fixture f;
    f.tap(f.row(0));
    f.tap(f.action(1));
    f.frame({ -100, -100 });
    // 警告文字可能增加模态高度，因此每次点击都重新取最后一行几何。
    // 坐标始终来自当前正式布局，不能因旧像素位置未点中而假通过禁用测试。
    // 测试还检查真实谱面保留外部正文，拒绝保存不只看按钮颜色。
    const auto save = [&]() {
        const auto* w = ImGui::FindWindowByName("###TouchAnnotationEditor");
        return ImVec2{
            w->DC.CursorStartPos.x +
                (ImGui::CalcTextSize(TR("ui.annotation.save").data()).x +
                 ImGui::GetStyle().FramePadding.x * 2) *
                    .5f,
            w->DC.CursorPosPrevLine.y + ImGui::GetFrameHeight() * .5f
        };
    };
    // 播放是外部前置变化，禁止确认，不把已在编辑中的草稿自动写回。
    // 内存夹具没有播放线程，该赋值只建立外部播放状态前置条件。
    // 数据层仍通过正式 refresh 读取许可，测试不直接替控件禁用按钮。
    // 播放结束只恢复本测试会话，不修改任何系统音量或实际应用状态。
    f.session->getContextMutable().isPlaying = true;
    f.frame({ -100, -100 });
    f.tap(save());
    check(f.commands.empty(), "playback disables save in an existing editor");
    f.session->getContextMutable().isPlaying = false;
    // 正式命令模拟另一编辑者更新，数据缓存必须发布新版本而非私改 UI 行。
    f.session->pushCommand(Logic::CmdUpsertBeatmapAnnotation{
        .annotationId = "annotation-0", .content = "External edit" });
    f.session->update(
        0, Logic::EditorEngine::instance().getEditorConfig(), true);
    f.session->update(
        0, Logic::EditorEngine::instance().getEditorConfig(), true);
    f.frame({ -100, -100 });
    f.tap(save());
    check(f.commands.empty() &&
              f.map->m_annotations.front().m_content == "External edit",
          "changed source cannot be overwritten by an old draft");
    // 方案自动切换也必须关闭旧模态；不通过手工清栈掩盖生产实现遗漏。
    UI::Interaction::synchronizeProfile("classic-desktop", true);
    UI::Interaction::synchronizeProfile("classic-desktop", true);
    f.frame({ -100, -100 });
    f.frame({ -100, -100 });
    check(ImGui::GetCurrentContext()->OpenPopupStack.empty() &&
              f.commands.empty(),
          "profile change closes the old editor without a write");
}
}  // namespace
/// @brief 三组独立上下文避免上一场景的菜单、滚动和触点掩盖下一场景故障。
int main()
{
    Translation::getActiveTranslator().loadLanguage("en_us",
                                                    MMM_T08_TRANSLATION);
    Translation::getActiveTranslator().switchLang("en_us");
    // 三组结果分别检查读取、历史与外部失效，不能相互抵消失败。
    navigation();
    editing();
    invalidation();
    return passed ? 0 : 1;
}
