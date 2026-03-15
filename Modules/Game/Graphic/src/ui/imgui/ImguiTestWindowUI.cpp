#include "ui/imgui/ImguiTestWindowUI.h"
#include "event/core/EventBus.h"
#include "event/ui/ClearColorUpdateEvent.h"
#include "imgui.h"

namespace MMM::Graphic::UI
{
ImguiTestWindowUI::ImguiTestWindowUI() {}

ImguiTestWindowUI::~ImguiTestWindowUI() {}

void ImguiTestWindowUI::update()
{
    // 2. Show a simple window that we create ourselves. We use a Begin/End pair
    // to create a named window.
    {
        ImGuiIO&     io      = ImGui::GetIO();
        static float f       = 0.0f;
        static int   counter = 0;

        ImGui::Begin(
            "Hello, world!",
            nullptr,
            ImGuiWindowFlags_NoBackground);  // Create a window called "Hello,
                                             // world!" and append into it.

        ImGui::Text("This is some useful text.");  // Display some text (you can
                                                   // use a format strings too)

        // Edit 1 float using a slider from 0.0f to 1.0f
        ImGui::SliderFloat("float", &f, 0.0f, 1.0f);

        // Edit 4 floats representing a color
        if ( ImGui::ColorEdit4("clear color", m_clear_color.data()) ) {
            // 发布ClearColorUpdate事件
            Event::EventBus::instance().publish<Event::ClearColorUpdateEvent>(
                { m_clear_color });
        }

        if ( ImGui::Button(
                 "Button") )  // Buttons return true when clicked (most widgets
                              // return true when edited/activated)
            counter++;
        ImGui::SameLine();
        ImGui::Text("counter = %d", counter);

        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
                    1000.0f / io.Framerate,
                    io.Framerate);
        ImGui::End();
    }

    // ImGui::ShowDemoWindow();  // 测试用，显示ImGui默认演示窗口
}

}  // namespace MMM::Graphic::UI
