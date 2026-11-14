/// @file TimingTableMouseSelectionTest.cpp
/// @brief 使用正式表格控件验证纯鼠标集合操作、筛选范围和输入收尾。
#include "canvas/TimelineCanvas.h"
#include "common/render/RenderSnapshot.h"
#include "config/AppConfig.h"
#include "config/EditorConfig.h"
#include "config/EditorSettings.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "presentation/TimelineInteractionPresentation.h"
#include "presentation/mobile/MobileTimelineInteractionPresentation.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/workspace/MobileTouchScroll.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
#include <string_view>

namespace MMM::Canvas
{
/// @brief 夹具只设置窗口与筛选前置条件，选择动作必须来自实际 ImGui 输入。
/// @details 不调用生产选择辅助方法，不直接改选区，避免遗漏控件绑定错误。
struct TimingTableMouseSelectionTestAccess {
    /// @brief 绑定快照并打开独立表格，不需要创建 Vulkan 或 Timeline 图像。
    /// @param snapshot 实例令牌与测试拥有的内存谱面匹配。
    /// @param canvas 由本场景拥有的视图，不能借用应用中正在编辑的窗口。
    /// @pre 快照必须比视图存活更久；夹具不复制快照或保存其共享所有权。
    /// @post 仅标记窗口打开，真实身份检查仍由生产 render 路由完成。
    static void open(TimelineCanvas&                 canvas,
                     Common::Render::RenderSnapshot& snapshot)
    {
        canvas.m_currentSnapshot                            = &snapshot;
        canvas.m_auxiliaryWindowState.timingPointsTableOpen = true;
    }
    /// @brief 仅替换平台呈现，行布局、数据源和集合更新仍为正式实现。
    static void mobile(TimelineCanvas& canvas)
    {
        canvas.m_timingPresentation =
            createMobileTimelineInteractionPresentation();
    }
    /// @brief 长按只能准备原参数副本，不能提前提交修改。
    static bool editing(const TimelineCanvas& c, entt::entity entity)
    {
        return c.m_isPopupOpen && c.m_editingEntity == entity;
    }
    /// @brief 每个输入帧走完整表格，不以独立集合运算替代 UI 行为。
    /// @pre 调用者已开始 ImGui 帧，且尚未调用 Render。
    /// @details 表格内部仍查询自己的完整数据源，不复用主画布裁剪后的元素。
    static void render(TimelineCanvas& canvas)
    {
        canvas.renderTimingPointsTableWindow();
    }
    /// @brief 仅观察即时本地选择，不等待逻辑线程发布快照。
    /// @param entity 正式测试 Registry 中创建的实体，不能用显示序号代替。
    /// @return 集合存在该实体时返回真，不要求该实体当前仍在筛选结果中。
    static bool selected(const TimelineCanvas& canvas, entt::entity entity)
    {
        return canvas.m_selectedTimingEntities.contains(entity);
    }
    /// @brief 检查集合数量，识别重复追加反向取消或减选误加入。
    /// @note 数量与具体成员断言配对，不能把选错但同数量的集合视作正确。
    static std::size_t count(const TimelineCanvas& canvas)
    {
        return canvas.m_selectedTimingEntities.size();
    }
    /// @brief 设置筛选前置条件，后续全选仍由实际可见按钮执行。
    /// @details 过滤不预先清除隐藏行的选择，才能检测全选与清空各自的范围。
    static void onlyBpm(TimelineCanvas& canvas)
    {
        canvas.m_tableOnlyShowBpm = true;
    }
    /// @brief 构造合法但无匹配的数值过滤，不模拟点击全选的副作用。
    /// @post 保留既有选区，禁用全选按钮的用例必须自行证明它不会清空选区。
    static void emptyFilter(TimelineCanvas& canvas)
    {
        // 固定数组原先为零，短文本复制后继续由原有 NUL
        // 终结，生产解析器不会越界。
        // 选择有限数而非非法文本，避免把无结果与搜索失败两种状态混为一谈。
        constexpr std::string_view value = "99999";
        std::copy(value.begin(),
                  value.end(),
                  canvas.m_tableSearchValueBuffer.begin());
    }
};
}  // namespace MMM::Canvas

namespace
{
using namespace MMM;
using Access = Canvas::TimingTableMouseSelectionTestAccess;

/// @brief 固定窗口只控制测试环境，表格和所有按钮均由正式实现布局。
/// @param pointer 屏幕坐标；释放可发生在表格和父窗口之外。
/// @param canvas 已绑定合法活动谱面快照的视图，不负责准备测试数据。
/// @pre 字体图集已经构建，输入事件队列关闭跨帧节流。
/// @post 左键释放由生产控件处理，测试不事后替它清除拖选标志。
/// @param down 左键保持状态，不直接设置 clicked 或 released 标志。
/// @warning 测试不接入操作系统窗口，不写 imgui.ini 或个人工程配置。
void frame(Canvas::TimelineCanvas& canvas, ImVec2 pointer = { 1300, 850 },
           bool down = false)
{
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(pointer.x, pointer.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
    // 全局过滤可能延后普通鼠标点击，正式触屏行必须只交付一次选择。
    // 结束过滤后恢复物理输入，下一帧仍能正确识别原始边沿。
    // 表格长按只创建参数副本，本夹具不主动绘制或确认那个参数窗。
    // 因此观察到原 editing 标志仍不等于已产生谱面修改命令。
    ImGui::NewFrame();
    // 位置每帧固定，排除拖动窗口或工作区 ini 对行点击坐标的影响。
    // 第一帧允许正式窗口使用自身的首次尺寸，第二帧取得稳定的测试尺寸。
    ImGui::SetNextWindowPos({ 20, 20 });
    ImGui::SetNextWindowSize({ 1200, 800 });
    Access::render(canvas);
    ImGui::Render();
}

/// @brief 从实际表格持久几何取得行与控件位置，不绑定翻译字符串的固定像素宽度。
/// @details 四个普通点行的高度一致，使用最后渲染行反推首行。
/// 所有字段在 Render 后读取，不能用尚未完成的请求尺寸代替实际布局。
struct Layout {
    /// @brief 当前独立表格父窗口，不跨上下文保存。
    ImGuiWindow* window{};
    /// @brief 表格内核几何只用于测试定位，不进入生产选择代码。
    ImGuiTable* table{};
    /// @brief 当前筛选结果数量，用于从最后一行反推第一行。
    int rows{};
    /// @brief 按实际内部 ID 查找，翻译变更不能令测试点击另一个窗口。
    /// @param count 当前过滤后的普通点行数，不是底层 Registry 的总实体数。
    /// @note 指针只在同一 ImGui 上下文中有效；窗口重排后重新构造 Layout。
    explicit Layout(int count) : rows(count)
    {
        window = ImGui::FindWindowByName("###TimingPointsTableWindow");
        if ( window ) {
            table = ImGui::GetCurrentContext()->Tables.GetByKey(
                window->GetID("TimingPointsTableMainV3"));
            // 同一上下文先验桌面再验触屏，两种表格都可能仍留在注册表。
            // 只选择本帧确实绘制的表格，避免复用上一场景的旧行几何。
            if ( !table || table->LastFrameActive != ImGui::GetFrameCount() )
                table = ImGui::GetCurrentContext()->Tables.GetByKey(
                    window->GetID("TimingPointsTouchTableV1"));
        }
    }
    /// @brief 序号列中点，不与行内时间输入框或删除按钮重叠。
    /// @param index 当前可见结果的零起始位置，不能越过 rows。
    /// @pre 测试的四个普通点都可见，表格内部没有额外滚动或展开行。
    /// @return 返回屏幕坐标，不是表格内容区局部坐标。
    ImVec2 row(int index) const
    {
        const auto& column = table->Columns[0];
        // 读取实际行高可包含主题的 CellPadding，不假定它等于默认字体高度。
        // 只使用普通点行，使最后一行度量能代表前面每一行。
        const float height = table->RowPosY2 - table->RowPosY1;
        return { (column.MinX + column.MaxX) * 0.5f,
                 (table->RowPosY1 + table->RowPosY2) * 0.5f -
                     (rows - 1 - index) * height };
    }
    /// @brief 表格实际使用软件级间距，不能读取已经被 PopStyleVar 恢复的值。
    static float spacing()
    {
        const auto& config = Config::AppConfig::instance();
        return std::floor(config.getEditorSettings().aesthetics.itemSpacing *
                          config.getWindowContentScale());
    }
    /// @brief 全选和清空位于模式行下方，按实际按钮高度和间距计算纵坐标。
    /// @details 从表格上边沿向前扣除 Spacing、Separator 和按钮行尾间距。
    /// 只定位控件中央，不依赖圆角边缘或上一帧 Hover 动画范围。
    float controlsY() const
    {
        return table->OuterRect.Min.y - ImGui::GetFrameHeight() * 0.5f -
               4 * spacing() - 1;
    }
    /// @brief 模式圆点按照正文与控件宽度排布，支持不同语言标签。
    /// @param index 替换、追加、减选分别为零、一、二。
    /// @return 命中圆点内部的坐标，避免较长说明文字改变整行中心。
    /// @pre 只有纯鼠标方案显示这组控件，经典场景不得调用本方法。
    ImVec2 mode(int index) const
    {
        constexpr std::array keys{ "ui.selection.replace",
                                   "ui.selection.add",
                                   "ui.selection.subtract" };
        float x = window->WorkRect.Min.x + ImGui::GetFrameHeight() * 0.5f;
        for ( int i = 0; i < index; ++i )
            x += ImGui::GetFrameHeight() +
                 ImGui::GetStyle().ItemInnerSpacing.x +
                 ImGui::CalcTextSize(TR(keys[i]).data()).x + spacing();
        return { x, controlsY() - ImGui::GetFrameHeight() - spacing() };
    }
    /// @brief 两个按钮的交互中心由各自可见标签度量计算，不猜测整窗中心。
    /// @param clear 为真定位清空，为假定位当前结果全选。
    /// @details 文字采用与生产相同的 TR 查询，英文和中文宽度都可参与布局。
    /// @note 禁用按钮也有几何，可用于验证空结果和播放态的输入门禁。
    ImVec2 control(bool clear) const
    {
        float x = window->WorkRect.Min.x;
        if ( clear )
            x += ImGui::CalcTextSize(TR("ui.selection.all_results").data()).x +
                 2 * ImGui::GetStyle().FramePadding.x + spacing();
        const auto text =
            TR(clear ? "ui.selection.clear" : "ui.selection.all_results");
        return { x + ImGui::CalcTextSize(text.data()).x * 0.5f +
                     ImGui::GetStyle().FramePadding.x,
                 controlsY() };
    }
    /// @brief 表格中的空白位置，仍在表格内容区内并远离最后一行。
    ImVec2 blank() const { return { row(0).x, table->OuterRect.Max.y - 20 }; }
};

/// @brief 完整按下释放，不借助键盘模拟菜单或模式选择。
/// @note 悬浮帧先稳定 ImGui 命中；模式切换后表格仍使用原实体身份。
/// @param canvas 待处理点击的正式表格视图。
/// @param point 由当前 Layout 计算的目标位置，不直接调用其业务回调。
/// @post 第三帧释放后仍保留窗口，便于下一笔检查跨操作的选择状态。
void tap(Canvas::TimelineCanvas& canvas, ImVec2 point)
{
    frame(canvas, point);
    frame(canvas, point, true);
    frame(canvas, point);
}

/// @brief 使用内存活动会话验证真实表格的点选、拖选、筛选和经典规则。
/// @details 完整表格数据来自 Registry，不能只给快照填几行绕过表格数据源。
/// 引擎循环不启动，测试拥有的会话在所有视图析构后关闭，且禁止工作区写回。
/// @return 真实控件、集合成员与输入收尾全部符合契约时返回真。
/// @pre 测试进程独占 ImGui 上下文和自己创建的活动会话。
/// @post 所有订阅在局部捕获变量析构前移除；不保存个人操作方案配置。
bool run()
{
    // 谱面只存在于当前测试进程，关闭会话时禁止工作区持久化。
    // 地址令牌来自真实活动实例，同路径名字不能代替该身份。
    // 正式载入至少推进一次，才能满足表格对活动谱面的归属检查。
    auto       map          = std::make_shared<BeatMap>();
    auto&      engine       = Logic::EditorEngine::instance();
    const auto sessionIndex = engine.createSession(
        map, "Mouse table test", false, "MouseTableTest", true);
    const auto session = engine.getActiveSession();
    if ( !session ) return false;
    // createSession
    // 只排队载入；先消费正式载入指令，才能获得表格要求的活动谱面令牌。
    // 不启动引擎循环，单次更新只建立内存测试前置条件，不参与鼠标选择断言。
    session->update(0.0, engine.getEditorConfig(), true);
    auto& registry = session->getContextMutable().timelineRegistry;
    // 载入可能补充默认 BPM，明确清空后才能让四个实体对应四条可见行。
    // 不启动逻辑线程，直接建立前置 Registry 不会与 UI 读取发生并发竞争。
    registry.clear();
    // 四个普通点都可见，行高一致，可以从实际表格几何反推各行位置。
    // 首末两行间距离超过滑动容差，正好用来区分滚动与旧式拖选。
    // 不同数值和稳定实体一起构成目标证据，不用显示序号替代实体身份。
    std::array<entt::entity, 4> entities{};
    constexpr std::array        types{ TimingEffect::BPM,
                                       TimingEffect::SCROLL,
                                       TimingEffect::JUMP,
                                       TimingEffect::BPM };
    for ( std::size_t i = 0; i < entities.size(); ++i ) {
        // 两个 BPM 中间夹入其他类型，以便筛选全选能识别隐藏行误选。
        entities[i] = registry.create();
        registry.emplace<Logic::TimelineComponent>(
            entities[i],
            static_cast<double>(i),
            types[i],
            types[i] == TimingEffect::BPM ? 120.0 : 1.0);
    }
    Common::Render::RenderSnapshot snapshot;
    snapshot.hasBeatmap     = true;
    snapshot.beatmapPathKey = "mouse-table.mmm";
    // 表格用对象地址令牌隔离同路径重载，不能随意填固定整数假装快照已就绪。
    // 数据源仍来自活动 Registry，快照只提供身份和通用显示参数。
    snapshot.beatmapInstanceId = reinterpret_cast<std::uintptr_t>(map.get());
    snapshot.currentTime       = 1.0;
    UI::Interaction::synchronizeProfile("mouse-only", true);
    UI::Interaction::synchronizeProfile("mouse-only", true);
    // 不提前短路整个流程，失败阶段可继续报告后续独立边界是否也受到影响。
    // 每条 expect 使用当帧真实状态，不把 UI 请求成功视为选择已经生效。
    bool passed = true;
    auto expect = [&](bool value, const char* phase) {
        if ( !value ) XERROR("Timing table mouse test: {}", phase);
        passed &= value;
    };
    int   commands = 0;
    auto& bus      = Event::EventBus::instance();
    // 只记录由正式命令出口发布的事件，不依赖窗口私有的预览颜色。
    // 计数从本场景开始，鼠标用例此前的载入和收尾不混入触屏结论。
    // 订阅在引用的局部计数离开作用域之前撤销，避免析构事件悬空回调。
    const auto subscription = bus.subscribe<Event::LogicCommandEvent>(
        [&](const auto&) { ++commands; });
    {
        Canvas::TimelineCanvas canvas("MouseTable", 500, 500, nullptr);
        Access::open(canvas, snapshot);
        // 两帧完成首用尺寸和后续测试窗口尺寸切换，防止用过期行高定位。
        // 字体与布局的初始化帧不包含点击，不能被当作选择行为的一部分。
        frame(canvas);
        frame(canvas);
        // 真实表格必须已完成两帧布局，才能读取最终列宽与行高。
        // 几何缺失会明确失败，不能跳过点击后把空测试视作通过。
        // 所有坐标在本上下文中使用，不能跨窗口销毁保存内部 ImGui 指针。
        Layout layout(4);
        if ( !layout.table ) {
            XERROR("Timing table layout missing");
            passed = false;
        } else {
            // 通过可见圆点切换三种模式，不仅调用策略 setter 验证返回值。
            tap(canvas, layout.mode(0));
            tap(canvas, layout.row(0));
            expect(Access::count(canvas) == 1 &&
                       Access::selected(canvas, entities[0]),
                   "replace click");
            // 先保留首行，再重复加入末行，区分显式追加与经典 Ctrl 切换。
            // 同一目标的第二次点击不应把它从集合中移出。
            tap(canvas, layout.mode(1));
            tap(canvas, layout.row(3));
            tap(canvas, layout.row(3));
            expect(Access::count(canvas) == 2 &&
                       Access::selected(canvas, entities[0]) &&
                       Access::selected(canvas, entities[3]),
                   "idempotent add");
            // 末行第一次减去后已不在集合中，再点一次也不能反向恢复。
            // 首行是不相关基线，必须始终保留，用于识别误用替换选择。
            tap(canvas, layout.mode(2));
            tap(canvas, layout.row(3));
            tap(canvas, layout.row(3));
            expect(Access::count(canvas) == 1 &&
                       Access::selected(canvas, entities[0]),
                   "idempotent subtract");
            tap(canvas, layout.mode(1));
            // 新拖选与已有首行不相连；追加必须保留之前那一笔的结果。
            // 每个位置都是独立输入帧，避免松手时一次性补算掩盖连续反馈错误。
            frame(canvas, layout.row(1), true);
            frame(canvas, layout.row(3), true);
            frame(canvas, layout.row(3));
            expect(Access::count(canvas) == 4,
                   "add drag preserves previous group");
            tap(canvas, layout.mode(2));
            // 减选先扩大到覆盖所有行，确认临时集合已真实清空。
            // 下一步缩回才检查恢复；若扩大没有生效，恢复断言不能独立算通过。
            frame(canvas, layout.row(0), true);
            frame(canvas, layout.row(3), true);
            expect(Access::count(canvas) == 0, "subtract expanded range");
            // 外部模式变更模拟另一视图设置；当前笔必须仍按起笔的减选解释。
            UI::Interaction::activeProfile().setSelectionOperation(
                Logic::SelectionOperation::Add);
            frame(canvas, layout.row(1), true);
            expect(Access::count(canvas) == 2 &&
                       Access::selected(canvas, entities[2]) &&
                       Access::selected(canvas, entities[3]),
                   "shrink restores baseline");
            // 释放位置未提前送入保持帧，验证最后跨到的行也会参与本次范围。
            frame(canvas, layout.row(0));
            expect(Access::count(canvas) == 3 &&
                       !Access::selected(canvas, entities[0]),
                   "release position and latched mode");
            // 手势守卫也必须清除，否则操作方案切换会被已经释放的表格永久阻挡。
            // 该查询只观察本地状态，不能借切换方案强制清理来让断言通过。
            expect(!canvas.hasActivePointerGesture(), "gesture finished");
            // 空白没有命中成员，追加操作应是并入空集，不能清掉多笔累积结果。
            tap(canvas, layout.blank());
            expect(Access::count(canvas) == 3, "add blank preserves selection");
            tap(canvas, layout.mode(2));
            tap(canvas, layout.blank());
            expect(Access::count(canvas) == 3,
                   "subtract blank preserves selection");
            tap(canvas, layout.mode(0));
            tap(canvas, layout.blank());
            expect(Access::count(canvas) == 0,
                   "replace blank clears selection");
            // 两个相距较远的输入帧分别按下和释放，中间没有保持帧。
            // 纯鼠标应在松手时应用最终范围，不能要求先收到一个持续拖动事件。
            frame(canvas, layout.row(0), true);
            frame(canvas, layout.row(1));
            expect(Access::count(canvas) == 2, "fast release range");
            // 退出表格后松手也必须结束守卫，已有最后预览范围应得到保留。
            // 不在图像之外推测新的行号，否则会把无关坐标误当作表格终点。
            tap(canvas, layout.mode(1));
            frame(canvas, layout.row(2), true);
            frame(canvas, layout.row(3), true);
            frame(canvas, { 1350, 850 });
            expect(
                Access::count(canvas) == 4 && !canvas.hasActivePointerGesture(),
                "outside release cleanup");
            // 使用替换模式检查禁用按钮，避免追加模式的空白保护掩盖输入穿透。
            // 新控件即使不可操作，也不能被表格空白分支当成一次清空点击。
            tap(canvas, layout.mode(0));
            // 过滤预置只改变结果集；全选和清空动作仍通过真实按钮完成。
            Access::onlyBpm(canvas);
            frame(canvas);
            frame(canvas);
            // 重建结果后按新的实际几何定位，全选不应读取过滤之前的行号。
            // 两个 BPM 分居原列表首尾，数量和成员都正确才能证明过滤起效。
            layout = Layout(2);
            tap(canvas, layout.control(false));
            expect(Access::count(canvas) == 2 &&
                       Access::selected(canvas, entities[0]) &&
                       Access::selected(canvas, entities[3]),
                   "select all filtered results");
            // 播放状态由外层编辑门禁控制，新增可见按钮不能绕过 BeginDisabled。
            // 先保留非空集合再点击清空，只有集合未变才能证明禁用确实生效。
            snapshot.isPlaying = true;
            frame(canvas);
            tap(canvas, layout.control(true));
            expect(Access::count(canvas) == 2,
                   "playback blocks selection controls");
            snapshot.isPlaying = false;
            frame(canvas);
            Access::emptyFilter(canvas);
            frame(canvas);
            // 结果为空时全选禁用，不能把不在结果中的原选择意外清空。
            // 没有结果时仍绘制全选控件，只是禁用，不得把按钮位置误当空白清空。
            // 清空按钮必须仍可处理隐藏选择，避免筛选后无键盘就无法取消选择。
            layout = Layout(0);
            tap(canvas, layout.control(false));
            expect(Access::count(canvas) == 2, "empty all disabled");
            tap(canvas, layout.control(true));
            expect(Access::count(canvas) == 0, "clear hidden selection");
        }
    }
    UI::Interaction::synchronizeProfile("classic-desktop", true);
    UI::Interaction::synchronizeProfile("classic-desktop", true);
    {
        // 新视图隔离筛选与窗口状态；经典测试继续使用同一 Registry 数据。
        Canvas::TimelineCanvas canvas("ClassicTable", 500, 500, nullptr);
        Access::open(canvas, snapshot);
        // 两帧完成首用尺寸和后续测试窗口尺寸切换，防止用过期行高定位。
        // 字体与布局的初始化帧不包含点击，不能被当作选择行为的一部分。
        frame(canvas);
        frame(canvas);
        Layout layout(4);
        if ( !layout.table )
            passed = false;
        else {
            tap(canvas, layout.row(0));
            // 经典追加继续由真实 Ctrl 驱动；纯鼠标的最后模式不能参与解释。
            // 本场景不使用新增控件，能检出将显式模式错误提升为全局默认的回归。
            // macOS 的追加选择使用 Command，事件交换后仍由逻辑 Ctrl 查询。
            // 不关闭平台行为，确保经典选择分支也在原生 Mac 上实际执行。
            const ImGuiKey inputModifier = ImGui::GetIO().ConfigMacOSXBehaviors
                                               ? ImGuiMod_Super
                                               : ImGuiMod_Ctrl;
            ImGui::GetIO().AddKeyEvent(inputModifier, true);
            tap(canvas, layout.row(1));
            expect(Access::count(canvas) == 2, "classic ctrl add");
            // 释放 Ctrl 后空白单击仍按原规则清空，不继承显式追加的空白保护。
            // 输入状态在场景结束前释放，后续套件析构不能看到半次键盘操作。
            ImGui::GetIO().AddKeyEvent(inputModifier, false);
            // 不将经典现有的零阈值点按行为扩展为本轮修复，只验证新增模式未泄漏。
            // 无 Ctrl 的替换仍由原分支执行，不读取最后一次纯鼠标减选意图。
            tap(canvas, layout.row(2));
            expect(Access::count(canvas) == 1 &&
                       Access::selected(canvas, entities[2]),
                   "classic replace after ctrl");
            tap(canvas, layout.blank());
            expect(Access::count(canvas) == 0, "classic blank clears");
        }
    }
    // 所有选择均为 UI 本地变化，不能意外发布创建、删除或移动等谱面命令。
    expect(commands == 0, "selection produces no beatmap command");
    // 会话关闭不刷新工作区，内存测试不能持久化空谱面路径或个人窗口状态。
    // 局部视图已先析构，关闭事件不会让表格持有跨会话的 Registry 观察引用。
    bus.unsubscribe<Event::LogicCommandEvent>(subscription);
    engine.closeSession(sessionIndex, false);
    return passed;
}
/// @brief 原始触点通过全局滚动仲裁，点选、滑动和长按必须拥有不同结果。
/// @details 真实表格读取内存会话 Registry；不以私有手势标志冒充选区变化。
// 触屏与前面的鼠标用例使用同一正式窗口和表格数据收集路径。
// 本段单独创建活动会话，不复用鼠标测试里已销毁的 Registry 或实体。
// 替换的只有平台呈现对象，集合语义仍由正式表格函数执行。
// 经典与纯鼠标用例先独立通过，不能用移动结果覆盖它们的兼容性结论。
// 表格不需要主画布可见，也不使用当前时间线视野内的裁剪行。
// 所有选择动作都必须保持命令计数为零，历史只属于后续确认的编辑。
bool touchTable()
{
    auto&      engine = Logic::EditorEngine::instance();
    auto       map    = std::make_shared<BeatMap>();
    const auto index  = engine.createSession(
        map, "TouchTable", false, "TouchTimingTable", true);
    const auto session = engine.getActiveSession();
    session->update(0, engine.getEditorConfig(), true);
    auto& registry = session->getContextMutable().timelineRegistry;
    registry.clear();
    std::array<entt::entity, 4> entities{};
    // 不同数值使编辑目标错位也能在后续草稿验证中被识别。
    for ( std::size_t i = 0; i < entities.size(); ++i ) {
        entities[i] = registry.create();
        registry.emplace<Logic::TimelineComponent>(
            entities[i], static_cast<double>(i), TimingEffect::BPM, 120.0 + i);
    }
    Common::Render::RenderSnapshot snapshot;
    snapshot.hasBeatmap        = true;
    snapshot.beatmapPathKey    = "touch-table.mmm";
    snapshot.beatmapInstanceId = reinterpret_cast<std::uintptr_t>(map.get());
    // 方案同步走正式注册表；重复同步用于满足已有的安全切换边界。
    // 这里不伪造 Ctrl、Shift 或鼠标右键来触发移动选择。
    // 下一笔模式可以显式切换，但已经发生的集合结果不会被设置直接改写。
    UI::Interaction::synchronizeProfile("touch-gestures", true);
    UI::Interaction::synchronizeProfile("touch-gestures", true);
    // 触屏用例的错误单独累计，不因前面的鼠标场景失败而跳过。
    // 后续取消、长按和收尾仍会执行，可以暴露同一输入造成的次生问题。
    // 最终退出码综合桌面和触屏结论，不用一次成功覆盖另一项失败。
    bool ok     = true;
    auto expect = [&](bool value, const char* text) {
        if ( !value ) XERROR("Touch timing table: {}", text);
        ok &= value;
    };
    int        writes{};
    const auto subscription =
        Event::EventBus::instance().subscribe<Event::LogicCommandEvent>(
            [&](const auto&) { ++writes; });
    {
        Canvas::TimelineCanvas canvas("TouchTable", 500, 500, nullptr);
        Access::mobile(canvas);
        // 真实设备打开时可能先走占位窗；它会消费相同内部 ID 的 Appearing。
        // 若此路径遗漏平台几何，快照随后到达也无法补回初次尺寸。
        // 先用未就绪快照验证实际窗口高度，再恢复正式数据进行后续选择。
        snapshot.hasBeatmap = false;
        Access::open(canvas, snapshot);
        // 上一鼠标场景复用了窗口 ID；插入未绘制帧模拟真实关闭后再次打开。
        ImGui::NewFrame();
        ImGui::Render();
        frame(canvas);
        const auto* waiting =
            ImGui::FindWindowByName("###TimingPointsTableWindow");
        expect(waiting && waiting->Size.y > ImGui::GetIO().DisplaySize.y * .8f,
               "syncing window keeps touch viewport geometry");
        snapshot.hasBeatmap = true;
        // 使用真实页面仲裁器，序号列滑动必须与其它可滚动内容遵守同一规则。
        // 原始触点分类与合成鼠标回放都在本帧执行，不能只验证其中一条路径。
        // 父窗口布局每帧固定，排除窗口拖移造成行命中变化。
        UI::MobileTouchScroll scroll;
        auto&                 editorSettings =
            Config::AppConfig::instance().getEditorSettings();
        const bool timelineWasVisible     = editorSettings.showTimelineWindow;
        editorSettings.showTimelineWindow = false;
        bool   previous{}, second{};
        double tick{};
        // 原始跟踪器与 ImGui 使用同一屏幕坐标，第二指不伪造右键或 Ctrl。
        // p 使用窗口屏幕坐标，只在原始后端入口做归一化。
        // 首指状态和第二指状态各自记忆，Down、Move、Up 不能混为重复按下。
        // 系统取消独立发布，不能让正常 Up 的成功掩盖 Cancel 的处理遗漏。
        // 帧时间固定，仅用于长按计时，不引入阻塞等待或后台线程同步。
        // 输入事件先于 NewFrame，滚动过滤先于表格控件提交。
        const auto input = [&](ImVec2 p,
                               bool   down    = false,
                               bool   another = false,
                               bool   cancel  = false) {
            auto& io = ImGui::GetIO();
            using Graphic::TouchPhase;
            if ( down || previous || cancel )
                Graphic::publishMobileTouch(
                    cancel ? TouchPhase::Cancel
                    : down ? (previous ? TouchPhase::Move : TouchPhase::Down)
                           : TouchPhase::Up,
                    88,
                    1,
                    p.x / io.DisplaySize.x,
                    p.y / io.DisplaySize.y,
                    tick);
            if ( another || second )
                Graphic::publishMobileTouch(
                    another ? (second ? TouchPhase::Move : TouchPhase::Down)
                            : TouchPhase::Up,
                    88,
                    2,
                    (p.x + 60) / io.DisplaySize.x,
                    p.y / io.DisplaySize.y,
                    tick);
            previous = down && !cancel;
            second   = another;
            tick += io.DeltaTime;
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMousePosEvent(p.x, p.y);
            io.AddMouseButtonEvent(0, previous);
            ImGui::NewFrame();
            scroll.beginFrame(1);
            ImGui::SetNextWindowPos({ 20, 20 });
            ImGui::SetNextWindowSize({ 1200, 800 });
            // 走生产隐藏窗口分支，草稿标志不能代替实际参数弹窗绘制成功。
            canvas.update(nullptr);
            scroll.endFrame();
            ImGui::Render();
        };
        const auto click = [&](ImVec2 p) {
            input(p);
            input(p, true);
            input(p);
            input(p);
        };
        input({ 1300, 850 });
        input({ 1300, 850 });
        Layout layout(4);
        expect(layout.table != nullptr, "production table exists");
        if ( layout.table ) {
            const auto a = layout.row(0), b = layout.row(2), c = layout.row(3);
            // 第一次正常点按是阳性对照，确保行输入不是被全部屏蔽。
            // 选中成员与集合数量同时判断，避免选错但数量相同的假通过。
            // 追加模式重复点击末行两次应保持选中，不能退化为 Ctrl 反选。
            click(a);
            expect(Access::count(canvas) == 1 &&
                       Access::selected(canvas, entities[0]),
                   "tap replaces selection");
            // 这些设置只选择下一笔意图；实际集合必须由正式行点击更新。
            UI::Interaction::activeProfile().setSelectionOperation(
                Logic::SelectionOperation::Add);
            click(c);
            click(c);
            expect(Access::count(canvas) == 2 &&
                       Access::selected(canvas, entities[3]),
                   "touch addition is idempotent");
            UI::Interaction::activeProfile().setSelectionOperation(
                Logic::SelectionOperation::Subtract);
            click(c);
            expect(Access::count(canvas) == 1 &&
                       !Access::selected(canvas, entities[3]),
                   "touch subtraction is a difference");
            UI::Interaction::activeProfile().setSelectionOperation(
                Logic::SelectionOperation::Replace);
            // 从首行滑到末行，旧桌面路径会形成连续范围拖选。
            // 触屏必须保留此前选择，让全局页面仲裁处理浏览动作。
            // 最后两帧分别是正常 Up 与迟到鼠标配对，均不能再改变集合。
            // 这也检查向下滑动达到滚动边界时不会回退为行选择。
            input(a, true);
            input(c, true);
            input(c);
            input(c);
            expect(Access::count(canvas) == 1 &&
                       Access::selected(canvas, entities[0]),
                   "swipe never starts row drag selection");
            input(b, true);
            // 两指分阶段进入同一代，第二指抬起不会恢复第一指的编辑候选。
            // 后续独立 Cancel 场景重新开始首指，不能借前一场景 blocked
            // 状态蒙混。
            // 检查同时包含选区、编辑目标与命令数量，覆盖误选和误写两类错误。
            input(b, true, true);
            // 长按后的松手不再成为另一笔轻点，已锁存目标不能重复激活。
            // 本场景不确认参数副本，所以最终命令数量始终必须为零。
            // 关闭内存会话也不保存这个临时参数窗到个人工作区。
            input(b);
            input(b);
            input(b, true);
            // 系统取消必须经过生产原始跟踪器，不能直接复位选择或编辑标志。
            // 随后空闲帧检查是否存在延后点击，普通抬手路径不能替它收尾。
            // 取消之后再进行阳性轻点，证明下一代输入仍然可用。
            input(b, false, false, true);
            input(b);
            expect(Access::count(canvas) == 1 &&
                       !Access::editing(canvas, entities[2]) && writes == 0,
                   "second finger and Cancel produce no edit");
            // 取消用例配对正常点按，不能让吞掉所有输入的实现也通过回归。
            click(b);
            expect(Access::count(canvas) == 1 &&
                       Access::selected(canvas, entities[2]),
                   "positive tap after cancellations");
            input(b, true);
            // 固定帧步长累计越过长按阈值，过程中没有任何 sleep。
            // 进入参数窗仍不允许发布更新；确认按钮是唯一提交边界。
            // 长按结束后的 Up 继续走正式表格，用于发现迟到点击重复选择。
            // 最后由会话自然收尾，不直接擦除生产输入门闩。
            for ( int i = 0; i < 35; ++i ) input(b, true);
            expect(Access::editing(canvas, entities[2]) && writes == 0,
                   "hold opens original-ID draft without publishing");
            input(b);
            input(b);
            // 检查的是正式 ImGui 模态窗口，不是只读私有 m_isPopupOpen 标志。
            // 未绘制的旧窗口仍可能留在 ImGui
            // 注册表，因此同时要求本帧活动且可见。 输入路径经过 update
            // 的隐藏时间线分支，可捕获独立表格遗漏弹窗的回归。
            // 这里只打开草稿，没有按应用；正常抬手不能自行补发更新命令。
            // 旧鼠标用例继续直接绘制原表格，两套输入不会共享触点解释器。
            // 还原全局显示偏好后才析构视图，避免本用例配置影响下一次会话。
            const auto* editor = ImGui::FindWindowByName("TimelineEventEditor");
            expect(editor && editor->Active && !editor->Hidden,
                   "hidden timeline still renders the actual event modal");
        }
        editorSettings.showTimelineWindow = timelineWasVisible;
    }
    Event::EventBus::instance().unsubscribe<Event::LogicCommandEvent>(
        subscription);
    engine.closeSession(index, false);
    return ok;
}
}  // namespace

/// @brief 创建 CPU 字体图集并执行真实鼠标帧，测试失败返回非零退出码。
/// @details 禁用输入事件分帧与双击识别，使幂等多次点按不会进入行内编辑。
/// @note 不读取桌面剪贴板，不创建图形或音频后端，所有状态只属于测试进程。
/// @pre 测试由独立 CTest 用例进程执行，不共享桌面应用的输入上下文。
/// @post 图集与表格窗口随 ImGui 上下文销毁，不能保存观察指针到下一用例。
/// @return 全部边界通过返回零，否则记录失败阶段并返回非零。
/// @note 日志被关闭时仍以退出码报告失败，不能依赖日志文本判断测试是否通过。
int main()
{
    // 当前用例只验证控件几何与选择状态，CPU 字体图集即可支持正式布局。
    // 不上传字体纹理或初始化 Vulkan，允许在无桌面的 CI 中重复运行。
    ImGui::CreateContext();
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr;
    // 显示范围包住父窗口和外部释放点，避免窗口裁剪造成另一套坐标门禁。
    // 固定帧步长只驱动 UI 状态，不依赖实际等待时间或后台播放进度。
    io.DisplaySize = { 1400, 900 };
    io.DeltaTime   = 1.0f / 60;
    // 连续点击同一行用于验证幂等，不能被系统的双击识别转入参数编辑。
    // 取消事件跨帧节流后，每次 frame 才精确对应一次期望的输入状态。
    io.MouseDoubleClickTime         = 0;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels           = nullptr;
    int            width = 0, height = 0;
    // 像素由 ImGui 拥有，测试只借用图集构建结果，不将其注册为应用纹理。
    // 上下文销毁时统一释放，无需独立资源清理或图形设备同步。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const bool passed = run() & touchTable();
    ImGui::DestroyContext();
    return passed ? 0 : 1;
}
