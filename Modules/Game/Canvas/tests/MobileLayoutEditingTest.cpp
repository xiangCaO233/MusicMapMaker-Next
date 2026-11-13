#include "canvas/Basic2DCanvasInteraction.h"
#include "common/render/RenderSnapshot.h"
#include "config/AppConfig.h"
#include "config/EditorConfig.h"
#include "config/LayoutPresets.h"
#include "config/VisualConfig.h"
#include "graphic/mobile/MobileTouchInput.h"
#include "log/colorful-log.h"
#include "ui/imgui/manager/LayoutPresetControls.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include <cmath>
#include <cstdlib>
#include <imgui.h>
#include <imgui_internal.h>

namespace MMM::Canvas
{
/// @brief 回归只进入完整生产布局处理器，不写入其私有目标或手势标志。
struct MobileLayoutEditingTestAccess {
    /// @brief 几何描述实际测试窗口中的画布，不替代生产命中或布局算法。
    /// @warning CPU 测试逐帧调用，配置写入仍走生产的结束边沿。
    static void draw(Basic2DCanvasInteraction&             c,
                     const Common::Render::RenderSnapshot& s, float width)
    {
        c.handleLayoutEditing(0, 0, 40, 60, width, 480, false, s);
    }
};
}  // namespace MMM::Canvas
namespace
{
using namespace MMM;
int failures{};  ///< 每项独立累计，后续通过不能覆盖前面失败。
/// @brief 检查生产可观测值，日志不输出个人配置内容。
void check(bool passed, const char* label)
{
    if ( !passed ) {
        ++failures;
        XERROR("Mobile layout: {}", label);
    }
}
/// @brief 使用真实 ImGui 窗口和原始触点，合成鼠标始终不进入画布。
/// @details 回归边界按实际生产调用划分：
/// - 原始落点由窗口树裁决，不以测试提供的 hovered=true 绕过遮挡。
/// - 布局几何由真实处理器读取并修改，不复制边缘、吸附或联动算法。
/// - 持续拖动立即检查内存值，不能等松手才断言一次最终结果。
/// - 按下偏差刻意位于原桌面命中带以外，验证新增触摸范围实际可用。
/// - 四键配置作为本次操作目标，七键配置作为不可修改的对照。
/// - 原始取消不伪造成正常 Up，保留平台阶段的实际区别。
/// - 同一代际的余指与旧释放都必须由输入门闩拒绝。
/// - 每个场景使用独立对象与 ImGui 上下文，捕获不得跨场景继承。
/// @details CTest 为本用例设置独立配置根，生产保存不会触碰用户目录。
struct Fixture : UI::IWorkspacePresentation {
    Canvas::Basic2DCanvasInteraction m_controller{ "MobileLayoutRegression",
                                                   "layout-test" };
    Common::Render::RenderSnapshot
          m_snapshot;      ///< 不可变测试场景，不访问 ECS。
    float m_width{ 800 };  ///< 可变几何用于检查旋转／重排取消。
    /// @brief 预设 UI 用例在同一个真实宿主中绘制正式控件，而非伪造打开标志。
    bool   m_controls{};
    ImVec2 m_comboPoint{};  ///< 从实际内容起点及字体高度定位下拉框。
    double m_time{};        ///< 注入时间只维持事件顺序，不影响状态机判定。
    /// @brief 测试显式指定移动呈现，不由宿主平台宏推断。
    bool allowsHoverFeedback() const override { return false; }
    /// @brief 本测试只需要原生窗口树，不创建 Vulkan 或完整工作区。
    void renderWorkspace(UI::MainDockSpaceUI&, UI::UIManager&) override {}
    /// @brief 每个场景从相同合法布局开始，七键覆盖用于检查跨键数隔离。
    Fixture()
    {
        // 配置单例不加载个人文件，仅构造用例所需的两种键数布局。
        // 生产 saveLayoutChanges 与 finishLayoutEditing 仍会执行，目标根由
        // CTest 隔离。 不拦截保存函数，也不把配置序列化替换成测试专用实现。
        auto& app = Config::AppConfig::instance();
        app.reset();
        app.setNativeContentScale(1);
        auto& v     = app.getVisualConfig();
        auto& four  = v.editableTrackLayoutForKeyCount(4);
        four.left   = .2f;
        four.right  = .8f;
        four.top    = .1f;
        four.bottom = .9f;
        v.editableTrackLayoutForKeyCount(7).left = .12f;
        // 先捕获起始布局，后续按住不修改时不能因基线缺失而生成新项。
        // 预设库仍使用实际内置保护和自动自定义副本规则。
        app.getLayoutPresets().captureCurrent(v);
        // 禁用 INI，防止上次运行的窗口位置影响起点命中或写入用户布局。
        // CPU 字体图集只满足绘制生命周期，不初始化窗口系统或 GPU。
        ImGui::CreateContext();
        auto& io       = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = { 1000, 600 };
        io.DeltaTime   = .016f;
        unsigned char* pixels{};
        int            w{}, h{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        m_snapshot.hasBeatmap        = true;
        m_snapshot.trackCount        = 4;
        m_snapshot.beatmapInstanceId = 1;
        // 清除上个场景的设备状态，新的 Down 才允许在当前窗口取得归属。
        // 方案切换按生产安全帧协议执行两次，不直接替换全局活动方案。
        Graphic::cancelMobileTouch();
        UI::Interaction::synchronizeProfile("touch-gestures", true);
        UI::Interaction::synchronizeProfile("touch-gestures", true);
        // 首两帧建立窗口树，实际按下必须通过原始位置的遮挡检查。
        frame();
        frame();
    }
    /// @brief 关闭前先正常结束输入，异步保存只落在 CTest 私有目录。
    ~Fixture()
    {
        // 析构不直接重置控制器私有标志；取消仍须被实际生产帧消费。
        // 最后销毁上下文，避免布局处理器使用已释放的绘制列表。
        // 即使断言失败也执行收尾，测试不能依靠提前退出掩盖捕获泄漏。
        Graphic::cancelMobileTouch();
        frame();
        ImGui::DestroyContext();
    }
    /// @brief 提交完整帧；窗口身份与图形生命周期均沿用 ImGui 正式入口。
    void frame()
    {
        // 保留非零画布起点，检查屏幕归一化、窗口和画布局部三者的换算。
        // 每个输入之后真实 Render，下一次命中使用 ImGui 已发布的窗口树。
        // 重排案例改变显示尺寸，原始触点和生产计时仍按正常顺序推进。
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ m_width + 200, 600 });
        ImGui::Begin("LayoutTest");
        UI::WorkspaceFrameScope scope(*this);
        if ( m_controls ) {
            // 模拟生产布局面板的 48dp 行高，位置来自实际窗口而非设备坐标。
            // 这里不复制控件 ID 或 popup 哈希，按下仍走生产 BeginCombo。
            const float height = 48;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                { 4, (height - ImGui::GetFontSize()) * .5f });
            const auto origin = ImGui::GetCursorScreenPos();
            m_comboPoint = { origin.x + 100,
                             origin.y + ImGui::GetTextLineHeightWithSpacing() +
                                 height * .5f };
            UI::drawLayoutPresetControls();
            ImGui::PopStyleVar();
        } else {
            Canvas::MobileLayoutEditingTestAccess::draw(
                m_controller, m_snapshot, m_width);
        }
        ImGui::End();
        ImGui::Render();
    }
    /// @brief 注入按真实屏幕归一化的阶段，不直接伪造状态机输出。
    void event(Graphic::TouchPhase phase, float x, float y, int finger = 1)
    {
        // 设备与手指身份固定，第二指仅改 finger，用来模拟数组压缩与剩余首指。
        // 像素转归一化只用于注入，期望结果直接读取实际配置字段。
        // 不设置合成鼠标位置，以便回归独立覆盖原始输入路径。
        Graphic::publishMobileTouch(
            phase, 77, finger, x / 1000, y / 600, m_time += .016);
        frame();
    }
    /// @brief 只读生产字段，用于比较未移动和结束后的合法布局。
    float left() const
    {
        return Config::AppConfig::instance()
            .getVisualConfig()
            .trackLayoutForKeyCount(4)
            .left;
    }
};
/// @brief 放大命中带仍保留抓取偏差，静止按住和轻点不能改动布局。
/// @details 起点 x216 与实际左边 x200 不重合，可识别按下即跳变。
/// 移动的期望由固定 800px 画布中的实际 80px 位移推得。
/// 最终 Up 多出的位移单独断言，不能用持续 Move 的结果代替。
/// 不检查配置命令次数来冒充几何正确，实际内存字段必须即时变化。
void dragAndTap()
{
    using enum Graphic::TouchPhase;
    Fixture f;
    // 左边真实位置为 x200，按在外侧 16px，已超出桌面的七像素半径。
    // 首帧必须捕获真实边而不是立即把边界改到指尖位置。
    f.event(Down, 216, 300);
    check(std::abs(f.left() - .2f) < 1e-6, "down must not jump");
    // 帧数覆盖原通用长按阈值，布局输入不能因此变为菜单或自动写配置。
    // 这里逐帧推进生产窗口，不通过系统 sleep 等待一个不确定墙钟时长。
    for ( int i = 0; i < 35; ++i ) f.frame();
    check(std::abs(f.left() - .2f) < 1e-6, "hold must not edit or open menu");
    f.event(Up, 216, 300);
    check(std::abs(f.left() - .2f) < 1e-6, "tap must not edit");
    f.event(Down, 216, 300);
    f.event(Move, 296, 300);
    check(std::abs(f.left() - .3f) < 1e-5,
          "wide hit must preserve grab offset");
    // 松手再额外移动十六像素，只看最后一个 Move 会遗漏这部分输入。
    // 抓取偏差持续存在，最终边界应等于起始边加真实位移。
    f.event(Up, 312, 300);
    // 正常 Up 的最后一点必须先落入配置，再清除捕获；不能丢失最后 16px。
    check(std::abs(f.left() - .32f) < 1e-5,
          "normal release must apply final point");
    check(std::abs(Config::AppConfig::instance()
                       .getVisualConfig()
                       .trackLayoutForKeyCount(7)
                       .left -
                   .12f) < 1e-6,
          "other key count unchanged");
}
/// @brief Cancel 和第二指保留最后合法值，余指以及迟到 Up 不能继续编辑。
/// @details 取消前的值从生产配置读取，而非根据测试里的手势算法计算。
/// 第二指接管保留第一指，专门验证数量回到一时不会再次取得编辑权。
/// 随后独立改变画布宽度，覆盖与平台多指无关的几何失效路径。
/// 每个结束后仍补送 Up，检查已经封锁的代际不会在抬手时写回。
void interruptions()
{
    // 本组先建立一次真实已生效的拖动，再触发取消，不能只测空闲取消。
    // 合法值保留与取消坐标不生效必须同时满足。
    using enum Graphic::TouchPhase;
    Fixture f;
    f.event(Down, 216, 300);
    f.event(Move, 296, 300);
    const float legal = f.left();
    Graphic::cancelMobileTouch();
    f.frame();
    f.event(Up, 500, 300);
    check(f.left() == legal, "cancel coordinate must not edit");
    // 新 Down 可以重新捕获，第二指随后接管；全部释放也不能恢复旧编辑。
    // 同一代际的任意剩余 Move 与 Up 都不能修改最后合法值。
    f.event(Down, 296, 300);
    f.event(Move, 336, 300);
    const float beforePair = f.left();
    f.event(Down, 500, 300, 2);
    f.event(Move, 600, 300);
    f.event(Up, 500, 300, 2);
    f.event(Up, 600, 300);
    check(f.left() == beforePair, "second finger must seal remainder");
    f.event(Down, 336, 300);
    // 显示尺寸变化模拟旋转／窗口尺寸改变，旧手指仍在屏幕中；必须主动结束。
    // 不能按新宽度重算旧抓取偏差，避免旋转或面板重排后跳变。
    f.m_width                  = 760;
    ImGui::GetIO().DisplaySize = { 960, 600 };
    f.event(Move, 400, 300);
    f.event(Up, 400, 300);
    check(f.left() == beforePair, "geometry change must stop old projection");
}
/// @brief 发布配置引起内容宽度变化不能被误判为窗口旋转。
/// @details 同一窗口的内容和外框宽度都采纳五像素变化，与模拟器故障一致。
/// 第二次移动必须继续产生可见变化，不能只检查按下和第一段移动。
/// 最后抬手还须应用剩余位移，验证内容变化后的捕获确实持续到结束。
/// 该用例保留显示尺寸和窗口／停靠身份，不触发旋转或切页门禁。
void contentAdjustment()
{
    using enum Graphic::TouchPhase;
    Fixture f;
    f.event(Down, 216, 300);
    f.event(Move, 236, 300);
    const float first = f.left();
    // 原场景第一段修改后被封锁；这个步进必须能够继续更新同一捕获。
    // 仍由原算法使用当前内容宽度转换归一化值，不在测试中预写结果。
    f.m_width += 5;
    f.event(Move, 296, 300);
    check(f.left() > first + .05f,
          "content adjustment must not terminate drag");
    f.event(Up, 312, 300);
    check(std::abs(f.left() - 256.f / 805.f) < 1e-5,
          "content adjustment must preserve final point");
}
/// @brief 窄辅助区中心仍能移动，不能被放大的主轨道或自身边缘吞掉。
/// @details 区域宽 16px，小于两侧 48dp 命中带；测试同时检查位置与宽度。
/// 主轨道右边界是独立对照，错误命中主轨道缩放也必须被检出。
void narrowAuxiliary()
{
    using enum Graphic::TouchPhase;
    Fixture f;
    auto&   v      = Config::AppConfig::instance().getVisualConfig();
    auto&   layout = v.editableTrackLayoutForKeyCount(4);
    // 左边与主轨道留 16px 间隔，中心仍在主轨道扩大命中带的边缘。
    // 固定布局值只提供测试前置场景，移动结果必须由生产命中和变换得到。
    // 将 BGM 放到远处以排除无关吸附，避免测试结果依赖默认邻接布局。
    layout.annotation.left  = .82f;
    layout.annotation.width = .02f;
    layout.bgmLanes.left    = 1.1f;
    layout.bgmLanes.width   = .2f;
    // 提供合法初始配置后仍由真实 Down 选择目标，不设置控制器捕获状态。
    // 区域中心离主轨道右边仅 24px，应由最近的中心把手获得归属。
    f.frame();
    f.event(Down, 704, 300);
    f.event(Move, 744, 300);
    f.event(Up, 744, 300);
    // 同时检查锚点、宽度和主轨道，区分 Move、误缩放和拖错区域三类结果。
    // 容差远小于一像素，不能把仅发生很小变动当成整笔移动成功。
    const auto& result = v.trackLayoutForKeyCount(4);
    check(result.annotation.left &&
              std::abs(*result.annotation.left - .87f) < 1e-5,
          "narrow auxiliary center must move");
    check(result.annotation.width &&
              std::abs(*result.annotation.width - .02f) < 1e-6,
          "auxiliary move must preserve width");
    check(std::abs(result.right - .8f) < 1e-6,
          "auxiliary touch must not resize main track");
}
/// @brief 打开真实预设下拉框时不得触发 ImGui 样式栈恢复错误。
/// @details 只展示默认库，不点击任何方案，因此不会发布引擎配置。
/// 回归必须确认 popup 确实打开，避免没有命中按钮却因无错误而假通过。
/// 生产反馈组合框同时管理弹层窗口 padding 和条目间距，必须使用对应结束入口。
/// 单纯检查函数返回后的全局栈会漏掉自动恢复过的缺失 Pop，因此比较错误计数。
/// 不依赖测试引擎或硬编码内部 Popup 哈希，按实际窗口里的控件矩形点击。
/// 展开与释放都接受检查，不把下拉框只在某个鼠标边沿打开当成业务契约。
void presetComboStyles()
{
    Fixture f;
    // 切换到正式预设 UI 只替换测试所绘制的内容，外壳和原生窗口仍相同。
    // 不通过调用业务回调或改库选择来假装下拉框已经打开。
    f.m_controls = true;
    f.frame();
    f.frame();
    auto& io = ImGui::GetIO();
    // 禁用错误断言仅为记录失败，恢复错误数量本身就是测试结果。
    // 不能只检查结束后的栈尺寸，因为 ImGui 自动恢复可能已经替错误代码弹栈。
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigInputTrickleEventQueue    = false;
    // 先定位再按下，输入队列与实际平台事件顺序一致。
    // 位置来自绘制时记录的内容起点、文字行高与控件高度，不复制 Combo 布局算法。
    io.AddMousePosEvent(f.m_comboPoint.x, f.m_comboPoint.y);
    io.AddMouseButtonEvent(0, true);
    f.frame();
    // 原生按下与释放都可能是有效激活沿，必须至少有一帧出现实际弹层。
    // 两帧错误分别检查，恢复机制不能把发生过的样式错误隐藏到下一帧。
    bool opened = GImGui->OpenPopupStack.Size > 0;
    check(GImGui->ErrorCountCurrentFrame == 0,
          "preset combo must pair feedback style scope");
    io.AddMouseButtonEvent(0, false);
    f.frame();
    opened = opened || GImGui->OpenPopupStack.Size > 0;
    check(opened, "preset combo must actually open");
    check(GImGui->ErrorCountCurrentFrame == 0,
          "open combo frame must preserve styles");
    // 键盘 Escape 只关闭 UI，不选择项目，避免引擎发布污染这个几何回归。
    // 正常关闭后继续一帧，确认没有残留的 FramePadding 或弹层间距。
    io.AddKeyEvent(ImGuiKey_Escape, true);
    f.frame();
    io.AddKeyEvent(ImGuiKey_Escape, false);
    f.frame();
    check(GImGui->StyleVarStack.Size == 0,
          "combo close must leave no style residue");
    // 退出前关闭实际 popup，后续场景不能继承其前景窗口或导航身份。
    // 不直接清理 OpenPopupStack，按键生命周期仍由 ImGui 执行。
}
/// @brief 合帧拖动仍按原点拾取，再补最终位置；窗口外起点不能中途移入。
/// @details 原始事件泵一次接收 Down、Move、Up 时，渲染没有中间按住帧。
/// 生产代码必须使用快照保存的起点与正常释放位置完成同一目标操作。
/// 外部起点反例在上次手势结束后运行，检查旧捕获不会跨代际泄漏。
/// 两次 frame 是输入协议的起点与终点交付，不是等待后台数据的超时。
void fastAndOutside()
{
    // 三条事件在下一次 NewFrame 前全部发布，模拟低帧率或快速划动。
    // 控制器须先按原始 Down 选边，再把正常 Up 作为唯一的最终位置。
    // 测试不手动调用按下处理函数或设置 m_layoutTouch 的私有阶段。
    using enum Graphic::TouchPhase;
    Fixture f;
    Graphic::publishMobileTouch(Down, 77, 1, .216f, .5f, 1);
    Graphic::publishMobileTouch(Move, 77, 1, .296f, .5f, 1.02);
    Graphic::publishMobileTouch(Up, 77, 1, .312f, .5f, 1.03);
    f.frame();
    f.frame();
    check(std::abs(f.left() - .32f) < 1e-5,
          "merged drag must preserve origin and final point");
    // 外部起点位于画布左边界之外，即使之后划过边缘也不能新建捕获。
    // 该场景与快速合法拖动共用同一对象，确认全新代际不会继承旧所有权。
    const float legal = f.left();
    f.event(Down, 20, 300);
    f.event(Move, 400, 300);
    f.event(Up, 400, 300);
    check(f.left() == legal, "outside gesture cannot acquire after entry");
}
}  // namespace
/// @brief 禁止脱离隔离配置根运行会保存配置的交互回归。
int main()
{
    // 此回归确实执行配置保存，直接手动运行时必须主动提供隔离根。
    // 未设置环境即拒绝执行，不以测试失败后的恢复冒险覆盖个人文件。
    // CTest 公共入口会自动注入该变量，无需修改用户 HOME 或系统环境。
    // 三组用例独立运行，任一组失败都会保留对应断言标签和非零退出码。
    if ( !std::getenv("MMM_CONFIG_ROOT") ) return 2;
    dragAndTap();
    interruptions();
    fastAndOutside();
    contentAdjustment();
    narrowAuxiliary();
    presetComboStyles();
    return failures ? 1 : 0;
}
