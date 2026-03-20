#include "ui/imgui/DebugWindowUI.h"
#include "event/core/DebugEvent.h"
#include "event/core/EventBus.h"
#include "imgui.h"
#include "ui/imgui/ImguiTools.h"
#include "ui/layout/box/CLayBox.h"

namespace MMM::Graphic::UI
{

float DebugWindowUI::slideValue1{ 0.3f };
float DebugWindowUI::slideValue2{ 48.f };
float DebugWindowUI::slideValue3{ .66f };
float DebugWindowUI::slideValue4{ 2.f };

DebugWindowUI::DebugWindowUI(const std::string& name) : IUIView(name) {}

DebugWindowUI::~DebugWindowUI() {}

class LayoutContext
{
public:
    LayoutContext(const char* name)
    {
        // 在 Begin 之前，推入样式变量，将窗口内边距设为 0
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

        ImGui::Begin(name);
        // 1. 获取 ImGui 的绘图起始点（绝对坐标）
        m_startPos = ImGui::GetCursorScreenPos();
        m_avail    = ImGui::GetContentRegionAvail();
        // 1. 获取鼠标状态并传给 Clay
        m_mousePos    = ImGui::GetMousePos();
        m_isMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);

        // Clay 需要知道相对于布局起点的坐标
        Clay_SetPointerState(
            { m_mousePos.x - m_startPos.x, m_mousePos.y - m_startPos.y },
            m_isMouseDown);
    }
    ~LayoutContext()
    {
        // 4. 重置 ImGui 游标，防止 Dummy 影响后续内容
        ImGui::SetCursorScreenPos(m_startPos);
        ImGui::Dummy(m_avail);  // 占位，确保滚动条正确

        ImGui::End();
        // 恢复样式，否则会影响到后面其他的窗口
        ImGui::PopStyleVar();
    }

    ImVec2 m_startPos;
    ImVec2 m_avail;
    ImVec2 m_mousePos;
    bool   m_isMouseDown;
};

void DebugWindowUI::update()
{
    LayoutContext layoutCtx("DebugWindow");

    CLayVBox clayVBox;
    clayVBox.setPadding(10, 10, 0, 0).setSpacing(0);

    auto sliderH = ImGui::GetFrameHeight();

    // 我们用一个数组来循环生成，确保 ID 唯一
    std::array<float*, 4> values = {
        &slideValue1, &slideValue2, &slideValue3, &slideValue4
    };
    clayVBox.addSpring();
    // 关键点：给每个 Slider 一个唯一的 ID 字符串
    clayVBox.addElement(
        "SliderSlot1",
        Sizing::Grow(),
        Sizing::Fixed(sliderH),
        [=](auto r, bool hovered) {
            ImGui::SetNextItemWidth(r.width);
            if ( ImGui::SliderFloat("##slider1", values[0], 0.1f, 3.0f) ) {
                Event::EventBus::instance().publish<Event::DebugEvent>(
                    { .mem     = (void*)*values.data(),
                      .memSize = values.size() * sizeof(float) });
            }
        });

    clayVBox.addSpring();
    clayVBox.addElement(
        "SliderSlot2",
        Sizing::Grow(),
        Sizing::Fixed(sliderH),
        [=](auto r, bool hovered) {
            ImGui::SetNextItemWidth(r.width);
            if ( ImGui::SliderFloat("##slider2", values[1], 24.f, 256.f) ) {
                Event::EventBus::instance().publish<Event::DebugEvent>(
                    { .mem     = (void*)*values.data(),
                      .memSize = values.size() * sizeof(float) });
            }
        });
    clayVBox.addSpring();
    clayVBox.addElement(
        "SliderSlot3",
        Sizing::Grow(),
        Sizing::Fixed(sliderH),
        [=](auto r, bool hovered) {
            ImGui::SetNextItemWidth(r.width);
            if ( ImGui::SliderFloat("##slider3", values[2], .5f, 1.f) ) {
                Event::EventBus::instance().publish<Event::DebugEvent>(
                    { .mem     = (void*)*values.data(),
                      .memSize = values.size() * sizeof(float) });
            }
        });
    clayVBox.addSpring();
    clayVBox.addElement(
        "SliderSlot4",
        Sizing::Grow(),
        Sizing::Fixed(sliderH),
        [=](auto r, bool hovered) {
            ImGui::SetNextItemWidth(r.width);
            if ( ImGui::SliderFloat("##slider4", values[3], 0.f, 10.f) ) {
                Event::EventBus::instance().publish<Event::DebugEvent>(
                    { .mem     = (void*)*values.data(),
                      .memSize = values.size() * sizeof(float) });
            }
        });
    clayVBox.addSpring();
    clayVBox.render(
        layoutCtx.m_avail.x, layoutCtx.m_avail.y, layoutCtx.m_startPos);
}

}  // namespace MMM::Graphic::UI
