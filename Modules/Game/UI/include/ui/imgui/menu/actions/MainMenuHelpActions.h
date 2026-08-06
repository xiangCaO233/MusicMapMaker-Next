#pragma once

#include "ui/imgui/menu/interfaces/IMainMenuItemActionHandler.h"

#include <memory>

namespace MMM::UI
{

/// @brief 创建检查更新动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createCheckUpdateAction();

/// @brief 创建在系统文件管理器中定位软件配置文件的动作处理器。
std::unique_ptr<IMainMenuItemActionHandler>
createOpenSoftwareConfigurationAction();

/// @brief 创建打开皮肤目录的动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenSkinsDirectoryAction();

/// @brief 创建打开插件目录的动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenPluginsDirectoryAction();

/// @brief 创建显示关于窗口动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createShowAboutAction();

}  // namespace MMM::UI
