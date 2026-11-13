#include "graphic/mobile/MobileTouchInput.h"
#include "log/colorful-log.h"
#include "presentation/mobile/MobileTimingCurveEditor.h"
#include "ui/workspace/MobileTouchScroll.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>

namespace
{
int failures{};  ///< 独立报告各场景，不能因前一项失败跳过取消阳性对照。
/// @brief 用真实图表布局与原始触点验证输入，不直接修改解释器内部状态。
void check(bool ok, const char* message)
{
    if ( !ok ) {
        XERROR("{}", message);
        ++failures;
    }
}
/// @brief 生产触屏图表与页面滚动共用完整帧，覆盖父页夺笔问题。
/// @details 不创建 GPU、会话或源文件；数学定义仅为窗口工作副本。
/// 所有输入都经过正式原始触点和 ImGui 事件队列，未伪造 ActiveID。
// 夹具保留真实的长正文，不能通过禁止全页滚动来让图内夺笔检查通过。
// 首帧建立布局，第二帧才提供有效的子窗口包围与滚动范围。
// 每轮新建 ImGui 上下文，窗口名相同也不能继承之前的焦点或位置。
// 原始跟踪器是线程内共享值，因此新副本执行与生产相同的 reset。
// 所有权和联系人数组均由生产代码维护，测试不直接设置内部标志。
// 没有音频设备、Vulkan 实例或会话线程，失败只能来自输入与布局路径。
// 控制点断言读取领域值，不以句柄颜色或 ActiveID 作为编辑成功。
// 手绘比较全部数值与覆盖标志，不能只观察最近触点索引。
// 页面滚动读取正式位置，不能把准备中的滚动目标当成已经应用。
// 预览比较实际显示范围，不以导航器内部倍率作为结果替身。
// 系统 Cancel 与第二指分别注入，即使最终都是零触点也不合并语义。
struct Fixture {
    MMM::Canvas::MobileTimingCurveEditor
        m_editor;  ///< 与移动窗口相同的图表实现。
    MMM::Canvas::TimingFunctionEditorState
                               m_state;   ///< 固定手绘格点，取消前后独立比较。
    MMM::TimingInterpolation   m_curve;   ///< 控制点真值，不读取私有拖动候选。
    MMM::UI::MobileTouchScroll m_scroll;  ///< 真正的父页滚动仲裁，不使用替身。
    ImVec2 m_origin{};             ///< 布局后记录屏幕原点，不猜测主题内边距。
    float  m_scale{}, m_offset{};  ///< 两种逻辑密度分别验证尺寸换算和页面位置。
    float  m_gap{ 100 };  ///< 第二指间距可独立改变，区分纯平移与明确捏合。
    bool   m_down{}, m_second{},
        m_show{ true };  ///< 平台触点与面板可见性分开驱动。
    int    m_kind{};     ///< 零为控制点，一为手绘，二为输出预览。
    double m_tick{};     ///< 固定步长推进输入，不等待真实时钟。
    /// @brief 独立上下文禁用 ini，场景之间不继承旧焦点与窗口位置。
    // 设备密度同时作用于窗口尺寸和真实手指位移，不修改领域归一化控制点。
    // 字体只准备 CPU 图集，测试不依赖正在运行的模拟器或桌面应用。
    // 背景与图表放在同一个根窗口，二者必须能正确区分触点归属。
    explicit Fixture(float scale) : m_scale(scale)
    {
        ImGui::CreateContext();
        auto& io                        = ImGui::GetIO();
        io.IniFilename                  = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        io.DisplaySize                  = { 1200 * scale, 900 * scale };
        io.DeltaTime                    = .02f;
        io.Fonts->AddFontDefault();
        unsigned char* pixels{};
        int            w{}, h{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        // 与生产打开参数窗口一样，清除上一窗口已经结束的触点代际。
        m_editor.reset();
        // 初值既不是全零也不是重叠点，抓错目标或清零取消都会被精确比较发现。
        // 第二枚控制点具有独立坐标，第一枚移动不能隐式改变整条曲线。
        m_curve.m_controlX1 = m_curve.m_controlY1 = .25;
        m_curve.m_controlX2 = m_curve.m_controlY2 = .75;
        m_state.m_minimum                         = 0;
        m_state.m_maximum                         = 2;
        m_state.m_values.fill(.7);
        m_state.m_drawn.fill(false);
        // 图表默认完整区间，后面缩放与平移只影响这个独立视野。
        // 格点和曲线参数故意采用非零初值，取消不能靠清空蒙混通过。
        frame({ -100, -100 });
        frame({ -100, -100 });
    }
    /// @brief 所有按下已结束后销毁，无文件持久化或后台线程。
    ~Fixture() { ImGui::DestroyContext(); }
    /// @brief 后端发布到原始跟踪器和 ImGui 的位置一致，多指不合成另一鼠标。
    /// @param cancel 系统中断与正常松手必须分开解释。
    /// @warning 测试逐帧固定预算，既不等待后台也不复制数学函数缓存。
    // p 是屏幕逻辑坐标；发布原始触点时才分轴除以 DisplaySize。
    // down 的真到假发布正式 Up，Cancel 则使用独立原始阶段。
    // 第二指使用独立 finger ID，不将其转写为键盘修饰键或鼠标右键。
    // 同帧先发布后端输入，再 NewFrame，最后由滚动控制器过滤业务输入。
    // 帧尾归还真实鼠标状态，防止下一帧继承点击补交时的暂存位置。
    void frame(ImVec2 p, bool down = false, bool second = false,
               bool cancel = false)
    {
        auto& io = ImGui::GetIO();
        using MMM::Graphic::TouchPhase;
        // 事件先于 NewFrame，与 SDL 后端实际调用顺序一致。
        // 原始取消单独发布，不能用一个正常 Up 冒充系统中断。
        if ( down || m_down || cancel )
            MMM::Graphic::publishMobileTouch(
                cancel ? TouchPhase::Cancel
                : down ? (m_down ? TouchPhase::Move : TouchPhase::Down)
                       : TouchPhase::Up,
                11,
                1,
                p.x / io.DisplaySize.x,
                p.y / io.DisplaySize.y,
                m_tick);
        // 首指松开后第二指仍可存在；跟踪器的 peak 必须阻止它重新成为单指编辑。
        // 初次第二指和后续第二指移动具有不同阶段，不能重复构造双指起点。
        if ( second || m_second )
            MMM::Graphic::publishMobileTouch(
                second ? (m_second ? TouchPhase::Move : TouchPhase::Down)
                       : TouchPhase::Up,
                11,
                2,
                (p.x + m_gap * m_scale) / io.DisplaySize.x,
                p.y / io.DisplaySize.y,
                m_tick);
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(p.x, p.y);
        io.AddMouseButtonEvent(0, down && !cancel);
        m_down   = down && !cancel;
        m_second = second;
        m_tick += .02;
        ImGui::NewFrame();
        // 先经过全局滚动仲裁再建立图表，与移动外壳提交顺序相同。
        // 若图的子窗边界缺失，此处会真实夺走后面的拖动，而非被夹具跳过。
        m_scroll.beginFrame(m_scale);
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 500 * m_scale, 600 * m_scale });
        ImGui::Begin(
            "Curve fixture",
            nullptr,
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        ImGui::Dummy({ 1, 30 * m_scale });
        const ImVec2 size{ 400 * m_scale, 240 * m_scale };
        m_offset = ImGui::GetScrollY();
        // 原点来自实际布局，固定数字只表示夹具的可用宽高。
        // 图表隐藏时仍执行生产收尾，不由测试代为调用取消或恢复备份。
        if ( m_show ) {
            if ( m_kind == 0 ) {
                m_origin = ImGui::GetCursorScreenPos();
                m_editor.bezier(m_curve, size, m_scale);
                // 手绘与输出图共享明确的 BeginChild/EndChild 协议。
                // BeginChild
                // 即使不可见也需要配对结束，不能靠隐藏测量帧避开绘图。
            } else {
                m_editor.beginSurface("graph", size);
                m_origin = ImGui::GetCursorScreenPos();
                if ( m_kind == 1 )
                    m_editor.sketch(m_state, m_origin, size);
                else
                    m_editor.preview(
                        m_state.m_outputPreview, m_origin, size, m_scale);
                ImGui::EndChild();
            }
        }
        // 真正的长正文产生滚动范围；图内绘制不得改变这个页面滚动值。
        // 隐藏活动图后仍调用帧收尾，让取消由生产实现自己完成。
        m_editor.finish(m_curve, m_state);
        ImGui::Dummy({ 100, 1200 * m_scale });
        ImGui::End();
        m_scroll.endFrame();
        ImGui::Render();
    }
    /// @brief 第一控制点按生产固定留白投影，热区测试额外加入边缘偏置。
    // 期望中心由公开几何和固定留白独立计算，不访问解释器的抓取点。
    // 偏置热区由调用用例额外加入，能够验证边缘按下不会跳位。
    ImVec2 first() const
    {
        return { m_origin.x + 16 + .25f * (400 * m_scale - 32),
                 m_origin.y + 16 + .75f * (240 * m_scale - 32) };
    }
};
/// @brief 控制点双向移动、边缘抓取不跳位，以及多指、Cancel、隐藏取消。
/// @details 放手保留本笔副本；取消恢复原四值，不改其它控制点。
// 正常结束是必要阳性对照，不能只靠所有输入都被吞掉而通过取消断言。
// 第二指、系统 Cancel、隐藏图分别代表不同的非正常结束来源。
// 返回原点已经发生过有效移动，区别于始终静止的一次轻点。
// 所有场景都通过正式 Up 与空闲帧收尾，不能在断言之后迟到提交。
void bezier(float scale)
{
    for ( int scenario = 0; scenario < 5; ++scenario ) {
        Fixture    f(scale);
        const auto a = f.first();
        // 起点偏离图形中心 12 逻辑像素，证明使用扩大热区与固定抓取偏置。
        const ImVec2 start{ a.x + 12 * scale, a.y },
            end{ start.x + 40 * scale, start.y - 30 * scale };
        // 从热区边缘抓取，真实 Down 只冻结身份与四个初值。
        // 首次反馈不能把图形中心吸到手指位置，X 与 Y 都应保持原值。
        f.frame(start, true);
        check(f.m_curve.m_controlX1 == .25 && f.m_curve.m_controlY1 == .25,
              "touch down does not jump a Bezier handle");
        f.frame(end, true);
        check(f.m_curve.m_controlX1 > .25 && f.m_curve.m_controlY1 > .25,
              "real touch changes both control coordinates");
        check(f.m_curve.m_controlX2 == .75 && f.m_curve.m_controlY2 == .75 &&
                  f.m_offset == 0,
              "other handle and parent scroll remain unchanged");
        // 先证明产生了实际坐标变化，再注入中断，排除从未起笔的假取消。
        // 另一控制点及父页滚动也必须保持，避免把整页移动误当作局部编辑。
        if ( scenario == 1 ) f.frame(end, true, true);
        if ( scenario == 2 ) f.frame(end, false, false, true);
        // 这里只改变面板可见性，不直接调用生产取消方法。
        // 生产帧收尾需自行发现活动图缺席并恢复窗口副本。
        if ( scenario == 3 ) {
            f.m_show = false;
            f.frame(end, true);
        }
        if ( scenario == 4 ) f.frame(start, true);
        f.frame(scenario == 4 ? start : end);
        f.frame(end);
        // 回到原点也是无变化；正常释放则必须保留真实编辑，而非全部吞掉。
        const bool changed =
            f.m_curve.m_controlX1 != .25 || f.m_curve.m_controlY1 != .25;
        check(changed == (scenario == 0),
              "Bezier release commits locally; takeover, hidden graph and "
              "return restore the baseline");
    }
}
/// @brief 手绘固定格点经过实际输入，连续扫过、取消和页面滚动分别检查。
/// @details 取消不清空此前的手绘状态；完整横扫必须覆盖全部 129 个格点。
// 格点与覆盖标志分别比较，取消不能仅清除绘制掩码却留下参数污染。
// 一次横扫故意只提供相距较远的样本，迫使实现补齐中间所有格点。
// 测试不直接调用格点填充或拟合函数；输入必须经过生产图表。
void sketch(float scale)
{
    Fixture f(scale);
    f.m_kind = 1;
    f.frame({ -100, -100 });
    f.frame({ -100, -100 });
    const auto   origin = f.m_origin;
    const ImVec2 a{ origin.x + 1, origin.y + 60 * scale },
        b{ origin.x + 400 * scale, origin.y + 120 * scale };
    // 独立期望值保存在测试中，不能读取生产事务内部的备份作为成功证据。
    // 起点略在左边缘以内，终点恰在右边缘，覆盖完整域而不越过窗口边界。
    const auto before = f.m_state.m_values;
    const auto drawn  = f.m_state.m_drawn;
    // 旧拟合只能在正常结束后失效，取消不允许丢弃之前可用的候选。
    // 通过真实领域编译器准备缓存，不伪造 TimingFunction 的内部结构。
    auto compiled = MMM::compileTimingFunction("1+t", .5);
    check(compiled.has_value(),
          "prepare a valid previous fit for cancellation");
    // 指针仅用于缓存身份观察，测试不为每帧调用复制额外共享所有权。
    // 若取消错误地重新编译等价表达式，后面的身份断言同样能够发现。
    if ( compiled ) f.m_state.m_fitFunction = std::move(*compiled);
    // 身份观察不增加共享所有权，连续帧也不会因断言额外持有缓存。
    // 正常结束后检查空候选，取消后检查原身份，两者必须具有相反结果。
    // 不能只检查绘图数组而遗漏取消对此前拟合的破坏。
    const auto* previousFit = f.m_state.m_fitFunction.get();
    f.frame(a, true);
    f.frame(b, true);
    f.frame(b, true, true);
    f.frame(b);
    check(f.m_state.m_values == before && f.m_state.m_drawn == drawn,
          "second finger restores exactly the previous sketch");
    check(f.m_state.m_fitFunction.get() == previousFit,
          "takeover preserves the previous fit cache");
    f.frame(a, true);
    f.frame(b, true);
    f.frame(b, false, false, true);
    f.frame(b);
    check(f.m_state.m_values == before && f.m_state.m_drawn == drawn,
          "system Cancel restores sketch without clearing previous points");
    check(f.m_state.m_fitFunction.get() == previousFit,
          "system Cancel preserves the previous fit cache");
    // 图表缺席与 Up 不同，即使平台还按住也应恢复本笔之前的完整格点。
    // 隐藏后再次显示只重建布局，不能以残留触点续写旧手绘。
    // 前一手势已经全抬，新 Down 必须能够建立正常事务。
    // 这样隐藏取消不能继承之前被多指永久封锁的状态而偶然通过。
    // 本场景只隐藏图表，不由测试调用生产取消或重新赋值数组。
    // 旧拟合作为不可变蓝色参考保留，半笔黄色输入不能替换其定义。
    f.frame(a, true);
    f.frame(b, true);
    f.m_show = false;
    f.frame(b, true);
    f.frame(b);
    check(f.m_state.m_values == before && f.m_state.m_drawn == drawn,
          "hiding an active sketch discards its partial stroke");
    // 重新显示先预热子窗布局，再由下一真实 Down 开始新笔。
    // 不能为了取消安全而永久冻结后续的单指绘制。
    f.m_show = true;
    f.frame({ -100, -100 });
    f.frame({ -100, -100 });
    f.frame(a, true);
    f.frame(b, true);
    f.frame(b);
    // 完整覆盖是自动拟合的前置条件，看到一段黄色线不等于所有区间都已绘制。
    // 129 个有效标志必须全部为真，能够发现快速输入只保留最后一个点的问题。
    check(std::all_of(f.m_state.m_drawn.begin(),
                      f.m_state.m_drawn.end(),
                      [](bool x) { return x; }),
          "fast sweep fills every drawing cell including endpoints");
    check(f.m_offset == 0, "drawing inside the graph never scrolls the form");
    check(!f.m_state.m_fitFunction,
          "completed drawing invalidates only the old fit candidate");
    // 图外空白起手仍须能滚动，不能靠禁用整个表单让图内测试通过。
    // 图外空白属于同一个表单，拖动过程中即使进入图也不能转交绘制。
    // 正向滚动断言排除永久屏蔽触摸或禁用整个表单的取巧修复。
    // 清除的是手绘候选而不是已保存函数，夹具没有模型会话可供修改。
    // 同一状态继续接受图外输入，便于发现滚动过程误绘新格点的问题。
    // 父页具有真实长内容，图外拖动必须产生正向滚动量。
    // 滚动检查在全部释放之后进行，迟到的过滤点击不能遮蔽结果。
    const ImVec2 out{ origin.x + 100 * scale, origin.y + 300 * scale };
    f.frame(out, true);
    f.frame({ out.x, out.y - 100 * scale }, true);
    f.frame({ out.x, out.y - 100 * scale });
    f.frame(out);
    check(f.m_offset > 20 * scale,
          "outside the graph the same form still scrolls");
}
/// @brief 预览导航改变显示范围，不修改绘图数据；原始双指含距离死区。
// 显示范围与曲线数据分开观察，导航成功必须改变视野但不改变图表参数。
// 单指横移、双指纯平移和明确捏合分别验证，不能由同一个无操作结果替代。
// 双指轻触产生的谱面历史意图在此表面不消费，避免弹窗内误撤销。
void preview(float scale)
{
    Fixture f(scale);
    f.m_kind = 2;
    f.frame({ -100, -100 });
    f.frame({ -100, -100 });
    // 全段视野无法继续平移，先通过公开显示接口建立合法的局部基线。
    // 准备只影响显示范围，后续平移仍必须由实际触点产生。
    f.m_state.m_outputPreview.zoomAt(.5, .5);
    const auto   values = f.m_state.m_values;
    const ImVec2 a{ f.m_origin.x + 150 * scale, f.m_origin.y + 80 * scale };
    const auto   start = f.m_state.m_outputPreview.viewStart();
    f.frame(a, true);
    f.frame({ a.x - 40 * scale, a.y }, true);
    f.frame({ a.x - 40 * scale, a.y });
    check(f.m_state.m_outputPreview.viewStart() > start,
          "single-finger horizontal preview pan is effective");
    // 单指左移应向更晚的时间推进，而不是移动曲线的真实时间域。
    // 保存跨度用于后面的纯平移对照，不从导航器内部读取预期倍率。
    const double span = f.m_state.m_outputPreview.viewEnd() -
                        f.m_state.m_outputPreview.viewStart();
    f.frame(a, true);
    f.frame(a, true, true);
    // 两指同向移动时保持初始间距，纵向微小偏移不能隐式解锁缩放。
    // 门槛前后均沿用主画布已验证的死区，不另造定时等待窗口。
    f.frame({ a.x + 40 * scale, a.y + 2 * scale }, true, true);
    f.frame(a);
    check(std::abs(f.m_state.m_outputPreview.viewEnd() -
                   f.m_state.m_outputPreview.viewStart() - span) < 1e-7,
          "two-finger translation preserves zoom in the pinch dead zone");
    check(
        f.m_state.m_values == values && f.m_offset == 0,
        "preview navigation does not change drawing data or scroll its parent");
    f.frame(a, true);
    f.frame(a, true, true);
    // 第二指独立张开并超过缩放死区，不能与上面的纯平移路径混淆。
    // 第一指不变、第二指独立移动，制造真实距离变化而非共同平移。
    // 放手后再读视野，缩放不能只停留在解释器的临时候选里。
    // 图表导航没有修改手绘数组的权限，前面的完整数组断言仍须成立。
    f.m_gap = 180;
    f.frame(a, true, true);
    f.frame(a);
    check(
        f.m_state.m_outputPreview.viewEnd() -
                f.m_state.m_outputPreview.viewStart() <
            span,
        "deliberate pinch zooms the output preview without changing the curve");
}
}  // namespace
/// @brief 两种密度下执行真实控件与原始输入回归，不读写任何项目文件。
int main()
{
    // 每种密度都运行完整场景，不能只缩放期望值而复用上一上下文的布局。
    // 当前测试不宣称完成真实手感验收，Android 后端和小屏裁剪另由设备检查。
    for ( float scale : { 1.f, 2.f } ) {
        bezier(scale);
        sketch(scale);
        preview(scale);
    }
    // 全部密度与场景执行完才返回，单项失败不会短路后续取消检查。
    // 错误由项目日志报告，不写入源码测试资源目录。
    return failures ? 1 : 0;
}
