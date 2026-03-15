#pragma once

#include "ui/IUIView.h"

namespace MMM::Graphic::UI
{
class MainDockSpaceUI : public IUIView
{
public:
    MainDockSpaceUI();
    MainDockSpaceUI(MainDockSpaceUI&&)                 = default;
    MainDockSpaceUI(const MainDockSpaceUI&)            = default;
    MainDockSpaceUI& operator=(MainDockSpaceUI&&)      = default;
    MainDockSpaceUI& operator=(const MainDockSpaceUI&) = default;
    ~MainDockSpaceUI();

    void update() override;

private:
};

}  // namespace MMM::Graphic::UI
