#include "ui/imgui/DebugWindowUI.h"
#include "event/core/DebugEvent.h"
#include "event/core/EventBus.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "ui/layout/box/CLayBox.h"

namespace MMM::Graphic::UI
{

float DebugWindowUI::slideValue1{ 0.3f };
float DebugWindowUI::slideValue2{ 48.f };
float DebugWindowUI::slideValue3{ .66f };
float DebugWindowUI::slideValue4{ 2.f };

DebugWindowUI::DebugWindowUI(const std::string& name) : IUIView(name) {}

void DebugWindowUI::update()
{
    LayoutContext ctx(m_layoutCtx, "DebugWindow");

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
    clayVBox.render(ctx.m_avail.x, ctx.m_avail.y, ctx.m_startPos);
}

}  // namespace MMM::Graphic::UI
