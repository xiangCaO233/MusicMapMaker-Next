#pragma once

#include "ui/imgui/menu/interfaces/IMainMenuItemActionHandler.h"

#include <memory>

namespace MMM::UI
{

/// @brief 创建打开新建项目向导的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenNewProjectWizardAction();

/// @brief 创建打开新建谱面向导的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenNewBeatmapWizardAction();

/// @brief 创建打开项目选择器的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenProjectAction();

/// @brief 创建打开当前项目目录的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenProjectDirectoryAction();

/// @brief 创建打开音频导入选择器的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenAudioImportAction();

/// @brief 创建打开最近项目的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenRecentProjectAction();

/// @brief 创建关闭当前项目的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createCloseProjectAction();

/// @brief 创建保存当前谱面的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createSaveBeatmapAction();

/// @brief 创建打开另存为流程的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createSaveBeatmapAsAction();

/// @brief 创建打开谱面打包流程的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createPackBeatmapAction();

}  // namespace MMM::UI
