/// @file MouseAnnotationMenuTest.cpp
/// @brief 用 CPU ImGui 帧验证批注菜单的身份、导航、编辑权限与释放归属。
/// @note 输入帧不包含键盘事件，阅读切换和编辑必须由可见按钮激活。
/// @details 需要维持的边界：
/// - 原标记只在打开时命中，所有后续按钮操作都远离原标记。
/// - 上一条和下一条循环阅读，不允许导航直接生成谱面修改。
/// - 编辑内容、作者、ID 与时间必须属于同一条已选批注。
/// - 播放与只读状态在菜单打开之后改变，不能继续沿用旧权限。
/// - 同路径新实例以及选中条目删除均使旧菜单失效。
/// - Markdown 链接与经典方案的普通右键都保留其原有输入归属。
/// - 菜单关闭的释放帧不能穿透成画布绘制、删除或再次打开菜单。
/// - 所有夹具拥有独立控制器，延迟弹窗请求不会跨场景继承。
/// - 长正文在固定预览区滚动，不能把编辑按钮挤出可用屏幕。
/// @par 完整批注栏路由验收
/// - 右键气泡后根弹窗必须仍是菜单，不能被旧空白新增 Modal 覆盖。
/// - 画笔持有释放事件时，菜单和旧新增入口都不能插入另一笔编辑事务。
/// - 经典气泡右键仍进入原新增弹窗，不能被纯鼠标菜单的身份取代。
/// - 验收读取真实 Popup ID，不依赖编辑请求标志或残留窗口对象猜测结果。
#include "canvas/Basic2DCanvasInteraction.h"
#include "common/render/AnnotationRenderData.h"
#include "common/render/RenderSnapshot.h"
#include "config/AppConfig.h"
#include "config/VisualConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "log/colorful-log.h"
#include "logic/session/CanvasCamera.h"
#include "ui/interaction/EditorInteraction.h"
#include <cstddef>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <string_view>
#include <utility>

namespace MMM::Canvas
{
/// @brief 只提供已命中的标记前置条件，动作由生产菜单控件处理。
/// @details 不直接调用编辑回调，也不把期望结果写入编辑器缓冲。
struct MouseAnnotationMenuTestAccess {
    /// @brief 接入与气泡、连线卡片相同的菜单入口，不创建图形设备。
    /// @param controller 当前场景拥有的正式控制器，不借用桌面应用的活动实例。
    /// @param snapshot 在整轮输入帧中保持存活的只读发布快照。
    /// @param hovered 表示当前输入命中标记；菜单内操作时必须为假。
    /// @param link 表示 Markdown 链接已取得指针，菜单不得抢走右键。
    /// @return 原入口的输入消费状态，关闭帧也必须继续返回真。
    static bool menu(Basic2DCanvasInteraction&             controller,
                     const Common::Render::RenderSnapshot& snapshot,
                     bool hovered, bool link)
    {
        return controller.renderMouseAnnotationMenu(
            snapshot,
            hovered && !snapshot.annotationMarkers.empty()
                ? &snapshot.annotationMarkers.front()
                : nullptr,
            0,
            link);
    }
    /// @brief 完整批注栏包含旧的空白新增路由，不能只测菜单 helper
    /// 而漏掉两者冲突。
    /// @param origin 当前父窗口内容原点，与正式画布使用同一种屏幕坐标。
    /// @param pointer 指针的屏幕位置，传给批注栏前转换为图像局部坐标。
    /// @return 真实批注栏是否消费输入，不直接打开任何弹窗。
    static bool gutter(Basic2DCanvasInteraction&             c,
                       const Common::Render::RenderSnapshot& s, ImVec2 origin,
                       ImVec2 pointer)
    {
        return c
            .renderAnnotationGutter(s,
                                    origin.x,
                                    origin.y,
                                    500,
                                    400,
                                    pointer.x - origin.x,
                                    pointer.y - origin.y,
                                    true)
            .blocksCanvas;
    }
    /// @brief 用正式投影计算气泡命中位置，不复制轨道布局的像素算法。
    /// @pre 父窗口至少完成过一帧，CursorStartPos 是真实布局结果。
    /// @note 轨道区、草稿区及 BGM
    /// 区共用正式投影，避免气泡命中落到错误的辅助轨。
    /// @return 当前父窗口中第一组批注气泡的中心屏幕坐标。
    static ImVec2 bubble(const Common::Render::RenderSnapshot& s)
    {
        const auto* window = ImGui::FindWindowByName("AnnotationMenuTest");
        const auto& layout = Config::AppConfig::instance()
                                 .getVisualConfig()
                                 .trackLayoutForKeyCount(s.trackCount);
        const auto  p =
            Logic::calculateCanvasLaneProjection(500,
                                                 s.trackCount,
                                                 s.bgmTrackCount,
                                                 layout,
                                                 s.canvasHorizontalOffsetX,
                                                 true,
                                                 s.bmsEditingEnabled,
                                                 s.draftLanesEnabled,
                                                 s.draftTrackCount,
                                                 true);
        return { window->DC.CursorStartPos.x +
                     (p.annotationLeftX + p.annotationRightX) * 0.5f,
                 window->DC.CursorStartPos.y +
                     s.annotationMarkers.front().canvasY };
    }
    /// @brief 核对当前根弹窗确实是本画布批注菜单，防止新增 Modal
    /// 替换菜单后假通过。
    /// @details 菜单使用相机 ID 压栈，测试按同一命名空间读取真实 ImGui Popup
    /// ID。
    static bool menuOpen(const Basic2DCanvasInteraction& c)
    {
        const auto* window = ImGui::FindWindowByName("AnnotationMenuTest");
        const auto  seed   = ImHashStr(c.m_cameraId.c_str(), 0, window->ID);
        const auto  id     = ImHashStr("MouseAnnotationMenu", 0, seed);
        const auto& stack  = ImGui::GetCurrentContext()->OpenPopupStack;
        return !stack.empty() && stack.front().PopupId == id;
    }
    /// @brief
    /// 只建立“画笔仍拥有释放”的前置条件，不在测试中提交或取消真实谱面动作。
    static void activeBrush(Basic2DCanvasInteraction& c, bool value)
    {
        c.m_leftPressStartedOnCanvas = value;
    }
    /// @brief 只观察下一阶段弹窗请求，不直接触发 OpenPopup 或写谱面。
    /// @return 本轮实际点击已准备确认弹窗时返回真，菜单打开本身不算成功。
    static bool requested(const Basic2DCanvasInteraction& c)
    {
        return c.m_annotationEditor.requestOpen;
    }
    /// @brief 当前阅读身份需跨越鼠标移入菜单而保持，不能使用实时 hover 代替。
    /// @return 借用控制器持有的稳定 ID，不能保存到控制器析构之后。
    static std::string_view selected(const Basic2DCanvasInteraction& c)
    {
        return c.m_annotationMenu.m_selectedId;
    }
    /// @brief 核对编辑缓冲的完整归属，包括作者、内容和菜单锁存时间。
    /// @details 固定文本由 fixture 提供，不能只判断弹窗是否打开。
    /// @return ID、作者、正文与时间全部属于第二条批注时返回真。
    /// @note 第二条与第一条使用不同文本，可检出菜单序号与缓冲未同步的回归。
    static bool editingSecond(const Basic2DCanvasInteraction& c)
    {
        return c.m_annotationEditor.annotationId == "two" &&
               c.m_annotationEditor.author == "Second author" &&
               std::string_view(c.m_annotationEditor.content.data()) ==
                   "Second body" &&
               c.m_annotationEditor.timestamp == 3.25;
    }
    /// @brief 新增入口必须清除旧条目 ID 与正文，仍使用同一组批注的显示时间。
    /// @return 没有继承上一条作者、内容或 ID 时才返回真。
    /// @note 后续编辑器会按默认作者处理新建，本菜单不能伪造历史作者归属。
    static bool adding(const Basic2DCanvasInteraction& c)
    {
        return c.m_annotationEditor.annotationId.empty() &&
               c.m_annotationEditor.author.empty() &&
               c.m_annotationEditor.content.front() == '\0' &&
               c.m_annotationEditor.timestamp == 3.25;
    }
};
}  // namespace MMM::Canvas

namespace
{
using namespace MMM;
using Controller = Canvas::Basic2DCanvasInteraction;
using Access     = Canvas::MouseAnnotationMenuTestAccess;

/// @brief 正式窗口驱动弹窗栈，只注入物理按键边沿，不伪造点击结果。
/// @param controller 本轮拥有弹窗状态和编辑缓冲的控制器。
/// @param snapshot 同一对象在不同输入帧可变化，用于模拟逻辑线程重新发布。
/// @param point 菜单打开后故意离开原标记位置，检验目标锁存。
/// @param left 左键状态；前后帧分开，才能验证按钮释放时的实际激活。
/// @param right 用于建立普通右键按下，不添加 Shift 或其他修饰键。
/// @param hovered 只在标记位置开启，所有菜单按钮帧都关闭该命中条件。
/// @param link 明确由上层 Markdown 命中提供，不通过临时按下修饰键伪造。
/// @param fullGutter 为真时包含整条批注栏路由，为假时隔离测试菜单本体。
/// @return 是否由批注菜单消费本帧输入，不能以弹窗窗口仍存在代替。
/// @pre 字体图集已经构建，输入事件在 NewFrame 中一次性发布。
/// @post 命中由调用者显式给出，菜单内按钮不得暗中继承原卡片命中状态。
/// @warning 夹具只借用本帧快照，不把其观察指针留到下一次 Render。
bool frame(Controller& controller, Common::Render::RenderSnapshot& snapshot,
           ImVec2 point, bool left = false, bool right = false,
           bool hovered = false, bool link = false, bool fullGutter = false)
{
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(point.x, point.y);
    io.AddMouseButtonEvent(0, left);
    io.AddMouseButtonEvent(1, right);
    ImGui::NewFrame();
    // - 固定父窗口几何，排除个人布局或 DPI 设置导致的坐标偶然命中。
    // - 不强制抢回焦点，否则可能让菜单按钮无法按正式输入所有权激活。
    ImGui::SetNextWindowPos({ 0, 0 });
    ImGui::SetNextWindowSize({ 700, 600 });
    ImGui::Begin(
        "AnnotationMenuTest", nullptr, ImGuiWindowFlags_NoSavedSettings);
    const UI::Interaction::SurfaceScope scope(
        UI::Interaction::Surface::MainCanvas);
    const bool consumed =
        fullGutter
            ? Access::gutter(
                  controller, snapshot, ImGui::GetCursorScreenPos(), point)
            : Access::menu(controller, snapshot, hovered, link);
    ImGui::End();
    ImGui::Render();
    return consumed;
}

/// @brief 使用真实右键打开菜单并推进布局，不把旧窗口对象当成打开成功。
/// @param controller 菜单必须通过其正式入口打开，不能直接调用
/// ImGui::OpenPopup。
/// @param snapshot 当前场景的谱面和批注身份，在后续按钮帧保持引用有效。
/// @param link 链接命中的反例预期菜单始终不打开。
/// @return 当前弹窗窗口，不存在时返回空指针，由用例显式报告失败。
/// @note 鼠标释放后留在另一处，菜单必须靠身份保持，而非持续命中标记。
ImGuiWindow* open(Controller&                     controller,
                  Common::Render::RenderSnapshot& snapshot, bool link = false)
{
    frame(controller, snapshot, { 150, 150 });
    frame(controller, snapshot, { 150, 150 }, false, true, true, link);
    frame(controller, snapshot, { 400, 400 });
    const auto& stack = ImGui::GetCurrentContext()->OpenPopupStack;
    return stack.empty() ? nullptr : stack.back().Window;
}

/// @brief 从实际弹窗位置定位底部编辑和新增按钮，与正文预览高度解耦。
/// @param popup 当前打开菜单的真实窗口，不能用曾经存在过的关闭窗口。
/// @param add 为真定位新增，为假定位已有批注编辑。
/// @note 只借用当前场景的弹窗几何，跨场景必须重新打开并测量。
/// @return 返回按钮内部的屏幕坐标，而非菜单内容区局部坐标。
/// @details 读取已完成布局的窗口尺寸，不假定中文标签或 DPI 对应固定宽度。
ImVec2 editPoint(const ImGuiWindow& popup, bool add)
{
    const auto& style  = ImGui::GetStyle();
    const float bottom = popup.Pos.y + popup.Size.y - style.WindowPadding.y;
    return { popup.Pos.x + style.WindowPadding.x + 10,
             bottom - ImGui::GetFrameHeight() * 0.5f -
                 (add ? 0 : ImGui::GetFrameHeightWithSpacing()) };
}

/// @brief 两个 SmallButton 采用正文高度，导航定位不依赖长批注的行数。
/// @param popup 已完成本帧布局的菜单窗口。
/// @param next 为真定位下一条，为假定位上一条。
/// @return 使用 SmallButton 高度定位文字行中央，不猜测整个弹窗中央。
/// @details 控件中的文本宽度与生产使用同一翻译来源，避免依赖固定语言。
ImVec2 navigationPoint(const ImGuiWindow& popup, bool next)
{
    const auto& style = ImGui::GetStyle();
    const float previousWidth =
        ImGui::CalcTextSize(TR("ui.annotation.previous").data()).x +
        2 * style.FramePadding.x;
    const auto text =
        TR(next ? "ui.annotation.next" : "ui.annotation.previous");
    return { popup.Pos.x + style.WindowPadding.x +
                 (next ? previousWidth + style.ItemSpacing.x : 0) +
                 ImGui::CalcTextSize(text.data()).x * 0.5f +
                 style.FramePadding.x,
             popup.DC.CursorStartPos.y + ImGui::GetTextLineHeightWithSpacing() +
                 ImGui::GetTextLineHeight() * 0.5f };
}

/// @brief 点击的三帧都离开原卡片，验证菜单不重新解释 hover 目标。
/// @return 释放帧仍消费输入时返回真，防止菜单关闭后同一事件穿透到谱面。
bool tap(Controller& c, Common::Render::RenderSnapshot& s, ImVec2 point)
{
    frame(c, s, point);
    frame(c, s, point, true);
    return frame(c, s, point);
}

/// @brief 构造两条同时间批注，菜单时间刻意不同于播放头和指针所在位置。
/// @return 值语义快照，无活动谱面、音频设备或可变 Registry 依赖。
/// @note 播放头为十秒而批注为 3.25 秒，可以识别误用实时播放位置的实现。
/// @note 两条批注共享时间戳但不共享 ID，不能用行号或作者名代替身份。
Common::Render::RenderSnapshot fixture()
{
    Common::Render::RenderSnapshot s;
    s.trackCount         = 4;
    s.hasBeatmap         = true;
    s.acceptsInteraction = true;
    s.beatmapPathKey     = "annotation-menu.mmm";
    s.beatmapInstanceId  = 7;
    s.currentTime        = 10;
    Common::Render::AnnotationRenderMarker marker;
    marker.timestamp = 3.25;
    marker.canvasY   = 200;
    marker.items     = {
        { .id = "one", .author = "First author", .content = "First body" },
        { .id = "two", .author = "Second author", .content = "Second body" }
    };
    // - 两条都放在同一标记中，导航只能在该组内循环，不能跳到别的时间。
    // - 快照拥有字符串，测试只在其生命周期内向生产菜单提供观察指针。
    s.annotationMarkers.push_back(std::move(marker));
    return s;
}

/// @brief 验证普通右键、持续导航、写权限及跨谱面失效，保留经典入口。
/// @pre 调用者已构建字体图集，不借用桌面应用的渲染或输入环境。
/// @return 所有权限、身份与输入归属断言均成立时返回真。
/// @details 菜单只能准备弹窗；保存尚未确认时不得发布谱面修改命令。
/// 所有视图按场景重建，避免前一场景的延迟编辑请求掩盖权限或身份错误。
bool run()
{
    // - 累积失败让每个输入边界都有独立诊断，后续成功不能掩盖早先的失败。
    // - 不根据日志开关决定是否通过，最终以累计退出码交给 CTest。
    bool passed = true;
    auto check  = [&](bool value, const char* phase) {
        if ( !value ) XERROR("Mouse annotation menu: {}", phase);
        passed &= value;
    };
    UI::Interaction::synchronizeProfile("mouse-only", true);
    UI::Interaction::synchronizeProfile("mouse-only", true);
    int        writes = 0;
    auto&      bus    = Event::EventBus::instance();
    const auto subscription =
        bus.subscribe<Event::LogicCommandEvent>([&](const auto&) { ++writes; });
    {
        auto       s = fixture();
        Controller c("AnnotationTest", "AnnotationCanvas");
        auto*      popup = open(c, s);
        check(popup != nullptr, "ordinary right opens");
        // - 只有真正存在当前弹窗才能计算坐标，失败不能回退到猜测的点击点。
        // - 观察指针不跨控制器场景或 ImGui 上下文析构保留。
        if ( popup ) {
            check(tap(c, s, navigationPoint(*popup, true)),
                  "navigation consumes input");
            // - 导航后不应准备编辑弹窗，阅读切换本身没有写入语义。
            // - 菜单保持打开才能继续下一次点击，而非重新右键改变锁存目标。
            check(Access::selected(c) == "two" && !Access::requested(c),
                  "next keeps menu open");
            tap(c, s, navigationPoint(*popup, true));
            check(Access::selected(c) == "one", "next wraps");
            tap(c, s, navigationPoint(*popup, false));
            check(Access::selected(c) == "two", "previous wraps");
            // 打开后播放或只读状态变化，编辑按钮必须立即禁用，但阅读导航保留。
            s.isPlaying = true;
            frame(c, s, { 400, 400 });
            tap(c, s, editPoint(*popup, false));
            check(!Access::requested(c), "playing disables edit");
            s.isPlaying          = false;
            s.acceptsInteraction = false;
            frame(c, s, { 400, 400 });
            tap(c, s, editPoint(*popup, false));
            check(!Access::requested(c), "readonly disables edit");
            // - 恢复写权限后使用同一菜单和当前阅读条目，无需重新命中原卡片。
            // - 这里不能重建控制器，否则会掩盖旧权限或旧目标被错误缓存的问题。
            s.acceptsInteraction = true;
            frame(c, s, { 400, 400 });
            check(tap(c, s, editPoint(*popup, false)),
                  "edit close consumes release");
            // - 不仅检查
            // requestOpen，还核对全部编辑字段，避免请求打开了错误对象。
            // - 编辑菜单已经退出，下一阶段只有既有确认弹窗可以提交谱面修改。
            check(Access::requested(c) && Access::editingSecond(c),
                  "edit freezes identity and time");
            check(ImGui::GetCurrentContext()->OpenPopupStack.empty(),
                  "menu closes before modal request");
        }
    }
    {
        auto       s = fixture();
        Controller c("AddTest", "AddCanvas");
        auto*      popup = open(c, s);
        check(popup != nullptr, "new scenario opens");
        // - 只有真正存在当前弹窗才能计算坐标，失败不能回退到猜测的点击点。
        // - 观察指针不跨控制器场景或 ImGui 上下文析构保留。
        if ( popup ) {
            tap(c, s, navigationPoint(*popup, true));
            // - 先读到第二条再新增，才能验证旧 ID、作者和正文被完整清理。
            // - 新建时间仍来自原批注组，不能使用按钮所在的屏幕 Y 或播放头。
            tap(c, s, editPoint(*popup, true));
            check(Access::requested(c) && Access::adding(c),
                  "add uses marker time and empty buffer");
        }
    }
    {
        auto s = fixture();
        // 长文使用单一字符重复，断言只关注布局是否有界，不依赖 Markdown 解析。
        // 父菜单仍要容纳导航和编辑按钮，不能靠窗口最大高度裁剪掉底部入口。
        s.annotationMarkers.front().items.front().content =
            std::string(4096, 'W');
        Controller c("LongTest", "LongCanvas");
        auto*      popup = open(c, s);
        check(popup && popup->Size.y < 400,
              "long body keeps menu controls visible");
        if ( popup ) {
            // 预览内容长度不改变导航命中位置；短文切换后仍读取同一稳定 ID。
            tap(c, s, navigationPoint(*popup, true));
            check(Access::selected(c) == "two", "navigate away from long body");
        }
        // 显式点到父窗口外关闭，下一场景不能继承这个仍打开的只读菜单。
        tap(c, s, { 800, 650 });
    }
    for ( bool reload : { false, true } ) {
        auto       s = fixture();
        Controller c("InvalidTest", "InvalidCanvas");
        auto*      popup = open(c, s);
        check(popup != nullptr, "identity scenario opens");
        if ( !reload && popup ) tap(c, s, navigationPoint(*popup, true));
        // 目标删除与同路径重载分别验证，不能退回到邻近索引继续编辑。
        if ( reload )
            ++s.beatmapInstanceId;
        else
            s.annotationMarkers.front().items.pop_back();
        check(frame(c, s, { 400, 400 }), "invalid menu close consumes frame");
        check(ImGui::GetCurrentContext()->OpenPopupStack.empty() &&
                  !Access::requested(c),
              "invalid identity closes without edit");
    }
    {
        auto       s = fixture();
        Controller c("LinkTest", "LinkCanvas");
        check(!open(c, s, true), "link right click retains ownership");
        // 返回经典方案后普通右键不能打开新增菜单，不改写其 Shift 编辑手势。
        UI::Interaction::synchronizeProfile("classic-desktop", true);
        UI::Interaction::synchronizeProfile("classic-desktop", true);
        check(!open(c, s), "classic right unchanged");
    }
    // - 观察事件总线实际出口，菜单若误触创建、删除、移动都不能悄悄通过。
    // - 仅选择编辑缓冲不会产生事件，保存必须留给用户随后确认的既有弹窗。
    // - 完整入口还需验证菜单与原空白右键分支互斥，不能被下层新增 Modal 替换。
    // - 同一点击若打开了编辑器，Popup ID
    // 检查会失败，即使画布仍被阻挡也不算通过。
    UI::Interaction::synchronizeProfile("mouse-only", true);
    UI::Interaction::synchronizeProfile("mouse-only", true);
    {
        auto       s = fixture();
        Controller c("GutterTest", "GutterCanvas");
        frame(c, s, { 600, 500 });
        const auto point = Access::bubble(s);
        frame(c, s, point, false, true, true, false, true);
        // - 根 Popup ID 能识别旧空白分支是否误开
        // Modal，单看输入被阻挡不足以证明互斥。
        // - 这里没有 Shift，普通右键必须直接进入纯鼠标菜单。
        check(Access::menuOpen(c), "gutter opens menu without fallback modal");
        frame(c, s, point, false, false, true, false, true);
        // 先自然释放再点到外部关闭，下一笔活动画笔用例不能继承菜单所有权。
        tap(c, s, { 800, 650 });
        // -
        // 释放归属在逻辑事务内可能比物理按钮存活更久，菜单和新增都必须尊重它。
        // - 否则右键取消旧画笔时可能突然被批注创建弹窗截走。
        Access::activeBrush(c, true);
        frame(c, s, point, false, true, true, false, true);
        check(ImGui::GetCurrentContext()->OpenPopupStack.empty(),
              "active brush prevents annotation popup");
        frame(c, s, point, false, false, true, false, true);
        // - 先结束事务前置状态再切换方案，不能让无效守卫阻止经典用例运行。
        Access::activeBrush(c, false);
        // 经典仍直接在气泡处新建批注；新增菜单的互斥规则不得改变这个入口。
        UI::Interaction::synchronizeProfile("classic-desktop", true);
        UI::Interaction::synchronizeProfile("classic-desktop", true);
        frame(c, s, point, false, true, true, false, true);
        // - 经典预期是原新增 Modal 而非新菜单，两个分支分别按真实 Popup
        // 身份判断。
        check(!Access::menuOpen(c) &&
                  !ImGui::GetCurrentContext()->OpenPopupStack.empty(),
              "classic gutter new annotation preserved");
    }
    check(writes == 0, "menu does not prematurely mutate beatmap");
    // - 先移除捕获局部计数的订阅，再结束其生命周期，避免后续析构事件悬空访问。
    bus.unsubscribe<Event::LogicCommandEvent>(subscription);
    return passed;
}
}  // namespace

/// @brief 仅初始化 CPU 控件度量，所有用例在独立进程内执行。
/// @return 失败始终返回非零退出码，不依赖日志是否开启。
/// @pre 用例在独立进程运行，不借用桌面应用的活动输入或编辑会话。
/// @post 字体、弹窗及控制器均在本上下文内析构，不保存任何 UI 观察指针。
/// @warning 不读取桌面剪贴板，不写 imgui.ini，不建立渲染或音频线程。
int main()
{
    ImGui::CreateContext();
    auto& io                        = ImGui::GetIO();
    io.IniFilename                  = nullptr;
    io.DisplaySize                  = { 900, 700 };
    io.DeltaTime                    = 1.0f / 60;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels{};
    int            width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const bool passed = run();
    ImGui::DestroyContext();
    return passed ? 0 : 1;
}
