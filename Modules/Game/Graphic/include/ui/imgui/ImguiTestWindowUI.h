#pragma once

#include "ui/IUIView.h"
#include <array>

namespace MMM::Graphic::UI
{
class ImguiTestWindowUI : public IUIView
{
public:
    ImguiTestWindowUI();
    ImguiTestWindowUI(ImguiTestWindowUI&&)                 = default;
    ImguiTestWindowUI(const ImguiTestWindowUI&)            = default;
    ImguiTestWindowUI& operator=(ImguiTestWindowUI&&)      = default;
    ImguiTestWindowUI& operator=(const ImguiTestWindowUI&) = default;
    ~ImguiTestWindowUI();

    void update() override;

private:
    std::array<float, 4> m_clear_color{ .23f, .23f, .23f, 1.f };
};

}  // namespace MMM::Graphic::UI
