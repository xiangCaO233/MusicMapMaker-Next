#include "ui/imgui/CLayoutTestUI.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "ui/IUIView.h"
#include "ui/imgui/ImguiTools.h"
#include "ui/layout/box/CLayBox.h"

namespace MMM::Graphic::UI
{

void CLayoutTestUI::update()
{
    LayoutContext ctx{ m_layoutCtx, "Clay Powered HBox" };

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

    // 2. 加填充区 (高度设为 Grow，它会占据剩下的所有空间)
    clayVBox.addElement(
        "HoverArea",
        Sizing::Grow(),
        Sizing::Grow(),
        [&ctx](auto r, bool isHovered) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p_min   = { ctx.m_startPos.x + r.x, ctx.m_startPos.y + r.y };
            ImVec2 p_max   = { p_min.x + r.width, p_min.y + r.height };

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

    auto sliderSize = WidgetSizeHelper::Calculate(ImGuiWidget::Slider, "");
    toolbar.addElement("Slider",
                       Sizing::Grow(),
                       Sizing::Fixed(sliderSize.y),
                       [](auto r, bool h) {
                           ImGui::SetNextItemWidth(r.width);
                           static float val = 0;
                           ImGui::SliderFloat("##p", &val, 0, 100);
                       });

    // 1. 先加工具栏 (高度设为 Fit，它会靠在最顶端)
    clayVBox.addLayout(
        "Toolbar_Container", toolbar, Sizing::Grow(), Sizing::Fit());

    rootHBox.addLayout("Contents", clayVBox);

    // 一键渲染：根布局尺寸必须固定为 avail
    rootHBox.render(ctx.m_avail.x, ctx.m_avail.y, ctx.m_startPos);
}

}  // namespace MMM::Graphic::UI
