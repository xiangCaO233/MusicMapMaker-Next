#pragma once

namespace MMM::Graphic::UI
{

class MainMenuView
{
public:
    MainMenuView();
    MainMenuView(MainMenuView&&)                 = default;
    MainMenuView(const MainMenuView&)            = default;
    MainMenuView& operator=(MainMenuView&&)      = default;
    MainMenuView& operator=(const MainMenuView&) = default;
    ~MainMenuView();

    void update();

private:
};

}  // namespace MMM::Graphic::UI
