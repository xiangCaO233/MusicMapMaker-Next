/**
 * @file MobileDockLayoutTest.cpp
 * @brief 使用无平台窗口的 ImGui 上下文检查移动分区布局。
 * @details 测试采样实际业务窗口的内容区域，而非重复布局内部公式。
 * 标签缺失、内容高度不齐和显隐后误停靠均可独立导致断言失败。
 * 不需要 GPU，也不读取或覆盖用户实际工作区文件。
 */
#include "ui/workspace/MobileDockLayout.h"
#include "log/colorful-log.h"
#include <array>
#include <cmath>
#include <imgui_internal.h>

namespace
{
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
    // 每次 Begin 累积检查，首次出现、缩放和拖动帧都不能短暂盖住标签。

    /// @brief 将一个测试窗口送入对应分区并读取其实际内容矩形。
    /// @param panel 真实布局提供的分区身份。
    /// @param name 稳定的模拟业务窗口名，不包含随帧变化的内容。
    /// @return Begin 完成后由 ImGui 计算的可用内容边界。
    Bounds pane(MMM::UI::MobileDockPanel panel, const char* name)
    {
        layout.target(panel, name);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            MMM::UI::MobileDockLayout::tabPadding(1));
        ImGui::Begin(
            name,
            nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar();
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
        layout.render({ 0, 0 }, { 900, height }, 1, timing, preview);
        // 既检查平衡恢复，也检查自定义美化宽度没有被固定触摸尺寸替换。
        styleIntact &= ImGui::GetStyle().FramePadding.x == padding.x &&
                       ImGui::GetStyle().FramePadding.y == padding.y &&
                       ImGui::GetStyle().DockingSeparatorSize == separator;
        bounds[0] = pane(MMM::UI::MobileDockPanel::Main, "Main");
        if ( timing )
            bounds[1] = pane(MMM::UI::MobileDockPanel::Timing, "Timing");
        if ( preview )
            bounds[2] = pane(MMM::UI::MobileDockPanel::Preview, "Preview");
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
};
}  // namespace

/// @brief 验证单窗口标签、实际内容等高、宿主缩放和分区显隐。
/// @details 所有输入来自 CPU 上下文，不读取用户 ini 或实际窗口状态。
int main()
{
    using namespace MMM::UI;
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
    fixture.settle();
    const auto* timingNode = ImGui::DockBuilderGetNode(timing);
    // 节点指针仅在下一次拓扑变化前有效，这里只做连续输入不切换显隐。
    const float timingBefore = timingNode->Size.x;
    fixture.drag(
        timingNode->Pos.x - ImGui::GetStyle().DockingSeparatorSize * .5f, -60);
    check(ImGui::DockBuilderGetNode(timing)->Size.x > timingBefore + 30,
          "touch resizes main/timing separator");
    // 第二条分隔线属于外层分割，同样必须可拖动且保留两侧标签。
    const auto* previewNode = ImGui::DockBuilderGetNode(preview);
    // 以第二次拖动开始时的实际尺寸为基准，避免依赖初始默认比例。
    const float previewBefore = previewNode->Size.x;
    fixture.drag(
        previewNode->Pos.x - ImGui::GetStyle().DockingSeparatorSize * .5f, -50);
    check(ImGui::DockBuilderGetNode(preview)->Size.x > previewBefore + 25,
          "touch resizes timing/preview separator");
    // 连续拖动不允许改变垂直内容边界；宽度调整不能变回独立浮窗。
    check(std::abs(fixture.bounds[2].height - fixture.bounds[0].height) < .5f,
          "resizing widths retains equal content heights");
    fixture.settle(true, false);
    // 隐藏后继续推进真实帧，验证结果不依赖已失效节点的残留指针。
    check(fixture.layout.node(MobileDockPanel::Preview) == 0 &&
              fixture.layout.node(MobileDockPanel::Timing) != 0,
          "hide one auxiliary panel");
    fixture.settle(false, false);
    // 最终回到单区时，清除辅助 ID，防止下一次打开误用旧叶节点。
    check(fixture.titleFits, "content cannot overlap dock tabs");
    check(fixture.layout.center() != 0 &&
              fixture.layout.node(MobileDockPanel::Timing) == 0,
          "main remains when auxiliaries close");
    ImGui::DestroyContext();
    // 所有窗口、字体和节点统一由上下文销毁，测试不保留跨用例状态。
    return failures == 0 ? 0 : 1;
}
