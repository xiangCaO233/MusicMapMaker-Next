#pragma once

#include "ui/IUIView.h"

namespace MMM::Graphic::UI
{
class MainDockSpaceUI : public IUIView
{
public:
    MainDockSpaceUI(const std::string& name) : IUIView(name) {}
    MainDockSpaceUI(MainDockSpaceUI&&)                 = delete;
    MainDockSpaceUI(const MainDockSpaceUI&)            = delete;
    MainDockSpaceUI& operator=(MainDockSpaceUI&&)      = delete;
    MainDockSpaceUI& operator=(const MainDockSpaceUI&) = delete;

    ~MainDockSpaceUI() override = default;

    void update() override;

private:
};

}  // namespace MMM::Graphic::UI
