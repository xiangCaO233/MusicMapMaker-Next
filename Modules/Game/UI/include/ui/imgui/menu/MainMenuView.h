#pragma once

#include "common/LogicCommands.h"

namespace MMM::UI
{
class UIManager;

class MainMenuView
{
public:
    MainMenuView();
    MainMenuView(MainMenuView&&)                 = default;
    MainMenuView(const MainMenuView&)            = delete;
    MainMenuView& operator=(MainMenuView&&)      = default;
    MainMenuView& operator=(const MainMenuView&) = delete;
    ~MainMenuView();

    void update(UIManager* sourceManager);
    void handleHotkeys(UIManager* sourceManager);

private:
    void openFolderPicker();
    void openPackFilePicker();
    void openExportFilePicker(const std::string& ext);
    void dispatchCommand(const Logic::LogicCommand& cmd);

    bool m_openFileMenuNextFrame = false;
    bool m_openEditMenuNextFrame = false;
    bool m_closeFileMenuNextFrame = false;
    bool m_closeEditMenuNextFrame = false;
};

}  // namespace MMM::UI
