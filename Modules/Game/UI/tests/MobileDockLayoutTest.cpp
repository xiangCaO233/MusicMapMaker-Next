/**
 * @file MobileDockLayoutTest.cpp
 * @brief 使用无平台窗口的 ImGui 上下文检查移动分区布局。
 * @details 测试采样实际业务窗口的内容区域，而非重复布局内部公式。
 * 标签缺失、内容高度不齐和显隐后误停靠均可独立导致断言失败。
 * 不需要 GPU，也不读取或覆盖用户实际工作区文件。
 */
#include "ui/workspace/MobileDockLayout.h"
#include "log/colorful-log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <imgui_internal.h>
#include <string>
#include <string_view>

namespace
{
/// @brief 在无 ini 的全新上下文中检查首次主画布确实进入中心节点。
/// @details 连续两次目标请求模拟外壳分类和标题适配，不预先伪造窗口设置。
/// Begin 使用正常持久化标志，覆盖真机清配置后 FirstUseEver 的真实语义。
/// @return 主画布属于中心叶且占据完整宿主时返回 true。
/// @note 浮窗即使仍在视图注册表中，也可能被宿主遮住，因此必须检查几何。
/// 测试前后完整销毁上下文，不能用前一轮已停靠窗口掩盖首次初始化失败。
/// 有限帧只推进 ImGui 状态机，不使用真实时间等待模拟业务就绪。
bool freshCanvasDocked()
{
    ImGui::CreateContext();
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = { 1000, 700 };
    io.DeltaTime   = 1.0f / 60;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    unsigned char* pixels{};
    int            width{}, height{};
    // 只建立 CPU 字体，不需要平台窗口或 GPU。
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    MMM::UI::MobileDockLayout layout;
    // 首帧提交后继续推进原生停靠状态，排除只检查 NextWindowData 的假阳性。
    // 不向窗口注入尺寸或 DockId，因此断言必须由布局策略本身满足。
    // 本夹具只活在独立上下文，不能把临时树或字体状态带入后续用例。
    for ( int frame = 0; frame < 5; ++frame ) {
        ImGui::NewFrame();
        layout.render({ 0, 0 }, { 1000, 700 }, 1, false, false);
        // 同一稳定窗口身份的重复请求必须保留尚未消费的首次停靠。
        layout.target(MMM::UI::MobileDockPanel::Main, "Logo###Canvas_fresh");
        layout.target(MMM::UI::MobileDockPanel::Main, "Logo###Canvas_fresh");
        ImGui::Begin("Logo###Canvas_fresh");
        ImGui::End();
        layout.finishFrame();
        ImGui::Render();
    }
    const auto* canvas = ImGui::FindWindowByName("Logo###Canvas_fresh");
    // 检查真实归属和几何，不能仅以节点已创建或窗口存在判定成功。
    const bool docked = canvas && canvas->DockId == layout.center() &&
                        canvas->DockId != 0 && canvas->Size.x > 900 &&
                        canvas->Size.y > 600;
    ImGui::DestroyContext();
    return docked;
}

/// @brief 一个业务分区 Begin 后的实际内容边界，不以宿主尺寸代替可用高度。
struct Bounds {
    /// @brief 内容左上角和剩余高度，用于比较标签下方的真实区域。
    ImVec2 origin{};
    float  height{};
};

/// @brief 以真实 ImGui 停靠节点驱动布局，不创建 Vulkan 或平台窗口。
struct Fixture {
    /// @brief 与移动外壳相同的布局对象，不直接改写被测私有字段。
    MMM::UI::MobileDockLayout layout;
    /// @brief 主画布、Timing 和预览各自 Begin 后采样的边界。
    std::array<Bounds, 3> bounds{};
    /// @brief 宿主的局部触摸样式必须成对恢复，不能泄漏到共享窗口。
    bool styleIntact{ true };
    /// @brief 内容上沿不得越过原生标签下沿，防止内容背景覆盖标签。
    bool titleFits{ true };
    /// @brief 模拟惰性设置页连续请求聚焦，不能覆盖恢复后的主区活动标签。
    bool lateFocus{};
    /// @brief 非整数倍率产生的宿主宽度，覆盖真实设备而非仅测试整数几何。
    float hostWidth{ 900.85f };
    /// @brief 菜单只返回意图；测试不销毁窗口，便于连续检查各个叶节点。
    int closeRequests{};
    /// @brief 模拟最后主画布受保护，其余测试标签仍可关闭。
    bool mainClosable{ false };
    /// @brief 独立长标题窗口，复用稳定后缀来检查重命名与滚动裁剪。
    bool longCaption{};
    /// @brief 同时提交两个分析视图，检查它们与主画布共用中央原生标签栏。
    bool analysisViews{};
    /// @brief
    /// 仅在工作台专属场景提交真实画布身份，其他既有标签仍模拟欢迎或设置。
    bool m_canvasPage{}, m_workbenchRequested{};
    /// @brief 仅在内存 ini 往返用例启用窗口设置，不触及用户磁盘。
    bool m_persistWindows{};
    /// @brief 完整标题用于展示，固定后缀用于检验窗口身份不随谱名变化。
    std::string longTitle{
        "Editor [4K] A deliberately long beatmap name that must keep its full "
        "title###LongCanvas"
    };
    // 每次 Begin 累积检查，首次出现、缩放和拖动帧都不能短暂盖住标签。

    /// @brief 将一个测试窗口送入对应分区并读取其实际内容矩形。
    /// @param panel 真实布局提供的分区身份。
    /// @param name 稳定的模拟业务窗口名，不包含随帧变化的内容。
    /// @return Begin 完成后由 ImGui 计算的可用内容边界。
    Bounds pane(MMM::UI::MobileDockPanel panel, const char* name)
    {
        // 共享业务视图可能先提供桌面默认落点；移动策略必须覆盖该条件而非沿用它。
        // 自定义合并、浮动和上下分割仍应在这个预设存在时保持稳定。
        ImGui::SetNextWindowDockID(layout.node(panel), ImGuiCond_Always);
        layout.target(panel, name);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            MMM::UI::MobileDockLayout::tabPadding(1));
        ImGui::Begin(name,
                     nullptr,
                     m_persistWindows ? 0 : ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar();
        if ( layout.tabCloseRequested(
                 "Close",
                 1,
                 std::string_view(name) != "Main" || mainClosable,
                 true) )
            ++closeRequests;
        // 比较实际节点标签边界，避免三个窗口同时错位仍被等高断言放过。
        const auto* node = ImGui::GetCurrentWindow()->DockNode;
        if ( node && node->TabBar )
            titleFits &= ImGui::GetCurrentWindow()->InnerRect.Min.y + .5f >=
                         node->TabBar->BarRect.Max.y;
        // 不绘制额外控件，三者的内容高度只由标签栏和内边距决定。
        const Bounds result{ ImGui::GetCursorScreenPos(),
                             ImGui::GetContentRegionAvail().y };
        ImGui::End();
        return result;
    }

    /// @brief 推进一帧，辅助分区未显示时不提交其业务窗口。
    /// @details 宿主高度可变化，用于验证调整尺寸后没有独立窗口残留旧高度。
    /// @param timing 是否提交时间线业务窗口。
    /// @param preview 是否提交预览业务窗口。
    /// @param height 宿主当前高度，模拟可用显示区域改变。
    void frame(bool timing = true, bool preview = true, float height = 600)
    {
        ImGui::NewFrame();
        const auto padding = ImGui::GetStyle().FramePadding;
        // 每帧捕获进入时的值；前一帧泄漏不能被下一帧取样掩盖。
        const auto separator = ImGui::GetStyle().DockingSeparatorSize;
        layout.render({ 0, 0 },
                      { hostWidth, height },
                      1,
                      timing,
                      preview,
                      m_workbenchRequested && layout.isMainCanvasSelected());
        // 既检查平衡恢复，也检查自定义美化宽度没有被固定触摸尺寸替换。
        styleIntact &= ImGui::GetStyle().FramePadding.x == padding.x &&
                       ImGui::GetStyle().FramePadding.y == padding.y &&
                       ImGui::GetStyle().DockingSeparatorSize == separator;
        bounds[0] = pane(MMM::UI::MobileDockPanel::Main, "Main");
        if ( lateFocus ) ImGui::SetNextWindowFocus();
        pane(MMM::UI::MobileDockPanel::Main, "Other");
        if ( m_canvasPage )
            pane(MMM::UI::MobileDockPanel::Main, "Chart###Canvas_workbench");
        if ( longCaption )
            pane(MMM::UI::MobileDockPanel::Main, longTitle.c_str());
        if ( analysisViews ) {
            // 复用生产标题分类，不能在测试里按显示语言另写窗口匹配规则。
            for ( const char* title : { "Wave###AudioWaveformViewGlobal",
                                        "频谱###AudioSpectrumViewGlobal" } )
                if ( MMM::UI::MobileDockLayout::isAudioAnalysisWindow(title) )
                    pane(MMM::UI::MobileDockPanel::Main, title);
        }
        if ( timing )
            bounds[1] =
                pane(MMM::UI::MobileDockPanel::Timing, "Timing###Timeline");
        if ( preview )
            bounds[2] = pane(MMM::UI::MobileDockPanel::Preview,
                             "Preview###PreviewWindow");
        if ( layout.node(MMM::UI::MobileDockPanel::Workbench) )
            pane(MMM::UI::MobileDockPanel::Workbench,
                 "Tools###MobileWorkbench");
        layout.finishFrame();
        ImGui::Render();
    }

    /// @brief 让原生停靠完成首次 Begin，不将过渡帧作为最终布局断言。
    /// @details 这里推进的是有限测试帧，不通过 sleep 等待真实时间。
    /// 所有断言都在停靠树稳定后读取，避免窗口首次出现的默认位置干扰。
    void settle(bool timing = true, bool preview = true, float height = 600)
    {
        for ( int frameIndex = 0; frameIndex < 5; ++frameIndex )
            frame(timing, preview, height);
    }

    /// @brief 通过真实指针事件拖动原生分隔条，不直接写节点尺寸。
    /// @param x 分隔条中心坐标，读取当前叶节点边界后计算。
    /// @param dx 水平位移，纵坐标固定在宿主中间。
    /// @details 每次输入之间推进一帧，模拟按下、持续拖动和释放。
    /// 使用触屏来源验证移动端路径，不依赖桌面鼠标悬浮提示。
    void drag(float x, float dx)
    {
        auto& io = ImGui::GetIO();
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        // 共享布局无需键盘修饰键，单根触指即可启动分区宽度调整。
        io.AddMousePosEvent(x, 300);
        // 先建立触点所在宿主的悬停状态，按下帧才能命中原生分隔条。
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMousePosEvent(x + dx, 300);
        frame();
        io.AddMouseButtonEvent(0, false);
        // 松开后继续普通帧，验证布局不会被默认比例立即覆盖回去。
        frame();
        settle();
    }

    /// @brief 从实际标签矩形发起触屏长按，可选移动用于验证手势取消。
    /// @details 推进有限 UI 帧模拟时间，不等待真实时钟或写入私有手势状态。
    /// @param name 已由夹具创建的稳定窗口名，不包含动态标题或翻译。
    /// @param move 按下后平移一次并继续保持，验证移回停留不会重新计时。
    /// @param frames 按住期间推进的帧数，默认超过长按阈值。
    /// @param revealAttempt 限制滚出屏幕标签的准备次数，失败不得无限递归。
    void holdTab(const char* name, bool move = false, int frames = 40,
                 int revealAttempt = 0)
    {
        // 窗口由本用例前面的 settle 提交，测试只借用当前帧有效的节点。
        auto* window = ImGui::FindWindowByName(name);
        auto* bar    = window->DockNode->TabBar;
        // 通过稳定窗口 TabId 找原生标签，不能把窗口 ID 当作 TabId。
        const auto* tab = ImGui::TabBarFindTabByID(bar, window->TabId);
        // 滚动动画可能来自上一轮标签切换，输入位置必须跟随当前可见偏移。
        const ImVec2 position{ bar->BarRect.Min.x + tab->Offset -
                                   bar->ScrollingAnim,
                               bar->BarRect.Min.y };
        // 宿主和标签已完成布局，后续只经官方输入队列模拟触摸。
        auto& io = ImGui::GetIO();
        // 恢复场景的窄辅助区可能裁剪标签，只能按到实际露出的部分。
        const float left = std::max(position.x, bar->ScrollingRectMinX);
        const float right =
            std::min(position.x + tab->Width, bar->ScrollingRectMaxX);
        // 整个标签已滚出可见区时先通过原生聚焦让其滚入，不能向负宽命中区点击。
        // 这只准备可见起点，后续长按、拖动与关闭仍使用真实触屏事件。
        if ( right <= left ) {
            if ( revealAttempt >= 2 ) {
                XERROR("Unable to reveal tab {}", name);
                return;
            }
            ImGui::SetWindowFocus(name);
            settle();
            holdTab(name, move, frames, revealAttempt + 1);
            return;
        }
        const ImVec2 point{ (left + right) * .5f,
                            (position.y + bar->BarRect.Max.y) * .5f };
        // 来源明确为触屏，不能让默认鼠标事件意外绕过移动端识别规则。
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(point.x, point.y);
        // 先使原生停靠宿主观察到位置，再在下一帧开始按下。
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        // 移动只发生一次，此后继续停留，验证取消后不会重新开始计时。
        if ( move ) io.AddMousePosEvent(point.x + 20, point.y);
        // 用 DeltaTime 累积长按时间，测试不调用 sleep 或修改生产状态。
        for ( int index = 0; index < frames; ++index ) frame();
        // 释放与后续稳定帧覆盖菜单首次出现后的输入保护。
        io.AddMouseButtonEvent(0, false);
        settle();
    }

    /// @brief 点击真实弹层的关闭项，测试返回值而不是直接调用业务关闭接口。
    /// @return 是否确实存在可点击菜单，避免失败后访问空窗口。
    /// @details 返回值只说明发出了触摸序列，是否成功关闭必须由计数单独断言。
    /// 菜单是否出现与关闭计数分别检查，不能把发出触摸序列直接视为关闭成功。
    bool clickCloseMenu()
    {
        // 菜单不存在时返回失败，不能用空指针继续制造一次假点击。
        auto& context = *ImGui::GetCurrentContext();
        if ( context.OpenPopupStack.empty() ||
             !context.OpenPopupStack.back().Window )
            return false;
        // 读取经过视口避让后的实际位置，不假设菜单始终在标签正下方。
        const auto* popup = context.OpenPopupStack.back().Window;
        // 唯一关闭项有 48 像素命中高度，选择中心避开窗口内边距。
        const ImVec2 point{ popup->Pos.x + popup->Size.x * .5f,
                            popup->Pos.y + popup->WindowPadding.y + 24 };
        auto&        io = ImGui::GetIO();
        // 单独的新按下和抬起才表示选择菜单项，不能复用发起菜单的长按。
        io.AddMousePosEvent(point.x, point.y);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        // 连续普通帧还检查关闭意图不会在菜单隐藏后反复发送。
        settle();
        return true;
    }
};
}  // namespace

/// @brief 验证单窗口标签、实际内容等高、宿主缩放和分区显隐。
/// @details 所有输入来自 CPU 上下文，不读取用户 ini 或实际窗口状态。
/// 触摸拖动后捕获布局，反复销毁布局对象再恢复，检查是否累积像素偏移。
/// 各次恢复都使用新对象，不复用私有缓存或旧节点观察指针。
/// 宿主的现存原生窗口仍参与新树构建，覆盖历史 ini 已提供窗口身份的场景。
int main()
{
    using namespace MMM::UI;
    const bool freshDocked = freshCanvasDocked();
    ImGui::CreateContext();
    // 上下文由本测试独占；禁用 ini，避免本机历史布局影响叶节点身份。
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = { 1000, 700 };
    io.DeltaTime   = 1.0f / 60;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // CPU 字体图集满足 NewFrame 前置条件，像素不会上传到图形设备。
    unsigned char* pixels = nullptr;
    int            width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    Fixture fixture;
    int     failures = 0;
    auto    check    = [&](bool valid, const char* message) {
        // 聚合失败后继续检查其余边界，日志可同时指出标签和高度回归。
        // 不在断言中销毁上下文，避免后续检查访问失效的节点。
        if ( !valid ) {
            ++failures;
            XERROR("Mobile dock: {}", message);
        }
    };
    check(freshDocked,
          "fresh logo canvas fills the center after repeated targeting");
    // 真实 UI 注册顺序允许画布先于移动宿主出现；下一帧仍须正确建立缺省停靠。
    // 不能在根 ID 尚未创建时就永久消费首次定位请求。
    ImGui::NewFrame();
    fixture.pane(MobileDockPanel::Main, "Main");
    ImGui::Render();
    fixture.settle();
    // 使用不同于默认值的分隔条尺寸，确保渲染尊重外部美化样式。
    ImGui::GetStyle().DockingSeparatorSize = 6;
    // 后续两次真实触摸拖动也沿用窄分隔条，验证外观与命中区独立。
    fixture.settle();
    check(fixture.styleIntact,
          "touch styles remain scoped and preserve separator width");
    // 首次显示同时覆盖两次水平分割；不能仅测试主画布的单叶情况。
    // 三个窗口均真实 Begin，原生停靠才会建立对应标签对象。
    // 三个叶节点必须独立，不能把辅助窗口误送进主画布的标签组。
    const auto center  = fixture.layout.center();
    const auto timing  = fixture.layout.node(MobileDockPanel::Timing);
    const auto preview = fixture.layout.node(MobileDockPanel::Preview);
    check(center && timing && preview && center != timing &&
              center != preview && timing != preview,
          "three independent leaves");
    // 检查真实节点边界，不能只验证三个窗口均存在而漏掉左右顺序回归。
    // 显隐与重建后节点身份会改变，每次检查重新查询，不复用初始叶指针。
    const auto checkLeftLayout = [&]() {
        float right = 0;
        // 显隐后的零身份不代表一个空白占位区，直接检查仍存在的实际叶节点。
        // 同一顺序同时覆盖独立预览、独立时间线和两者并排三种布局。
        for ( const auto panel : { MobileDockPanel::Preview,
                                   MobileDockPanel::Main,
                                   MobileDockPanel::Timing,
                                   MobileDockPanel::Workbench } ) {
            const auto* leaf =
                ImGui::DockBuilderGetNode(fixture.layout.node(panel));
            if ( !leaf ) continue;
            check(leaf->Pos.x >= right - .5f,
                  "panel order is preview, main, timing, workbench");
            right = leaf->Pos.x + leaf->Size.x;
        }
        // 最后一个可见分区铺到宿主右边，不额外创建空白占位叶节点。
        // 工具栏由外壳提前扣除；此处只检查传入宿主，不能把工具栏宽度重复扣除。
        check(std::abs(right - std::floor(fixture.hostWidth)) < .5f,
              "last visible panel reaches the right edge of the dock host");
    };
    checkLeftLayout();
    // 单窗口的辅助节点也保留标签，不使用 NoTitleBar 的浮窗模拟分区。
    for ( const auto id : { center, timing, preview } ) {
        // 辅助区只有一个标签也必须存在，不能依赖多窗口碰巧显示标题。
        const auto* node = ImGui::DockBuilderGetNode(id);
        check(node && node->TabBar && !node->IsHiddenTabBar() &&
                  !node->IsNoTabBar(),
              "all panels retain tabs");
    }
    // 比较 Begin 后的内容起点与可用高度，避免只比较外框而遗漏标签高度差。
    for ( int index = 1; index < 3; ++index ) {
        // 使用亚像素容差兼容节点尺寸取整，但不允许少一整行标签高度。
        // 这里比较的是内容而不是窗口尺寸，能发现辅助区没有标题栏的问题。
        check(std::abs(fixture.bounds[index].origin.y -
                       fixture.bounds[0].origin.y) < .5f,
              "content starts align");
        check(std::abs(fixture.bounds[index].height -
                       fixture.bounds[0].height) < .5f,
              "content heights align");
    }
    fixture.settle(true, true, 480);
    // 改变宿主高度后重新采样实际内容，排除只在初始分辨率上等高。
    check(
        std::abs(fixture.bounds[1].height - fixture.bounds[0].height) < .5f &&
            std::abs(fixture.bounds[2].height - fixture.bounds[0].height) < .5f,
        "resize preserves equal height");
    // 显式隐藏只移除对应叶节点，保留另一个辅助分区和主画布。
    // 实际拖动值从节点捕获；后续重建必须恢复这些值而非代码默认比例。
    fixture.settle();
    const auto* timingNode = ImGui::DockBuilderGetNode(timing);
    // 节点指针仅在下一次拓扑变化前有效，这里只做连续输入不切换显隐。
    const float timingBefore = timingNode->Size.x;
    fixture.drag(
        timingNode->Pos.x - ImGui::GetStyle().DockingSeparatorSize * .5f, -60);
    check(ImGui::DockBuilderGetNode(timing)->Size.x > timingBefore + 30,
          "touch resizes main/timing separator");
    // 左预览取右边界正向拖动、右时间线取左边界负向拖动，不能整体镜像两者。
    // 第二次重新读取几何，因为上一次拖动可能已经改变相邻节点的位置。
    // 第二条分隔线属于预览外层分割，同样必须可拖动且保留两侧标签。
    const auto* previewNode = ImGui::DockBuilderGetNode(preview);
    // 以第二次拖动开始时的实际尺寸为基准，避免依赖初始默认比例。
    const float previewBefore = previewNode->Size.x;
    fixture.drag(previewNode->Pos.x + previewNode->Size.x +
                     ImGui::GetStyle().DockingSeparatorSize * .5f,
                 50);
    check(ImGui::DockBuilderGetNode(preview)->Size.x > previewBefore + 25,
          "touch resizes preview/main separator");
    checkLeftLayout();
    // 连续拖动不允许改变垂直内容边界；宽度调整不能变回独立浮窗。
    check(std::abs(fixture.bounds[2].height - fixture.bounds[0].height) < .5f,
          "resizing widths retains equal content heights");
    float   savedTiming = 0, savedPreview = 0;
    ImGuiID savedTab = 0;
    fixture.layout.capture(savedTiming, savedPreview, savedTab);
    // 捕获后还会执行隐藏与重建，保存值必须独立于节点指针的生命周期。
    fixture.settle(true, false);
    // 隐藏后继续推进真实帧，验证结果不依赖已失效节点的残留指针。
    check(fixture.layout.node(MobileDockPanel::Preview) == 0 &&
              fixture.layout.node(MobileDockPanel::Timing) != 0,
          "hide one auxiliary panel");
    checkLeftLayout();
    // 两个辅助窗口既可同时打开，也可分别打开，预览始终在主区左侧。
    // 这次拓扑转换还覆盖原先隐藏的预览再次出现时使用缓存宽度。
    fixture.settle(false, true);
    check(fixture.layout.node(MobileDockPanel::Timing) == 0 &&
              fixture.layout.node(MobileDockPanel::Preview) != 0,
          "preview can remain on the left without timing");
    checkLeftLayout();
    fixture.settle(false, false);
    // 最终回到单区时，清除辅助 ID，防止下一次打开误用旧叶节点。
    check(fixture.titleFits, "content cannot overlap dock tabs");
    check(fixture.layout.center() != 0 &&
              fixture.layout.node(MobileDockPanel::Timing) == 0,
          "main remains when auxiliaries close");
    checkLeftLayout();
    // 新布局对象模拟外壳重建，旧节点仍存在也不能覆盖已读取的客户端值。
    fixture.layout = MobileDockLayout{};
    // 清空内部 ID 与缓存，只给新对象公开的恢复接口提供已保存标量。
    fixture.layout.restore(savedTiming, savedPreview, savedTab);
    fixture.settle();
    float   restoredTiming = 0, restoredPreview = 0;
    ImGuiID restoredTab = 0;
    fixture.layout.capture(restoredTiming, restoredPreview, restoredTab);
    // 允许像素取整产生小比例差，不能允许重新采用默认宽度的明显跳变。
    // 标签身份也必须恢复，防止尺寸正确但主区跳回另一页签。
    check(std::abs(restoredTiming - savedTiming) < .01f &&
              std::abs(restoredPreview - savedPreview) < .01f &&
              restoredTab == savedTab,
          "restart restores resized panes and selected tab");
    checkLeftLayout();
    // 多轮恢复必须仍落在同一像素宽度，不能每次扣除一条缝隙而逐渐漂移。
    // 从旧节点重新创建对象时，也必须重新套用当前的左右布局约定。
    // 保存字段只表达宽度和活动标签，不应隐含窗口仍在旧版右侧的位置。
    // 先把初次捕获值固定作为基线，循环里不以刚漂移的值覆盖预期。
    const float baselineTiming  = restoredTiming,
                baselinePreview = restoredPreview;
    for ( int cycle = 0; cycle < 12; ++cycle ) {
        fixture.layout = MobileDockLayout{};
        fixture.layout.restore(restoredTiming, restoredPreview, restoredTab);
        fixture.settle();
        fixture.layout.capture(restoredTiming, restoredPreview, restoredTab);
    }
    check(std::abs(restoredTiming - baselineTiming) < .001f &&
              std::abs(restoredPreview - baselinePreview) < .001f,
          "repeated restores do not accumulate pixel drift");
    checkLeftLayout();
    // 恢复非默认的第一个标签，并模拟另一窗口在随后每帧请求焦点。
    // 保存的是窗口身份，不能把原生 TabId 与窗口 ID 混用而碰巧回到默认页。
    const auto mainWindow = ImGui::FindWindowByName("Main")->ID;
    fixture.layout        = MobileDockLayout{};
    fixture.layout.restore(restoredTiming, restoredPreview, mainWindow);
    fixture.lateFocus = true;
    // 连续请求模拟异步惰性创建，不允许靠猜测固定等待帧数恢复选中页。
    // 此时没有用户按下，恢复保护才可覆盖初始化请求，不能改写真实交互。
    fixture.settle();
    fixture.layout.capture(restoredTiming, restoredPreview, restoredTab);
    check(restoredTab == mainWindow,
          "late windows cannot override restored active tab");
    // 一次真实按下必须终止启动恢复保护，后续导航不能被旧活动标签锁住。
    // 让另一窗口继续请求焦点，检验释放保护后的普通聚焦确实重新生效。
    io.AddMousePosEvent(850, 650);
    // 触点位于宿主外，单独验证恢复生命周期边界，不依赖标签命中结果。
    io.AddMouseButtonEvent(0, true);
    fixture.frame();
    io.AddMouseButtonEvent(0, false);
    fixture.settle();
    fixture.layout.capture(restoredTiming, restoredPreview, restoredTab);
    check(restoredTab == ImGui::FindWindowByName("Other")->ID,
          "first interaction releases startup focus protection");
    // 长按测试使用普通交互焦点，不让上面的恢复场景主动抢选标签。
    // 仍复用上面经多次恢复和缩窄的停靠树，覆盖实际标签裁剪后的命中区。
    fixture.lateFocus = false;
    fixture.settle();
    // 查询任意层弹层，避免只在当前测试窗口 ID 栈下查找而漏掉菜单。
    const auto menuOpen = [&]() {
        return ImGui::IsPopupOpen(
            nullptr,
            ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    };
    // 短按只切换页面；在标签内移动后持续停留也不能变成关闭长按。
    // 三帧显著短于手势阈值，仍经过完整按下和释放序列。
    fixture.holdTab("Other", false, 3);
    // 此处不检查选中项名称，选中行为归原生 TabBar，菜单只负责关闭入口。
    check(!menuOpen(), "short tap does not open tab menu");
    // 后续保持四十帧，排除只是计时不足而没有出现菜单的伪通过。
    fixture.holdTab("Other", true);
    check(!menuOpen(), "moving touch cancels long press");
    // 普通可关闭标签不附带业务销毁，用计数器观察唯一关闭意图。
    fixture.holdTab("Other");
    check(menuOpen() && fixture.closeRequests == 0,
          "long press opens menu without closing on release");
    // 菜单出现和关闭是两个独立动作，第一次松手不能增加计数。
    check(fixture.clickCloseMenu() && fixture.closeRequests == 1,
          "menu closes only after explicit selection");
    // 最后主画布没有关闭项，当前没有其他操作时也不显示空菜单。
    fixture.holdTab("Main");
    check(!menuOpen() && fixture.closeRequests == 1,
          "last main canvas exposes no close item or empty menu");
    // 从可关闭标签再次打开菜单，单独检验不选择任何操作时的外部取消。
    fixture.holdTab("Other");
    check(menuOpen(), "closable tab menu can reopen");
    // 点击菜单外部取消，使用完整触屏序列而不依赖键盘导航配置。
    io.AddMousePosEvent(850, 650);
    fixture.frame();
    io.AddMouseButtonEvent(0, true);
    fixture.frame();
    io.AddMouseButtonEvent(0, false);
    fixture.settle();
    // 取消既不能留下弹层，也不能让独立的关闭释放保护永久阻塞画布。
    check(!menuOpen() && !fixture.layout.tabMenuOwnsPointer(),
          "outside tap dismisses menu and releases input protection");
    // 辅助区只有一个标签仍可长按关闭，不能按叶节点标签数误禁用。
    for ( const char* name :
          { "Timing###Timeline", "Preview###PreviewWindow" } ) {
        // 每个辅助标签单独检查增量，避免另一个标签误关闭时抵消结果。
        const int before = fixture.closeRequests;
        fixture.holdTab(name);
        const bool opened  = menuOpen();
        const bool clicked = opened && fixture.clickCloseMenu();
        if ( !opened || !clicked || fixture.closeRequests != before + 1 )
            XERROR("Auxiliary menu {}: opened={}, clicked={}, count={}/{}",
                   name,
                   opened,
                   clicked,
                   fixture.closeRequests,
                   before);
        check(opened && clicked && fixture.closeRequests == before + 1,
              "auxiliary tab supports explicit menu close");
    }
    // 有额外主画布时恢复同一菜单操作，关闭权限与标签数量解耦。
    // 权限从受保护切到可关闭后无需重建布局，原窗口身份和菜单 ID 不变。
    // 这里只改变调用方给出的关闭权限，不直接改被测菜单内部状态。
    fixture.mainClosable = true;
    const int before     = fixture.closeRequests;
    fixture.holdTab("Main");
    check(menuOpen() && fixture.clickCloseMenu() &&
              fixture.closeRequests == before + 1,
          "additional main canvas can close");
    // 长标题仍保留原生窗口名与 ID，宽度约束只影响标签外观。
    // 新窗口与短标题同处原生节点，验证上限不是靠只有一个标签时的巧合。
    fixture.longCaption = true;
    fixture.settle();
    // 从原生 TabBar 读取真实宽度，不重复计算被测代码的测量公式。
    // 该断言同样覆盖已存在节点中首次加入长标题的常见路径。
    auto* longWindow = ImGui::FindWindowByName(fixture.longTitle.c_str());
    auto* longTab    = ImGui::TabBarFindTabByID(longWindow->DockNode->TabBar,
                                                longWindow->TabId);
    check(longTab && longTab->Width <= 240.5f,
          "long caption has a bounded native tab width");
    // 视觉裁剪不能破坏隐藏的完整业务标题，后续滚动需要仍能访问其末尾。
    check(std::string_view(longWindow->Name) == fixture.longTitle,
          "caption clipping does not truncate the window name");
    // 保存标量身份；下一次 Begin 可能重分配窗口名缓冲区，不能保存其字符串视图。
    const auto titleId = longWindow->ID;
    // 重命名、脏标记和非 ASCII 谱名必须保持同一标签，不创建第二个业务窗口。
    fixture.longTitle =
        "Editor* [7K] 更改后的超长谱面标题与协作状态###LongCanvas";
    fixture.settle();
    // 修改标题后重新取得窗口，只以 ### 后缀比较身份稳定性。
    longWindow = ImGui::FindWindowByName(fixture.longTitle.c_str());
    longTab    = ImGui::TabBarFindTabByID(longWindow->DockNode->TabBar,
                                          longWindow->TabId);
    check(longWindow->ID == titleId && longTab && longTab->Width <= 240.5f,
          "renaming preserves identity and width limit");
    // 非 ASCII 标题走同一字体测量路径，不能按 UTF-8 字节数截短名称。
    // 文字透明仅限原生绘制阶段，不能污染下一帧的窗口主题缓存。
    // 原主题色的 Alpha 必须可见，否则下一帧原生页签会持续丢失文字。
    check((longWindow->DockStyle.Colors[ImGuiWindowDockStyleCol_Text] &
           IM_COL32_A_MASK) != 0,
          "caption rendering restores the native text color");
    // 自动滚动只改变文字，缩窄后的真实标签仍须能长按并明确关闭。
    const int beforeCaptionClose = fixture.closeRequests;
    fixture.holdTab(fixture.longTitle.c_str());
    check(menuOpen() && fixture.clickCloseMenu() &&
              fixture.closeRequests == beforeCaptionClose + 1,
          "bounded caption preserves native long press interaction");
    // 分析页必须真正出现在中央标签栏，不能仅确认分类函数返回了 true。
    fixture.analysisViews = true;
    // 原有谱面和辅助分区仍存在，分析页不能替换它们取得空间。
    // 先推进原生停靠结算，首次 Begin 的浮动位置不算成功。
    fixture.settle();
    for ( const char* title : { "Wave###AudioWaveformViewGlobal",
                                "频谱###AudioSpectrumViewGlobal" } ) {
        // DockId 相同不足以证明可切换，还须找到真实原生 TabId。
        // 两种语言同时提交，验证路由不依赖显示名称。
        const auto* analysis = ImGui::FindWindowByName(title);
        check(analysis && analysis->DockId == fixture.layout.center() &&
                  analysis->DockNode && analysis->DockNode->TabBar &&
                  ImGui::TabBarFindTabByID(analysis->DockNode->TabBar,
                                           analysis->TabId),
              "audio analysis is a real central tab");
    }
    // 音频页沿用中央标签长按关闭，独立于最后主画布的保护策略。
    const int beforeAnalysisClose = fixture.closeRequests;
    // 复用真实长按与菜单点击，不直接修改关闭请求计数。
    // 分析页没有最后画布保护，必须提供可操作关闭入口。
    // 此处只返回关闭意图，生产注销仍由下一帧公共生命周期执行。
    fixture.holdTab("Wave###AudioWaveformViewGlobal");
    check(menuOpen() && fixture.clickCloseMenu() &&
              fixture.closeRequests == beforeAnalysisClose + 1,
          "audio analysis tab can close through touch menu");
    // 工作台只由主区选中的真实画布打开，后台会话存在不能让欢迎和设置占着它。
    // 独立工具标签抢焦点不等于主区切走，因此也不能将整个工作台隐藏。
    // 新窗口先经历真实停靠，再请求选中，不改写原生私有选中字段。
    // 显示标题故意不同，识别只依赖 ### 后面的画布身份。
    // 页签切换注入真实短按，启动恢复保护必须由用户交互而非外部聚焦命令解除。
    // 这样同时覆盖原生点击、显隐重建和焦点恢复的完整顺序。
    fixture.m_canvasPage = fixture.m_workbenchRequested = true;
    fixture.settle();
    fixture.holdTab("Chart###Canvas_workbench", false, 3);
    fixture.settle();
    check(fixture.layout.isMainCanvasSelected() &&
              fixture.layout.node(MobileDockPanel::Workbench),
          "selected real canvas shows workbench");
    checkLeftLayout();
    // 工作台拖动来自左边界，不把相邻时间线整块内容当作分隔条。
    // 最终宽度读回真实节点，排除只绘制握柄但不能调整的情况。
    const auto  workbenchId = fixture.layout.node(MobileDockPanel::Workbench);
    const auto* workbench   = ImGui::DockBuilderGetNode(workbenchId);
    // 缺失节点已经由上面的契约报错；继续解引用只会掩盖真正的显隐故障。
    if ( !workbench ) {
        ImGui::DestroyContext();
        return 1;
    }
    const float workbenchBefore = workbench->Size.x;
    fixture.drag(
        workbench->Pos.x - ImGui::GetStyle().DockingSeparatorSize * .5f, -30);
    check(ImGui::DockBuilderGetNode(workbenchId)->Size.x > workbenchBefore + 15,
          "touch resizes timing/workbench separator");
    // 捕获拖动结束后的实际比例，后续显隐必须复用此值。
    // 节点观察指针不得跨越下面的拓扑删除与重建。
    float savedWorkbench = 0;
    fixture.layout.capture(
        restoredTiming, restoredPreview, restoredTab, &savedWorkbench);
    // 工具按钮会取得焦点，但主区选中的画布保持不变。
    // 因此不能通过全局聚焦窗口判断工作台是否仍应显示。
    // 工作台与其他窗口一样保留真实原生标签，不能仅绘制一行不可拖动的装饰文字。
    // 标签参与停靠和长按关闭，内容从标签栏下方开始且不覆盖命中区域。
    const auto* toolWindow = ImGui::FindWindowByName("Tools###MobileWorkbench");
    check(toolWindow && toolWindow->DockNode &&
              !toolWindow->DockNode->IsNoTabBar() &&
              toolWindow->DockNode->TabBar,
          "workbench retains a draggable tab strip");
    // 触摸工具正文取得焦点，不能让工具页焦点误清除当前文档上下文。
    auto& toolIo = ImGui::GetIO();
    toolIo.AddMousePosEvent(toolWindow->Pos.x + 30, toolWindow->Pos.y + 80);
    toolIo.AddMouseButtonEvent(0, true);
    fixture.frame();
    toolIo.AddMouseButtonEvent(0, false);
    fixture.settle();
    check(fixture.layout.node(MobileDockPanel::Workbench) != 0,
          "workbench focus keeps the selected canvas context");
    // 旧窗口模拟欢迎与设置，后台画布仍存在也须释放工具区。
    // 断言节点消失，而非仅隐藏正文，避免空白栏继续挤占画布。
    fixture.holdTab("Other", false, 3);
    fixture.settle();
    check(!fixture.layout.isMainCanvasSelected() &&
              !fixture.layout.node(MobileDockPanel::Workbench),
          "non-canvas page releases workbench space");
    // 重新显示不能恢复默认宽度；同一对象先捕获旧叶再重建，隐藏期间不丢失比例。
    fixture.holdTab("Chart###Canvas_workbench", false, 3);
    fixture.settle();
    // 切回同一画布身份，不重建业务对象，也不重写保存宽度。
    // 再次读回公开捕获接口，检查恢复完成后的最终几何。
    float afterWorkbench = 0;
    fixture.layout.capture(
        restoredTiming, restoredPreview, restoredTab, &afterWorkbench);
    check(std::abs(afterWorkbench - savedWorkbench) < .002f,
          "returning to canvas preserves resized workbench width");
    checkLeftLayout();
    // 显隐循环最后再检查四栏顺序，恢复工作台不能改变时间线的位置。
    // 欢迎或设置文档独立分屏且始终可见，工作台仍应属于最近聚焦的画布。
    // 这覆盖按登记顺序选中旁边文档、导致工作台每帧显隐翻转的真实回归。
    ImGuiID sideDocument{}, remainingDocuments{};
    ImGui::DockBuilderSplitNode(fixture.layout.center(),
                                ImGuiDir_Left,
                                .25f,
                                &sideDocument,
                                &remainingDocuments);
    ImGui::DockBuilderDockWindow("Other", sideDocument);
    ImGui::SetWindowFocus("Chart###Canvas_workbench");
    fixture.settle();
    // 工作台也可与主画布合并；选中工具标签不能让自己的显示条件反复翻转。
    // 原生停靠 API 模拟用户已完成的落点，后续普通帧不得把它拉回默认右侧。
    // 合并前读取真实画布节点，避免复制生产默认分栏比例掩盖错误。
    // 窗口数量并非关闭权限的依据，最后主画布和工具页仍各自检查。
    const auto canvasDock =
        ImGui::FindWindowByName("Chart###Canvas_workbench")->DockId;
    ImGui::DockBuilderDockWindow("Tools###MobileWorkbench", canvasDock);
    ImGui::SetWindowFocus("Tools###MobileWorkbench");
    fixture.settle();
    check(ImGui::FindWindowByName("Tools###MobileWorkbench")->DockId ==
                  canvasDock &&
              fixture.layout.isMainCanvasSelected(),
          "merged workbench keeps canvas context and docking position");
    // 选中工作台时应保存其文档身份，不能在重启后等待未创建的工具页才能展开工具页。
    // 工具标签的布局由原生 ini 保存，文档上下文与工具焦点分开记录。
    ImGuiID toolDocument{};
    fixture.layout.capture(restoredTiming, restoredPreview, toolDocument);
    check(
        toolDocument == ImGui::FindWindowByName("Chart###Canvas_workbench")->ID,
        "selected workbench saves its document context");
    // 关闭菜单必须来自新的触摸长按，再由独立轻点确认。
    // 计数是消费业务意图的观测值，不直接修改被测内部状态。
    const int beforeToolClose = fixture.closeRequests;
    fixture.holdTab("Tools###MobileWorkbench");
    check(menuOpen() && fixture.clickCloseMenu() &&
              fixture.closeRequests == beforeToolClose + 1,
          "workbench tab supports touch close menu");
    // 合并后的预览仍可隐藏和重开，主区标签和其它节点不得被整树重建覆盖。
    // 欢迎或设置文档成为当前页后，工具区应正常释放。
    // 此处设置原生焦点以建立新场景，后续只检验拖动结果是否被回写覆盖。
    ImGui::SetWindowFocus("Other");
    fixture.settle();
    const auto documentDock = ImGui::FindWindowByName("Other")->DockId;
    ImGui::DockBuilderDockWindow("Preview###PreviewWindow", documentDock);
    fixture.settle();
    check(ImGui::FindWindowByName("Preview###PreviewWindow")->DockId ==
              documentDock,
          "user merged preview stays with documents");
    // 隐藏辅助窗口不应清空整个节点，因为节点还承载可见的主文档。
    // 再次显示时沿用原生 DockId，不能按旧分类强制切回最左侧。
    fixture.settle(true, false);
    fixture.settle();
    check(ImGui::FindWindowByName("Preview###PreviewWindow")->DockId ==
              documentDock,
          "hide and reopen preserve custom merged preview");
    // 上下分割补充旧水平默认布局没有覆盖的拓扑；只调整目标文档叶。
    // 记录最终原生 ID，不能用“看起来差不多”的比例判断实际落点。
    // 两个输出 ID 只在稳定帧后比较，不保留 DockNode 观察指针跨越分割。
    // 同时检查时间线与主文档，排除两者都被错误送回默认树的情况。
    ImGuiID lower{}, upper{};
    ImGui::DockBuilderSplitNode(
        documentDock, ImGuiDir_Down, .35f, &lower, &upper);
    ImGui::DockBuilderDockWindow("Timing###Timeline", lower);
    fixture.settle();
    check(ImGui::FindWindowByName("Timing###Timeline")->DockId == lower &&
              ImGui::FindWindowByName("Other")->DockId == upper,
          "user vertical split survives ordinary frames");
    // 内存 ini 完整往返覆盖真正的全新 ImGui 上下文，排除复用旧节点的假恢复。
    // 打开窗口设置后只序列化到内存，明确禁用 IniFilename，避免改动本机配置。
    // 原先几何夹具故意禁用保存；本场景显式启用后先推进正常帧。
    // 这样生成的窗口条目来自正式 Begin，而不是测试手工拼接 ini 文本。
    fixture.m_persistWindows = true;
    fixture.settle();
    std::size_t       iniSize{};
    const char*       iniText = ImGui::SaveIniSettingsToMemory(&iniSize);
    const std::string savedIni(iniText, iniSize);
    // 全新上下文没有旧运行期窗口或节点，因此能检验真正的启动恢复路径。
    // 保存字符串独立拥有内存，不持有已销毁上下文的缓冲区。
    ImGui::DestroyContext();
    ImGui::CreateContext();
    auto& restoredIo       = ImGui::GetIO();
    restoredIo.IniFilename = nullptr;
    restoredIo.DisplaySize = { 1000, 700 };
    restoredIo.DeltaTime   = 1.0f / 60;
    // 不启动平台视口或 GPU 后端，布局恢复应仅依赖原生设置和字体度量。
    // 新字体图集必须在第一次 NewFrame
    // 前构建，避免把前置条件失败误判为布局问题。
    restoredIo.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    restoredIo.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    // 加载发生在首个移动宿主提交之前，顺序与生产环境一致。
    // 若外壳首帧仍无条件重建整树，下面保存的上下分割 ID 将无法保留。
    ImGui::LoadIniSettingsFromMemory(savedIni.data(), savedIni.size());
    // 新布局对象同样不保留旧绑定；只允许读取正式的原生设置和缺省比例。
    // 选中标签设置为零，防止强制恢复焦点掩盖窗口归属错误。
    fixture.layout = MobileDockLayout{};
    fixture.layout.restore(savedTiming, savedPreview, 0);
    fixture.settle();
    check(
        ImGui::FindWindowByName("Timing###Timeline")->DockId == lower &&
            ImGui::FindWindowByName("Other")->DockId == upper &&
            ImGui::FindWindowByName("Preview###PreviewWindow")->DockId == upper,
        "fresh context restores custom native topology");
    ImGui::DestroyContext();
    // 所有窗口、字体和节点统一由上下文销毁，测试不保留跨用例状态。
    return failures == 0 ? 0 : 1;
}
