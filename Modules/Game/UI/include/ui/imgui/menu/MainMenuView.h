#pragma once

namespace MMM::UI
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

}  // namespace MMM::UI
