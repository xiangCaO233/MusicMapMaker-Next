#pragma once

#include "common/LogicCommands.h"
#include <atomic>

namespace MMM::Network
{
class UpdateChecker;
}

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
    void renderMenus(UIManager* sourceManager);
    void renderInfoText();
    void handleHotkeys(UIManager* sourceManager);

    /// @brief 获取状态信息 (用于状态栏显示)
    std::string getStatusMessage() const
    {
        return m_statusMessageTimer > 0.0f ? m_statusMessage : "";
    }

private:
    void openFolderPicker();
    void openPackFilePicker();
    void openExportFilePicker(const std::string& ext);
    void openAudioImportPicker();
    void dispatchCommand(const Logic::LogicCommand& cmd);
    void renderHelpMenu(UIManager* sourceManager);
    void renderAboutPopup();
    void renderUpdatePopup();
    void renderUpdateCheckingPopup();
    void renderUpdateSuccessPopup();
    void renderSaveTooltip();
    void startUpdateCheck();

    bool m_openFileMenuNextFrame  = false;
    bool m_openEditMenuNextFrame  = false;
    bool m_openHelpMenuNextFrame  = false;
    bool m_closeFileMenuNextFrame = false;
    bool m_closeEditMenuNextFrame = false;
    bool m_closeHelpMenuNextFrame = false;

    bool m_showAboutPopup         = false;
    bool m_showUpdatePopup        = false;
    bool m_showCheckingPopup      = false;
    bool m_showUpdateSuccessPopup = false;

    bool m_hasCheckedOnStartup = false;  ///< 是否已完成启动时的自动更新检查
    bool m_isSilentCheck       = false;  ///< 是否为静默检查 (启动时)

    float       m_saveTooltipTimer   = 0.0f;
    float       m_statusMessageTimer = 0.0f;
    std::string m_statusMessage;

    std::unique_ptr<MMM::Network::UpdateChecker> m_updateChecker;
};

}  // namespace MMM::UI
