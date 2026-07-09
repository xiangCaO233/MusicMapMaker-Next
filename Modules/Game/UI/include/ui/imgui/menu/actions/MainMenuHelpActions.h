#pragma once

#include "ui/imgui/menu/interfaces/IMainMenuItemActionHandler.h"

#include <memory>

namespace MMM::UI
{

/// @brief 创建检查更新动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createCheckUpdateAction();

/// @brief 创建显示关于窗口动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createShowAboutAction();

}  // namespace MMM::UI
