#include "ui/imgui/CLayoutTestUI.h"
#include "imgui.h"
#include "ui/imgui/ImguiTools.h"
#include "ui/layout/box/CLayBox.h"

namespace MMM::Graphic::UI
{


CLayoutTestUI::CLayoutTestUI() {}

CLayoutTestUI::~CLayoutTestUI() {}

void CLayoutTestUI::update()
{
    // 在 Begin 之前，推入样式变量，将窗口内边距设为 0
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    ImGui::Begin("Clay Powered HBox");
    // 1. 获取 ImGui 的绘图起始点（绝对坐标）
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 avail    = ImGui::GetContentRegionAvail();
    // 1. 获取鼠标状态并传给 Clay
    ImVec2 mousePos    = ImGui::GetMousePos();
    bool   isMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    // Clay 需要知道相对于布局起点的坐标
    Clay_SetPointerState({ mousePos.x - startPos.x, mousePos.y - startPos.y },
                         isMouseDown);
    // --- 水平布局 ---
    CLayHBox rootHBox;
    rootHBox.setPadding(10, 10, 10, 10).setSpacing(10);
    rootHBox.addElement(
        "FullButton", Sizing::Grow(), Sizing::Grow(), [](auto r, bool h) {
            ImGui::Button("FullButton", { r.width, r.height });
        });

    // --- 垂直布局 ---
    CLayVBox clayVBox;
    clayVBox.setPadding(10, 10, 10, 10).setSpacing(10);

    // 2. 后加填充区 (高度设为 Grow，它会占据剩下的所有空间)
    clayVBox.addElement(
        "HoverArea",
        Sizing::Grow(),
        Sizing::Grow(),
        [startPos](auto r, bool isHovered) {
            ImDrawList* dl    = ImGui::GetWindowDrawList();
            ImVec2      p_min = { startPos.x + r.x, startPos.y + r.y };
            ImVec2      p_max = { p_min.x + r.width, p_min.y + r.height };

            dl->AddRectFilled(p_min, p_max, IM_COL32(33, 233, 233, 255), 4.0f);
            if ( isHovered ) {
                dl->AddRect(
                    p_min, p_max, IM_COL32(255, 255, 0, 255), 4.0f, 0, 2.0f);
                ImGui::SetCursorScreenPos({ p_min.x + 10, p_min.y + 10 });
                ImGui::TextColored({ 1, 1, 0, 1 }, "Hovering Content Area");
            }
        });


    // --- 子布局：水平工具栏 (高度设为 Fit) ---
    CLayHBox toolbar;
    toolbar.setSpacing(5).setAlignment(Alignment::Center());

    auto btnSize = WidgetSizeHelper::Calculate(ImGuiWidget::Button, "Play");
    toolbar.addElement(
        "PlayBtn",
        Sizing::Fixed(btnSize.x),
        Sizing::Fixed(btnSize.y),
        [](auto r, bool h) { ImGui::Button("Play", { r.width, r.height }); });
    toolbar.addElement(
        "StopBtn",
        Sizing::Fixed(btnSize.x),
        Sizing::Fixed(btnSize.y),
        [](auto r, bool h) { ImGui::Button("Stop", { r.width, r.height }); });
    toolbar.addElement(
        "Slider", Sizing::Grow(), Sizing::Fixed(30), [](auto r, bool h) {
            ImGui::SetNextItemWidth(r.width);
            static float val = 0;
            ImGui::SliderFloat("##p", &val, 0, 100);
        });

    // 1. 先加工具栏 (高度设为 Fit，它会靠在最顶端)
    clayVBox.addLayout(
        "Toolbar_Container", toolbar, Sizing::Grow(), Sizing::Fit());

    rootHBox.addLayout("Contents", clayVBox);

    // 一键渲染：根布局尺寸必须固定为 avail
    rootHBox.render(avail.x, avail.y, startPos);

    // CLayVBox rootVBox;
    // rootVBox.addElement(
    //     "HoverArea",
    //     Sizing::Grow(),
    //     Sizing::Grow(),
    //     [=](auto r, bool isHovered) {
    //         ImDrawList* dl = ImGui::GetWindowDrawList();

    //         // 1. 计算该区域在屏幕上的绝对坐标
    //         ImVec2 p_min = ImVec2(startPos.x + r.x, startPos.y + r.y);
    //         ImVec2 p_max = ImVec2(p_min.x + r.width, p_min.y + r.height);

    //         // 2. 先画一个底色（否则你看不到这个区域到底占了多大）
    //         dl->AddRectFilled(p_min, p_max, IM_COL32(35, 35, 35, 255));

    //         // 3. 如果 Hover 了，画黄色高亮边框
    //         if ( isHovered ) {
    //             dl->AddRect(
    //                 p_min, p_max, IM_COL32(255, 255, 0, 255), 0.0f, 0, 2.0f);

    //             // 在该区域内写点字测试
    //             ImGui::SetCursorScreenPos({ p_min.x + 10, p_min.y + 10 });
    //             ImGui::TextColored({ 1, 1, 0, 1 }, "Clay Hover Active!");
    //         }
    //     });

    // CLayHBox layout;
    // layout.setSpacing(5)
    //     .setPadding(5, 5, 2, 2)
    //     .setAlignment(Alignment::Center());

    // // 添加元素的同时绑定绘制逻辑
    // auto btn_text_size =
    //     WidgetSizeHelper::Calculate(ImGuiWidget::Button, "Play");
    // layout.addElement(
    //     "Btn_Play",
    //     Sizing::Fixed(btn_text_size.x),
    //     Sizing::Fixed(btn_text_size.y),
    //     [](auto r, bool isHovered) {
    //         if ( ImGui::Button("Play", { r.width, r.height }) ) { /* 开始播放
    //         */
    //         }
    //     });

    // auto btn_text_size2 =
    //     WidgetSizeHelper::Calculate(ImGuiWidget::Button, "Stop");
    // layout.addElement(
    //     "Btn_Stop",
    //     Sizing::Fixed(btn_text_size2.x),
    //     Sizing::Fixed(btn_text_size2.y),
    //     [](auto r, bool isHovered) {
    //         if ( ImGui::Button("Stop", { r.width, r.height }) ) { /* 停止播放
    //         */
    //         }
    //     });


    // auto progress_size = WidgetSizeHelper::Calculate(ImGuiWidget::Slider,
    // ""); layout.addElement(
    //     "Slider",
    //     Sizing::Grow(),
    //     Sizing::Fixed(progress_size.y),
    //     [](auto r, bool isHovered) {
    //         // 1. 获取当前 ImGui 的行高
    //         float imguiHeight = ImGui::GetFrameHeight();
    //         // 2. 计算垂直偏移量，让 Slider 在 Clay 的 40px 高度内居中
    //         float offsetY = (r.height - imguiHeight) * 0.5f;

    //         // 3. 内部偏移游标
    //         ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offsetY);

    //         ImGui::SetNextItemWidth(r.width);
    //         static float val = 0;
    //         ImGui::SliderFloat("##progress", &val, 0, 100);
    //     });

    // rootVBox.addLayout("Content", layout);

    // // 一键执行：自动计算、自动设游标、自动调绘制逻辑
    // rootVBox.render(avail.x, avail.y, startPos);

    // 4. 重置 ImGui 游标，防止 Dummy 影响后续内容
    ImGui::SetCursorScreenPos(startPos);
    ImGui::Dummy(avail);  // 占位，确保滚动条正确

    ImGui::End();
    // 恢复样式，否则会影响到后面其他的窗口
    ImGui::PopStyleVar();
}

}  // namespace MMM::Graphic::UI
