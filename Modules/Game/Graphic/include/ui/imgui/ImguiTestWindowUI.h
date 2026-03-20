#pragma once

#include "ui/IUIView.h"
#include <array>

namespace MMM::Graphic::UI
{
class ImguiTestWindowUI : public IUIView
{
public:
    ImguiTestWindowUI(const std::string& name) : IUIView(name) {}
    ImguiTestWindowUI(ImguiTestWindowUI&&)                 = delete;
    ImguiTestWindowUI(const ImguiTestWindowUI&)            = delete;
    ImguiTestWindowUI& operator=(ImguiTestWindowUI&&)      = delete;
    ImguiTestWindowUI& operator=(const ImguiTestWindowUI&) = delete;
    ~ImguiTestWindowUI() override                          = default;

    void update() override;

private:
    std::array<float, 4> m_clear_color{ .23f, .23f, .23f, 1.f };
};

}  // namespace MMM::Graphic::UI
