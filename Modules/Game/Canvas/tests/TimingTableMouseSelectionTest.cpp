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
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/interaction/EditorInteraction.h"
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
        if ( window )
            table = ImGui::GetCurrentContext()->Tables.GetByKey(
                window->GetID("TimingPointsTableMainV3"));
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
    int        commands     = 0;
    auto&      bus          = Event::EventBus::instance();
    const auto subscription = bus.subscribe<Event::LogicCommandEvent>(
        [&](const auto&) { ++commands; });
    {
        Canvas::TimelineCanvas canvas("MouseTable", 500, 500, nullptr);
        Access::open(canvas, snapshot);
        // 两帧完成首用尺寸和后续测试窗口尺寸切换，防止用过期行高定位。
        // 字体与布局的初始化帧不包含点击，不能被当作选择行为的一部分。
        frame(canvas);
        frame(canvas);
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
            ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
            tap(canvas, layout.row(1));
            expect(Access::count(canvas) == 2, "classic ctrl add");
            // 释放 Ctrl 后空白单击仍按原规则清空，不继承显式追加的空白保护。
            // 输入状态在场景结束前释放，后续套件析构不能看到半次键盘操作。
            ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
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
    const bool passed = run();
    ImGui::DestroyContext();
    return passed ? 0 : 1;
}
