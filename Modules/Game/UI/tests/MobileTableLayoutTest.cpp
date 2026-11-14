/**
 * @file MobileTableLayoutTest.cpp
 * @brief 验证移动表格的实际命中区域、留白及长列表滚动边界。
 * @details 使用生产尺寸策略与 FeedbackSelectable，不复制尺寸公式代替控件。
 * 无平台窗口、文件系统和音频设备；桌面基线也在同一上下文中比较。
 * 覆盖手机和平板 DPI，用户更大的既有留白不得被移动下限覆盖。
 */
#include "log/colorful-log.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include <array>
#include <cmath>
#include <imgui_internal.h>

namespace
{
/// @brief 仅发布触屏能力，复用生产公共表格策略，不建立工作区或业务会话。
class TouchPresentation final : public MMM::UI::IWorkspacePresentation
{
public:
    /// @brief 测试直接创建表格，无需提交抽屉或停靠树。
    void renderWorkspace(MMM::UI::MainDockSpaceUI&,
                         MMM::UI::UIManager&) override
    {
    }
    /// @brief 与生产移动外壳一致，关闭悬浮并启用触屏布局。
    bool allowsHoverFeedback() const override { return false; }
};

/// @brief 保留真实控件矩形与滚动数据，不借助内部赋值伪造可见行。
struct TableFixture {
    /// @brief 不同 DPI 共用相同逻辑窗口面积，尺寸值仅属于该夹具。
    float scale{ 1 };
    /// @brief 真实首行选择区、首行原点及下一行原点，用于检查布局步长。
    ImRect first{};
    /// @brief 原点差与滚动上限均由原生表格采样，独立于尺寸 helper。
    float firstY{}, nextY{}, offset{}, maximum{};
    /// @brief 各帧实际遍历数量和最后可见行，检查虚拟化与末尾可达性。
    /// @brief 点击次数跨帧保留，可检测一次轻点是否触发了重复激活。
    int rendered{}, last{}, clicks{};
    /// @brief 显式请求下一帧滚到底部，不改写 ImGui 窗口内部滚动字段。
    /// @brief 末尾请求持续提交到几何稳定，实际滚动仍由 ImGui 执行。
    bool bottom{};

    // 夹具只测固定高度列表，避免混入资源加载与业务排序造成时序噪声。
    // 分组头若复用相同行高，也应采用同样的首行实测裁剪契约。
    // 可变高度文件名换行由文件树已有回归覆盖，不由本夹具虚拟化。

    /// @brief 提交包含固定高度项目的真实滚动表格。
    /// @details 首行由裁剪器实测，调用方式与谱面、音轨列表保持一致。
    /// 所有选择区使用生产触摸高度，单元格留白单独参与原生行步长。
    /// @warning 测试逐帧调用；不执行持久化，不创建额外资源。
    void frame(ImVec2 pointer = { -100, -100 }, bool down = false)
    {
        // 使用 IO 事件队列驱动原生点击，不直接写 HoveredId 或 ActiveId。
        // 分离按下和释放帧，保留 Selectable 本身的激活时机。
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(pointer.x, pointer.y);
        io.AddMouseButtonEvent(0, down);
        ImGui::NewFrame();
        // 外壳只临时覆盖 CellPadding，结束后不能改变用户主题。
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
                            MMM::UI::workspaceTableCellPadding(scale));
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 360 * scale, 520 * scale });
        // 固定窗口复现手机窄面板，禁止 ini 或鼠标移动改变断言基准。
        // 表格滚动仍完全交给原生窗口，不手工切换可见行。
        ImGui::Begin("TableFixture",
                     nullptr,
                     ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        rendered = 0;
        last     = -1;
        if ( ImGui::BeginTable("Rows",
                               1,
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg,
                               { 0, 400 * scale }) ) {
            // 内容高度不含留白，二者由原生表格分别结算。
            const float height =
                MMM::UI::workspaceTableItemHeight(24 * scale, scale);
            // 未显式传入步长是刻意的：行包含样式留白，不能只传内容高度。
            // 这与谱面卡片的虚拟化入口一致，防止两端对高度含义理解不同。
            ImGuiListClipper clipper;
            // 首行实测后定位远端行，不在测试中指定假步长。
            // 一百行明显超出视口，裁剪失效将被遍历数量断言捕获。
            clipper.Begin(100);
            while ( clipper.Step() ) {
                for ( int row = clipper.DisplayStart; row < clipper.DisplayEnd;
                      ++row ) {
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, height);
                    ImGui::TableNextColumn();
                    // 同名文本需要独立身份，否则点击与反馈会错误共享。
                    // 生产谱面列表同样以稳定资源身份区分重名内容。
                    ImGui::PushID(row);
                    const float y = ImGui::GetCursorScreenPos().y;
                    if ( MMM::UI::FeedbackSelectable(
                             "Row",
                             false,
                             ImGuiSelectableFlags_SpanAllColumns,
                             { 0, scale == 1 ? 0 : height }) )
                        ++clicks;
                    if ( row == 0 ) {
                        // 采样原生矩形而非假定分配高度，底部点击据此定位。
                        first  = { ImGui::GetItemRectMin(),
                                   ImGui::GetItemRectMax() };
                        firstY = y;
                    }
                    // 第二行独立采样，不用首行高度推算下一行位置。
                    if ( row == 1 ) nextY = y;
                    ImGui::PopID();
                    last = row;
                    ++rendered;
                }
            }
            offset  = ImGui::GetScrollY();
            maximum = ImGui::GetScrollMaxY();
            // 原生滚动请求延后到下一帧，结果采样必须早于新请求。
            // 多推进几帧可覆盖内容首次结算前上限尚未建立的状态。
            if ( bottom ) ImGui::SetScrollY(maximum);
            ImGui::EndTable();
        }
        ImGui::End();
        ImGui::PopStyleVar();
        // 完成窗口结算而不上传绘制数据，测试不依赖 GPU 后端。
        ImGui::Render();
    }
};
}  // namespace

/// @brief 在真实 ImGui 帧中验证触屏尺寸与桌面不变的契约。
/// @details 每个 DPI 使用新上下文，避免前一用例的滚动和样式污染结果。
/// 点击位于首行下半部，旧的文字高度命中区无法通过此检查。
/// 末行验证覆盖扩大行高后滚动范围与裁剪步长不一致的常见回归。
int main()
{
    using namespace MMM::UI;
    int        failures = 0;
    const auto check    = [&](bool valid, const char* message) {
        // 聚合失败仍清理上下文，便于同时观察几何与交互回归。
        if ( !valid ) {
            ++failures;
            XERROR("Mobile table: {}", message);
        }
    };
    for ( float scale : std::array{ 1.0f, 2.0f } ) {
        // 每种 DPI 重建上下文，已滚动窗口不能污染下一个用例。
        // 物理视口随密度变大，逻辑面板面积保持一致。
        ImGui::CreateContext();
        auto& io       = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = { 800 * scale, 600 * scale };
        io.DeltaTime   = 1.0f / 60;
        // CPU 字体图集满足原生表格布局前提，像素不上传到 GPU。
        unsigned char* pixels{};
        int            width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        check(workspaceTableItemHeight(24 * scale, scale) == 24 * scale,
              "desktop item height is unchanged");
        const auto original = ImGui::GetStyle().CellPadding;
        {
            // 帧推进前绑定平台能力，与 UIManager 的业务提交顺序一致。
            // 策略不拥有窗口，不在析构时访问业务管理器。
            TouchPresentation   presentation;
            WorkspaceFrameScope scope(presentation);
            TableFixture        fixture;
            fixture.scale = scale;
            // 多帧让滚动窗口结算真实内容尺寸，不能把首帧占位当作最终布局。
            fixture.frame();
            fixture.frame();
            fixture.frame();
            check(fixture.first.GetHeight() >= 48 * scale,
                  "selectable hit rectangle reaches touch target height");
            check(fixture.nextY - fixture.firstY >= fixture.first.GetHeight(),
                  "adjacent rows do not overlap");
            check(fixture.rendered < 20, "long table remains virtualized");
            // 此上限远高于一屏可见行数，允许边缘预取，但禁止全量提交一百行。
            // 触摸目标变大后，一屏行数减少不应导致扫描整个缓存。
            // 按真实矩形底部定位，旧文字高度命中区无法通过该检查。
            // 边缘留白避免把原生 ItemSpacing 的取整差异当成回归。
            const ImVec2 tap{ fixture.first.Min.x + 30 * scale,
                              fixture.first.Max.y - 5 * scale };
            fixture.frame(tap, true);
            fixture.frame(tap, false);
            check(fixture.clicks == 1, "lower row area activates exactly once");
            // 第一屏正确不代表远端步长正确，必须检查滚到底部。
            // 保留相同列表和上下文，不重建表格掩盖累计高度误差。
            fixture.bottom = true;
            fixture.frame();
            fixture.frame();
            fixture.frame();
            check(fixture.last == 99 &&
                      std::abs(fixture.offset - fixture.maximum) < 1,
                  "last row remains reachable after enlarged row clipping");
            // 在结束帧后比较样式，不能把仍在作用域里的临时值当作泄漏。
            check(ImGui::GetStyle().CellPadding.y == original.y,
                  "frame style is restored");
            // 用户更大的留白是审美配置，不得被触屏最小值缩小。
            ImGui::GetStyle().CellPadding = { 80 * scale, 90 * scale };
            const auto large              = workspaceTableCellPadding(scale);
            check(large.x == 80 * scale && large.y == 90 * scale,
                  "larger user padding is preserved");
        }
        // 平台能力作用域结束后再次确认普通桌面调用不被遗留状态影响。
        check(workspaceTableItemHeight(24 * scale, scale) == 24 * scale,
              "touch policy does not leak into desktop");
        // 夹具只持有几何值，销毁上下文后不会再释放 ImGui 资源。
        ImGui::DestroyContext();
    }
    return failures == 0 ? 0 : 1;
}
